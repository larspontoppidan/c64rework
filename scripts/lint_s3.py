#!/usr/bin/env python3
"""lint-s3 — static checks for Stage 3 replica plugins (game-root Stage3/).

Catches the mechanically checkable Stage 3 landmines at file:line granularity
so agents get immediate feedback instead of rediscovering each rule at a
failing frontier. Built on tree-sitter (real C++ concrete syntax tree); a
false positive gets an inline suppression, not a weaker rule.

Policy + API how-to: docs/agents/STAGE3-MANUAL.md

Usage:
    ./rework lint stage3            # scan Stage3/*.{cpp,hpp} (from the game root)
    scripts/lint_s3.py --selftest   # run embedded rule fixtures
    scripts/lint_s3.py --list-rules # print the rule table

The game-root launcher bootstraps .venv from scripts/requirements.txt.
Direct use needs those installed.

Exit status: 1 on any unsuppressed error-severity finding (warnings too with
--warnings-as-errors), else 0.

Game-specific exceptions live in Stage3/stage3-lint.json:

    {
      "none_mask_join_pcs": { "0xF3E7": "place_pickups interval body" }
    }

One-off sites use an inline suppression on the same or the previous line:

    Sync::JoinAtPcBounded(host_, 0xF5BE, {.max_frames = 64, .no_twin_frames = 8});
    host_.diag().TwinPeek(0x00D3); // s3lint:allow S3-014 — why
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tree_sitter_cpp as tscpp
    from tree_sitter import Language, Parser

    PARSER = Parser(Language(tscpp.language()))
except ImportError:  # reported in main()
    PARSER = None

# ------------------------------------------------------------------ rules ---

RULES = {
    "S3-000": ("error", "forbidden host/private API call"),
    "S3-001": ("error", "JoinAtPc then RAM AtPc at the same PC in one function"),
    "S3-002": ("error", "timed write of an unsynced +/-1 (fake RMW)"),
    "S3-003": ("error", "CompareMask::None()/{} join outside a documented interval"),
    "S3-004": ("error", "stepper or multi-frame join inside an interrupt handler"),
    "S3-005": ("warning", "ReturnIrq/ReturnNmi without a preceding ICR/IRR ACK"),
    "S3-007": ("warning", "SID offset outside canonical $D400..$D41F"),
    "S3-008": ("error", "sprite/voice index out of range (sp 0..7, voice 0..2)"),
    "S3-009": ("warning", "non-literal PC in join/AtPc"),
    "S3-011": ("warning", "Sync::Compare with None() is a no-op"),
    "S3-012": ("warning", "AtPc inside an installed interrupt handler body"),
    "S3-013": ("error", "armed AtPc is double-armed, unconsumed, or non-adjacent"),
    "S3-014": ("error", "host.diag() outside a REVM_LOG* argument list"),
    "S3-015": ("error", "implicit mem/io bag read or write"),
}

ARMED_METHODS = {
    "read", "write", "inc", "dec", "asl", "lsr", "rol", "ror",
}
# Public Sync steppers plus leftover private names a plugin might still write.
SYNC_STEPPERS = {
    "AdvanceCycles", "AdvanceToVSync", "JoinAtPcBounded",
    "TwinRunToPc", "MainRunToTwinCycle", "MainCatchUpVSync", "FrameWait",
}
SYNC_FORBIDDEN = {
    "TwinRunToPc", "MainCatchUpVSync", "MainRunToTwinCycle", "FrameWait",
    "JoinAtPcTestScan", "align_after_arrival",
}
HOST_FORBIDDEN = {
    "TwinPeekMem", "TwinFinishInstruction", "TwinExecuteIrqRti",
    "TwinLastIoRead", "TwinLastIoWrite",
    "MainInjectReadAtCycle", "MainInjectWriteAtCycle",
    "CompareNow", "FinishMockIrq", "FinishMockNmi", "Read", "Write",
    "TwinPeek", "TwinCpu", "PeekMain", "TwinWatchHits",
    "WatchTwinPc", "ClearTwinPcWatches",
    "MainTakeTwinPhaseCia1", "MainTakeTwinPhaseCia2",
    "AssertTwinPc", "AssertVsTwinCycleSync",
    "IFlag", "InIrq", "InNmi", "TwinInInterruptNest",
    "Quit", "LogVsTwinIrqSources",
    "MainRunToTwinCycle", "AdvanceToVSync", "AdvanceCycles",
    "TwinRunToPc", "FrameWait", "MainCatchUpVSync",
}
INSTALL_HANDLERS = {"InstallIrqHandler", "InstallNmiHandler"}

SUPP_RE = re.compile(
    r"s3lint\s*:\s*allow\s+((?:S3-\d{3}|all)(?:\s*,\s*S3-\d{3})*)", re.I)
PLUSMINUS1_RE = re.compile(r"[+\-]\s*1(?![\w])")
NONE_MASKS = {"CompareMask::None()", "{}"}

# ---------------------------------------------------------------- helpers ---


def walk(node):
    """Pre-order (document order) traversal of a tree-sitter node."""
    yield node
    for child in node.children:
        yield from walk(child)


def txt(node) -> str:
    return node.text.decode("utf-8")


def is_host_recv(recv) -> bool:
    if recv is None:
        return False
    rt = txt(recv)
    return (rt in ("host_", "host") or rt == "host()"
            or rt.endswith(".host()"))


def in_revm_log_args(node) -> bool:
    """True if `node` sits in the argument list of a REVM_LOG* call."""
    p = node.parent
    while p is not None:
        if p.type == "call_expression":
            parts = call_parts(p)
            if parts is not None and parts[0].startswith("REVM_LOG"):
                return True
        p = p.parent
    return False


def norm_pc(s: str) -> str | None:
    s = s.strip()
    try:
        return f"0x{int(s, 0):04X}"
    except ValueError:
        return None


def call_parts(node):
    """call_expression -> (name, scope|None, receiver node|None, arg nodes)."""
    fn = node.child_by_field_name("function")
    if fn is None:
        return None
    scope = None
    recv = None
    if fn.type == "qualified_identifier":
        name_node = fn.child_by_field_name("name")
        scope_node = fn.child_by_field_name("scope")
        if name_node is None:
            return None
        name = txt(name_node)
        scope = txt(scope_node) if scope_node is not None else None
    elif fn.type == "field_expression":
        field_node = fn.child_by_field_name("field")
        if field_node is None:
            return None
        name = txt(field_node)
        recv = fn.child_by_field_name("argument")
    elif fn.type == "identifier":
        name = txt(fn)
    else:
        return None
    args_node = node.child_by_field_name("arguments")
    args = args_node.named_children if args_node is not None else []
    return name, scope, recv, args


def bag_kind(node) -> str | None:
    """'mem'/'io' if node is a bag access chain (mem.* / io.*, also through a
    receiver like g.io.*), else None."""
    n = node
    while n is not None and n.type in ("field_expression",
                                       "subscript_expression"):
        if n.type == "field_expression":
            f = n.child_by_field_name("field")
            a = n.child_by_field_name("argument")
            if (f is not None and txt(f) in ("mem", "io")
                    and a is not None and a.type == "identifier"):
                return txt(f)
            n = a
        else:
            n = n.child_by_field_name("argument")
    if n is not None and n.type == "identifier" and txt(n) in ("mem", "io"):
        return txt(n)
    return None


def bag_read_expr(val) -> str | None:
    """Location text if val is an ordinary bag read, optionally uint8_t()-wrapped."""
    if bag_kind(val):
        return txt(val)
    if val.type == "call_expression":
        parts = call_parts(val)
        if parts is not None:
            name, _scope, recv, _args = parts
            if name == "read" and recv is not None and bag_kind(recv):
                return txt(recv)
        fn = val.child_by_field_name("function")
        args = val.child_by_field_name("arguments")
        if (fn is not None and fn.type == "primitive_type"
                and args is not None and len(args.named_children) == 1):
            inner = args.named_children[0]
            if bag_kind(inner):
                return txt(inner)
    return None


def function_name(func_def) -> str | None:
    d = func_def.child_by_field_name("declarator")
    while d is not None and d.type in ("function_declarator",
                                       "parenthesized_declarator",
                                       "pointer_declarator",
                                       "reference_declarator"):
        d = d.child_by_field_name("declarator")
    if d is None:
        return None
    if d.type == "qualified_identifier":
        n = d.child_by_field_name("name")
        return txt(n) if n is not None else None
    if d.type in ("identifier", "field_identifier"):
        return txt(d)
    return None


# ------------------------------------------------------------------ model ---


@dataclass
class Config:
    none_mask_join_pcs: dict[str, str] = field(default_factory=dict)


@dataclass
class FuncState:
    join_pcs: list[tuple[str, int]] = field(default_factory=list)
    reads: dict[str, str] = field(default_factory=dict)  # var -> location
    ack_line: int | None = None
    pending_atpc: "ArmedAtPcState | None" = None


@dataclass
class ArmedAtPcState:
    pc: str
    line: int
    end_byte: int
    node: object


@dataclass
class Finding:
    path: str
    line: int
    rule: str
    sev: str
    msg: str


def load_config(game_dir: Path) -> Config:
    p = game_dir / "Stage3" / "stage3-lint.json"
    if not p.exists():
        return Config()
    data = json.loads(p.read_text(encoding="utf-8"))
    pcs = data.get("none_mask_join_pcs", {})
    if isinstance(pcs, list):
        pcs = {k: "" for k in pcs}
    normed = {}
    for k, v in pcs.items():
        npc = norm_pc(str(k))
        if npc is None:
            raise SystemExit(f"{p}: bad PC key {k!r}")
        normed[npc] = str(v)
    return Config(normed)

# ------------------------------------------------------------------ rules ---


class FileScan:
    def __init__(self, path: str, src: str, cfg: Config,
                 handlers: set[str]):
        self.path = path
        self.src = src
        self.src_bytes = src.encode("utf-8")
        self.tree = PARSER.parse(src.encode("utf-8"))
        self.cfg = cfg
        self.handlers = handlers
        self.findings: list[Finding] = []
        # comment rows -> text, for inline suppressions
        self.comments: list[tuple[int, str]] = [
            (n.start_point[0] + 1, txt(n))
            for n in walk(self.tree.root_node) if n.type == "comment"]

    def add(self, rule: str, node, msg: str) -> None:
        line = node.start_point[0] + 1
        for crow, ctext in self.comments:
            if crow in (line, line - 1):
                for m in SUPP_RE.finditer(ctext):
                    ids = {s.strip().upper() for s in m.group(1).split(",")}
                    if rule in ids or "ALL" in ids:
                        return
        self.findings.append(Finding(self.path, line, rule,
                                     RULES[rule][0], msg))

    def has_comment_near(self, node, above: int) -> bool:
        line = node.start_point[0] + 1
        return any(line - above <= crow <= line
                   for crow, _ in self.comments)

    # ---- stateless rules (whole file) ----

    def check_stateless(self) -> None:
        for node in walk(self.tree.root_node):
            self.check_explicit_bag_style(node)
            if node.type == "call_expression":
                parts = call_parts(node)
                if parts is None:
                    continue
                name, scope, recv, _args = parts
                if name in HOST_FORBIDDEN and is_host_recv(recv):
                    self.add("S3-000", node,
                             f"host.{name}() is private REVM API — use "
                             "mem/io bags and Sync:: (STAGE3 §13)")
                elif scope == "Sync" and name in SYNC_FORBIDDEN:
                    self.add("S3-000", node,
                             f"Sync::{name} is private — use JoinAtPc / "
                             "JoinAtPcBounded / AdvanceCycles / "
                             "AdvanceToVSync (STAGE3 §13)")
                elif name == "diag" and is_host_recv(recv):
                    if not in_revm_log_args(node):
                        self.add("S3-014", node,
                                 "host.diag() is the oracle door — only "
                                 "inside a REVM_LOG* argument list, or "
                                 "suppress with // s3lint:allow S3-014 — "
                                 "reason")
            elif node.type == "subscript_expression":
                arg = node.child_by_field_name("argument")
                idx = node.child_by_field_name("indices")
                if arg is None or idx is None or not idx.named_children:
                    continue
                index = idx.named_children[0]
                if index.type != "number_literal":
                    continue
                try:
                    value = int(txt(index), 0)
                except ValueError:
                    continue
                atext = txt(arg)
                if atext.endswith("io.sid.raw") and value >= 0x20:
                    self.add("S3-007", node,
                             f"io.sid.raw[{txt(index)}] is outside canonical "
                             "$D400..$D41F — mirror offsets need a documented "
                             "reason (STAGE3 §5)")
                elif atext.endswith("io.vic.sp") and value > 7:
                    self.add("S3-008", node,
                             f"io.vic.sp[{txt(index)}] — sprite number is "
                             "0..7, not the 6510 X value")
                elif atext.endswith("io.sid.voice") and value > 2:
                    self.add("S3-008", node,
                             f"io.sid.voice[{txt(index)}] — voice is 0..2")

    @staticmethod
    def _is_bag_value(node) -> bool:
        if node.type in ("field_expression", "subscript_expression"):
            return bag_kind(node) is not None
        if node.type == "call_expression":
            parts = call_parts(node)
            return (parts is not None and parts[0] == "at"
                    and parts[2] is not None and bag_kind(parts[2]) is not None)
        return False

    @staticmethod
    def _is_nested_bag_chain(node) -> bool:
        parent = node.parent
        return (parent is not None
                and parent.type in ("field_expression", "subscript_expression")
                and bag_kind(parent) is not None)

    @staticmethod
    def _is_method_selector(node) -> bool:
        parent = node.parent
        return (node.type == "field_expression" and parent is not None
                and parent.type == "call_expression"
                and parent.child_by_field_name("function") == node)

    @staticmethod
    def _is_explicit_receiver(node) -> bool:
        parent = node.parent
        if parent is None or parent.type != "field_expression":
            return False
        if parent.child_by_field_name("argument") != node:
            return False
        field = parent.child_by_field_name("field")
        if field is None:
            return False
        grand = parent.parent
        return (grand is not None and grand.type == "call_expression"
                and grand.child_by_field_name("function") == parent
                and txt(field) in {"read", "write", "inc", "dec", "asl",
                                   "lsr", "rol", "ror", "at"})

    def check_explicit_bag_style(self, node) -> None:
        if not self._is_bag_value(node) or self._is_nested_bag_chain(node):
            return
        if self._is_method_selector(node) or self._is_explicit_receiver(node):
            return
        if node.type == "field_expression":
            field = node.child_by_field_name("field")
            if field is not None and txt(field) in ("addr", "size"):
                return

        parent = node.parent
        if (parent is not None and parent.type == "assignment_expression"
                and parent.child_by_field_name("left") == node):
            self.add("S3-015", node,
                     f"implicit bag write {txt(node)!r} — use .write(value)")
            return
        if parent is not None and parent.type == "update_expression":
            self.add("S3-015", node,
                     f"implicit bag RMW {txt(parent)!r} — use the explicit "
                     ".inc()/.dec() helper")
            return
        self.add("S3-015", node,
                 f"implicit bag read {txt(node)!r} — use .read()")

    # ---- function-scoped rules (document order per function body) ----

    def check_function(self, func_def) -> None:
        name = function_name(func_def)
        in_handler = name in self.handlers
        fs = FuncState()
        body = func_def.child_by_field_name("body")
        if body is None:
            return
        for node in walk(body):
            if node.type == "call_expression":
                self.check_call(node, name, in_handler, fs)
            elif node.type in ("init_declarator", "assignment_expression"):
                self.record_read(node, fs)
            elif node.type == "field_expression":
                f = node.child_by_field_name("field")
                if (f is not None and txt(f) in ("icr", "irr")
                        and fs.ack_line is None):
                    fs.ack_line = node.start_point[0] + 1
            elif node.type == "subscript_expression":
                arg = node.child_by_field_name("argument")
                idx = node.child_by_field_name("indices")
                if (arg is not None and idx is not None
                        and idx.named_children
                        and fs.ack_line is None):
                    atext = txt(arg)
                    try:
                        value = int(txt(idx.named_children[0]), 0)
                    except ValueError:
                        value = None
                    if atext.endswith("io.vic.raw") and value == 0x19:
                        fs.ack_line = node.start_point[0] + 1
        if fs.pending_atpc is not None:
            pending = fs.pending_atpc
            self.add("S3-013", pending.node,
                     f"Sync::AtPc at line {pending.line} is not consumed by "
                     "the next explicit mem/io read/write/RMW operation")
        # S3-013 (Twin walk + AdvanceToVSync) is gone: JoinAtPcBounded owns
        # the interval-exit shape.

    def record_read(self, node, fs: FuncState) -> None:
        if node.type == "init_declarator":
            var = node.child_by_field_name("declarator")
            val = node.child_by_field_name("value")
        else:
            var = node.child_by_field_name("left")
            val = node.child_by_field_name("right")
        if var is None or val is None or var.type != "identifier":
            return
        loc = bag_read_expr(val)
        if loc is not None:
            fs.reads.setdefault(txt(var), loc)

    def check_call(self, node, func: str | None, in_handler: bool,
                   fs: FuncState) -> None:
        parts = call_parts(node)
        if parts is None:
            return
        _n, _scope, _recv, _a = parts
        name, scope, recv, args = parts

        if scope == "Sync" and name == "AtPc":
            self.check_armed_marker(node, args, func, in_handler, fs)
            return

        if in_handler and scope == "Sync" and name in SYNC_STEPPERS:
            self.add("S3-004", node,
                     f"Sync::{name} inside handler {func}() — handlers may "
                     "not call steppers (STAGE3 §6)")

        if name == "JoinAtPc":
            self.reject_pending(node, fs, "JoinAtPc")
            pc = None
            if len(args) >= 2:
                if args[1].type == "number_literal":
                    pc = norm_pc(txt(args[1]))
                else:
                    self.add("S3-009", node,
                             f"non-literal join PC {txt(args[1])!r} — pass "
                             "the opcode PC from the listing")
            if pc:
                fs.join_pcs.append((pc, node.start_point[0] + 1))
            if len(args) >= 3:
                mask = re.sub(r"\s+", "", txt(args[2]))
                if mask in NONE_MASKS:
                    if pc is None or pc not in self.cfg.none_mask_join_pcs:
                        self.add("S3-003", node,
                                 f"PC-only join (CompareMask::None()) at "
                                 f"{pc or txt(args[1])} — allowed only inside "
                                 "a documented interval body; allowlist with "
                                 "a reason in stage3-lint.json "
                                 "(STAGE3 §8)")

        elif name == "JoinAtPcBounded":
            self.reject_pending(node, fs, "JoinAtPcBounded")
            if len(args) >= 2 and args[1].type != "number_literal":
                self.add("S3-009", node,
                         f"non-literal join PC {txt(args[1])!r} — pass "
                         "the opcode PC from the listing")

        elif name in ARMED_METHODS and recv is not None:
            kind = bag_kind(recv)
            if kind is not None and fs.pending_atpc is not None:
                self.check_armed_operation(node, name, recv, args, kind, fs)

        elif name in ("ReturnIrq", "ReturnNmi"):
            if fs.ack_line is None:
                self.add("S3-005", node,
                         f"{name} in {func}() without a preceding ICR/IRR "
                         "ACK — ACK (LDA ICR / STA VIC IRR) then Return "
                         "(STAGE3 §6)")

        elif name == "Compare":
            if len(args) >= 2 and re.sub(r"\s+", "", txt(args[1])) \
                    in NONE_MASKS:
                self.add("S3-011", node,
                         "Sync::Compare with None() is a no-op")

    def check_armed_marker(self, node, args, func: str | None,
                           in_handler: bool, fs: FuncState) -> None:
        if len(args) < 2:
            self.add("S3-013", node,
                     "Sync::AtPc requires host and an opcode PC")
            return
        pc = None
        if args[1].type == "number_literal":
            pc = norm_pc(txt(args[1]))
        else:
            self.add("S3-009", node,
                     f"non-literal AtPc PC {txt(args[1])!r} — pass "
                     "the opcode PC from the listing")
        if pc is None:
            return
        if fs.pending_atpc is not None:
            old = fs.pending_atpc
            self.add("S3-013", node,
                     f"Sync::AtPc double-arm; previous marker at line "
                     f"{old.line} was not consumed")
        fs.pending_atpc = ArmedAtPcState(
            pc=pc, line=node.start_point[0] + 1,
            end_byte=node.end_byte, node=node)
        if in_handler:
            self.add("S3-012", node,
                     f"AtPc inside handler {func}() — AtPc in a handler "
                     "body is display-visible pairing; suppress with "
                     "// s3lint:allow S3-012 — reason")

    @staticmethod
    def _adjacent_armed_operation(text: str) -> bool:
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        text = text.strip()
        if text.startswith(";"):
            text = text[1:].strip()
        if not text:
            return True
        # A declaration, assignment, cast, or control expression may
        # introduce the operation, e.g. `uint8_t a = mem.x.read()` or
        # `if ((io.vic.msigx.read() & 2) != 0)`.  Do not permit an unrelated
        # call, second statement, or block between the marker and the bag
        # operation.  The operation itself is found from the parsed call
        # expression, so this only checks the prefix before it.
        if ";" in text or "{" in text or "}" in text:
            return False
        allowed_calls = {
            "if", "for", "while", "switch", "catch", "sizeof",
            "alignof", "decltype", "static_cast", "reinterpret_cast",
            "const_cast", "dynamic_cast",
        }
        for name in re.findall(r"\b([A-Za-z_]\w*)\s*\(", text):
            if name in allowed_calls or name.endswith("_t"):
                continue
            return False
        return True

    def reject_pending(self, node, fs: FuncState, boundary: str) -> None:
        pending = fs.pending_atpc
        if pending is None:
            return
        self.add("S3-013", pending.node,
                 f"Sync::AtPc at line {pending.line} is not consumed before "
                 f"Sync::{boundary}()")
        fs.pending_atpc = None

    def check_armed_operation(self, node, name: str, recv, args, kind: str,
                              fs: FuncState) -> None:
        pending = fs.pending_atpc
        if pending is None:
            return
        if not self._adjacent_armed_operation(
                self.src_bytes[pending.end_byte:node.start_byte].decode(
                    "utf-8")):
            self.add("S3-013", node,
                     f"Sync::AtPc at line {pending.line} must be immediately "
                     "followed by its mem/io operation")
            fs.pending_atpc = None
            return
        if name == "write" and args:
            for nested in walk(args[0]):
                if nested.type != "call_expression":
                    continue
                parts = call_parts(nested)
                if parts is None:
                    continue
                nested_name, _scope, nested_recv, _nested_args = parts
                if (nested_recv is not None and self._is_bag_value(nested_recv)
                        and nested_name in ARMED_METHODS):
                    self.add("S3-013", nested,
                             f"explicit bag {nested_name}() is evaluated "
                             "before the armed destination write — compute "
                             "the value before Sync::AtPc")
                    fs.pending_atpc = None
                    return
        fs.pending_atpc = None
        if kind == "mem":
            for jpc, jline in fs.join_pcs:
                if jpc == pending.pc:
                    self.add("S3-001", node,
                             f"RAM {name} at {pending.pc} follows a "
                             f"JoinAtPc at the same PC (line {jline}) — the "
                             "AtPc IS the fence; Sync::Compare after it if a "
                             "compare is wanted (STAGE3 §4)")
        if name == "write" and args:
            self.check_fake_rmw(node, txt(recv), args[0], fs)

    def check_fake_rmw(self, node, loc: str, val, fs: FuncState) -> None:
        if not PLUSMINUS1_RE.search(txt(val)):
            return
        hit = False
        for d in walk(val):
            if d.type in ("field_expression", "subscript_expression") \
                    and bag_kind(d) and txt(d) == loc:
                hit = True
                break
            if d.type == "identifier" and fs.reads.get(txt(d)) == loc:
                hit = True
                break
        if hit:
            self.add("S3-002", node,
                     f"{loc}.write() of an unsynced +/-1 — timed INC/DEC "
                     "memory uses the armed member RMW operation, never "
                     "read+write "
                     "(STAGE3 §5)")

    def run(self) -> list[Finding]:
        self.check_stateless()
        for node in walk(self.tree.root_node):
            if node.type == "function_definition":
                self.check_function(node)
        return self.findings


def find_handlers(src: str) -> set[str]:
    tree = PARSER.parse(src.encode("utf-8"))
    handlers: set[str] = set()
    for node in walk(tree.root_node):
        if node.type != "call_expression":
            continue
        parts = call_parts(node)
        if parts is None or parts[0] not in INSTALL_HANDLERS:
            continue
        args_node = node.child_by_field_name("arguments")
        if args_node is None:
            continue
        for d in walk(args_node):
            if d.type == "call_expression":
                p2 = call_parts(d)
                if p2 is not None and p2[2] is not None:
                    handlers.add(p2[0])
    return handlers


def scan_sources(sources: dict[str, str], cfg: Config) -> list[Finding]:
    handlers: set[str] = set()
    for name, src in sources.items():
        if name.endswith("plugin.cpp"):
            handlers |= find_handlers(src)
    findings: list[Finding] = []
    for name in sorted(sources):
        findings.extend(FileScan(name, sources[name], cfg, handlers).run())
    return findings

# ---------------------------------------------------------------- selftest ---


def _selftest() -> int:
    clean_routine = """
