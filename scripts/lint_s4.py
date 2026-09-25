#!/usr/bin/env python3
"""lint-s4 -- static migration checks for Stage 4 replica plugins.

Stage 4 starts as a copy of the accepted Stage 3 plugin.  This linter is
intentionally a debt report: it makes the memory-map work visible while the
game is being migrated to Linked* state.  It does not replace lint-s3 and it
does not attempt to decide whether a translation is behaviourally correct.

Usage::

    ./rework lint stage4
    scripts/lint_s4.py --selftest
    scripts/lint_s4.py --list-rules

One-off exceptions belong next to the finding::

    // s4lint:allow S4-006 -- address is a documented asset table

The only file-level exception is ``Stage4/stage4-lint.json``;
it accepts exact ``Stage4/file.cpp:line`` keys under ``raw_address_exceptions``
and requires a non-empty reason for each entry.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


RULES = {
    "S4-001": ("error", "game state uses Mem/Mem16/MemTable"),
    "S4-002": ("error", "generic mem. memory-map access"),
    "S4-003": ("error", "raw I/O array access (io.*.raw[])"),
    "S4-004": ("error", "implicit or overloaded bag value access"),
    "S4-005": ("error", "goto in migrated game code"),
    "S4-006": ("warning", "raw machine address outside Sync/Linked provenance"),
    "S4-007": ("error", "free opcode-mimicking helper call or import"),
}

SUPP_RE = re.compile(
    r"s4lint\s*:\s*allow\s+((?:S4-\d{3}|all)(?:\s*,\s*S4-\d{3})*)",
    re.IGNORECASE,
)

MEM_TYPE_RE = re.compile(r"\b(?:Mem|Mem16|MemTable)\s*<")
MEM_ACCESS_RE = re.compile(r"(?<![A-Za-z0-9_])(?:[A-Za-z_]\w*\.)*mem\b")
RAW_IO_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:[A-Za-z_]\w*\.)*io\s*\.\s*"
    r"[A-Za-z_]\w*\s*\.\s*raw\s*\["
)
GOTO_RE = re.compile(r"\bgoto\b")
ADDRESS_RE = re.compile(r"\b0x[0-9A-Fa-f]{4,}\b")

# Bag operations are deliberately explicit in both Stage 3 and Stage 4.
EXPLICIT_OPS = {"read", "write", "inc", "dec", "asl", "lsr", "rol", "ror", "at"}
BAG_CHAIN_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:[A-Za-z_]\w*\.)*"
    r"(?:mem|io)(?:\s*\.\s*[A-Za-z_]\w*|\s*\[[^\]\n]*\])+"
)
OPERATOR_RE = re.compile(
    r"\boperator\s*(?:\+\+|--|(?:<<|>>|[+\-*/%&|^])?="
    r"|(?:u?int(?:8|16|32|64)_t|bool|char|short|int|long|float|double)\b)"
)
FREE_RMW_CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_.])"
    r"(?:(?:[A-Za-z_]\w*)\s*::\s*)*"
    r"(?P<helper>asl|lsr|rol|ror|inc|dec)\s*\("
)
FREE_RMW_USING_RE = re.compile(
    r"\busing\s+(?:(?:[A-Za-z_]\w*)\s*::\s*)+"
    r"(?P<helper>asl|lsr|rol|ror|inc|dec)\s*;"
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    severity: str
    message: str


@dataclass
class Config:
    # Keys are normalized paths as printed by the scanner.  Values are the
    # required human reason; empty reasons are rejected while loading.
    raw_address_exceptions: dict[str, str] = field(default_factory=dict)


def _normal_path(path: Path) -> str:
    try:
        return str(path.relative_to(Path.cwd()))
    except ValueError:
        return str(path)


def load_config(game_dir: Path) -> Config:
    path = game_dir / "Stage4" / "stage4-lint.json"
    if not path.exists():
        return Config()
    data = json.loads(path.read_text(encoding="utf-8"))
    raw = data.get("raw_address_exceptions", {})
    if not isinstance(raw, dict):
        raise SystemExit(f"{path}: raw_address_exceptions must be an object")
    exceptions: dict[str, str] = {}
    for key, reason in raw.items():
        if not isinstance(reason, str) or not reason.strip():
            raise SystemExit(f"{path}: exception {key!r} needs a reason")
        exceptions[str(key)] = reason.strip()
    return Config(exceptions)


def _mask_cpp(src: str) -> str:
    """Blank comments and literals, preserving line/column offsets."""
    out = list(src)
    i = 0
    state = "code"
    while i < len(src):
        c = src[i]
        n = src[i + 1] if i + 1 < len(src) else ""
        if state == "code":
            if c == "/" and n == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if c == "/" and n == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if c == '"':
                out[i] = " "
                i += 1
                state = "string"
                continue
            if c == "'":
                out[i] = " "
                i += 1
                state = "char"
                continue
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
            continue
        if state == "block":
            if c == "*" and n == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
            else:
                if c != "\n":
                    out[i] = " "
                i += 1
            continue
        # string / char; retain newlines, blank everything else.  Escapes
        # consume their following byte so an escaped quote cannot terminate.
        if c == "\\" and i + 1 < len(src):
            out[i] = " "
            if src[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
        elif (state == "string" and c == '"') or (state == "char" and c == "'"):
            out[i] = " "
            i += 1
            state = "code"
        else:
            if c != "\n":
                out[i] = " "
            i += 1
    return "".join(out)


class Scanner:
    def __init__(self, path: str, source: str, config: Config):
        self.path = path
        self.source = source
        self.masked = _mask_cpp(source)
        self.config = config
        self.findings: list[Finding] = []
        self.lines = source.splitlines()
        self.masked_lines = self.masked.splitlines()

    def suppressed(self, line: int, rule: str) -> bool:
        for candidate in (line - 1, line):
            if candidate < 1 or candidate > len(self.lines):
                continue
            for match in SUPP_RE.finditer(self.lines[candidate - 1]):
                ids = {s.strip().upper() for s in match.group(1).split(",")}
                if rule in ids or "ALL" in ids:
                    return True
        return False

    def add(self, rule: str, line: int, message: str) -> None:
        if self.suppressed(line, rule):
            return
        self.findings.append(Finding(
            self.path, line, rule, RULES[rule][0], message))

    @staticmethod
    def _is_sync_line(line: str) -> bool:
        return bool(re.search(
            r"\bSync\s*::\s*(?:AtPc|JoinAtPc|JoinAtPcBounded|WatchMark)\s*\(",
            line))

    @staticmethod
    def _is_linked_provenance(line: str) -> bool:
        return bool(re.search(
            r"\bLinked(?:Byte|Word|Array)\s*<", line))

    def check_line(self, line_no: int, line: str) -> None:
        # All regexes run against a lexical mask, so examples in comments and
        # string literals do not become migration debt.
        if MEM_TYPE_RE.search(line):
            self.add("S4-001", line_no,
                     "Mem/Mem16/MemTable is Stage 3 state; use explicitly "
                     "initialized local state or Linked*")

        for match in MEM_ACCESS_RE.finditer(line):
            self.add("S4-002", line_no,
                     f"generic memory-map access {match.group(0)!r}; migrate "
                     "to local state, Linked*, or a named hardware bag")

        for match in RAW_IO_RE.finditer(line):
            self.add("S4-003", line_no,
                     f"raw I/O array access {match.group(0).strip()!r}; use "
                     "the named IoMap register/bag")

        for match in OPERATOR_RE.finditer(line):
            self.add("S4-004", line_no,
                     f"overloaded value access {match.group(0)!r}; Linked* "
                     "and I/O bags require explicit read/write/RMW methods")

        # Find implicit bag values.  The final member is allowed only when it
        # is one of the explicit operation selectors and is actually called.
        for match in BAG_CHAIN_RE.finditer(line):
            chain = match.group(0)
            tail = re.search(r"(?:^|\.)\s*([A-Za-z_]\w*)\s*$", chain)
            suffix = line[match.end():]
            if tail and tail.group(1) in EXPLICIT_OPS and re.match(r"\s*\(", suffix):
                continue
            # A chain ending in raw[...] is handled by S4-003; do not produce
            # an unhelpful second implicit-access error for the same token.
            if re.search(r"\.\s*raw\s*\[", chain):
                continue
            self.add("S4-004", line_no,
                     f"implicit bag value access {chain.strip()!r}; use "
                     ".read(), .write(), or an explicit RMW method")

        for match in GOTO_RE.finditer(line):
            self.add("S4-005", line_no,
                     "goto obscures migratable control flow; replace with "
                     "structured C++")

        for match in FREE_RMW_CALL_RE.finditer(line):
            helper = match.group("helper")
            self.add("S4-007", line_no,
                     f"free {helper}() helper preserves opcode-shaped Stage 3 "
                     "code; use explicit C++ value and carry operations")

        for match in FREE_RMW_USING_RE.finditer(line):
            helper = match.group("helper")
            self.add("S4-007", line_no,
                     f"free {helper}() helper import is not part of the "
                     "Stage 4 game-facing surface")

        # Four-or-more digit hex constants are address-shaped.  Sync PCs and
        # Linked provenance are deliberate address metadata and are exempt;
        # small constants (0x00FF etc.) remain ordinary game data for now.
        if self._is_sync_line(line) or self._is_linked_provenance(line):
            return
        for match in ADDRESS_RE.finditer(line):
            value = int(match.group(0), 16)
            if value < 0x1000:
                continue
            key = f"{self.path}:{line_no}"
            if key in self.config.raw_address_exceptions:
                continue
            self.add("S4-006", line_no,
                     f"raw machine address {match.group(0)}; keep address "
                     "metadata in Sync PCs/Linked provenance or name the "
                     "asset/hardware surface")

    def run(self) -> list[Finding]:
        for line_no, line in enumerate(self.masked_lines, 1):
            self.check_line(line_no, line)
        return self.findings


def scan_sources(sources: dict[str, str], config: Config) -> list[Finding]:
    findings: list[Finding] = []
    for path in sorted(sources):
        findings.extend(Scanner(path, sources[path], config).run())
    return findings


def _selftest() -> int:
    clean = """
