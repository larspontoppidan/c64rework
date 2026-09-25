#!/usr/bin/env python3
"""Lower accepted Stage 4 C++ onto the gamehost facade.

Tree-sitter is the sole authority for C++ syntax recognition. Preflight
(audit), lowering, and postflight (check) share the same construct
definitions. Unknown and ambiguous forms are errors with a source location.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tree_sitter_cpp as tscpp
    from tree_sitter import Language, Parser
except ImportError as exc:  # pragma: no cover - wrapper supplies dependencies
    raise SystemExit(
        "lower-standalone-game: tree-sitter-cpp is not installed; "
        "run './rework setup-venv' from the game root"
    ) from exc


PARSER = Parser(Language(tscpp.language()))
CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".h"}
DEAD_STATEMENT_CALLS = {"AtPc", "JoinAtPc", "Compare", "ExpectWatchDelta"}
CLOCK_CALLS = {
    "AdvanceCycles", "AdvanceToVSync", "JoinAtPcBounded", "ReturnIrq", "ReturnNmi",
}

# --- gamehost facade rebinding -------------------------------------------
# Exported game source binds to the standalone runtime's gamehost facade
# (src/gamehost/GameHost.hpp, src/gamehost/Log.hpp) instead of the REVM
# CpuMockHost/util surfaces. Every unknown REVM identifier, qualified name,
# include, or namespace is an export error with a source location.

FACADE_HOST_INCLUDES = {
    b"cpumock/CpuMockHost.hpp",
    b"cpumock/Mem.hpp",
    b"cpumock/VideoAssets.hpp",
}
FACADE_HOST_INCLUDE_TARGET = b'#include "gamehost/GameHost.hpp"\n'
FACADE_LOG_INCLUDE = b"util/Log.hpp"
FACADE_LOG_INCLUDE_TARGET = b'#include "gamehost/Log.hpp"\n'

LOG_IDENTIFIER_MAP = {
    "REVM_LOG_TIMED": "GAME_LOG_TIMED",
    "REVM_LOG_MODULE": "GAME_LOG_MODULE",
    "REVM_LOG": "GAME_LOG",
    "REVM_VERBOSE": "GAME_VERBOSE",
    "REVM_DEBUG": "GAME_DEBUG",
    "REVM_INFO": "GAME_INFO",
    "REVM_ERROR": "GAME_ERROR",
}

# Bare host type name (e.g. the InstallGame parameter inside the Stage 3
# entry namespace). Runs after the qualified-name pass so the two rebinds
# never overlap.
TYPE_IDENTIFIER_MAP = {
    "CpuMockHost": "GameHost",
}

FACADE_QUALIFIED_MAP = {
    "revm::cpumock::CpuMockHost": "gamehost::GameHost",
    "revm::cpumock::CpuMockStart": "gamehost::CpuMockStart",
    "revm::cpumock::IoMap": "gamehost::IoMap",
    "revm::cpumock::VideoAssets": "gamehost::VideoAssets",
    "revm::cpumock::MemTable": "gamehost::MemTable",
    "revm::cpumock::ByteAt": "gamehost::ByteAt",
    "revm::cpumock::RamSync": "gamehost::RamSync",
    "revm::cpumock::VSyncPolicy": "gamehost::VSyncPolicy",
    "revm::AdvanceResult": "gamehost::AdvanceResult",
}

FACADE_ENTRY_NAMESPACE = "revm::cpumock"
NAMESPACE_CLOSE_COMMENT = re.compile(rb"([ \t]*//[ \t]*namespace[ \t]+)(revm::cpumock)")

HOST_METHOD_MAP = {
    "Sei": "IrqDisable",
    "Cli": "IrqEnable",
    "SoftQuit": "Fail",
    "QuitRequested": "ShouldQuit",
    "AssertBegin": "AssertEntry",
    "InstallIrqHandler": "SetIrqHandler",
    "InstallNmiHandler": "SetNmiHandler",
}
HOST_CONTROL_METHODS = {
    "Sei", "Cli", "SoftQuit", "QuitRequested", "AssertBegin",
}
HOST_HANDLER_METHODS = {"InstallIrqHandler", "InstallNmiHandler"}
HOST_TYPE_NAMES = {"CpuMockHost", "GameHost"}
INTEGRAL_CAST_NAMES = frozenset({
    "unsigned", "signed", "int", "long", "short", "char",
    "uint8_t", "int8_t", "uint16_t", "int16_t",
    "uint32_t", "int32_t", "uint64_t", "int64_t",
    "size_t", "ptrdiff_t", "bool",
})
INVENTORY_CATEGORIES = (
    "twin", "sync_fence", "sync_clock", "linked_state", "linked_op",
    "linked_registration", "cpu_mock_host", "host_control", "plugin_handler",
    "revm_namespace", "revm_include", "revm_log", "diag",
    "io_surface", "asset_install",
)
RETAINED_CATEGORIES = {"io_surface", "asset_install"}
FUNCTIONAL_CAST_RELOP = re.compile(
    r"^(?:unsigned|signed|int|long|short|char|"
    r"u?int(?:8|16|32|64)_t|size_t|ptrdiff_t|bool)"
    r"\s*\([^;{}]*\)\s*(?:>=|<=|==|!=|>|<)$"
)


@dataclass(frozen=True)
class Edit:
    start: int
    end: int
    replacement: bytes
    rule: str


@dataclass(frozen=True)
class Finding:
    category: str
    operation: str
    file: str
    line: int
    column: int
    start: int
    end: int
    disposition: str
    rule: str | None = None
    detail: str = ""

    def location(self) -> str:
        return f"{self.file}:{self.line}:{self.column}"

    def to_json(self) -> dict[str, object]:
        return {
            "category": self.category,
            "operation": self.operation,
            "file": self.file,
            "line": self.line,
            "column": self.column,
            "start": self.start,
            "end": self.end,
            "disposition": self.disposition,
            "rule": self.rule,
            "detail": self.detail,
        }


@dataclass
class TypeInfo:
    name: str
    fields: dict[str, str] = field(default_factory=dict)
    methods: dict[str, str] = field(default_factory=dict)
    linked_fields: dict[str, str] = field(default_factory=dict)


@dataclass
class Analysis:
    root: Path
    files: int
    findings: list[Finding]
    parse_errors: list[Finding] = field(default_factory=list)

    def by_category(self) -> dict[str, list[Finding]]:
        grouped: dict[str, list[Finding]] = {name: [] for name in INVENTORY_CATEGORIES}
        for finding in self.findings:
            grouped.setdefault(finding.category, []).append(finding)
        return grouped

    def blocking(self, *, leftover: bool) -> list[Finding]:
        blocked = []
        for finding in self.findings:
            if finding.category in RETAINED_CATEGORIES:
                continue
            if leftover:
                if finding.disposition in {"lowerable", "unsupported", "forbidden"}:
                    blocked.append(finding)
            elif finding.disposition in {"unsupported", "forbidden"}:
                blocked.append(finding)
        return blocked


@dataclass
class Counts:
    twin_branches: int = 0
    twin_conditionals: int = 0
    at_pc: int = 0
    joins: int = 0
    compares: int = 0
    watches: int = 0
    advance_cycles: int = 0
    advance_to_vsync: int = 0
    advance_frames: int = 0
    return_irq: int = 0
    return_nmi: int = 0
    sync_includes: int = 0
    sync_usings: int = 0
    diag_watch_calls: int = 0
    diag_align_calls: int = 0
    compare_mask_usings: int = 0
    fence_slack_usings: int = 0
    linked_bytes: int = 0
    linked_words: int = 0
    linked_arrays: int = 0
    linked_reads: int = 0
    linked_writes: int = 0
    linked_increments: int = 0
    linked_decrements: int = 0
    linked_shifts: int = 0
    linked_includes: int = 0
    linked_usings: int = 0
    linked_registration_methods: int = 0
    linked_registration_calls: int = 0
    facade_includes: int = 0
    log_macros: int = 0
    qualified_types: int = 0
    host_methods: int = 0
    entry_namespaces: int = 0

    def add(self, other: "Counts") -> None:
        for name in self.__dataclass_fields__:
            setattr(self, name, getattr(self, name) + getattr(other, name))


def walk(node):
    yield node
    for child in node.named_children:
        yield from walk(child)


def parse(source: bytes, label: Path):
    return PARSER.parse(source)


def _inside_type(node, type_name: str) -> bool:
    current = node
    while current is not None:
        if current.type == type_name:
            return True
        current = current.parent
    return False


def _is_integral_cast_type(node) -> bool:
    text = node.text.decode("utf-8").split()[0] if node.text else ""
    return text in INTEGRAL_CAST_NAMES


def is_allowed_parse_error(node) -> bool:
    """Allow tree-sitter-cpp's known misparse of functional casts vs relations.

    Valid C++ such as `if (unsigned(x) >= cols)` or `if (unsigned(n) > 7u)`
    is sometimes reported as a fake declaration, leaving ERROR nodes on the
    relational operator or on the `TYPE(expr) OP` prefix. Missing nodes and
    every other error shape remain hard failures.
    """
    if node.is_missing or not node.is_error:
        return False
    parent = node.parent
    if parent is None:
        return False
    text = node.text.decode("utf-8", "replace").strip()
    if parent.type == "condition_clause" and FUNCTIONAL_CAST_RELOP.match(text):
        return True
    if parent.type == "declaration" and text in {">", "<", ">=", "<=", "==", "!=", "="}:
        if not _inside_type(parent, "condition_clause"):
            return False
        has_type = any(
            child.type in {"sized_type_specifier", "primitive_type", "type_identifier"}
            and _is_integral_cast_type(child)
            for child in parent.named_children
        )
        has_paren = any(
            child.type == "parenthesized_declarator" for child in parent.named_children
        )
        return has_type and has_paren
    return False


def disallowed_parse_errors(tree, label: Path) -> list[str]:
    messages = []
    for node in walk(tree.root_node):
        if not (node.is_error or node.is_missing):
            continue
        if is_allowed_parse_error(node):
            continue
        kind = "missing" if node.is_missing else "error"
        snippet = node.text.decode("utf-8", "replace")[:40]
        messages.append(f"{locate(node, label)}: C++ parse {kind}: {snippet!r}")
    return messages


def assert_parse_ok(source: bytes, label: Path) -> None:
    messages = disallowed_parse_errors(parse(source, label), label)
    if messages:
        raise ValueError(messages[0])


def call_name(call) -> tuple[str, object | None] | None:
    if call.type != "call_expression":
        return None
    function = call.child_by_field_name("function")
    arguments = call.child_by_field_name("arguments")
    if function is None or arguments is None:
        return None
    if function.type == "field_expression":
        field = function.child_by_field_name("field")
        receiver = function.child_by_field_name("argument")
        if field is None or receiver is None:
            return None
        return field.text.decode("utf-8"), receiver
    if function.type in {"qualified_identifier", "identifier"}:
        name = function.child_by_field_name("name") or function
        return name.text.decode("utf-8"), None
    return None


@dataclass
class LinkedInventory:
    scalars: frozenset[str]
    arrays: frozenset[str]
    registration_methods: frozenset[str]
    types: dict[str, TypeInfo] = field(default_factory=dict)
    aliases: dict[str, str] = field(default_factory=dict)


LINKED_OPERATION_CALLS = {
    "read", "write", "inc", "dec", "asl", "lsr", "rol", "ror", "address",
}
FREE_RMW_HELPERS = {"inc", "dec", "asl", "lsr", "rol", "ror"}


def declarator_name(node) -> str | None:
    if node is None:
        return None
    if node.type in {"identifier", "field_identifier"}:
        return node.text.decode("utf-8")
    child = node.child_by_field_name("declarator")
    if child is not None:
        return declarator_name(child)
    for child in node.named_children:
        name = declarator_name(child)
        if name is not None:
            return name
    return None


def linked_type(node) -> tuple[str, list[object]] | None:
    if node is None or node.type != "template_type":
        return None
    name = node.child_by_field_name("name")
    if name is None:
        name = next((child for child in node.named_children
                     if child.type in {"type_identifier", "identifier"}), None)
    if name is None:
        return None
    kind = name.text.decode("utf-8").split("::")[-1]
    if kind not in {"LinkedByte", "LinkedWord", "LinkedArray"}:
        return None
    arguments = next((child for child in node.named_children
                      if child.type == "template_argument_list"), None)
    if arguments is None:
        return None
    return kind, list(arguments.named_children)


def function_name(node) -> str | None:
    declarator = node.child_by_field_name("declarator")
    while declarator is not None:
        if declarator.type in {"identifier", "field_identifier"}:
            return declarator.text.decode("utf-8").split("::")[-1]
        name = declarator.child_by_field_name("declarator")
        if name is None:
            name = declarator.child_by_field_name("name")
        if name is None:
            candidates = [child for child in declarator.named_children
                          if child.type in {"identifier", "field_identifier",
                                            "qualified_identifier",
                                            "function_declarator"}]
            name = candidates[0] if candidates else None
        declarator = name
    return None


def inventory_linked(sources: list[tuple[Path, bytes]]) -> LinkedInventory:
    scalars: set[str] = set()
    arrays: set[str] = set()
    registration_methods: set[str] = set()
    for label, source in sources:
        root = parse(source, label).root_node
        for node in walk(root):
            if node.type in {"field_declaration", "declaration"}:
                info = linked_type(node.child_by_field_name("type"))
                if info is None:
                    continue
                kind, _ = info
                name = declarator_name(node.child_by_field_name("declarator"))
                if name is None:
                    raise ValueError(f"{locate(node, label)}: cannot name linked state")
                (arrays if kind == "LinkedArray" else scalars).add(name)
            elif node.type == "function_definition" and b".Links()" in node.text:
                name = function_name(node)
                if name is None:
                    raise ValueError(
                        f"{locate(node, label)}: cannot name linked registration method"
                    )
                body = node.child_by_field_name("body")
                calls: list[str] = []
                if body is not None:
                    for statement in body.named_children:
                        if (statement.type != "expression_statement" or
                            len(statement.named_children) != 1):
                            calls = []
                            break
                        parts = call_name(statement.named_children[0])
                        if parts is None or not parts[0].startswith("Register"):
                            calls = []
                            break
                        calls.append(parts[0])
                if not calls:
                    raise ValueError(
                        f"{locate(node, label)}: method using Links() mixes "
                        "registration with standalone behavior"
                    )
                registration_methods.add(name)
    overlap = scalars & arrays
    if overlap:
        raise ValueError("linked scalar/array names overlap: " + ", ".join(sorted(overlap)))
    types, aliases = collect_program_types(sources)
    return LinkedInventory(frozenset(scalars), frozenset(arrays),
                           frozenset(registration_methods), types, aliases)


def canon_type(name: str) -> str:
    return qualified_head(name).split("::")[-1]


def type_name_from_node(node) -> str | None:
    if node is None:
        return None
    if node.type in {"type_identifier", "primitive_type"}:
        return canon_type(node.text.decode("utf-8"))
    if node.type == "sized_type_specifier":
        text = node.text.decode("utf-8").split()
        return text[0] if text else None
    if node.type == "qualified_identifier":
        return canon_type(node.text.decode("utf-8"))
    if node.type == "template_type":
        linked = linked_type(node)
        if linked is not None:
            return linked[0]
        return type_name_from_node(node.child_by_field_name("name"))
    if node.type in {"struct_specifier", "class_specifier"}:
        return type_name_from_node(node.child_by_field_name("name"))
    if node.type in {"type_descriptor", "placeholder_type_specifier"}:
        for child in node.named_children:
            got = type_name_from_node(child)
            if got is not None:
                return got
    return None


def collect_program_types(
    sources: list[tuple[Path, bytes]],
) -> tuple[dict[str, TypeInfo], dict[str, str]]:
    types: dict[str, TypeInfo] = {}
    aliases: dict[str, str] = {}

    def ensure(name: str) -> TypeInfo:
        info = types.get(name)
        if info is None:
            info = TypeInfo(name)
            types[name] = info
        return info

    def collect_method(info: TypeInfo, func) -> None:
        name = function_name(func)
        ret = type_name_from_node(func.child_by_field_name("type"))
        if name is not None and ret is not None:
            info.methods[name] = aliases.get(ret, ret)

    def collect_field(info: TypeInfo, declaration) -> None:
        type_node = declaration.child_by_field_name("type")
        name = declarator_name(declaration.child_by_field_name("declarator"))
        if type_node is not None and type_node.type in {"struct_specifier", "class_specifier"}:
            nested = type_name_from_node(type_node)
            if nested is not None and name is not None:
                info.fields[name] = nested
            return
        linked = linked_type(type_node)
        if linked is not None and name is not None:
            kind, _ = linked
            info.linked_fields[name] = "array" if kind == "LinkedArray" else "scalar"
            info.fields[name] = kind
            return
        tname = type_name_from_node(type_node)
        if tname is not None and name is not None:
            info.fields[name] = aliases.get(tname, tname)
        declarator = declaration.child_by_field_name("declarator")
        if (declarator is not None and name is not None and tname is not None
                and any(child.type == "function_declarator"
                        or child.type == "parameter_list"
                        for child in walk(declarator))):
            info.methods[name] = aliases.get(tname, tname)

    for _, source in sources:
        root = parse(source, Path("<types>")).root_node
        for node in walk(root):
            if node.type in {"class_specifier", "struct_specifier"}:
                name = type_name_from_node(node)
                if name is None:
                    continue
                info = ensure(name)
                body = node.child_by_field_name("body")
                if body is None:
                    continue
                for child in body.named_children:
                    if child.type in {"field_declaration", "declaration"}:
                        collect_field(info, child)
                    elif child.type == "function_definition":
                        collect_method(info, child)
            elif node.type == "alias_declaration":
                ident = next((child for child in node.named_children
                              if child.type == "type_identifier"), None)
                aliased = type_name_from_node(
                    next((child for child in node.named_children
                          if child.type != "type_identifier"), None)
                )
                if ident is not None and aliased is not None:
                    aliases[ident.text.decode("utf-8")] = aliased
    return types, aliases


def resolve_alias(name: str | None, inventory: LinkedInventory) -> str | None:
    if name is None:
        return None
    seen: set[str] = set()
    while name in inventory.aliases and name not in seen:
        seen.add(name)
        name = inventory.aliases[name]
    return name


def is_host_type_name(name: str | None, inventory: LinkedInventory) -> bool:
    canonical = resolve_alias(name, inventory)
    return canonical in HOST_TYPE_NAMES


def enclosing_class_name(node) -> str | None:
    current = node
    while current is not None:
        if current.type in {"class_specifier", "struct_specifier"}:
            return type_name_from_node(current)
        current = current.parent
    return None


def method_class_from_qualified(func) -> str | None:
    declarator = func.child_by_field_name("declarator")
    if declarator is None:
        return None
    for node in walk(declarator):
        if node.type != "qualified_identifier":
            continue
        parts = [part for part in node.text.decode("utf-8").split("::") if part]
        if len(parts) >= 2:
            return parts[-2]
        break
    return None


def enclosing_function(node):
    current = node
    while current is not None:
        if current.type == "function_definition":
            return current
        current = current.parent
    return None


def function_param_map(func, inventory: LinkedInventory) -> dict[str, str]:
    result: dict[str, str] = {}
    declarator = func.child_by_field_name("declarator") if func is not None else None
    if declarator is None:
        return result
    for node in walk(declarator):
        if node.type != "parameter_list":
            continue
        for parameter in node.named_children:
            if parameter.type != "parameter_declaration":
                continue
            name = declarator_name(parameter.child_by_field_name("declarator"))
            tname = resolve_alias(
                type_name_from_node(parameter.child_by_field_name("type")),
                inventory,
            )
            if name is not None and tname is not None:
                result[name] = tname
        break
    return result


def site_params(node, inventory: LinkedInventory) -> dict[str, str]:
    params: dict[str, str] = {}
    current = node
    while current is not None:
        if current.type == "function_definition":
            params.update(function_param_map(current, inventory))
        current = current.parent
    return params


def site_class(node) -> str | None:
    nested = enclosing_class_name(node)
    if nested is not None:
        return nested
    func = enclosing_function(node)
    if func is not None:
        return method_class_from_qualified(func)
    return None


def lookup_identifier_type(name: str, node, inventory: LinkedInventory) -> str | None:
    params = site_params(node, inventory)
    if name in params:
        return params[name]
    class_name = site_class(node)
    if class_name is not None:
        info = inventory.types.get(class_name)
        if info is not None and name in info.fields:
            return resolve_alias(info.fields[name], inventory)
        if info is not None and name in info.linked_fields:
            return "LinkedArray" if info.linked_fields[name] == "array" else "LinkedByte"
    return None


def resolve_expr_type(node, inventory: LinkedInventory) -> str | None:
    if node is None:
        return None
    while node.type in {"parenthesized_expression"}:
        node = node.named_children[0] if node.named_children else node
        if node.type == "parenthesized_expression":
            continue
        break
    if node.type == "this":
        return site_class(node)
    if node.type in {"identifier", "field_identifier"}:
        return lookup_identifier_type(node.text.decode("utf-8"), node, inventory)
    if node.type == "field_expression":
        field = node.child_by_field_name("field")
        obj_type = resolve_expr_type(node.child_by_field_name("argument"), inventory)
        if field is None or obj_type is None:
            return None
        info = inventory.types.get(obj_type)
        name = field.text.decode("utf-8")
        if info is None:
            return None
        if name in info.linked_fields:
            return "LinkedArray" if info.linked_fields[name] == "array" else "LinkedByte"
        return resolve_alias(info.fields.get(name), inventory)
    if node.type == "call_expression":
        function = node.child_by_field_name("function")
        if function is None:
            return None
        if function.type == "identifier":
            class_name = site_class(node)
            info = inventory.types.get(class_name) if class_name else None
            name = function.text.decode("utf-8")
            if info is not None and name in info.methods:
                return resolve_alias(info.methods[name], inventory)
            return None
        if function.type == "field_expression":
            field = function.child_by_field_name("field")
            obj_type = resolve_expr_type(
                function.child_by_field_name("argument"), inventory)
            if field is None or obj_type is None:
                return None
            info = inventory.types.get(obj_type)
            if info is None:
                return None
            return resolve_alias(info.methods.get(field.text.decode("utf-8")), inventory)
        return None
    if node.type == "subscript_expression":
        base = (node.child_by_field_name("argument") or
                (node.named_children[0] if node.named_children else None))
        base_type = resolve_expr_type(base, inventory)
        if base_type == "LinkedArray":
            return "LinkedByte"
        return None
    return None


def linked_kind_of_expr(node, inventory: LinkedInventory) -> str | None:
    if node is None:
        return None
    if node.type == "identifier":
        name = node.text.decode("utf-8")
        if name in inventory.arrays:
            return "array"
        if name in inventory.scalars:
            return "scalar"
    resolved = resolve_expr_type(node, inventory)
    if resolved == "LinkedArray":
        return "array"
    if resolved in {"LinkedByte", "LinkedWord"}:
        return "scalar"
    if node.type == "field_expression":
        field = node.child_by_field_name("field")
        obj_type = resolve_expr_type(node.child_by_field_name("argument"), inventory)
        if field is not None and obj_type is not None:
            info = inventory.types.get(obj_type)
            if info is not None:
                return info.linked_fields.get(field.text.decode("utf-8"))
        name = field.text.decode("utf-8") if field is not None else None
        if name in inventory.arrays:
            return None
        if name in inventory.scalars and obj_type is None:
            return None
    return None


def function_template_definitions(root) -> list[object]:
    return [child
            for node in walk(root) if node.type == "template_declaration"
            for child in node.named_children if child.type == "function_definition"]


def template_type_parameters(func) -> set[str]:
    declared = func.parent
    if declared is None or declared.type != "template_declaration":
        return set()
    listed = next((child for child in declared.named_children
                   if child.type == "template_parameter_list"), None)
    names: set[str] = set()
    if listed is not None:
        for parameter in listed.named_children:
            if parameter.type != "type_parameter_declaration":
                continue
            # This grammar has no name field; the sole type_identifier child
            # is the parameter name.
            for child in parameter.named_children:
                if child.type == "type_identifier":
                    names.add(child.text.decode("utf-8"))
                    break
    return names


def template_parameter_list(func) -> list[tuple[str | None, int]]:
    """All function parameters as (name, index); None marks a non-generic one."""
    type_parameters = template_type_parameters(func)
    declarator = func.child_by_field_name("declarator")
    if declarator is None:
        return []
    for node in walk(declarator):
        if node.type != "parameter_list":
            continue
        result: list[tuple[str | None, int]] = []
        for index, parameter in enumerate(node.named_children):
            if parameter.type != "parameter_declaration":
                continue
            ptype = parameter.child_by_field_name("type")
            pdecl = parameter.child_by_field_name("declarator")
            if (ptype is None or pdecl is None or
                    ptype.type != "type_identifier" or
                    ptype.text.decode("utf-8") not in type_parameters):
                result.append((None, index))
                continue
            name = declarator_name(pdecl)
            if name is None:
                result.append((None, index))
                continue
            result.append((name, index))
        return result
    return []


def linked_op_receiver_name(receiver) -> str | None:
    if receiver.type == "subscript_expression":
        base = (receiver.child_by_field_name("argument") or
                (receiver.named_children[0] if receiver.named_children else None))
        return last_member_name(base)
    return last_member_name(receiver)


def body_uses_linked_op(func, parameter: str) -> bool:
    for node in walk(func):
        parts = call_name(node)
        if parts is None or parts[0] not in LINKED_OPERATION_CALLS:
            continue
        _, receiver = parts
        if receiver is None:
            continue
        if linked_op_receiver_name(receiver) == parameter:
            return True
    return False


def resolve_template_linked(root, label: Path,
                            inventory: LinkedInventory,
                            extra_calls: dict[str, list[object]] | None = None
                            ) -> tuple[set[str], set[str]]:
    """Resolve function-template parameters that carry linked state.

    A parameter declared over one of the function template's own type
    parameters is linked-generic when every usable call site passes an
    inventoried linked member at that position. Call sites from the rest of
    the copied tree are used when the template name is unique.
    """
    templates: dict[str, tuple[object, list[tuple[str | None, int]]]] = {}
    for func in function_template_definitions(root):
        name = function_name(func)
        if name is None:
            continue
        parameters = template_parameter_list(func)
        if not any(parameter is not None for parameter, _ in parameters):
            continue
        if name in templates:
            raise ValueError(f"{label}: ambiguous function template name: {name}")
        templates[name] = (func, parameters)
    if not templates:
        return set(), set()

    calls: dict[str, list[object]] = {}
    for node in walk(root):
        if node.type != "call_expression":
            continue
        function = node.child_by_field_name("function")
        if function is None or function.type != "identifier":
            continue
        name = function.text.decode("utf-8")
        if name in templates:
            calls.setdefault(name, []).append(node)
    if extra_calls:
        for name, nodes in extra_calls.items():
            if name in templates:
                calls.setdefault(name, []).extend(nodes)

    scalars: set[str] = set()
    arrays: set[str] = set()
    for name, (func, parameters) in templates.items():
        for parameter, index in parameters:
            if parameter is None:
                continue
            kinds: set[str] = set()
            unresolved = not calls.get(name)
            for call in calls.get(name, []):
                arguments = call_args(call)
                if index >= len(arguments):
                    raise ValueError(
                        f"{locate(call, label)}: {name} call does not match its "
                        "parameter list"
                    )
                kind = linked_kind_of_expr(arguments[index], inventory)
                if kind is None:
                    terminal = last_member_name(arguments[index])
                    if terminal in inventory.arrays:
                        kind = "array"
                    elif terminal in inventory.scalars:
                        kind = "scalar"
                if kind == "array":
                    kinds.add("array")
                elif kind == "scalar":
                    kinds.add("scalar")
                else:
                    unresolved = True
            if unresolved or len(kinds) != 1:
                if body_uses_linked_op(func, parameter):
                    raise ValueError(
                        f"{locate(func, label)}: template parameter '{parameter}' "
                        f"of {name}() receives ambiguous or non-linked state; its "
                        "linked operations cannot be lowered"
                    )
                continue
            (arrays if kinds == {"array"} else scalars).add(parameter)
    for name in scalars & arrays:
        raise ValueError(f"{label}: template parameter '{name}' is both scalar and array")
    return scalars, arrays


def collect_identifier_calls(root) -> dict[str, list[object]]:
    calls: dict[str, list[object]] = {}
    for node in walk(root):
        if node.type != "call_expression":
            continue
        function = node.child_by_field_name("function")
        if function is None or function.type != "identifier":
            continue
        calls.setdefault(function.text.decode("utf-8"), []).append(node)
    return calls


def resolve_templates_tree(
    sources: list[tuple[Path, bytes]],
    inventory: LinkedInventory,
) -> dict[Path, tuple[set[str], set[str]]]:
    parsed = [(path, parse(source, path).root_node) for path, source in sources]
    names: dict[str, list[Path]] = {}
    for path, root in parsed:
        for func in function_template_definitions(root):
            name = function_name(func)
            if name is not None:
                names.setdefault(name, []).append(path)
    all_calls: dict[str, list[object]] = {}
    for _, root in parsed:
        for name, nodes in collect_identifier_calls(root).items():
            all_calls.setdefault(name, []).extend(nodes)
    bindings: dict[Path, tuple[set[str], set[str]]] = {}
    for path, root in parsed:
        extra: dict[str, list[object]] = {}
        for name, owners in names.items():
            if path in owners and len(owners) == 1:
                extra[name] = all_calls.get(name, [])
        bindings[path] = resolve_template_linked(root, path, inventory, extra)
    return bindings


def no_twin_predicate(node) -> bool | None:
    """Return the predicate's standalone truth value, or None if unrelated."""
    while node.type in {"condition_clause", "parenthesized_expression"}:
        children = node.named_children
        if len(children) != 1:
            return None
        node = children[0]

    if node.type == "binary_expression":
        operator = next((child for child in node.children if not child.is_named), None)
        operands = node.named_children
        if operator is None or len(operands) != 2:
            return None
        left, right = operands
        left_value = no_twin_predicate(left)
        right_value = no_twin_predicate(right)
        # Respect left-to-right short-circuit evaluation. An absorbing Twin
        # predicate on the left is always safe. On the right, selecting the
        # branch would erase evaluation of the left operand, so accept only
        # syntax that cannot perform a call, assignment, or increment.
        if operator.text == b"&&":
            if left_value is False:
                return False
            if left_value is True and right_value is not None:
                return right_value
            if right_value is False and side_effect_free_predicate(left):
                return False
        if operator.text == b"||":
            if left_value is True:
                return True
            if left_value is False and right_value is not None:
                return right_value
            if right_value is True and side_effect_free_predicate(left):
                return True
        return None

    negate = False
    if node.type == "unary_expression":
        operator = next((child for child in node.children if not child.is_named), None)
        children = node.named_children
        if operator is None or operator.text != b"!" or len(children) != 1:
            return None
        negate = True
        node = children[0]

    parts = call_name(node)
    if parts is None:
        return None
    name, receiver = parts
    arguments = node.child_by_field_name("arguments")
    if receiver is None or arguments is None or arguments.named_child_count != 0:
        return None
    if name == "NoTwin":
        value = True
    elif name == "HasTwin":
        value = False
    else:
        return None
    return not value if negate else value