void Game::load() {
	Sync::JoinAtPc(host_, 0xF305);
	mem.level.write(0x01);
	uint8_t a = mem.score[3].read();
	bool c = false;
	asl(a, c);
	io.vic.sp[0].x.write(0x40);
}
"""
    handler_install = """
void InstallGame(CpuMockHost & host) {
	host.InstallIrqHandler([&game] { game.irq_game(); });
}
"""
    cases: list[tuple[str, dict[str, str], set[str], Config | None]] = [
        ("clean routine", {"game.cpp": clean_routine}, set(), None),
        ("armed two-line access is fine", {"game.cpp": """
void Game::armed() {
	Sync::AtPc(host_, 0xEE84);
	io.vic.sp[7].y.write(uint8_t(0x15));
	Sync::AtPc(host_, 0xEE85);
	uint8_t y = io.vic.sp[0].y.read();
	Sync::AtPc(host_, 0xEE86);
	io.vic.sp[0].y.inc();
}
"""}, set(), None),
        ("armed same-PC RAM join", {"game.cpp": """
void Game::tick() {
	Sync::JoinAtPc(host_, 0xEC3C);
	Sync::AtPc(host_, 0xEC3C);
	uint8_t a = mem.input_armed.read();
}
"""}, {"S3-001"}, None),
        ("armed same-PC I/O join is fine", {"game.cpp": """
