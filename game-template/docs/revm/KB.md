# Knowledge base (`*.kb.json`)

Stage 2 annotations shared by **REVM** and **`revm-tool`**. One file per game,
e.g. `Stage2/<game>.kb.json`.

Simple JSON. Unknown keys are ignored.

## Example

```json
{
  "version": 1,
  "objects": [
    {
      "name": "coldstart",
      "kind": "function",
      "addr": "8009",
      "end": "807E",
      "trace": "yes",
      "comment": "IOINIT/CINT; clear $8000-$9FFF; JMP title"
    },
    {
      "name": "coldstart_loop",
      "kind": "label",
      "addr": "801C",
      "comment": "inner clear loop"
    },
    {
      "name": "joy_shadow",
      "kind": "variable",
      "addr": "006B",
      "watch": "yes",
      "comment": "joystick/key shadow"
    },
    {
      "name": "spr_ptr",
      "kind": "variable",
      "addr": "07F8",
      "end": "07FF",
      "watch": "verbose",
      "comment": "sprite pointers"
    },
    {
      "name": "playfield_map",
      "kind": "blob",
      "addr": "4000",
      "end": "4FFF",
      "format": "playfield_map",
      "comment": "Tile map; see Stage2/FORMATS.md"
    },
    {
      "name": "chrout",
      "kind": "label",
      "addr": "FFD2",
      "bank": "kernal",
      "comment": "KERNAL CHROUT"
    },
    {
      "addr": "8015",
      "comment": "border/bg black"
    }
  ]
}
```

Line notes omit `kind` (defaults to **`comment`**).

## Top level

| Key | Required | Meaning |
|-----|----------|---------|
| `version` | yes | `1` |
| `objects` | yes | Array of objects (may be empty) |

## Object

| Key | Required | Meaning |
|-----|----------|---------|
| `addr` | yes | Start address as a hexadecimal string (`"8009"`, `"$8009"`, or `"0x8009"`) |
| `kind` | no | `variable` \| `function` \| `label` \| `blob` \| `opcodes` \| `comment` — **default `comment`** |
| `bank` | no | Memory bank — **default `ram`** (see below) |
| `end` | see rules | Inclusive end of range |
| `name` | no | Label / symbol name |
| `comment` | no | Free text |
| `format` | no | Format id for `blob` (section in `Stage2/FORMATS.md`) |
| `watch` | no | `no` \| `yes` \| `verbose` — **default `no`** |
| `trace` | no | `no` \| `yes` \| `false` \| `true` — **default `no`** (fetch at `addr`) |

### `bank`

Which CPU-visible bank this annotation refers to. Omit or `"ram"` for ordinary
game RAM (including under-ROM RAM). The same `addr` may appear more than once
with different banks (e.g. a RAM trampoline and a KERNAL jump-table entry).

| Value | Meaning |
|-------|---------|
| `ram` | Default. Game / under-ROM RAM |
| `kernal` | KERNAL ROM (`$E000-$FFFF` when banked in) |
| `basic` | BASIC ROM (`$A000-$BFFF` when banked in) |
| `char` | Character ROM (`$D000-$DFFF` when CHAREN selects it) |
| `chip` | Memory-mapped I/O / CPU port (VIC/SID/CIA/`$0000`–`$0001`, …) |
| `color` | Color-RAM nybble file (`$D800-$DBFF`). |

Lookups that take only an address (disasm operand names, watches, etc.) prefer
the **RAM** object when several banks share an address. `--dump-blobs` uses
RAM-bank objects. `--watch` / Stage 3 kb-check enroll `ram` (including omitted
`bank`) and `color`. Address **map** emits **sections** (RAM /
BASIC / KERNAL). With `--cov`, BASIC and
KERNAL rows require a real ROM-plane hit (`pc_rom` / `mem_rom`) or a
`bank:"basic"` / `bank:"kernal"` KB object — under-ROM RAM code-operand refs in
`$A000+` / `$E000+` stay in the **RAM** section only. `bank: "chip"` entries appear as **CHIP** rows in the RAM section.

### Support KB (`--kb-support`)