def side_effect_free_predicate(node) -> bool:
    """Conservative syntax check for an operand erased by branch selection."""
    while node.type in {"condition_clause", "parenthesized_expression"}:
        children = node.named_children
        if len(children) != 1:
            return False
        node = children[0]
    if node.type in {
        "identifier", "field_identifier", "number_literal", "char_literal",
        "string_literal", "true", "false", "nullptr", "this",
    }:
        return True
    if node.type == "field_expression":
        receiver = node.child_by_field_name("argument")
        return receiver is not None and side_effect_free_predicate(receiver)
    if node.type == "unary_expression":
        operator = next((child for child in node.children if not child.is_named), None)
        children = node.named_children
        return (operator is not None and operator.text in {b"!", b"~", b"+", b"-"}
                and len(children) == 1 and side_effect_free_predicate(children[0]))
    if node.type == "binary_expression":
        return all(side_effect_free_predicate(child) for child in node.named_children)
    return False


def apply_edits(source: bytes, edits: list[Edit], label: Path) -> bytes:
    ordered = sorted(edits, key=lambda edit: (edit.start, edit.end))
    previous_end = -1
    for edit in ordered:
        if edit.start < previous_end:
            raise ValueError(f"{label}: overlapping lowering edits ({edit.rule})")
        previous_end = edit.end
    result = source
    for edit in reversed(ordered):
        result = result[:edit.start] + edit.replacement + result[edit.end:]
    return result