void Game::wait() {
	Sync::JoinAtPc(host_, 0xE5AB);
	Sync::AtPc(host_, 0xE5AB);
	uint8_t p = io.cia1.pra.read();
}
"""}, set(), None),
        ("armed fake RMW via write", {"game.cpp": """
void Game::up() {
	uint8_t y = mem.timer.read();
	Sync::AtPc(host_, 0xEB08);
	mem.timer.write(uint8_t(y + 1));
}
"""}, {"S3-002"}, None),
        ("armed non-literal PC", {"game.cpp": """
void Game::r() {
	Sync::AtPc(host_, pc);
	uint8_t a = mem.rng.read();
}
"""}, {"S3-009"}, None),
        ("armed marker unconsumed", {"game.cpp": """
void Game::r() {
	Sync::AtPc(host_, 0xF574);
	return;
}
"""}, {"S3-013"}, None),
        ("armed marker double", {"game.cpp": """
void Game::r() {
	Sync::AtPc(host_, 0xF574);
	Sync::AtPc(host_, 0xF575);
	mem.rng.write(0x01);
}
"""}, {"S3-013"}, None),
        ("armed marker must be adjacent", {"game.cpp": """
void Game::r() {
	Sync::AtPc(host_, 0xF574);
	prepare();
	mem.rng.write(0x01);
}
"""}, {"S3-013"}, None),
        ("implicit bag read is forbidden", {"game.cpp": """