Snapshot listing and map generation accept an optional `--kb-support` file for
shared C64 definitions (chip registers, KERNAL jump table, …). The repository
support KB is:

`REVM_ROOT/reference/c64.kb.json`

Same schema as the game KB. **Difference:** support objects are a **naming
fallback only** — they do not expand listing ranges, cause blob regions to be
skipped, emit function headers, or add KB-only map rows. If the game `--kb` already names an address,
support is ignored for that address. Operand fallback respects the listing
section bank: **`chip` always**, plus **`ram`** in the RAM section, or matching
`basic`/`kernal`/`char` in ROM sections — so under-ROM RAM writes (e.g. `$FFFE`)
are not mislabeled with KERNAL vector names.

Rules:

- **`function`** — callable routine spanning `[addr, end]` inclusive. **Set
  `end`** so listings know the body length for `# ----` headers. Do **not**
  infer length from RTS (mid-routine RTS / shared tails are common). If `end`
  is omitted, listing falls back to 128 bytes for the header only. `trace`
  fires on fetch at `addr` only.
- **`label`** — named code address (branch target, mid-routine site). Usually
  no `end`. Shown in disasm / call-target scores; not a listing section header
  (use `function` for `# ----` headers).
- **`variable`** — one byte at `addr`, or `[addr, end]` inclusive.
- **`blob`** — named binary region `[addr, end]` (**`end` required**). Not
  disassembled as code — the listing emits a `# blob $LO-$HI — name` comment
  and skips the bytes. Optional `format` → `Stage2/FORMATS.md`.
- **`opcodes`** — force-disassemble `[addr, end]` (**`end` required**) even when
  coverage has holes (e.g. untaken branch fallthrough). Listing builds two
  masks: **play** (each covered fetch PC expanded to full instruction bytes;
  `pc_ram` lengths from snap/RAM image so under-ROM game code is correct;
  `pc_rom` lengths from the configured BASIC/KERNAL ROM images)
  and **kb** (`function` + `opcodes` spans); disassembly is `play ∨ kb` with
  **no gap bridging**. Between consecutive listing ranges the tool emits
  `; gap range: $LO - $HI`. At the first instruction inside each `kb ∧ ¬play` run it
  emits `; not in coverage range: $LO - $HI`. Play-only runs get
  `; coverage range: $LO-$HI (N bytes)`. Suppress all three with
  `--list-no-cov-rng`. **`name` is ignored**. Optional `comment` is not
  printed. Fill a `; gap range:` with an `opcodes` (or `function`) span to pull those
  bytes into the listing.
- **`comment`** (or omitted `kind`) — line note at `addr` (usually unnamed).
  Listing prints `; <comment>` before the instruction; map emits a **COMMENT**
  row. Use for annotations that are not symbols (control-flow idioms, “data
  follows JSR”, etc.). Prefer `label` once you give it a `name`.
- Names, when present, should be unique across the file (including across
  banks). Overlapping ranges in the **same** bank: tools may warn.

### `watch` (REVM)

A watch samples a **named memory plane**. It never follows `$01` / `io_in` and
never reads VIC, SID, or CIA registers.

| Plane | `bank` | What is sampled |
|-------|--------|-----------------|
| DRAM | omitted, or `"ram"` | `RAM[addr]` |
| Color RAM | `"color"` | nybble file at `$D800-$DBFF` (`Color[addr & $3FF] & $0F`) |

Omitted `bank` defaults to DRAM. If the object's range overlays I/O chips 
(`$0000-$0001` or `$D000-$DFFF`), `bank` is **required**. `kernal`, `basic`, 
`char`, and `chip` cannot be watched.

| Value | When enabled |
|-------|----------------|
| `no` | Never print (default) |
| `yes` | Shown in `--watch` stderr / `--watch-window`; Stage 3 kb-check with `--kb` |
| `verbose` | Shown only with `--watch-verbose` / `--kb-check-verbose` |