def lower_twin_control(source: bytes, label: Path, counts: Counts) -> bytes:
    # Apply outermost predicates, reparse, then repeat. This keeps edits
    # non-overlapping while still handling arbitrarily nested Twin branches.
    while True:
        root = parse(source, label).root_node
        candidates: list[Edit] = []
        candidate_kinds: list[str] = []
        for node in walk(root):
            if node.type == "if_statement":
                condition = node.child_by_field_name("condition")
                value = no_twin_predicate(condition) if condition is not None else None
                if value is None:
                    continue
                selected = node.child_by_field_name("consequence") if value else None
                if not value:
                    alternative = node.child_by_field_name("alternative")
                    if alternative is not None and alternative.named_children:
                        selected = alternative.named_children[0]
                replacement = selected.text if selected is not None else b""
                candidates.append(Edit(node.start_byte, node.end_byte, replacement,
                                       "select-no-twin-if"))
                candidate_kinds.append("if")
            elif node.type == "conditional_expression":
                condition = node.child_by_field_name("condition")
                value = no_twin_predicate(condition) if condition is not None else None
                if value is None:
                    continue
                selected = node.child_by_field_name(
                    "consequence" if value else "alternative")
                if selected is None:
                    raise ValueError(f"{label}: malformed conditional expression")
                candidates.append(Edit(node.start_byte, node.end_byte, selected.text,
                                       "select-no-twin-conditional"))
                candidate_kinds.append("conditional")
        if not candidates:
            return source

        edits: list[Edit] = []
        kinds: list[str] = []
        containing_end = -1
        for edit, kind in zip(candidates, candidate_kinds):
            if edit.start < containing_end:
                continue
            edits.append(edit)
            kinds.append(kind)
            containing_end = edit.end
        source = apply_edits(source, edits, label)
        counts.twin_branches += kinds.count("if")
        counts.twin_conditionals += kinds.count("conditional")


def sync_call_kind(call) -> str | None:
    if call.type != "call_expression":
        return None
    function = call.child_by_field_name("function")
    if function is None or function.type != "qualified_identifier":
        return None
    # tree-sitter-cpp nests qualified_identifier nodes from the left, so use
    # the parsed function node's qualified components rather than assuming a
    # particular namespace depth.
    parts = [part for part in function.text.decode("utf-8").split("::") if part]
    if len(parts) < 2 or parts[-2] != "Sync":
        return None
    return parts[-1]


def lower_dead_sync(source: bytes, label: Path, counts: Counts) -> bytes:
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        kind = sync_call_kind(node)
        if kind == "WatchMark":
            edits.append(Edit(node.start_byte, node.end_byte, b"uint64_t{0}",
                              "lower-watch-mark"))
            counts.watches += 1
            continue
        if kind not in DEAD_STATEMENT_CALLS:
            continue
        statement = node.parent
        if statement is None or statement.type != "expression_statement":
            row, column = node.start_point
            raise ValueError(
                f"{label}:{row + 1}:{column + 1}: Sync::{kind} is not a "
                "standalone expression statement"
            )
        in_block = (statement.parent is not None and
                    statement.parent.type == "compound_statement")
        start = statement.start_byte
        if in_block:
            line_start = source.rfind(b"\n", 0, start) + 1
            if not source[line_start:start].strip():
                start = line_start
        replacement = b"" if in_block else b";"
        edits.append(Edit(start, statement.end_byte, replacement,
                          f"remove-{kind}"))
        if kind == "AtPc":
            counts.at_pc += 1
        elif kind == "JoinAtPc":
            counts.joins += 1
        else:
            counts.compares += 1
    return apply_edits(source, edits, label)


DIAG_CALLS = {
    "WatchTwinPc": "diag_watch_calls",
    "AlignMainCiaPhaseToTwin": "diag_align_calls",
}