void Game::r() {
	uint8_t a = mem.rng;
}
"""}, {"S3-015"}, None),
        ("implicit bag write is forbidden", {"game.cpp": """
void Game::w() {
	io.vic.border = 0;
}
"""}, {"S3-015"}, None),
        ("implicit indirect byte read is forbidden", {"game.cpp": """
void Game::r() {
	uint8_t a = mem.ptr.at(2);
}
"""}, {"S3-015"}, None),
        ("armed write evaluates source first", {"game.cpp": """
void Game::w() {
	Sync::AtPc(host_, 0xF574);
	mem.dst.write(mem.src.read());
}
"""}, {"S3-013"}, None),
        ("None() join needs allowlist", {"game.cpp": """
void Game::place() {
	Sync::JoinAtPc(host_, 0xF3E7, CompareMask::None(), FenceSlack::Exact);
}
"""}, {"S3-003"}, None),
        ("None() join allowlisted", {"game.cpp": """
void Game::place() {
	Sync::JoinAtPc(host_, 0xF3E7, CompareMask::None());
}
"""}, set(), Config({"0xF3E7": "interval body"})),
        ("None() join suppressed inline", {"game.cpp": """
void Game::place() {
	Sync::JoinAtPc(host_, 0xF3E7, CompareMask::None()); // s3lint:allow S3-003
}
"""}, set(), None),
        ("stepper in handler", {
            "plugin.cpp": handler_install,
            "game.cpp": """