void Game::ok() {
    Sync::AtPc(host_, 0xF574);
    state.input_armed.write(1);
    LinkedByte<0x003F> input{0};
    io.vic.border.write(0x00);
}
"""
    cases: list[tuple[str, str, set[str]]] = [
        ("clean explicit linked/io and Sync PC", clean, set()),
        ("memory type", "Mem<0x003F> input{host};", {"S4-001"}),
        ("generic mem", "uint8_t x = g.mem.input.read();", {"S4-002"}),
        ("raw io", "io.vic.raw[0x19].write(0xFF);", {"S4-003"}),
        ("implicit bag", "uint8_t x = io.vic.border;", {"S4-004"}),
        ("operator", "LinkedByte& operator++();", {"S4-004"}),
        ("index operator allowed", "Element operator[](size_t i);", set()),
        ("goto", "goto old_path;", {"S4-005"}),
        ("address", "constexpr auto kTable = 0xA1F4;", {"S4-006"}),
        ("Sync PC exempt", "Sync::JoinAtPc(host_, 0xA000);", set()),
        ("Linked provenance exempt", "LinkedWord<0xA1F4> table{0};", set()),
        ("comment exempt", "// mem.foo; goto old; 0xA000", set()),
        ("inline suppression", "goto old; // s4lint:allow S4-005 -- legacy", set()),
        ("free helper", "asl(acc, carry);", {"S4-007"}),
        ("qualified free helper", "revm::cpumock::inc(index);", {"S4-007"}),
        ("free helper import", "using revm::cpumock::ror;", {"S4-007"}),
        ("bag RMW allowed", "io.vic.border.rol(&carry);", set()),
    ]
    failures = 0
    for name, source, expected in cases:
        got = {f.rule for f in Scanner("fixture.cpp", source, Config()).run()}
        if got == expected:
            print(f"PASS {name}")
        else:
            failures += 1
            print(f"FAIL {name}: expected {sorted(expected)}, got {sorted(got)}")
    print(f"selftest: {len(cases) - failures}/{len(cases)} passed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="lint-s4",
        description="Static migration checks for Stage 4 plugins "
                    "(docs/agents/STAGE4.md).")
    parser.add_argument("game", nargs="?",
                        help="game dir containing Stage4/")
    parser.add_argument("--warnings-as-errors", action="store_true",
                        help="exit 1 on warnings too")
    parser.add_argument("--selftest", action="store_true",
                        help="run embedded rule fixtures")
    parser.add_argument("--list-rules", action="store_true",
                        help="print the rule table")
    args = parser.parse_args()

    if args.list_rules:
        for rule, (severity, summary) in RULES.items():
            print(f"{rule}  [{severity}] {summary}")
        return 0
    if args.selftest:
        return _selftest()
    if not args.game:
        parser.error("missing game root (directory containing Stage4/)")

    game_dir = Path(args.game)
    stage4 = game_dir / "Stage4"
    if not stage4.is_dir():
        print(f"lint-s4: no Stage4/ directory under {game_dir}", file=sys.stderr)
        return 2
    config = load_config(game_dir)
    sources: dict[str, str] = {}
    for source in sorted(stage4.rglob("*.cpp")) + sorted(stage4.rglob("*.hpp")):
        sources[_normal_path(source)] = source.read_text(encoding="utf-8")
    findings = scan_sources(sources, config)
    findings.sort(key=lambda f: (f.path, f.line, f.rule))
    counts = {severity: 0 for severity in ("error", "warning", "info")}
    by_rule: dict[str, int] = {}
    for finding in findings:
        counts[finding.severity] += 1
        by_rule[finding.rule] = by_rule.get(finding.rule, 0) + 1
        print(f"{finding.path}:{finding.line}: [{finding.rule}] "
              f"{finding.severity}: {finding.message}")
    summary = ", ".join(f"{rule}={by_rule[rule]}" for rule in sorted(by_rule))
    if not summary:
        summary = "none"
    print(f"lint-s4: {len(sources)} files, {counts['error']} error(s), "
          f"{counts['warning']} warning(s), {counts['info']} info; "
          f"by rule: {summary}")
    if counts["error"] or (args.warnings_as_errors and counts["warning"]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