def dead_diag_call_kind(node) -> str | None:
    """Return the dead diagnostic method called by a parsed call node."""
    if node.type != "call_expression":
        return None
    function = node.child_by_field_name("function")
    if function is None or function.type != "field_expression":
        return None
    field = function.child_by_field_name("field")
    receiver = function.child_by_field_name("argument")
    if field is None or receiver is None:
        return None
    kind = field.text.decode("utf-8")
    if kind not in DIAG_CALLS or receiver.type != "call_expression":
        return None
    diag_function = receiver.child_by_field_name("function")
    diag_args = receiver.child_by_field_name("arguments")
    if diag_function is None or diag_function.type != "field_expression":
        return None
    diag_field = diag_function.child_by_field_name("field")
    if diag_field is None or diag_field.text != b"diag":
        return None
    if diag_args is None or diag_args.named_child_count != 0:
        return None
    return kind


def _diag_statement(call):
    """Find the expression statement containing a removable diag call.

    Only an expression statement whose value is the diagnostic call, optionally
    wrapped in parentheses or a C-style ``(void)`` cast, is supported.  This
    deliberately rejects arguments, returns, assignments, conditionals, and
    other contexts where deleting a nested expression could alter semantics.
    """
    current = call
    while current.parent is not None:
        parent = current.parent
        if parent.type == "parenthesized_expression":
            if parent.named_child_count != 1 or parent.named_children[0] != current:
                return None
            current = parent
            continue
        if parent.type == "cast_expression":
            value = parent.child_by_field_name("value")
            cast_type = parent.child_by_field_name("type")
            if value != current or cast_type is None or cast_type.text.strip() != b"void":
                return None
            current = parent
            continue
        break

    statement = current.parent
    if statement is None or statement.type != "expression_statement":
        return None
    if statement.named_child_count != 1 or statement.named_children[0] != current:
        return None
    return statement


def lower_dead_diag(source: bytes, label: Path, counts: Counts) -> bytes:
    """Remove diagnostic-only Twin probes that have no standalone meaning."""
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        kind = dead_diag_call_kind(node)
        if kind is None:
            continue
        statement = _diag_statement(node)
        if statement is None:
            raise ValueError(
                f"{locate(node, label)}: standalone-dead {kind}() must be "
                "a standalone expression statement (optionally (void)-cast)"
            )
        in_block = (statement.parent is not None and
                    statement.parent.type == "compound_statement")
        start = statement.start_byte
        if in_block:
            line_start = source.rfind(b"\n", 0, start) + 1
            if not source[line_start:start].strip():
                start = line_start
        replacement = b"" if in_block else b";"
        edits.append(Edit(start, statement.end_byte, replacement,
                          f"remove-{kind}"))
        setattr(counts, DIAG_CALLS[kind],
                getattr(counts, DIAG_CALLS[kind]) + 1)
    return apply_edits(source, edits, label)


UNUSED_USING_IMPORTS = {
    "CompareMask": "compare_mask_usings",
    "FenceSlack": "fence_slack_usings",
}


def lower_unused_using_imports(source: bytes, label: Path, counts: Counts) -> bytes:
    """Remove imports made dead after standalone-only calls are lowered."""
    root = parse(source, label).root_node
    targets: list[tuple[object, str]] = []
    for node in walk(root):
        if node.type != "using_declaration" or not node.named_children:
            continue
        imported = node.named_children[-1].text.decode("utf-8").split("::")[-1]
        if imported in UNUSED_USING_IMPORTS:
            targets.append((node, imported))

    if not targets:
        return source

    edits: list[Edit] = []
    for target, imported in targets:
        used = False
        for node in walk(root):
            if node is target or (
                target.start_byte <= node.start_byte and
                node.end_byte <= target.end_byte
            ):
                continue
            if node.text == imported.encode("utf-8"):
                used = True
                break
        if used:
            continue
        edits.append(Edit(target.start_byte, target.end_byte, b"",
                          f"remove-unused-{imported}-using"))
        setattr(counts, UNUSED_USING_IMPORTS[imported],
                getattr(counts, UNUSED_USING_IMPORTS[imported]) + 1)
    return apply_edits(source, edits, label)


def locate(node, label: Path) -> str:
    row, column = node.start_point
    return f"{label}:{row + 1}:{column + 1}"


def call_args(call):
    arguments = call.child_by_field_name("arguments")
    if arguments is None:
        return None
    return list(arguments.named_children)


def trailing_args_source(source: bytes, call, skip: int) -> bytes:
    arguments = call.child_by_field_name("arguments")
    named = list(arguments.named_children)
    if len(named) <= skip:
        return b""
    return source[named[skip].start_byte:named[-1].end_byte]


def decimal_literal(node) -> str | None:
    if node.type != "number_literal":
        return None
    text = node.text.decode("utf-8")
    if not text.isdigit():
        return None
    return text


def designated_fields(node) -> dict[str, object] | None:
    if node.type == "compound_literal_expression":
        inner = next((child for child in node.named_children
                      if child.type == "initializer_list"), None)
        if inner is None:
            return None
        node = inner
    if node.type != "initializer_list":
        return None
    fields: dict[str, object] = {}
    for child in node.named_children:
        if child.type != "initializer_pair":
            return None
        designator = next((part for part in child.named_children
                           if part.type == "field_designator"), None)
        if designator is None or len(child.named_children) < 2:
            return None
        ident = next((part for part in designator.named_children
                      if part.type == "field_identifier"), None)
        if ident is None:
            return None
        name = ident.text.decode("utf-8")
        if name in fields:
            return None
        fields[name] = child.named_children[-1]
    return fields


def lower_clock(source: bytes, label: Path, counts: Counts) -> bytes:
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        kind = sync_call_kind(node)
        if kind not in CLOCK_CALLS:
            continue
        args = call_args(node)
        if not args:
            raise ValueError(f"{locate(node, label)}: Sync::{kind} has no arguments")
        host = args[0].text
        if kind == "AdvanceCycles":
            if len(args) < 2:
                raise ValueError(
                    f"{locate(node, label)}: Sync::AdvanceCycles needs a cycle count"
                )
            rest = trailing_args_source(source, node, 1)
            replacement = host + b".AdvanceCycles(" + rest + b")"
            counts.advance_cycles += 1
        elif kind == "AdvanceToVSync":
            if len(args) != 1:
                raise ValueError(
                    f"{locate(node, label)}: Sync::AdvanceToVSync takes only the host"
                )
            replacement = host + b".AdvanceToVSync()"
            counts.advance_to_vsync += 1
        elif kind == "ReturnIrq":
            if len(args) not in {3, 4}:
                raise ValueError(
                    f"{locate(node, label)}: Sync::ReturnIrq needs host, rti_pc, cycles"
                )
            replacement = host + b".ReturnIrq(" + args[2].text + b")"
            counts.return_irq += 1
        elif kind == "ReturnNmi":
            if len(args) not in {2, 3}:
                raise ValueError(
                    f"{locate(node, label)}: Sync::ReturnNmi needs host, cycles"
                )
            replacement = host + b".ReturnNmi(" + args[1].text + b")"
            counts.return_nmi += 1
        else:
            if len(args) != 3:
                raise ValueError(
                    f"{locate(node, label)}: Sync::JoinAtPcBounded needs host, pc, "
                    "and a designated wait"
                )
            fields = designated_fields(args[2])
            if fields is None or "no_twin_frames" not in fields:
                raise ValueError(
                    f"{locate(node, label)}: Sync::JoinAtPcBounded requires "
                    ".no_twin_frames"
                )
            frames = decimal_literal(fields["no_twin_frames"])
            if frames is None:
                raise ValueError(
                    f"{locate(node, label)}: Sync::JoinAtPcBounded .no_twin_frames "
                    "must be a decimal integer literal"
                )
            replacement = host + b".AdvanceFrames(" + frames.encode("ascii") + b")"
            counts.advance_frames += 1
        edits.append(Edit(node.start_byte, node.end_byte, replacement,
                          f"rebind-{kind}"))
    return apply_edits(source, edits, label)


def lower_sync_imports(source: bytes, label: Path, counts: Counts) -> bytes:
    """Remove imports made dead by the Sync call lowering above."""
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        if node.type == "preproc_include":
            target = next(
                (child for child in node.named_children
                 if child.type in {"string_literal", "system_lib_string"}),
                None,
            )
            if target is None:
                continue
            include = target.text.strip(b'"<>')
            if include != b"cpumock/Sync.hpp":
                continue
            edits.append(Edit(node.start_byte, node.end_byte, b"",
                              "remove-sync-include"))
            counts.sync_includes += 1
            continue

        if node.type != "using_declaration" or not node.named_children:
            continue
        imported = node.named_children[-1].text
        if imported.split(b"::")[-1] != b"Sync":
            continue
        edits.append(Edit(node.start_byte, node.end_byte, b"",
                          "remove-sync-using"))
        counts.sync_usings += 1
    return apply_edits(source, edits, label)


def last_member_name(node) -> str | None:
    while node is not None:
        if node.type in {"identifier", "field_identifier"}:
            return node.text.decode("utf-8")
        if node.type == "field_expression":
            field = node.child_by_field_name("field")
            return field.text.decode("utf-8") if field is not None else None
        if node.type in {"parenthesized_expression", "subscript_expression"}:
            node = (node.child_by_field_name("argument") or
                    (node.named_children[0] if node.named_children else None))
            continue
        return None
    return None


def is_linked_receiver(receiver, inventory: LinkedInventory) -> bool:
    if receiver.type == "subscript_expression":
        base = (receiver.child_by_field_name("argument") or
                (receiver.named_children[0] if receiver.named_children else None))
        return linked_kind_of_expr(base, inventory) == "array"
    return linked_kind_of_expr(receiver, inventory) == "scalar"


def remove_whole_statement(source: bytes, statement) -> tuple[int, int, bytes]:
    in_block = (statement.parent is not None and
                statement.parent.type == "compound_statement")
    start = statement.start_byte
    if in_block:
        line_start = source.rfind(b"\n", 0, start) + 1
        if not source[line_start:start].strip():
            start = line_start
    return start, statement.end_byte, b"" if in_block else b";"


def lower_linked_registration(source: bytes, label: Path, counts: Counts,
                              inventory: LinkedInventory) -> bytes:
    if not inventory.registration_methods:
        return source
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        if node.type == "function_definition" and function_name(node) in inventory.registration_methods:
            if b".Links()" not in node.text:
                continue
            start = source.rfind(b"\n", 0, node.start_byte) + 1
            if source[start:node.start_byte].strip():
                start = node.start_byte
            edits.append(Edit(start, node.end_byte, b"",
                              "remove-linked-registration-method"))
            counts.linked_registration_methods += 1
    source = apply_edits(source, edits, label)

    root = parse(source, label).root_node
    edits = []
    for node in walk(root):
        parts = call_name(node)
        if parts is None or parts[0] not in inventory.registration_methods:
            continue
        statement = node.parent
        if statement is None or statement.type != "expression_statement":
            raise ValueError(
                f"{locate(node, label)}: linked registration is not a statement"
            )
        start, end, replacement = remove_whole_statement(source, statement)
        edits.append(Edit(start, end, replacement, "remove-linked-registration-call"))
        counts.linked_registration_calls += 1
    return apply_edits(source, edits, label)