void Game::irq_game() {
	Sync::AdvanceCycles(host_, 64);
	(void)io.cia1.icr.read();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, {"S3-004"}, None),
        ("bounded join in handler", {
            "plugin.cpp": handler_install,
            "game.cpp": """
void Game::irq_game() {
	Sync::JoinAtPcBounded(host_, 0xF201, {.max_frames = 2, .no_twin_frames = 1});
	(void)io.cia1.icr.read();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, {"S3-004"}, None),
        ("Return without ACK", {"game.cpp": """
void Game::irq_game() {
	mem.timer.dec();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, {"S3-005"}, None),
        ("VIC irr ACK counts", {"game.cpp": """
void Game::irq_vic() {
	Sync::AtPc(host_, 0xD019);
	io.vic.irr.write(0xFF);
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, set(), None),
        ("VIC raw D019 ACK counts", {"game.cpp": """
void Game::irq_vic() {
	io.vic.raw[0x19].write(0xFF);
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, set(), None),
        ("clean handler", {
            "plugin.cpp": handler_install,
            "game.cpp": """
void Game::irq_game() {
	if (mem.timer.read() != 0)
		mem.timer.dec();
	(void)io.cia1.icr.read();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, set(), None),
        ("private Sync stepper", {"game.cpp": """
void Game::scan() {
	Sync::JoinAtPcTestScan(host_, 64, 0xF5BE);
}
"""}, {"S3-000"}, None),
        ("SID mirror offset", {"game.cpp": """
void Game::vol() {
	io.sid.raw[0x118].write(0x0F);
	io.sid.raw[0x18].write(0x0F);
}
"""}, {"S3-007"}, None),
        ("sprite/voice index range", {"game.cpp": """
void Game::s() {
	io.vic.sp[8].x.write(0);
	io.sid.voice[3].ctrl.write(0);
}
"""}, {"S3-008"}, None),
        ("no-op compare", {"game.cpp": """
void Game::c() {
	Sync::Compare(host_, CompareMask::None());
}
"""}, {"S3-011"}, None),
        ("armed AtPc in handler body", {
            "plugin.cpp": handler_install,
            "game.cpp": """
void Game::irq_game() {
	Sync::AtPc(host_, 0xF170);
	mem.timer.write(0x01);
	(void)io.cia1.icr.read();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, {"S3-012"}, None),
        ("AtPc in handler suppressed", {
            "plugin.cpp": handler_install,
            "game.cpp": """
void Game::irq_game() {
	// s3lint:allow S3-012 — display-visible store
	Sync::AtPc(host_, 0xF170);
	uint8_t a = mem.timer.read();
	(void)io.cia1.icr.read();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, set(), None),
        ("AtPc in handler callee is fine", {
            "plugin.cpp": handler_install,
            "game.cpp": """
void Game::player_input() {
	Sync::AtPc(host_, 0xE988);
	uint8_t a = mem.timer.read();
	(void)a;
}
void Game::irq_game() {
	player_input();
	(void)io.cia1.icr.read();
	Sync::ReturnIrq(host_, 0xF200, 0);
}
"""}, set(), None),
        ("forbidden host call", {"game.cpp": """
void Game::w() {
	host_.Write(0xD020, 1);
}
"""}, {"S3-000"}, None),
        ("hidden TwinPeek is forbidden", {"game.cpp": """
void Game::w() {
	host_.TwinPeek(0x00D3);
}
"""}, {"S3-000"}, None),
        ("diag outside log needs allow", {"game.cpp": """
void Game::w() {
	uint8_t v = host_.diag().TwinPeek(0x00D3);
	(void)v;
}
"""}, {"S3-014"}, None),
        ("diag inside REVM_LOG is fine", {"game.cpp": """
void Game::w() {
	REVM_LOG(REVM_INFO, "twin_pc=$%04X", unsigned(host_.diag().TwinCpu().pc));
}
"""}, set(), None),
        ("diag suppressed inline", {"game.cpp": """
void Game::w() {
	// s3lint:allow S3-014 — join vs skip based on Twin PC
	uint16_t pc = host_.diag().TwinCpu().pc;
	(void)pc;
}
"""}, set(), None),
    ]
    failed = 0
    for name, sources, expect, cfg in cases:
        got = {f.rule for f in scan_sources(sources, cfg or Config())}
        if got == expect:
            print(f"PASS {name}")
        else:
            failed += 1
            print(f"FAIL {name}: expected {sorted(expect)}, "
                  f"got {sorted(got)}")
    print(f"selftest: {len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0

# -------------------------------------------------------------------- main ---


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="lint-s3",
        description="Static checks for Stage 3 replica plugins "
                    "(docs/agents/STAGE3-MANUAL.md).")
    ap.add_argument("game", nargs="?",
                    help="game root containing Stage3/")
    ap.add_argument("--warnings-as-errors", action="store_true",
                    help="exit 1 on warnings too")
    ap.add_argument("--selftest", action="store_true",
                    help="run embedded rule fixtures")
    ap.add_argument("--list-rules", action="store_true",
                    help="print the rule table")
    args = ap.parse_args()

    if args.list_rules:
        for rid, (sev, summary) in RULES.items():
            print(f"{rid}  [{sev}] {summary}")
        return 0

    if PARSER is None:
        print("lint-s3: tree-sitter is not installed. Run:\n"
              "  ./rework setup-venv   # from the game root",
              file=sys.stderr)
        return 2

    if args.selftest:
        return _selftest()
    if not args.game:
        ap.error("missing game root (directory containing Stage3/)")

    game_dir = Path(args.game)
    stage3 = game_dir / "Stage3"
    if not stage3.is_dir():
        print(f"lint-s3: no Stage3/ directory under {game_dir}",
              file=sys.stderr)
        return 2

    cfg = load_config(game_dir)
    sources: dict[str, str] = {}
    for f in sorted(stage3.glob("*.cpp")) + sorted(stage3.glob("*.hpp")):
        rel = f.relative_to(Path.cwd()) if f.is_relative_to(Path.cwd()) else f
        sources[str(rel)] = f.read_text(encoding="utf-8")

    findings = scan_sources(sources, cfg)
    findings.sort(key=lambda f: (f.path, f.line))

    counts = {"error": 0, "warning": 0, "info": 0}
    for f in findings:
        counts[f.sev] += 1
        print(f"{f.path}:{f.line}: [{f.rule}] {f.sev}: {f.msg}")
    print(f"lint-s3: {len(sources)} files, "
          f"{counts['error']} error(s), {counts['warning']} warning(s), "
          f"{counts['info']} info")
    if counts["error"] or (args.warnings_as_errors and counts["warning"]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