`--watch-window` opens a second SDL window listing current values (fit/truncate,
redraw on change). Default font is Spleen **12x24** (better on HiDPI); use
`--watch-font 8x16` for the denser face. With `--watch-window`, values are
**not** printed to the terminal (use `--watch` alone for stderr logging).
Click a watched `variable` row to select its first byte, then press the configured
`[cheats]` `increment` or `decrement` key to modify it at the next VSYNC (8-bit
wrapping). The defaults are Page Up and Page Down; change them in `revm.cfg` for
laptop keyboards.
The selection and cheat keys remain active when keyboard focus returns to the
C64 window; select another row to change the target.
Stage 3 applies the same operation to Main and Twin. A `--record-play` session
stores these operations in the play JSON's sparse `mod_events` object; replay
applies them at the recorded frame.

F9 records a frame/cycle timestamp during play recording. F10 pauses or resumes
live execution, and F12 quits cleanly so recordings and media are finalized.
Ctrl+Tab swaps joystick ports 1 and 2; Tab alone remains the C64 Ctrl alias.
These host shortcuts are fixed rather than `revm.cfg` bindings.

**Stage 3 twin watch:** with `--kb` (Stage 3 plugin build, Twin on), compare
`watch:"yes"` Main vs Twin at `JoinAtPc` / `Sync::Compare` (not VBLANK); add
`--kb-check-verbose` for `watch:"verbose"`. Prints each mismatch:
`kb-check: [main=F:C …] FAIL frame F cycle C: <name> $ADDR: main=$XX twin=$YY`.
The end-of-run tally includes `first_fail_frame` / `cycle` when any slot failed.
Any mismatch aborts the run (exit code 1). `CompareMask::None()` skips kb.
See [`STAGE3-MANUAL.md`](../agents/STAGE3-MANUAL.md).

### `trace` (REVM)

With `--trace-calls`, log an instruction fetch when PC equals this object's
`addr` and `trace` is `yes`. Default `no`.

Breakpoints are **not** in the KB — set them from the REVM command line.

## Consumers

**REVM** — `--kb FILE`; `--watch` / `--watch-verbose` / `--watch-window`;
`--watch-timeline FILE` (JSONL of watch changes at VSYNC; pairs with
`--trace-calls` for last-trace); Stage 3 kb-check with `--kb` (optional
`--kb-check-verbose`); `--trace-calls` for `trace:"yes"`; `--coverage FILE`
writes REVMCOV2 with saturating u16 counts for the PC-RAM, PC-ROM, MEM-RAM,
and MEM-ROM planes.

**Disassembler / `revm-tool`** — `--kb` annotates ordinary `snap` and `ram`
disassembly. Snapshot `--listing` without coverage disassembles the continuous
RAM span covering the KB's `function` and `opcodes` ranges. With `--cov`, it
instead emits the exact union of executed instructions and KB-forced ranges in
RAM, BASIC, and KERNAL sections, marking rather than bridging gaps. Function
entries get `# ---- $ADDR — name (N bytes, …)` headers; `blob` regions are
skipped with a `# blob` comment; `label`s appear as names in the text only.
Snapshot `--dump-blobs` writes each named `blob` as `<name>.bin`; `--map` maps
code operand refs + MEM-plane hits plus
KB-only addresses in RAM / BASIC / KERNAL sections (CHIP / VAR / BLOB / FUNC /
LABEL / BRANCH / COMMENT / UNDOC) and reports `named (non-UNDOC) / referenced`
as a progress signal (not a Stage-2 gate — prefer UNDOC over placeholder
names; Stage 2↔3 iterate per README / GUIDE). Use
`--map-tally` for the tally alone. `--call-graph` / `--hot-undoc` need `--cov`
(REVMCOV2 u16 counts): call-graph weights JSR/JMP sites by PC-RAM fetch count;
hot-undoc ranks unnamed MEM hits (ZP first). `--watch-diff OTHER` compares KB
`watch` bytes between two FullSnapshots. `revm-tool sort-kb IN OUT` rewrites a KB with
objects sorted by bank, then `addr` (range start). MEM hits with
no KB entry are UNDOC (`mem-ram` / `mem-rom`). Optional `--kb-support` supplies
C64 chip/KERNAL names without forcing extra rows. Bank is implied by the section
header (no per-line `[KERNAL]` / `[BASIC]` tag).