def lower_linked_operations(source: bytes, label: Path, counts: Counts,
                            inventory: LinkedInventory) -> bytes:
    # Lower innermost operations first. A write value commonly contains a
    # read of the same field, so doing the whole file in one edit pass would
    # create overlapping syntax-tree replacements.
    while True:
        root = parse(source, label).root_node
        candidates: list[tuple[object, str, object, list[object]]] = []
        for node in walk(root):
            parts = call_name(node)
            if parts is None:
                continue
            kind, receiver = parts
            if receiver is None or kind not in LINKED_OPERATION_CALLS:
                continue
            if not is_linked_receiver(receiver, inventory):
                continue
            args = call_args(node)
            assert args is not None
            if kind == "address":
                raise ValueError(
                    f"{locate(node, label)}: LinkedArray address() has no "
                    "address-free standalone meaning"
                )
            candidates.append((node, kind, receiver, args))
        if not candidates:
            return source

        edits: list[Edit] = []
        for node, kind, receiver, args in candidates:
            if any(node.start_byte < inner.start_byte and
                   inner.end_byte <= node.end_byte
                   for inner, _, _, _ in candidates):
                continue
            if kind == "read":
                if args:
                    raise ValueError(
                        f"{locate(node, label)}: linked read takes no arguments"
                    )
                replacement = receiver.text
                counts.linked_reads += 1
            elif kind == "write":
                if len(args) != 1:
                    raise ValueError(
                        f"{locate(node, label)}: linked write needs one value"
                    )
                if node.parent is None or node.parent.type != "expression_statement":
                    raise ValueError(
                        f"{locate(node, label)}: linked write is not a statement"
                    )
                replacement = receiver.text + b" = " + args[0].text
                counts.linked_writes += 1
            elif kind in {"inc", "dec"}:
                if args:
                    raise ValueError(
                        f"{locate(node, label)}: linked {kind} takes no arguments"
                    )
                replacement = (b"++" if kind == "inc" else b"--") + receiver.text
                if kind == "inc":
                    counts.linked_increments += 1
                else:
                    counts.linked_decrements += 1
            else:
                if len(args) != 1 or args[0].type != "pointer_expression" or not args[0].text.startswith(b"&"):
                    raise ValueError(
                        f"{locate(node, label)}: linked {kind} requires &carry"
                    )
                carry = args[0].text[1:].strip()
                capture = (
                    b"[&rework_rmw_value = " + receiver.text +
                    b", &rework_rmw_carry = " + carry + b"]() -> uint8_t { "
                )
                if kind == "asl":
                    body = (
                        b"rework_rmw_carry = (rework_rmw_value & 0x80u) != 0; "
                        b"rework_rmw_value = uint8_t(rework_rmw_value << 1); "
                    )
                elif kind == "lsr":
                    body = (
                        b"rework_rmw_carry = (rework_rmw_value & 0x01u) != 0; "
                        b"rework_rmw_value = uint8_t(rework_rmw_value >> 1); "
                    )
                elif kind == "rol":
                    body = (
                        b"const bool rework_carry_out = "
                        b"(rework_rmw_value & 0x80u) != 0; "
                        b"rework_rmw_value = uint8_t("
                        b"uint8_t(rework_rmw_value << 1) | "
                        b"(rework_rmw_carry ? 1u : 0u)); "
                        b"rework_rmw_carry = rework_carry_out; "
                    )
                else:
                    body = (
                        b"const bool rework_carry_out = "
                        b"(rework_rmw_value & 0x01u) != 0; "
                        b"rework_rmw_value = uint8_t("
                        b"uint8_t(rework_rmw_value >> 1) | "
                        b"(rework_rmw_carry ? 0x80u : 0u)); "
                        b"rework_rmw_carry = rework_carry_out; "
                    )
                replacement = (
                    b"(" + capture + body +
                    b"return rework_rmw_value; }())"
                )
                counts.linked_shifts += 1
            edits.append(Edit(node.start_byte, node.end_byte, replacement,
                              f"lower-linked-{kind}"))
        if not edits:
            raise ValueError(f"{label}: linked lowering made no progress")
        source = apply_edits(source, edits, label)


def include_target(node) -> bytes | None:
    if node.type != "preproc_include":
        return None
    target = next((child for child in node.named_children
                   if child.type in {"string_literal", "system_lib_string"}), None)
    if target is None:
        return None
    return target.text.strip(b'"<>')


def lower_facade_includes(source: bytes, label: Path, counts: Counts) -> bytes:
    """Rebind host and log includes onto the gamehost facade headers."""
    root = parse(source, label).root_node
    installed_host_include = any(
        include_target(node) == b"gamehost/GameHost.hpp" for node in walk(root)
        if node.type == "preproc_include"
    )
    edits: list[Edit] = []
    for node in walk(root):
        include = include_target(node)
        if include in FACADE_HOST_INCLUDES:
            # All three host headers resolve transitively through the
            # facade; keep one include, drop the rest.
            if installed_host_include:
                replacement = b""
            else:
                replacement = FACADE_HOST_INCLUDE_TARGET
                installed_host_include = True
            edits.append(Edit(node.start_byte, node.end_byte, replacement,
                              "rebind-facade-include"))
            counts.facade_includes += 1
        elif include == FACADE_LOG_INCLUDE:
            edits.append(Edit(node.start_byte, node.end_byte,
                              FACADE_LOG_INCLUDE_TARGET, "rebind-log-include"))
            counts.facade_includes += 1
    return apply_edits(source, edits, label)


def lower_facade_identifiers(source: bytes, label: Path, counts: Counts) -> bytes:
    """Rewire logging macros, severity tokens, and the bare host type name."""
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        if node.type not in {"identifier", "type_identifier"}:
            continue
        text = node.text.decode("utf-8")
        mapped = LOG_IDENTIFIER_MAP.get(text)
        if mapped is not None:
            edits.append(Edit(node.start_byte, node.end_byte,
                              mapped.encode("ascii"), "rebind-log-macro"))
            counts.log_macros += 1
            continue
        mapped = TYPE_IDENTIFIER_MAP.get(text)
        if mapped is not None:
            edits.append(Edit(node.start_byte, node.end_byte,
                              mapped.encode("ascii"), "rebind-gamehost-name"))
            counts.qualified_types += 1
    return apply_edits(source, edits, label)


def qualified_head(text: str) -> str:
    # tree-sitter-cpp folds template arguments into the qualified identifier
    # (revm::cpumock::MemTable<0x4800, 1024> is one node); only the head is
    # a namespace-qualified name.
    return text.split("<", 1)[0]


def lower_facade_qualified(source: bytes, label: Path, counts: Counts) -> bytes:
    """Rebind REVM-qualified names onto the gamehost facade, fail-closed."""
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        if node.type != "qualified_identifier":
            continue
        text = node.text.decode("utf-8")
        if not text.startswith("revm::"):
            continue
        head = qualified_head(text)
        mapped = FACADE_QUALIFIED_MAP.get(head)
        if mapped is None:
            raise ValueError(
                f"{locate(node, label)}: unsupported revm-qualified name "
                f"has no gamehost rewrite: {head}"
            )
        edits.append(Edit(node.start_byte, node.start_byte + len(head.encode("ascii")),
                          mapped.encode("ascii"), "rebind-gamehost-name"))
        counts.qualified_types += 1
    return apply_edits(source, edits, label)


def host_call_receiver_name(call) -> str | None:
    """Terminal member of a host-method call receiver (host_, host, g.host())."""
    function = call.child_by_field_name("function")
    if function is None or function.type != "field_expression":
        return None
    receiver = function.child_by_field_name("argument")
    if receiver is not None and receiver.type == "call_expression":
        inner = receiver.child_by_field_name("function")
        if inner is not None and inner.type == "field_expression":
            receiver = inner
    if receiver is None:
        return None
    if receiver.type == "field_expression":
        field = receiver.child_by_field_name("field")
        return field.text.decode("utf-8") if field is not None else None
    if receiver.type in {"identifier", "this"}:
        return receiver.text.decode("utf-8")
    return None


def is_host_accessor_call(node) -> bool:
    """True for the supported zero-argument host() accessor."""
    if node is None or node.type != "call_expression":
        return False
    arguments = node.child_by_field_name("arguments")
    function = node.child_by_field_name("function")
    if arguments is None or function is None or arguments.named_child_count != 0:
        return False
    if function.type == "identifier":
        return function.text == b"host"
    if function.type == "field_expression":
        field = function.child_by_field_name("field")
        return field is not None and field.text == b"host"
    return False


def is_host_expr(node, inventory: LinkedInventory) -> bool:
    if is_host_accessor_call(node):
        return True
    return is_host_type_name(resolve_expr_type(node, inventory), inventory)


def receiver_is_host(call, inventory: LinkedInventory) -> bool:
    function = call.child_by_field_name("function")
    if function is None or function.type != "field_expression":
        return False
    return is_host_expr(function.child_by_field_name("argument"), inventory)


def lower_facade_host_methods(source: bytes, label: Path, counts: Counts,
                              inventory: LinkedInventory | None = None) -> bytes:
    """Rename host control calls onto the gamehost facade methods."""
    if inventory is None:
        inventory = inventory_linked([(label, source)])
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        if node.type != "call_expression":
            continue
        function = node.child_by_field_name("function")
        if function is None or function.type != "field_expression":
            continue
        field = function.child_by_field_name("field")
        if field is None:
            continue
        name = field.text.decode("utf-8")
        if name not in HOST_METHOD_MAP:
            continue
        if not receiver_is_host(node, inventory):
            receiver = host_call_receiver_name(node)
            raise ValueError(
                f"{locate(node, label)}: {name}() on non-host receiver "
                f"{receiver!r} has no gamehost rewrite"
            )
        edits.append(Edit(field.start_byte, field.end_byte,
                          HOST_METHOD_MAP[name].encode("ascii"),
                          "rebind-host-method"))
        counts.host_methods += 1
    return apply_edits(source, edits, label)


def lower_facade_namespace(source: bytes, label: Path, counts: Counts) -> bytes:
    """Move the Stage 3 entry namespace to its gamehost home, fail-closed."""
    root = parse(source, label).root_node
    edits: list[Edit] = []
    for node in walk(root):
        if node.type != "namespace_definition":
            continue
        name = node.child_by_field_name("name")
        if name is None or "revm" not in name.text.decode("utf-8"):
            continue
        text = name.text.decode("utf-8")
        if text != FACADE_ENTRY_NAMESPACE:
            raise ValueError(
                f"{locate(node, label)}: unsupported revm namespace: {text}"
            )
        edits.append(Edit(name.start_byte, name.end_byte, b"gamehost",
                          "rebind-entry-namespace"))
        counts.entry_namespaces += 1
        # Keep the block's trailing namespace comment in step with the name.
        match = NAMESPACE_CLOSE_COMMENT.match(source, node.end_byte)
        if match is not None:
            # Match spans are absolute positions in `source`.
            edits.append(Edit(match.start(2), match.end(2),
                              b"gamehost", "rebind-entry-namespace-comment"))
    return apply_edits(source, edits, label)


def lower_game_facade(source: bytes, label: Path, counts: Counts,
                      inventory: LinkedInventory | None = None) -> bytes:
    source = lower_facade_includes(source, label, counts)
    source = lower_facade_qualified(source, label, counts)
    source = lower_facade_identifiers(source, label, counts)
    source = lower_facade_host_methods(source, label, counts, inventory)
    source = lower_facade_namespace(source, label, counts)
    return source


def lower_linked_types(source: bytes, label: Path, counts: Counts) -> bytes:
    root = parse(source, label).root_node
    edits: list[Edit] = []
    saw_array = False
    for node in walk(root):
        info = linked_type(node)
        if info is None:
            continue
        kind, args = info
        if kind == "LinkedByte":
            if len(args) != 1:
                raise ValueError(f"{locate(node, label)}: malformed LinkedByte")
            replacement = b"uint8_t"
            counts.linked_bytes += 1
        elif kind == "LinkedWord":
            if len(args) != 1:
                raise ValueError(f"{locate(node, label)}: malformed LinkedWord")
            replacement = b"uint16_t"
            counts.linked_words += 1
        else:
            if len(args) != 2:
                raise ValueError(f"{locate(node, label)}: malformed LinkedArray")
            replacement = b"std::array<uint8_t, " + args[1].text + b">"
            counts.linked_arrays += 1
            saw_array = True
        edits.append(Edit(node.start_byte, node.end_byte, replacement,
                          f"lower-{kind}"))
    source = apply_edits(source, edits, label)

    root = parse(source, label).root_node
    edits = []
    for node in walk(root):
        if node.type == "preproc_include":
            target = next((child for child in node.named_children
                           if child.type in {"string_literal", "system_lib_string"}), None)
            if target is not None and target.text.strip(b'"<>') == b"cpumock/Linked.hpp":
                replacement = b"#include <array>\n" if saw_array else b""
                edits.append(Edit(node.start_byte, node.end_byte, replacement,
                                  "remove-linked-include"))
                counts.linked_includes += 1
        elif node.type == "using_declaration" and node.named_children:
            imported = node.named_children[-1].text.decode("utf-8").split("::")[-1]
            if imported in {"LinkedByte", "LinkedWord", "LinkedArray"}:
                edits.append(Edit(node.start_byte, node.end_byte, b"",
                                  "remove-linked-using"))
                counts.linked_usings += 1
    return apply_edits(source, edits, label)


def _finding(category: str, operation: str, node, label: Path, disposition: str,
             rule: str | None = None, detail: str = "") -> Finding:
    row, column = node.start_point
    return Finding(
        category=category,
        operation=operation,
        file=str(label),
        line=row + 1,
        column=column + 1,
        start=node.start_byte,
        end=node.end_byte,
        disposition=disposition,
        rule=rule,
        detail=detail,
    )


def _twin_condition_finding(node, label: Path) -> Finding | None:
    condition = node.child_by_field_name("condition")
    if condition is None:
        return None
    value = no_twin_predicate(condition)
    kind = "if" if node.type == "if_statement" else "conditional"
    if value is not None:
        return _finding(
            "twin", "HasTwin" if value is False else "NoTwin", node, label,
            "lowerable",
            "select-no-twin-if" if kind == "if" else "select-no-twin-conditional",
        )
    for child in walk(condition):
        parts = call_name(child)
        if parts is not None and parts[0] in {"HasTwin", "NoTwin"}:
            return _finding(
                "twin", parts[0], child, label, "unsupported",
                detail=f"unsupported {parts[0]}() context",
            )
    return None


def _io_chain(node) -> bool:
    current = node
    while current is not None:
        if current.type in {"identifier", "field_identifier"}:
            if current.text == b"io":
                return True
        if current.type == "field_expression":
            field = current.child_by_field_name("field")
            if field is not None and field.text == b"io":
                return True
            current = current.child_by_field_name("argument")
            continue
        if current.type == "call_expression":
            current = current.child_by_field_name("function")
            continue
        if current.type == "subscript_expression":
            current = (current.child_by_field_name("argument") or
                       (current.named_children[0] if current.named_children else None))
            continue
        break
    return False


def _asset_install(call) -> bool:
    parts = call_name(call)
    if parts is None or parts[0] != "install" or parts[1] is None:
        return False
    receiver = parts[1]
    if receiver.type == "field_expression":
        field = receiver.child_by_field_name("field")
        if field is not None and field.text == b"charset":
            return True
    if receiver.type == "call_expression":
        inner = call_name(receiver)
        return inner is not None and inner[0] == "slot"
    return False


def analyze_file(source: bytes, label: Path,
                 inventory: LinkedInventory,
                 template_scalars: set[str] | None = None,
                 template_arrays: set[str] | None = None) -> list[Finding]:
    if template_scalars or template_arrays:
        inventory = LinkedInventory(
            frozenset(inventory.scalars | (template_scalars or set())),
            frozenset(inventory.arrays | (template_arrays or set())),
            inventory.registration_methods,
            inventory.types,
            inventory.aliases,
        )
    tree = parse(source, label)
    findings: list[Finding] = []
    for node in walk(tree.root_node):
        if node.type in {"if_statement", "conditional_expression"}:
            found = _twin_condition_finding(node, label)
            if found is not None:
                findings.append(found)

        kind = sync_call_kind(node)
        if kind in DEAD_STATEMENT_CALLS | {"WatchMark"}:
            statement = node.parent
            supported = statement is not None and statement.type == "expression_statement"
            if kind == "WatchMark":
                supported = True
            findings.append(_finding(
                "sync_fence", kind, node, label,
                "lowerable" if supported else "unsupported",
                "lower-watch-mark" if kind == "WatchMark" else f"remove-{kind}",
            ))
        elif kind in CLOCK_CALLS:
            findings.append(_finding(
                "sync_clock", kind, node, label, "lowerable", f"rebind-{kind}"))

        diag = dead_diag_call_kind(node)
        if diag is not None:
            supported = _diag_statement(node) is not None
            findings.append(_finding(
                "diag", diag, node, label,
                "lowerable" if supported else "unsupported",
                f"remove-{diag}",
            ))

        include = include_target(node)
        if include == b"cpumock/Sync.hpp":
            findings.append(_finding(
                "revm_include", "Sync.hpp", node, label, "lowerable",
                "remove-sync-include"))
        elif include == b"cpumock/Linked.hpp":
            findings.append(_finding(
                "revm_include", "Linked.hpp", node, label, "lowerable",
                "remove-linked-include"))
        elif include in FACADE_HOST_INCLUDES or include == FACADE_LOG_INCLUDE:
            findings.append(_finding(
                "revm_include", include.decode("utf-8"), node, label, "lowerable",
                "rebind-facade-include"))
        elif include is not None and (
                include.startswith(b"cpumock/") or include.startswith(b"util/")):
            findings.append(_finding(
                "revm_include", include.decode("utf-8"), node, label, "unsupported"))

        if node.type == "using_declaration" and node.named_children:
            imported = node.named_children[-1].text.decode("utf-8").split("::")[-1]
            if imported == "Sync":
                findings.append(_finding(
                    "revm_namespace", "Sync", node, label, "lowerable",
                    "remove-sync-using"))
            elif imported in {"LinkedByte", "LinkedWord", "LinkedArray"}:
                findings.append(_finding(
                    "revm_namespace", imported, node, label, "lowerable",
                    "remove-linked-using"))
            elif imported in UNUSED_USING_IMPORTS:
                findings.append(_finding(
                    "sync_fence", imported, node, label, "lowerable",
                    f"remove-unused-{imported}-using"))

        info = linked_type(node)
        if info is not None:
            kind_name, _ = info
            findings.append(_finding(
                "linked_state", kind_name, node, label, "lowerable",
                f"lower-{kind_name}"))

        parts = call_name(node)
        if parts is not None:
            name, receiver = parts
            if name in LINKED_OPERATION_CALLS and receiver is not None:
                if is_linked_receiver(receiver, inventory):
                    disposition = "unsupported" if name == "address" else "lowerable"
                    findings.append(_finding(
                        "linked_op", name, node, label, disposition,
                        None if name == "address" else f"lower-linked-{name}",
                        "LinkedArray address() has no address-free standalone meaning"
                        if name == "address" else "",
                    ))
                elif _io_chain(receiver) and name in {
                    "read", "write", "inc", "dec", "asl", "lsr", "rol", "ror",
                }:
                    findings.append(_finding(
                        "io_surface", name, node, label, "retained"))
            if name in inventory.registration_methods:
                findings.append(_finding(
                    "linked_registration", name, node, label, "lowerable",
                    "remove-linked-registration-call"))
            if receiver is None and name in FREE_RMW_HELPERS:
                findings.append(_finding(
                    "linked_op", name, node, label, "forbidden",
                    detail=f"free {name}() helper is forbidden in Stage 4"))
            if name in HOST_CONTROL_METHODS and receiver is not None:
                findings.append(_finding(
                    "host_control", name, node, label,
                    "lowerable" if receiver_is_host(node, inventory) else "unsupported",
                    "rebind-host-method",
                ))
            if name in HOST_HANDLER_METHODS and receiver is not None:
                findings.append(_finding(
                    "plugin_handler", name, node, label,
                    "lowerable" if receiver_is_host(node, inventory) else "unsupported",
                    "rebind-host-method",
                ))
            if _asset_install(node):
                findings.append(_finding(
                    "asset_install", "install", node, label, "retained"))

        if (node.type == "function_definition"
                and function_name(node) in inventory.registration_methods
                and b".Links()" in node.text):
            findings.append(_finding(
                "linked_registration", function_name(node) or "Register",
                node, label, "lowerable", "remove-linked-registration-method"))

        if node.type in {"identifier", "type_identifier"}:
            text = node.text.decode("utf-8")
            if text in LOG_IDENTIFIER_MAP:
                findings.append(_finding(
                    "revm_log", text, node, label, "lowerable", "rebind-log-macro"))
            elif text == "CpuMockHost":
                findings.append(_finding(
                    "cpu_mock_host", text, node, label, "lowerable",
                    "rebind-gamehost-name"))

        if node.type == "qualified_identifier":
            if node.parent is not None and node.parent.type == "qualified_identifier":
                continue
            text = node.text.decode("utf-8")
            if not text.startswith("revm::"):
                continue
            head = qualified_head(text)
            parts = [part for part in head.split("::") if part]
            if len(parts) >= 2 and parts[-2] == "Sync":
                continue
            mapped = FACADE_QUALIFIED_MAP.get(head)
            removed = parts[-1] in {
                "CompareMask", "FenceSlack", "Sync",
                "LinkedByte", "LinkedWord", "LinkedArray",
            }
            findings.append(_finding(
                "revm_namespace", head, node, label,
                "lowerable" if mapped or removed else "unsupported",
                "rebind-gamehost-name" if mapped else (
                    "remove-revm-using" if removed else None),
                "" if mapped or removed else
                f"unsupported revm-qualified name has no gamehost rewrite: {head}",
            ))

        if node.type == "namespace_definition":
            name = node.child_by_field_name("name")
            if name is not None and "revm" in name.text.decode("utf-8"):
                text = name.text.decode("utf-8")
                findings.append(_finding(
                    "revm_namespace", text, node, label,
                    "lowerable" if text == FACADE_ENTRY_NAMESPACE else "unsupported",
                    "rebind-entry-namespace" if text == FACADE_ENTRY_NAMESPACE else None,
                    "" if text == FACADE_ENTRY_NAMESPACE else
                    f"unsupported revm namespace: {text}",
                ))

        if (node.type in {"identifier", "namespace_identifier"}
                and node.text == b"revm"):
            parent = node.parent
            if parent is not None and parent.type in {
                "qualified_identifier", "namespace_definition",
                "nested_namespace_specifier",
            }:
                continue
            findings.append(_finding(
                "revm_namespace", "revm", node, label, "forbidden",
                detail="revm namespace identifier remains",
            ))

    return findings


def analyze_sources(sources: list[tuple[Path, bytes]], root: Path) -> Analysis:
    if not sources:
        raise ValueError(f"{root}: no C++ sources found")
    parse_findings: list[Finding] = []
    for path, source in sources:
        messages = disallowed_parse_errors(parse(source, path), path)
        for message in messages:
            parse_findings.append(Finding(
                category="parse", operation="error", file=str(path),
                line=1, column=1, start=0, end=0,
                disposition="unsupported", detail=message,
            ))
    if parse_findings:
        return Analysis(root, len(sources), [], parse_findings)
    inventory = inventory_linked(sources)
    bindings = resolve_templates_tree(sources, inventory)
    findings: list[Finding] = []
    for path, source in sources:
        scalars, arrays = bindings.get(path, (set(), set()))
        findings.extend(analyze_file(source, path, inventory, scalars, arrays))
    return Analysis(root, len(sources), findings, [])


def analyze_tree(root: Path) -> Analysis:
    files = cpp_files(root)
    sources = [(path, path.read_bytes()) for path in files]
    return analyze_sources(sources, root)


def relabel_analysis(analysis: Analysis) -> Analysis:
    findings = []
    for finding in analysis.findings:
        try:
            relative = Path(finding.file).resolve().relative_to(
                analysis.root.resolve()).as_posix()
        except ValueError:
            relative = finding.file
        findings.append(Finding(
            finding.category, finding.operation, relative, finding.line,
            finding.column, finding.start, finding.end, finding.disposition,
            finding.rule, finding.detail,
        ))
    return Analysis(analysis.root, analysis.files, findings, analysis.parse_errors)


def print_inventory(analysis: Analysis) -> None:
    print(f"standalone-lowering: {analysis.files} C++ files under {analysis.root}")
    grouped = analysis.by_category()
    for category in INVENTORY_CATEGORIES:
        items = grouped.get(category, [])
        if not items:
            continue
        sites = ", ".join(f"{item.file}:{item.line}" for item in items[:4])
        suffix = "" if len(items) <= 4 else ", ..."
        retained = " (retained)" if category in RETAINED_CATEGORIES else ""
        print(f"  {category}: {len(items)} ({sites}{suffix}){retained}")


def analysis_json(analysis: Analysis) -> dict[str, object]:
    grouped = analysis.by_category()
    return {
        "root": str(analysis.root),
        "files": analysis.files,
        "counts": {name: len(grouped.get(name, [])) for name in INVENTORY_CATEGORIES},
        "findings": [finding.to_json() for finding in analysis.findings],
    }


def fail_analysis(analysis: Analysis, *, leftover: bool) -> None:
    if analysis.parse_errors:
        raise ValueError(analysis.parse_errors[0].detail)
    blocked = analysis.blocking(leftover=leftover)
    if not blocked:
        return
    first = blocked[0]
    detail = first.detail or f"{first.operation} ({first.disposition})"
    raise ValueError(f"{first.location()}: {detail}")


def audit_tree(root: Path) -> Analysis:
    analysis = relabel_analysis(analyze_tree(root))
    fail_analysis(analysis, leftover=False)
    return analysis


def check_tree(root: Path) -> Analysis:
    analysis = relabel_analysis(analyze_tree(root))
    fail_analysis(analysis, leftover=True)
    return analysis


def lower_source(source: bytes, label: Path,
                 inventory: LinkedInventory | None = None,
                 template_bindings: tuple[set[str], set[str]] | None = None
                 ) -> tuple[bytes, Counts]:
    counts = Counts()
    assert_parse_ok(source, label)
    if inventory is None:
        inventory = inventory_linked([(label, source)])
    if template_bindings is None:
        template_scalars, template_arrays = resolve_template_linked(
            parse(source, label).root_node, label, inventory)
    else:
        template_scalars, template_arrays = template_bindings
    if template_scalars or template_arrays:
        inventory = LinkedInventory(
            frozenset(inventory.scalars | template_scalars),
            frozenset(inventory.arrays | template_arrays),
            inventory.registration_methods,
            inventory.types,
            inventory.aliases,
        )

    def step(fn, current: bytes, *args) -> bytes:
        result = fn(current, label, counts, *args)
        assert_parse_ok(result, label)
        return result

    lowered = step(lower_twin_control, source)
    lowered = step(lower_dead_sync, lowered)
    lowered = step(lower_dead_diag, lowered)
    lowered = step(lower_clock, lowered)
    lowered = step(lower_sync_imports, lowered)
    lowered = step(lower_unused_using_imports, lowered)
    lowered = step(lower_linked_registration, lowered, inventory)
    lowered = step(lower_linked_operations, lowered, inventory)
    lowered = step(lower_linked_types, lowered)
    lowered = step(lambda src, lab, cnt: lower_game_facade(src, lab, cnt, inventory),
                   lowered)
    leftover = analyze_file(lowered, label, inventory)
    blocked = [finding for finding in leftover
               if finding.category not in RETAINED_CATEGORIES
               and finding.disposition in {"lowerable", "unsupported", "forbidden"}]
    if blocked:
        first = blocked[0]
        detail = first.detail or f"{first.operation} remains after lowering"
        raise ValueError(f"{first.location()}: {detail}")
    return lowered, counts


def cpp_files(root: Path) -> list[Path]:
    if root.is_file():
        return [root] if root.suffix in CPP_SUFFIXES else []
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and path.suffix in CPP_SUFFIXES)


def lower_tree(root: Path) -> Counts:
    files = cpp_files(root)
    if not files:
        raise ValueError(f"{root}: no C++ sources found")
    sources = [(path, path.read_bytes()) for path in files]
    preflight = analyze_sources(sources, root)
    fail_analysis(preflight, leftover=False)
    lowerable = [finding for finding in preflight.findings
                 if finding.disposition == "lowerable"]
    if not lowerable:
        raise ValueError(
            f"{root}: no lowerable Stage 4 constructs (already lowered?)"
        )
    inventory = inventory_linked(sources)
    bindings = resolve_templates_tree(sources, inventory)
    total = Counts()
    for path, source in sources:
        lowered, counts = lower_source(
            source, path, inventory, bindings.get(path))
        if lowered != source:
            path.write_bytes(lowered)
        total.add(counts)
    check_tree(root)
    return total


def selftest() -> int:
    fixture = b'''\
#include "cpumock/Sync.hpp"
#include "cpumock/Mem.hpp"
using revm::cpumock::CompareMask;
using revm::cpumock::FenceSlack;
using revm::cpumock::Sync;
using revm::cpumock::VSyncPolicy;
void fixture() {
  if (host_.NoTwin()) main_path(); else twin_path();
  if (host_.HasTwin()) { twin_only(); }
  if (!host_.HasTwin()) main_too();
  if (host_.NoTwin()) { if (host_.HasTwin()) twin_nested(); else nested_main(); }
  const char* mode = host_.NoTwin() ? "main" : "twin";
  if (host_.HasTwin() && paired) Sync::AtPc(host_, 0xAA01);
  if (paired && host_.HasTwin() && extra) twin_and(); else main_and();
  if (host_.NoTwin() || paired) main_or_a(); else twin_or_a();
  if (paired || host_.NoTwin()) main_or_b(); else twin_or_b();
  Sync::AtPc(host_, 0x1234);
  revm::cpumock::Sync::JoinAtPc(host_, 0x1235);
  Sync::Compare(host_);
  mark = Sync::WatchMark(host_, 0x1236);
  if (ordinary()) Sync::AtPc(host_, 0x1237);
  const char* text = "Sync::AtPc host_.HasTwin()";
  // Sync::JoinAtPc(host_, 0x9999);
  Sync::AdvanceCycles(host_, 10);
  Sync::AdvanceCycles(host_, 64, VSyncPolicy::Cross);
  (void)Sync::AdvanceToVSync(host_);
  Sync::ReturnIrq(host_, 0xF200, 0);
  Sync::ReturnNmi(g.host(), 0);
  const uint32_t n = Sync::JoinAtPcBounded(
      host_, 0x03C0,
      {.max_frames = 8, .no_twin_frames = 1, .mask = CompareMask::None(),
       .fence = FenceSlack::Exact});
  revm::cpumock::Sync::AdvanceCycles(host_, 5u);
  host_.diag().WatchTwinPc(0x1234, "watch");
  (void)host_.diag().AlignMainCiaPhaseToTwin();
}
'''
    expected = b'''\
#include "gamehost/GameHost.hpp"



using gamehost::VSyncPolicy;
void fixture() {
  main_path();
\x20\x20
  main_too();
  { nested_main(); }
  const char* mode = "main";
\x20\x20
  main_and();
  main_or_a();
  main_or_b();



  mark = uint64_t{0};
  if (ordinary()) ;
  const char* text = "Sync::AtPc host_.HasTwin()";
  // Sync::JoinAtPc(host_, 0x9999);
  host_.AdvanceCycles(10);
  host_.AdvanceCycles(64, VSyncPolicy::Cross);
  (void)host_.AdvanceToVSync();
  host_.ReturnIrq(0);
  g.host().ReturnNmi(0);
  const uint32_t n = host_.AdvanceFrames(1);
  host_.AdvanceCycles(5u);


}
'''
    lowered, counts = lower_source(fixture, Path("selftest.cpp"))
    if lowered != expected:
        print("lower-standalone-game selftest: FAIL", file=sys.stderr)
        print(lowered.decode("utf-8"), file=sys.stderr)
        return 1
    if counts != Counts(twin_branches=9, twin_conditionals=1, at_pc=2,
                        joins=1, compares=1, watches=1, advance_cycles=3,
                        advance_to_vsync=1, advance_frames=1, return_irq=1,
                        return_nmi=1, sync_includes=1, sync_usings=1,
                        diag_watch_calls=1, diag_align_calls=1,
                        compare_mask_usings=1, fence_slack_usings=1,
                        facade_includes=1, qualified_types=1):
        print(f"lower-standalone-game selftest: bad counts {counts}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "fixture.cpp"
        path.write_bytes(fixture)
        lower_tree(path)
        if path.read_bytes() != expected:
            print("lower-standalone-game selftest: tree write FAIL", file=sys.stderr)
            return 1
    linked_fixture = b'''\
#include "cpumock/Linked.hpp"
using revm::cpumock::LinkedArray;
using revm::cpumock::LinkedByte;
struct S {
  LinkedByte<0x20> value{7};
  LinkedArray<0x30, 2> data{{1, 2}};
  void Register(Host &host) { value.Register(host.Links(), "value"); }
};
void f(S &s, Io &io) {
  s.value.write(uint8_t(s.value.read() + 1));
  const auto next = s.data[0].inc();
  bool carry = true;
  const auto shifted = s.data[1].rol(&carry);
  io.vic.bg0.write(io.vic.bg0.read());
}
'''
    linked, linked_counts = lower_source(linked_fixture, Path("linked.cpp"))
    if (b"Linked" in linked or b".Register(" in linked or b".Links()" in linked):
        print("lower-standalone-game selftest: linked state survived", file=sys.stderr)
        return 1
    if (b"uint8_t value{7}" not in linked or
        b"std::array<uint8_t, 2> data{{1, 2}}" not in linked or
        b"s.value = uint8_t(s.value + 1)" not in linked or
        b"const auto next = ++s.data[0]" not in linked or
        b"gamehost::rol" in linked or
        b"rework_rmw_value = uint8_t(" not in linked or
        b"rework_rmw_carry = rework_carry_out" not in linked or
        b"io.vic.bg0.write(io.vic.bg0.read())" not in linked):
        print("lower-standalone-game selftest: bad linked lowering", file=sys.stderr)
        print(linked.decode("utf-8"), file=sys.stderr)
        return 1
    if (linked_counts.linked_bytes != 1 or linked_counts.linked_arrays != 1 or
        linked_counts.linked_reads != 1 or linked_counts.linked_writes != 1 or
        linked_counts.linked_increments != 1 or
        linked_counts.linked_shifts != 1 or
        linked_counts.linked_registration_methods != 1):
        print(f"lower-standalone-game selftest: bad linked counts {linked_counts}",
              file=sys.stderr)
        return 1
    template_fixture = b'''\
#include "cpumock/Linked.hpp"
struct Tables {
  LinkedWord<0x10> count;
  LinkedArray<0x30, 2> xs;
};
template <typename Count, typename Xs>
bool scan(const Count & count, const Xs & xs) {
  int8_t xi = int8_t(count.read());
  const uint8_t lx = xs[xi].read();
  return lx != 0;
}
bool caller(Tables & t) { return scan(t.count, t.xs); }
'''
    template, template_counts = lower_source(template_fixture, Path("template.cpp"))
    if (b"int8_t xi = int8_t(count);" not in template or
            b"const uint8_t lx = xs[xi];" not in template or
            b".read()" in template):
        print("lower-standalone-game selftest: bad template lowering", file=sys.stderr)
        print(template.decode("utf-8"), file=sys.stderr)
        return 1
    if template_counts.linked_reads != 2:
        print(f"lower-standalone-game selftest: bad template counts {template_counts}",
              file=sys.stderr)
        return 1
    ambiguous_template = b'''\
template <typename Xs>
void fill(Xs & xs) { xs[0].write(1); }
void caller(Plain & p) { fill(p); }
'''
    try:
        lower_source(ambiguous_template, Path("ambiguous-template.cpp"))
    except ValueError as exc:
        if "receives ambiguous or non-linked state" not in str(exc):
            print(f"lower-standalone-game: wrong template diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: ambiguous template accepted",
              file=sys.stderr)
        return 1
    facade_fixture = b'''\
#define REVM_LOG_MODULE "ff-fixture"
#include "util/Log.hpp"
#include "cpumock/CpuMockHost.hpp"
#include "cpumock/Mem.hpp"
#include "cpumock/VideoAssets.hpp"
using revm::cpumock::VSyncPolicy;
using revm::AdvanceResult;
namespace revm::cpumock {
void InstallGame(CpuMockHost & host) {
\thost.InstallMainStart({});
\thost.SetEntryHandler([&host] {
\t\thost.InstallIrqHandler([] {});
\t\thost.InstallNmiHandler([] {});
\t});
}
} // namespace revm::cpumock
struct G {
\trevm::cpumock::CpuMockHost & host_;
\trevm::cpumock::IoMap io{host_};
\trevm::cpumock::VideoAssets video{host_};
\trevm::cpumock::MemTable<0x4800, 1024> storage{host_};
\tusing At = revm::cpumock::ByteAt<revm::cpumock::RamSync>;
\tvoid f() {
\t\thost_.Sei();
\t\thost_.Cli();
\t\thost_.AssertBegin(0x0394);
\t\tif (host_.QuitRequested()) { }
\t\tg().host().SoftQuit(1, "x %d", 1);
\t\tREVM_LOG(REVM_INFO, "info %u", 1u);
\t\tREVM_LOG_TIMED(REVM_VERBOSE, "v");
\t\tconst AdvanceResult r = host_.AdvanceCycles(1);
\t}
\tG & g();
};
'''
    facade_expected = b'''\
#define GAME_LOG_MODULE "ff-fixture"
#include "gamehost/Log.hpp"
#include "gamehost/GameHost.hpp"
using gamehost::VSyncPolicy;
using gamehost::AdvanceResult;
namespace gamehost {
void InstallGame(GameHost & host) {
\thost.InstallMainStart({});
\thost.SetEntryHandler([&host] {
\t\thost.SetIrqHandler([] {});
\t\thost.SetNmiHandler([] {});
\t});
}
} // namespace gamehost
struct G {
\tgamehost::GameHost & host_;
\tgamehost::IoMap io{host_};
\tgamehost::VideoAssets video{host_};
\tgamehost::MemTable<0x4800, 1024> storage{host_};
\tusing At = gamehost::ByteAt<gamehost::RamSync>;
\tvoid f() {
\t\thost_.IrqDisable();
\t\thost_.IrqEnable();
\t\thost_.AssertEntry(0x0394);
\t\tif (host_.ShouldQuit()) { }
\t\tg().host().Fail(1, "x %d", 1);
\t\tGAME_LOG(GAME_INFO, "info %u", 1u);
\t\tGAME_LOG_TIMED(GAME_VERBOSE, "v");
\t\tconst AdvanceResult r = host_.AdvanceCycles(1);
\t}
\tG & g();
};
'''
    facade, facade_counts = lower_source(facade_fixture, Path("facade.cpp"))
    if facade != facade_expected:
        print("lower-standalone-game selftest: FAIL facade", file=sys.stderr)
        print(facade.decode("utf-8"), file=sys.stderr)
        return 1
    if facade_counts != Counts(facade_includes=4, log_macros=5,
                               qualified_types=9, host_methods=7,
                               entry_namespaces=1):
        print(f"lower-standalone-game selftest: bad facade counts {facade_counts}",
              file=sys.stderr)
        return 1
    forbidden_helpers = {
        "free-helper.cpp": (
            b"void f(uint8_t &v, bool &c) { asl(v, c); }\n",
            "free asl() helper is forbidden",
        ),
        "qualified-helper.cpp": (
            b"void f(uint8_t &v) { revm::cpumock::inc(v); }\n",
            "unsupported revm-qualified name",
        ),
    }
    for name, (source, diagnostic) in forbidden_helpers.items():
        try:
            lower_source(source, Path(name))
        except ValueError as exc:
            if diagnostic not in str(exc):
                print(f"lower-standalone-game selftest: wrong diagnostic: {exc}",
                      file=sys.stderr)
                return 1
        else:
            print(f"lower-standalone-game selftest: {name} accepted",
                  file=sys.stderr)
            return 1
    unsupported_predicates = {
        "mixed.cpp": (
            b"void f() { if (host.NoTwin() && ready()) run(); }\n",
            "unsupported NoTwin() context",
        ),
        "and-side-effect.cpp": (
            b"void f() { if (bump() && host.HasTwin()) twin(); else main(); }\n",
            "unsupported HasTwin() context",
        ),
        "or-side-effect.cpp": (
            b"void f() { if (bump() || host.NoTwin()) main(); else twin(); }\n",
            "unsupported NoTwin() context",
        ),
    }
    for name, (unsupported, diagnostic) in unsupported_predicates.items():
        try:
            lower_source(unsupported, Path(name))
        except ValueError as exc:
            if diagnostic not in str(exc):
                print(f"lower-standalone-game selftest: wrong diagnostic: {exc}",
                      file=sys.stderr)
                return 1
        else:
            print(f"lower-standalone-game selftest: {name} accepted",
                  file=sys.stderr)
            return 1
    missing_frames = (
        b"void f() { Sync::JoinAtPcBounded(host_, 0x03C0, {.max_frames = 8}); }\n"
    )
    try:
        lower_source(missing_frames, Path("bounded.cpp"))
    except ValueError as exc:
        if ".no_twin_frames" not in str(exc):
            print(f"lower-standalone-game selftest: wrong bounded diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: JoinAtPcBounded without "
              "no_twin_frames accepted", file=sys.stderr)
        return 1
    unsupported_diag = b"void f() { consume(host_.diag().WatchTwinPc(1)); }\n"
    try:
        lower_source(unsupported_diag, Path("unsupported-diag.cpp"))
    except ValueError as exc:
        if "standalone-dead WatchTwinPc()" not in str(exc):
            print(f"lower-standalone-game: wrong diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: unsupported diag context accepted",
              file=sys.stderr)
        return 1
    unknown_qualified = b"void f(revm::cpumock::Mem8 & m) { }\n"
    try:
        lower_source(unknown_qualified, Path("unknown-qualified.cpp"))
    except ValueError as exc:
        if "no gamehost rewrite: revm::cpumock::Mem8" not in str(exc):
            print(f"lower-standalone-game: wrong qualified diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: unknown qualified name accepted",
              file=sys.stderr)
        return 1
    non_host_receiver = b"struct T { void Sei(); };\nvoid f(T t) { t.Sei(); }\n"
    try:
        lower_source(non_host_receiver, Path("non-host.cpp"))
    except ValueError as exc:
        if "non-host receiver" not in str(exc):
            print(f"lower-standalone-game: wrong receiver diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: non-host receiver accepted",
              file=sys.stderr)
        return 1
    wrong_namespace = b"namespace revm { void f(); }\n"
    try:
        lower_source(wrong_namespace, Path("wrong-namespace.cpp"))
    except ValueError as exc:
        if "unsupported revm namespace: revm" not in str(exc):
            print(f"lower-standalone-game: wrong namespace diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: wrong revm namespace accepted",
              file=sys.stderr)
        return 1
    for label, source in (
        ("namespace-alias.cpp", b"namespace alias = revm;\n"),
        ("using-namespace.cpp", b"using namespace revm;\n"),
    ):
        try:
            lower_source(source, Path(label))
        except ValueError as exc:
            if "revm namespace identifier remains" not in str(exc):
                print(f"lower-standalone-game: wrong namespace identifier "
                      f"diagnostic: {exc}", file=sys.stderr)
                return 1
        else:
            print(f"lower-standalone-game selftest: {label} accepted",
                  file=sys.stderr)
            return 1
    comment_only = (
        b'// Sync::AtPc(host, 1); LinkedByte value;\n'
        b'void f() { const char *s = "HasTwin CpuMockHost"; }\n'
    )
    comment_analysis = analyze_file(
        comment_only, Path("comments.cpp"),
        inventory_linked([(Path("comments.cpp"), comment_only)]))
    if comment_analysis:
        print(f"lower-standalone-game selftest: comment/string produced findings: "
              f"{comment_analysis}", file=sys.stderr)
        return 1
    allowed_cast = (
        b"struct S { int cols; void f(int x) { if (unsigned(x) >= cols) {} } };\n"
    )
    try:
        assert_parse_ok(allowed_cast, Path("allowed-cast.cpp"))
    except ValueError as exc:
        print(f"lower-standalone-game selftest: allowed functional cast rejected: "
              f"{exc}", file=sys.stderr)
        return 1
    bad_parse = b"void f( { }\n"
    try:
        assert_parse_ok(bad_parse, Path("bad-parse.cpp"))
    except ValueError as exc:
        if "parse" not in str(exc):
            print(f"lower-standalone-game selftest: wrong parse diagnostic: {exc}",
                  file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: malformed C++ accepted",
              file=sys.stderr)
        return 1
    named_host = (
        b"struct T { void Sei(); int host_; void f() { host_.Sei(); } };\n"
    )
    try:
        lower_source(named_host, Path("named-host.cpp"))
    except ValueError as exc:
        if "non-host receiver" not in str(exc):
            print(f"lower-standalone-game selftest: wrong named-host diagnostic: "
                  f"{exc}", file=sys.stderr)
            return 1
    else:
        print("lower-standalone-game selftest: int host_.Sei() accepted",
              file=sys.stderr)
        return 1
    colliding = (
        b"struct Io { struct V { struct { void inc(); } x; } vic; };\n"
        b"struct Ledges { LinkedArray<0x10, 2> x{}; };\n"
        b"void f(Io & io, Ledges & ledges) {\n"
        b"  io.vic.x.inc();\n"
        b"  ledges.x[0].inc();\n"
        b"}\n"
    )
    colliding_out, colliding_counts = lower_source(colliding, Path("collide.cpp"))
    if (b"io.vic.x.inc()" not in colliding_out or
            b"++ledges.x[0]" not in colliding_out or
            colliding_counts.linked_increments != 1):
        print("lower-standalone-game selftest: linked/io x collision failed",
              file=sys.stderr)
        print(colliding_out.decode("utf-8"), file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as directory:
        tree = Path(directory)
        (tree / "a.cpp").write_bytes(fixture)
        first = analyze_tree(tree)
        second = analyze_tree(tree)
        if [f.to_json() for f in first.findings] != [f.to_json() for f in second.findings]:
            print("lower-standalone-game selftest: check is not deterministic",
                  file=sys.stderr)
            return 1
        lower_tree(tree)
        try:
            lower_tree(tree)
        except ValueError as exc:
            if "already lowered" not in str(exc):
                print(f"lower-standalone-game selftest: wrong relower diagnostic: "
                      f"{exc}", file=sys.stderr)
                return 1
        else:
            print("lower-standalone-game selftest: relower accepted",
                  file=sys.stderr)
            return 1
        check_tree(tree)
    print("lower-standalone-game selftest: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Audit, lower, or check Stage 4 C++ with the shared "
                    "tree-sitter analyzer")
    parser.add_argument(
        "command", nargs="?",
        choices=("audit", "lower", "check", "selftest"),
        help="audit inventories Stage 4 constructs; lower rewrites a copy; "
             "check requires zero leftover REVM/Twin/Sync constructs",
    )
    parser.add_argument("path", nargs="?", type=Path,
                        help="C++ file or directory")
    parser.add_argument("--json", action="store_true",
                        help="emit machine-readable findings for audit/check")
    parser.add_argument("--selftest", action="store_true",
                        help="alias for the selftest command")
    args = parser.parse_args()
    command = args.command
    if args.selftest:
        command = "selftest"
    if command is None:
        parser.error("command is required (audit, lower, check, or selftest)")
    if command == "selftest":
        return selftest()
    if args.path is None:
        parser.error(f"{command} requires a source path")
    if not args.path.exists():
        parser.error(f"path does not exist: {args.path}")
    try:
        if command == "audit":
            analysis = audit_tree(args.path)
            if args.json:
                print(json.dumps(analysis_json(analysis), indent=2))
            else:
                print_inventory(analysis)
            return 0
        if command == "check":
            analysis = check_tree(args.path)
            if args.json:
                print(json.dumps(analysis_json(analysis), indent=2))
            else:
                print_inventory(analysis)
            print("standalone-lowering: PASS no REVM/Twin/Sync leftovers")
            return 0
        counts = lower_tree(args.path)
    except (OSError, ValueError) as exc:
        print(f"lower-standalone-game: FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        "lower-standalone-game: PASS "
        f"branches={counts.twin_branches} "
        f"conditionals={counts.twin_conditionals} "
        f"at_pc={counts.at_pc} joins={counts.joins} "
        f"compares={counts.compares} watches={counts.watches} "
        f"advance_cycles={counts.advance_cycles} "
        f"advance_to_vsync={counts.advance_to_vsync} "
        f"advance_frames={counts.advance_frames} "
        f"return_irq={counts.return_irq} return_nmi={counts.return_nmi} "
        f"sync_includes={counts.sync_includes} sync_usings={counts.sync_usings} "
        f"diag_watch_calls={counts.diag_watch_calls} "
        f"diag_align_calls={counts.diag_align_calls} "
        f"compare_mask_usings={counts.compare_mask_usings} "
        f"fence_slack_usings={counts.fence_slack_usings}"
        f" linked_bytes={counts.linked_bytes} linked_words={counts.linked_words} "
        f"linked_arrays={counts.linked_arrays} linked_reads={counts.linked_reads} "
        f"linked_writes={counts.linked_writes} "
        f"linked_inc={counts.linked_increments} linked_dec={counts.linked_decrements} "
        f"linked_shifts={counts.linked_shifts} "
        f"linked_registration_methods={counts.linked_registration_methods} "
        f"linked_registration_calls={counts.linked_registration_calls} "
        f"facade_includes={counts.facade_includes} "
        f"log_macros={counts.log_macros} "
        f"qualified_types={counts.qualified_types} "
        f"host_methods={counts.host_methods} "
        f"entry_namespaces={counts.entry_namespaces}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
