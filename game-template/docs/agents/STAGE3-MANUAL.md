# Stage 3 reference manual — C++ translation with Twin as oracle

This is the detailed technical reference for Stage 3. The working procedure
lives in `docs/agents/STAGE3.md`; come here for exact translation rules,
synchronization semantics, API usage, failure diagnosis, and checkpoint shape.
The sections are organized by subject so an agent can read the material needed
for the current slice. Read §1–§2 before beginning Stage 3, then consult §5–§12
as the translated path reaches those concerns.

Authority order: the user → `docs/agents/STAGE3.md` → this file → the headers in
`REVM_ROOT/src/revm/src/cpumock/` (`Sync.hpp`, `CpuMockHost.hpp`, `Mem.hpp`) and
`./rework revm --help` / the Stage 3 binary's `--help`. If this file and a
header disagree, the header is right; report the mismatch. Game facts live in
`Stage3/NOTES.md` (checkpoint), the game KB named by `rework.toml`, and
`Stage2/NOTES.md`. Commands run from the game root via `./rework`.

Never edit `src/revm/` during game work. If an API call seems to misbehave,
follow §15.

---

## 1. Glossary

Terms in this file mean exactly this. Do not infer meanings from other docs.

| Term | Meaning |
|---|---|
| **Main** | The board whose 6510 is replaced by your C++ plugin. It has real VIC/SID/CIA chips and RAM; it has no instruction stream. Main is the game. |
| **Twin** | A second, complete emulator running the original PRG from the same BEGIN snapshot. Read-only oracle. Twin never produces game state for Main. |
| **BEGIN** | The frozen snapshot both boards start from (`begin/snap.bin`) and its entry PC (Stage 1). |
| **play** | A recorded input sequence with periodic snapshot hashes (`plays/<name>.json`). Plays are the deterministic test inputs. |
| **fence** | Any point where the plugin stops and requires Twin to be at a specific original PC before continuing. `JoinAtPc`, `JoinAtPcBounded`, and every armed access are fences. |
| **join** | A fence at a routine boundary that also compares. `JoinAtPc` / `JoinAtPcBounded`. |
| **AtPc** | A fence at one original *instruction*: `Sync::AtPc(host, pc);` arms the immediately following explicit bag `.read()`, `.write(v)`, or RMW operation. A timing sample, not a compare. |
| **compare** | Main↔Twin equality check at a fence: KB watch bytes (`kb`), SID (`sid`), rendered frame (`screen`), VIC/CIA config (`vic`, `cia1`, `cia2`), and opt-in `.vic_state`. Selected by `CompareMask`. |
| **kb (compare)** | The KB watch-slot compare: every KB entry with `watch:"yes"` is compared Main vs Twin.
May compare DRAM or `bank:"color"` plane, never any chip registers. Unrelated to the keyboard. |
| **interval** | A stretch of original code Main executes in zero emulated time (a plain C++ body) while Twin still has to run it natively. Ends with `JoinAtPcBounded`. |
| **Φ2 / cycle** | One 6510 clock. Both boards count cycles from BEGIN; "aligned" means equal cycle counters. |
| **steal / accept** | The cycle at which a 6510 takes an interrupt. Twin's accept is where your C++ handler is dispatched. |
| **nest** | Being inside an interrupt handler. Twin "in the nest" means Twin is between its accept and its RTI. |
| **VSYNC / barrier** | VBLANK boundary. Frames are counted from BEGIN; `--max-frames` counts these. |
| **`--no-twin`** | Run Main alone. Every fence degrades to "do nothing" or "run Main for the stated duration". The plugin must still play the game. |
| **slice** | One unit of work: translate the next untranslated original code on the failing path until the frontier moves. Defined mechanically in §4. |
| **frontier** | One signature: the earliest first-failure among required plays (play, frame, cycle, exact message). Recorded in the checkpoint. |
| **checkpoint** | `Stage3/NOTES.md`. Overwritten, never appended. One frontier, all plays listed. |
| **SoftQuit** | The plugin/host aborting the run with a message; exit code 1. All fence failures are SoftQuits. |

---

## 2. The model

### 2.1 Main owns WHAT, Twin owns WHEN

Main executes your translation of the original program: every store, every
branch, every table lookup, in the original order, with original semantics.
Twin executes the original binary. Twin is used for exactly two things:

1. **Timing.** When a hardware-visible access must land on the same cycle as
   the original (a VIC register write, a CIA timer read, a byte an IRQ also
   touches), Twin tells the plugin *when* that instant is. That is what a
   fence does.
2. **Verification.** At joins, Main's state is compared to Twin's.

Twin is never a source of *values*. Reading Twin RAM, Twin registers or Twin
chip state into Main's computation makes Main a puppet that cannot run under
`--no-twin` and is rejected at review. The one door to Twin values is
`host.diag()`, and it is for log messages only (§6.3, lint S3-014).

### 2.2 Twin leads

In lockstep, Twin steps first and Main trails by ≤1 Φ2. At a fence Twin is
parked at the target PC and Main is brought up to Twin's cycle. Main never
runs past Twin. Two consequences you will feel:

- A fence at PC X is "walk Twin until it fetches X, then bring Main to that
  cycle". If Twin never fetches X on the expected path, the fence fails.
- Interrupts are chip-driven on **both** boards. Twin's accept is only the
  *trigger* for dispatching your C++ handler; the host then checks that
  Main's own chips raised the same line within a small window. If Main's chip
  programming differs from the original, this check fails ("silent line").

### 2.3 The one classification question

For every original instruction you translate, ask:

> Can anything *other than this instruction* change or observe this byte at
> this instant — a chip, an interrupt handler, the raster?

- **No** (most instructions): explicit ordinary C++ through the bags
  (`mem.x.write(v)`, `a = mem.tbl[i].read()`). No fence.
- **Yes, a chip**: the instruction is timing-critical. Arm its explicit I/O
  operation with `Sync::AtPc` and the instruction's PC (§7).
- **Yes, an interrupt handler shares this RAM**: arm its explicit RAM
  operation so a pending accept can nest before the access (§7.3).
- **The instruction is a routine entry**: `JoinAtPc` (§6.1).
- **Main just executed a body Twin still has to run** (a delay loop, a spin
  on a CIA register, a screen fade): that is an interval; close it with
  `JoinAtPcBounded` (§9).

Everything in this file follows from that question.

---

## 3. Files and wiring

### 3.1 Layout

```
<game-root>/               # this folder
  rework                   launcher (framework trampoline)
  rework.toml              game, revm_root, minimum version, kb_json, original_load; optional rom_dir
  Stage2/<game>.kb.json    KB (Stage 2). Names, addresses, watch flags.
  begin/snap.bin           BEGIN snapshot
  plays/original1.json …   Twin-backed recordings
  plays/no-twin1.json …    --no-twin recordings
  Stage3/
    NOTES.md               checkpoint (§14). Overwrite, never append.
    CMakeLists.txt         globs *.cpp — add files freely
    stage3-lint.json       lint exceptions (§13)
    plugin.cpp             InstallGame: entry handler + interrupt thunks
    game.hpp               Game class: RAM bag, IoMap, routine declarations
    game.cpp               run(), coldstart, main loop
    title.cpp, play.cpp, level.cpp, sound.cpp …   one file per region
    REVIEW.md              last review snapshot (not the frontier)
```

Partition by game region from the first slice. One C++ member function per
original routine, named from the KB, with the original entry PC in a comment
on the definition. Do not accumulate in `game.cpp`.

### 3.2 Build and run

```bash
./rework build stage3                         # → bin/revm-s3-<Game>
./rework test-play stage3 original1           # Twin compares on, headless
./rework test-play stage3 no-twin1 --no-twin  # Main alone
./rework test-play stage3 original1 --ignore-checks  # compares log but do not abort
./rework lint stage3                        # must be 0 errors before hand-off
```

`test-play` loads BEGIN + the play, the game KB, and adds `--headless`
(and `--dump-fail fail/` unless already passed). Extra options
after `TARGET`/`PLAY` go to REVM. Useful options (`--help` is
authoritative): `--max-frames N`, `--events - --report /tmp/s3.json`
(structured fence/accept trail, §12), `--log-debug CYCLE`,
`--log-verbose CYCLE`, `--kb-check-verbose`. Windowed replay:
`./rework show-play stage3 original1`.

### 3.3 `plugin.cpp`

```cpp
void InstallGame(CpuMockHost & host) {
	host.SetEntryHandler([&host] {
		replica::Game game(host);
		host.InstallIrqHandler([&game] { game.irq_game(); });   // when translated
		host.InstallNmiHandler([&game] { game.nmi_music(); });  // when translated
		game.run();
	});
}
```

Interrupts are chip-driven on both boards from the start; there is no
enable call. Until a handler body is translated, do not install the thunk —
the run will SoftQuit at Twin's first accept, which is the correct loud
frontier for "IRQ not yet translated".

### 3.4 `game.hpp` — the bags

Declare one member per original byte/word/table, named from the KB:

```cpp
struct Ram {
	Mem<0x0002>          level{host_};        // scalar byte
	Mem16<0x00FB>        ptr{host_};          // LE word; ptr.at(y) == (ptr),Y
	MemTable<0x0400,1000> screen{host_};      // table; [] SoftQuits on OOB
} mem{host_};
IoMap io{host_};                              // io.vic / io.sid / io.cia1 / io.cia2 / io.color_ram
```

Bags are the only way to touch Main memory. Raw `host.Read/Write` does not
compile from a plugin. Full accessor list in §6.4.

### 3.5 `run()`

```cpp
void Game::run() {
	host_.AssertBegin(0x0394);   // BEGIN entry PC from STAGE1.md
	coldstart();                 // each routine opens with its own JoinAtPc
	title_screen();
	main_loop();
}
```

---

## 4. The slice loop

Do these steps in order. Each has a command and an observable result.

**Step 0 — Reproduce the checkpoint.** Read `Stage3/NOTES.md`,
`docs/agents/STAGE3.md`. Build. Run every play in the checkpoint table with
the command shown there. Confirm the frontier reproduces (same play, same
frame, same message). If it does not, that is your slice: find out why
before translating anything.

**Step 1 — Pick the slice.** The slice is: *the earliest original
instruction on the failing path that Main does not yet execute.* Find it:

- Frontier is `JoinAtPcBounded miss` / `JoinAtPc miss` → Twin reached a PC
  Main did not fence, or took a path Main did not translate. Look at
  `twin_pc` in the message and the listing: what did the original do between
  the last passing fence and `twin_pc`?
- Frontier is a SoftQuit you wrote ("not yet translated $XXXX") → translate
  that routine.
- Frontier is a kb/sid/screen compare fail → §12.
- Frontier is an interrupt dispatch with no handler → translate the handler
  body (§8).

Never skip an original instruction to reach a more interesting one. Never
stub a routine as a no-op that Twin will execute natively (§10, trap 1).
The slice ends when the frontier signature moves later on the same play or
a later play becomes the earliest failure.

**Step 2 — Investigate the original.** Read the disassembly for the routine.
Note: entry PC, every JSR/JMP/branch target, every store to `$D000–$DFFF`,
every read of CIA timers/ports, every ZP byte the KB marks as IRQ-shared,
loop counts (`LDX #n … DEX … BPL` runs n+1 times), and the flags the routine
leaves for its caller. Confirm names in the KB; add missing ones to the KB
with provenance (address, disassembly line). Do not invent placeholder
entries.

**Step 3 — Translate.** One member function, `JoinAtPc` first statement,
then the listing in order (§5). Apply §2.3 to each instruction. Add the file
if it is a new region.

**Step 4 — Build and lint.**

```bash
./rework build stage3 && ./rework lint stage3
```

Zero lint errors. Each warning either fixed or carrying an inline
`// s3lint:allow S3-0NN — <reason>`.

**Step 5 — Gates.** Run, in this order, and record the output:

```bash
./rework test-play stage3 <play> --max-frames <last_pass>    # must exit 0
./rework test-play stage3 <play> --max-frames <first_fail>   # shows the new frontier
./rework test-play stage3 <play> --no-twin --max-frames <N>  # ordinary replica smoke (§11)
```

Then the full play table once. The tally lines to record from each run:
`irq paired=… silent=…`, `nmi paired=… silent=…`, `screen: compared=…
failed=…`, `sid: …`, `kb: …`, exit code.

**Step 6 — Checkpoint.** Overwrite `Stage3/NOTES.md` per §14. One
frontier. All plays in the table. Nothing dated, nothing narrative.

**Step 7 — Hand off.** Your final message: exact commands run, last pass /
first fail per play, `--no-twin` result, lint result, ignores/exceptions in
force, and the KB entries you added. Review process:
[`STAGE3-REVIEW.md`](STAGE3-REVIEW.md).

Definition of done for a slice: lint 0 errors; the three Step-5 commands
behave as stated; checkpoint overwritten; no diff under `src/revm/`.

---

## 5. Translating a routine

### 5.1 Shape

```cpp
// $EC20 play_tick — called each main_loop pass; BCS → death.
bool Game::play_tick() {
	Sync::JoinAtPc(host_, 0xEC20);            // entry fence + Kb() compare
	uint8_t a = mem.timer.read();              // LDA $63
	a = uint8_t(a - 1);                        // SEC / SBC #$01
	mem.timer.write(a);                        // STA $63
	if (a == 0) goto EC40;                     // BEQ $EC40
	…
EC40:
	return mem.lives.read() == 0;              // $EC40 … RTS with C
}
```

Rules:

- **The join is the first statement.** Entry PC from the listing. Leaf
  routines of a few instructions may be inlined into the caller without a
  join; say so in a comment.
- **Instruction-complete.** Every original instruction has a C++ equivalent
  in order. If you must leave a gap, `host_.SoftQuit(1, "not yet translated
  $XXXX <name>")` at that point. A silent gap is the most expensive bug in
  this stage.
- **Mirror the listing, not clean C++.** Tangled 6502 becomes `goto` with
  PC-named labels. Registers are locals `a, x, y`; flags are locals
  `bool c, z, n` only where the original tests them across instructions.
- **Loops:** `LDX #n; …; DEX; BPL` iterates n+1 times. `BNE` after `DEX`
  iterates n times. Write the C++ loop to match, and comment the count.
- **Indexing:** `mem.tbl[x]` SoftQuits on out-of-range — that is a
  translation bug, not a nuisance. `Mem16::at(y)` is `(ptr),Y`.
- **RMW on memory:** `mem.x.inc()` / `.dec()` / `.asl(&c)` / `.lsr(&c)` /
  `.rol(&c)` / `.ror(&c)` for `INC $x` etc. (Main-only, no fence).
  `asl(a, c)` etc. for accumulator shifts. Never `mem.x = mem.x + 1` for an
  original INC — the RMW helpers exist so the timing-critical variant
  (an armed `.inc()`) remains mechanically obvious.
- **Provenance:** the PC of every store to I/O and every fence in a trailing
  comment. Function names from the KB; KB gets the name if it lacks one.
- **Return the flag the caller branches on** (`bool` for C/Z), not an
  interpretation of it.

### 5.2 KERNAL and ROM

If the original calls KERNAL/BASIC ROM routines, translate the routine
bodies the game actually exercises (Main owns them too). Twin runs the real
ROM. Do not "let Twin do the ROM part".

---

## 6. API reference

Everything a plugin may call. Anything not listed here is private and will
not compile (or is flagged by lint S3-000).

### 6.1 `Sync` — fences and durations (`Sync.hpp`)

```cpp
// Routine boundary. Fence Twin at pc (same frame), Φ2-align, compare(mask).
void  Sync::JoinAtPc(host, uint16_t pc,
                     CompareMask mask = CompareMask::Kb(),
                     FenceSlack slack = FenceSlack::AllowOneVSync);

// Arm exactly the next explicit bag access. RAM accepts FenceSlack; I/O
// retains its native retry rules. The following statement must consume it.
void  Sync::AtPc(host, uint16_t pc,
                 FenceSlack slack = FenceSlack::AllowOneVSync);

// Interval exit. Walk Twin (paired) until it fetches pc, catching Main up one
// VBLANK at a time, at most max_frames. Φ2-align, compare(mask). Returns the
// number of VBLANKs crossed. --no-twin: run Main no_twin_frames VBLANKs.
struct Sync::Bounded {
	uint32_t    max_frames;       // Twin-side budget (required)
	uint32_t    no_twin_frames;   // Main-owned duration without Twin (required)
	CompareMask mask = CompareMask::Kb();
	uint16_t    no_lap_pc = 0;    // SoftQuit if Twin crosses this loop head meanwhile
	bool        yield_at_target = false; // park ON the fetch so a pending accept
	                                     // is yielded before a following AtPc
};
uint32_t Sync::JoinAtPcBounded(host, uint16_t pc, Bounded);

// Compare now, no fence. Use after a same-PC AtPc when a compare is wanted.
void  Sync::Compare(host, CompareMask mask = CompareMask::Kb());

// Handler teardown (§8). Caller has already ACKed the chip.
void  Sync::ReturnIrq(host, uint16_t rti_pc, uint32_t no_twin_cycles,
                      CompareMask mask = CompareMask::None());
void  Sync::ReturnNmi(host, uint32_t no_twin_cycles,
                      CompareMask mask = CompareMask::None());

// Main-owned durations. With Twin: lockstep (both boards run). Use for
// original busy-waits of known length and for --no-twin pacing.
AdvanceResult Sync::AdvanceCycles(
    host, uint64_t cycles,
    VSyncPolicy policy = VSyncPolicy::Cross);
AdvanceResult Sync::AdvanceToVSync(host);

// Program lockstep (§9.3). WatchMark registers the watch itself.
uint64_t Sync::WatchMark(host, uint16_t loop_head_pc);
void     Sync::ExpectWatchDelta(host, uint16_t pc, uint64_t mark,
                                uint64_t expected, const char* what);
```

`CompareMask`: `None()` (fence only), `Kb()` (default), `All()` (kb + sid +
screen + vic + cia1 + cia2). `.vic_state` is opt-in (§10.1). Designated
initialiser e.g. `CompareMask{.vic_state = true, .kb = true}`.
`FenceSlack::AllowOneVSync` tolerates one silent extra Main VBLANK if Twin misses the
fence in this frame; `FenceSlack::Exact` makes that a failure. Prefer `Exact`
once a fence is understood; the default will become `Exact`.

**Never** use `JoinAtPc` as a wait. A join is a claim that the original is
*at this PC now*. Waiting is `AdvanceCycles` / `AdvanceToVSync` (Main-owned) or
`JoinAtPcBounded` (Twin-led interval exit).

### 6.2 `CpuMockHost` — wiring and control (`CpuMockHost.hpp`)

```cpp
host.InstallIrqHandler(fn);  host.InstallNmiHandler(fn);  host.SetEntryHandler(fn);
host.AssertBegin(uint16_t pc);   // once, first statement of run()
host.Sei();  host.Cli();         // the original SEI / CLI — mirror them
bool host.HasTwin();             // branch for Twin-backed synchronization
bool host.NoTwin();              // branch for --no-twin siblings (§11)
bool host.QuitRequested();       // poll in long C++ loops
[[noreturn]] host.SoftQuit(int code, const char* fmt, ...);
```

### 6.3 `host.diag()` — oracle values for log text only

```cpp
host.diag().TwinCpu()            // CpuState: .pc .a .x .y .sp .p
host.diag().TwinPeek(addr)       // Twin RAM byte
host.diag().PeekMain(addr)       // Main RAM byte (read-only)
host.diag().WatchTwinPc(pc, label)   // log probe when Twin fetches pc
host.diag().LogVsTwinIrqSources(why) // side-by-side VIC/CIA IRQ state dump
host.diag().AlignMainCiaPhaseToTwin()  // copy Twin CIA phase onto Main (§9.4) — debt
```

Allowed only inside `REVM_LOG*` arguments, or with `// s3lint:allow S3-014 —
<reason>` (lint error otherwise). A `diag()` value that influences a branch,
a store, or a mask is copying Twin.

### 6.4 Bags (`Mem.hpp`)

| Object | Ordinary (no fence) | Fence at one instruction |
|---|---|---|
| `Mem<A> x` | `x.read()`, `x.write(v)`, `x.inc()/.dec()`, `x.asl(&c)/.lsr(&c)/.rol(&c)/.ror(&c)` | `Sync::AtPc(host, pc[, slack]);` then the same explicit operation |
| `MemTable<A,N> t` | `t[i]` → byte object with everything above | arm, then `t[i].read()` / `.write(v)` / RMW |
| `Mem16<A> p` | `p = w`, `uint16_t(p)`, `p[0]`/`p[1]` (lo/hi byte objects), `p.at(y)` → byte at `p+y` | via the byte objects |
| `Io<A>` members of `io.*` | `io.vic.border.write(0)`, `io.cia1.icr.read()`, `.inc()` … | arm, then the same explicit operation (I/O ignores `FenceSlack`) |
| Free helpers | `inc(a)`, `dec(a)`, `asl(a, c)`, `lsr(a, c)`, `rol(a, c)`, `ror(a, c)` on locals | — |

Implicit bag conversion/assignment and `.get()`/`.set()` remain framework
compatibility shims for older replicas, but Stage 3 source must not use them
(lint S3-015). One vocabulary prevents an armed marker from being consumed by
an unexpected C++ conversion.

`IoMap`: `io.vic.{raw, msigx, scroly, raster, lpx, lpy, spena, scrolx,
spexpy, vmcsb, irr, irqmask, spbgpr, spmc, spexpx, spspcl, spbgcl, border,
bg0..bg3, spmc0, spmc1, sp[0..7].{x,y,color}}`, `io.sid.{raw, fc_lo, fc_hi,
res_filt, vol, pot_x, pot_y, osc3, env3,
voice[0..2].{freq_lo, freq_hi, pw_lo, pw_hi, ctrl, ad, sr}}`,
`io.cia1.{pra, prb, ddra, ddrb, talo, tahi, tblo, tbhi, tod10, todsec,
todmin, todhrs, sdr, icr, cra, crb}`, `io.cia2.{pra, prb, ddra, ddrb, talo,
tahi, tblo, tbhi, tod10, todsec, todmin, todhrs, sdr, icr, cra, crb}`,
`io.color_ram[i]`. `raw[off]` is the escape hatch for unnamed registers at
the canonical base — never `$D040`/`$D500` mirrors. SID `$D500` mirrors are
the same 32-byte file as `$D400` (a write to +18 is `vol`).

Shift RMW methods require `bool *carry` (SoftQuit if null). `INC/DEC` take
none.

---

## 7. Timing-critical access (AtPc)

### 7.1 When

An instruction is timing-critical when the classification question (§2.3)
answers "a chip" or "an interrupt shares this byte". In practice:

- **Every VIC register access** (`$D000–$D02E`) outside a documented SEI
  interval. Sprite positions, raster, control registers, colors mid-frame.
- **Every CIA access that samples or programs time**: timer reads/writes,
  ICR reads/writes, control registers, TOD, SDR spins, and joystick/keyboard
  port reads *when the original polls them in a timing-sensitive loop*.
- **SID**: ordinary by default (SID compare at joins covers it), except the
  writes that program an interrupt-driven music driver's first tick.
- **RAM that an IRQ/NMI also reads or writes** (KB should mark these): the
  main-loop instruction that touches it.
- **Every store from inside an interrupt handler body** that is visible on
  screen (sprite pointers, screen RAM, color RAM) — see §8.3.

Ordinary access to these compiles and runs. The failure appears later at a
kb/screen compare, on a different frame. That is why the default for VIC/CIA
is AtPc, not "add it when it fails".

### 7.2 The contract

The canonical form is two adjacent lines:

```cpp
Sync::AtPc(host_, 0xF574);       // opcode address of this instruction
mem.screen_matrix[6 + x].write(mem.score_bcd[x]);
```

`Sync::AtPc` arms exactly the next explicit bag `.read()`, `.write(v)`,
`.inc()`, `.dec()`, `.asl(&c)`, `.lsr(&c)`, `.rol(&c)`, or `.ror(&c)`.
That operation consumes the marker and delegates to the proven legacy AtPc
backend. The marker is cleared before interrupt dispatch, so a nested handler
cannot inherit it. The two statements must be adjacent except for comments;
double-arm, a synchronization boundary, or an unconsumed marker SoftQuits and
lint S3-013 rejects non-adjacent source. An implicit conversion or assignment
does not consume a marker. This is intentional: an expression such as the
example above may evaluate ordinary source data before its explicit timed
destination write.

`pc` is the **opcode address of that one instruction** in the listing. One
armed operation per original instruction. Surrounding ALU stays plain C++.
| | I/O (`io.*`) | RAM (`mem.*`) |
|---|---|---|
| Fence | Twin walked to the fetch *after* the instruction, so Twin's chip access is known | Twin parked *at* the instruction's fetch, so a pending accept nests first |
| armed `.read()` returns | Twin's operand (Main's own read is logged if different) | Main's byte |
| armed `.write()` checks | SoftQuit if Twin stored a different value | Nothing (a wrong value surfaces at kb) |
| armed RMW | Twin's read→write must match the op; Main's too | Main applies the op after the fence |
| `FenceSlack` | none (VBLANK crossings retried internally) | `AllowOneVSync` default / `Exact` |
| `--no-twin` | ordinary access | ordinary access |

`WriteIoAtPc value mismatch … twin=$XX main=$YY` is the earliest, most
local translation check you have. Treat it as a gift.

### 7.3 Rules

- **Do not `JoinAtPc` and then arm a RAM read at the same PC.** The join parks Twin
  without executing the fetch; the later AtPc cannot pair a pending accept.
  The AtPc *is* the fence; add `Sync::Compare` after it if you want a
  compare. (Lint S3-001 within one function; across functions it is on you.)
- **Never fake an RMW.** An armed `io.vic.sp[0].x.write(old + 1)` for an
  original `INC $D000` is wrong (lint S3-002). Arm `.inc()` instead.
- **MSB fixups after sprite X RMW**: translate the original's `BNE` + `$D010`
  update as its own timed access on `io.vic.msigx`, matching the opcode:
  armed `.inc()` for `INC $D010`, `.write()` for `STA $D010`, `.read()` for the
  `LDA`/`ORA`/`EOR` that reads it. Never rewrite an RMW as read + write.
- **Do not AtPc speculatively.** Each AtPc is a claim about a specific
  instruction; a wrong PC (e.g. the operand address, or the next opcode)
  SoftQuits with `AtPc I/O: not an I/O-sized opcode` or a miss.
- **Handler bodies**: AtPc inside an installed IRQ/NMI body is allowed but
  reviewed (lint S3-012 warning) — see §8.3.

---

## 8. Interrupt handlers

### 8.1 Model

Chips raise the line on both boards. When Twin's 6510 accepts, the host
dispatches your installed thunk, first checking that Main's line fired
within a small skew window of Twin's steal. Your thunk runs on Main at that
instant. Twin then runs its native ISR to its RTI; `ReturnIrq` waits for
that and tears down.

### 8.2 Shape

```cpp
// $F169 irq_game — raster IRQ each frame.
void Game::irq_game() {
	// translated body: bookkeeping is ordinary, racing chip access is AtPc
	mem.frame_ctr.dec();                                 // DEC $A0
	Sync::AtPc(host_, 0xF1C0);
	io.vic.sp[3].y.write(uint8_t(mem.y3));               // STA $D007 (visible)
	…
	(void)uint8_t(io.cia1.icr);                          // LDA $DC0D — ACK
	Sync::ReturnIrq(host_, 0xF200, /*no_twin_cycles=*/0);  // original RTI PC
}

// $A0E4 nmi_music — CIA2 Timer A.
void Game::nmi_music() {
	host_.Sei();
	(void)uint8_t(io.cia2.icr);                          // ACK
	… music driver body (SID writes ordinary) …
	Sync::ReturnNmi(host_, /*no_twin_cycles=*/0);
}
```

- ACK exactly as the original does (CIA `LDA $DC0D`, or VIC `STA $D019`).
  Lint S3-005 warns when `Return*` has no preceding ACK.
- `ReturnIrq` needs the original RTI PC so Twin can be walked to it.
  `ReturnNmi` does not chase Twin (the NMI body's duration is owned by later
  fences).
- `no_twin_cycles` is the handler's original body length used only under
  `--no-twin`; `0` is acceptable until timing under `--no-twin` matters.
- Inside a handler you may use `JoinAtPc` (same frame), armed accesses, `Compare`,
  `Return*`. You may **not** use `JoinAtPcBounded`, `AdvanceCycles` or
  `AdvanceToVSync` (lint
  S3-004): a handler is an instant, not a duration.
- Mirror the original's `SEI`/`CLI` with `host_.Sei()` / `host_.Cli()`.
- Do not install a thunk before its body is translated (§3.3).

### 8.3 Handler stores visible on screen

A store from the handler to sprite pointers, screen RAM, color RAM or VIC
registers lands mid-frame. If it is ordinary, Main's store happens at
dispatch (Twin's accept), which is usually right; but where the original ISR
does work *before* that store, the raster may already have passed the row.
Symptom: a `screen` compare fail with a small bounding box in one sprite or
one character row, with kb equal. Fix: arm that store only. Do
not convert ZP counters or anything not visible.

### 8.4 Diagnosing interrupt divergence

`IRQ schedule FAIL: Main line silent for N consecutive accepts` means Twin
accepted an interrupt Main's chips did not raise. Almost always Main's CIA
timer / VIC raster programming differs from the original: an armed write is
missing on the store that programs the latch, or a write has the wrong
value (which `WriteIoAtPc value mismatch` would have caught if it were
AtPc). Run with `--events - --report /tmp/s3.json`, find the first `accept`
event with `silent`, and look at the last I/O writes before it. Do not fix
by walking Twin around it.

---

## 9. Intervals and `JoinAtPcBounded`

### 9.1 What an interval is

Main executes a translated body in zero emulated time. Twin still has to
run that body natively — a delay loop, a `LDA $DC0C` spin, a fade, a death
animation, a music-init busy wait. Until Twin arrives at the body's exit,
the boards are legitimately at different program positions. An interval is:

1. an entry `JoinAtPc` (both boards at the body's start);
2. the translated body (ordinary C++; internal fences use
   `CompareMask::None()` and are listed in `stage3-lint.json`);
3. `JoinAtPcBounded` at the exit PC.

```cpp
Sync::JoinAtPc(host_, 0xE500);                        // entry
… body …
Sync::JoinAtPcBounded(host_, 0xE584,
	{.max_frames = 24, .no_twin_frames = 18});        // exit: Kb() compare
```

The helper walks Twin, paired, up to `max_frames` VBLANKs; when a VBLANK is
crossed it brings Main to the same barrier (Main-only, never a lockstep
frame — a lockstep frame would let Twin run a frame of code Main did not
mirror). On arrival it aligns, compares, and returns the measured frame
count, logged at INFO when > 0:

```
JoinAtPcBounded $E584 arrived after 3 frame(s) (max 24) at title.cpp:612
```

On timeout: `JoinAtPcBounded miss want=$E584 twin_pc=$E5AB result=HitVSync
n=25 max=24 at title.cpp:612`. `twin_pc` tells you where the original
actually is: usually an untranslated path, or a wrong exit PC.

### 9.2 Choosing the fields

- **`max_frames`**: measured + margin. First run with a generous value,
  read the "arrived after n" line on each play, set `max_frames` to the
  largest n plus a small stated margin. Comment the measurement. **Never
  raise `max_frames` to make a miss go away** — a miss means the original
  is somewhere else.
- **`no_twin_frames`**: what Main does when there is no Twin. Usually the
  measured n on the plays, since Main's chips are on the same schedule. If
  the original's duration is data-dependent (a CIA spin), it may be more
  honest to keep a `host_.NoTwin()` branch that polls the same register
  Main-side; say so in a comment.
- **`mask`**: `Kb()` unless the exit is a place where the picture is settled
  (then `All()`); `None()` only for interior fences (§10).
- **`no_lap_pc`**: the main-loop head, when the interval sits inside a loop
  Twin could lap while Main is parked (§9.3).
- **`yield_at_target`**: only when the very next statement is an AtPc at the
  same PC that must see a pending accept nest first (rare).

### 9.3 Program lockstep

Cycle lockstep is not program lockstep. Boards can be Φ2-aligned while Twin
has completed a whole extra pass of the main loop Main never mirrored.
Compares on quiet bytes stay green; a chaotic byte (RNG, input latch) fails
tens of frames later. Guard every loop that an interior fence could lap:

```cpp
const uint64_t mark = Sync::WatchMark(host_, 0x03C0);   // loop head
… fences / interval …
Sync::ExpectWatchDelta(host_, 0x03C0, mark, 0, "main_loop lapped during load");
```

or, inside an interval exit, `.no_lap_pc = 0x03C0`. Under `--no-twin` both
are no-ops.

### 9.4 CIA phase after an unpaired stretch

If an interval left Main's CIA timers programmed at a different phase than
Twin's (because the programming writes could not be paired), the next timed
interrupt will be "silent". `host.diag().AlignMainCiaPhaseToTwin()` copies Twin's
CIA phase onto Main. It is copying Twin state: allowed only with an
`s3lint:allow S3-014 — <reason>` and recorded in the checkpoint as debt.
Prefer pairing the programming writes with `Sync::AtPc` plus `.write()`.

---

## 10. Compare masks — honesty at the fence

- `Kb()` at every routine join is the baseline. It costs nothing and
  catches most translation bugs at the next boundary.
- `All()` at a fence where a complete picture exists and both boards are
  between frames on the same path (after a VBLANK return to the loop head;
  after the title is fully drawn). Never mid-raster, never inside a handler,
  never during a fade or a mid-mix state. A wrong `All()` produces sprite-
  position or SID noise that is not a translation bug.
- `None()` only on interior fences of a documented interval, each one
  listed in `stage3-lint.json` with a reason (lint S3-003). `Compare(None())`
  is a no-op (S3-011).
- **Weakening a mask to make a fail disappear is forbidden.** Dropping
  `screen` because sprites drift, dropping `sid` because a voice differs,
  dropping `vic`/`cia*` because a config register differs, or moving a join
  later so a byte has "settled" hides the translation bug you were paid to
  find. Keep the mask; fix the translation; or record the fail as the
  frontier.
- `--ignore-checks` is for looking past a known frontier during
  investigation. It never appears in a checkpoint command.

Trap 1 (the puppet): a run with `kb: failed=0` where Main computed nothing
because every routine is a stub and Twin's native code did the work during
walks. Guard with the Step-1 rule that no original instruction is skipped,
review, and bounded `--no-twin` smokes. Do not require the smoke to follow
the original Twin-backed recording to the same frame or state (§11).

### 10.1 Chip registers versus Twin

KB watches are DRAM or `bank:"color"`. They never sample VIC, SID, or CIA.
Chip compares are `CompareMask` channels on `JoinAtPc` / `Sync::Compare` —
the same snapshot path as SID (`GetState`, never a chip read). `--no-twin`
they no-op.

**SID** (`.sid`) is on `All()`. Public SID write registers `$D400–$D418` plus
pots.

**VIC** `.vic` is on `All()`. It is the programmed file (sprite coords, ctrl
without RST8, enable/expand, vbase, irq_mask, irq_raster, priority,
multicolor, colours). Not live raster, `irq_flag`, collision latches, or
lightpen — those are beam-phase. `.screen` remains the picture check.
`.vic_state` is the full `MOS6569State` (SC internals) and stays opt-in:

```cpp
Sync::Compare(host_, CompareMask{.vic_state = true, .kb = true});
```

| Channel | What |
|---------|------|
| `.vic` | Config (on `All()`). Status latches and live raster are out. |
| `.vic_state` | Full `MOS6569State`, including SC internals. Diagnostic / strict. Not on `All()`. |
| `.cia1` / `.cia2` | Config (on `All()`): DDR, CRA/CRB, timer **latches**, interrupt mask. CIA2 also PRA bits 0–1 (VIC bank). Not live counters, ICR flags, TOD, or CIA1 ports. |

`All()` is `screen` + `sid` + `vic` + `cia1` + `cia2` + `kb`. Not `.vic_state`.

A KB watch over the I/O window is DRAM or color RAM, not a chip compare.

---

## 11. `--no-twin`

Main alone is a first-class playable target; Stage 4/5 are built on it. Every
fence degrades:

| With Twin | `--no-twin` |
|---|---|
| `JoinAtPc` | no-op (no compare) |
| `JoinAtPcBounded` | `no_twin_frames` Main VBLANKs |
| armed explicit bag access | the same explicit operation, ordinary Main-only |
| `ReturnIrq/Nmi` | `no_twin_cycles` then teardown |
| `AdvanceCycles` / `AdvanceToVSync` | same (Main-only) |
| `WatchMark/ExpectWatchDelta` | no-op |
| `diag().*` | hard quit — keep it out of paths that run without Twin |

Where an original wait is data-dependent and you cannot give a frame count,
write the `host_.NoTwin()` sibling explicitly (poll the same Main register
the original polls).

Original-game recordings are authoritative only with Twin attached. Without
Twin, changed fence timing can alter CIA samples, collisions and RNG; the game
then follows a valid but different trajectory, so later recorded inputs may no
longer fit.

Once no-Twin recordings exists, they are the identity contract for all stages
going forward in the `--no-twin` mode and for the standalone export.

Name Twin-backed recordings `plays/originalN.json`. Name `--no-twin`
recordings `plays/no-twinN.json`; those files store `"no-twin": true`.

Run bounded `--no-twin` smokes every slice even with Twin-backed recordings.
They check that Main starts, accepts input, advances hardware, renders and 
services interrupts without crashing, hanging or hitting a missing no-Twin path.
A different pickup, death or level path is not a frontier against Twin. 
`mod_events` plays are unsuitable; smoke only a useful natural prefix.

Play feel cannot be automated. Stage 3 finishes when all original plays pass
end-to-end with Twin, lint/review are clear, no-Twin smokes run, and the user
accepts no-Twin behavior. The user then records fresh `no-twinN.json` plays
for Stage 4.

---

## 12. Reading failures

Always start with:

```bash
./rework test-play stage3 <play> --max-frames <first_fail+2> --events - --report /tmp/s3.json
```

`/tmp/s3.json` has `first_fail`, the fence trail (`op`: `join`, `bounded`,
`io_read`, `io_write`, `io_rmw`, `ram_read`, `ram_write`, `ram_rmw`;
`result`: `ok`, `hit_vsync`, `miss`, `timeout`), accept events with skew,
and watch deltas. Then branch:

**`JoinAtPc miss want=$X twin_pc=$Y result=<R> at f.cpp:L`**
- `Y` is inside a routine you have not translated → slice = that routine.
- `Y` is later on the same path than `X` → Main skipped an instruction or
  joined the wrong PC; compare the listing between the previous fence and `X`.
- `Y` is earlier → a branch Main took that the original did not (a flag,
  an off-by-one loop, a stale local).
- `R = HitVSync` with `FenceSlack::Exact` → the original needed more than this
  frame to reach `X`: this is an interval exit; use `JoinAtPcBounded`, not a
  looser slack.

**`JoinAtPcBounded miss … twin_pc=$Y n=… max=…`**
- `n == max` and `Y` is in the interval body → the body is data-dependent
  and longer on this play: measure, then raise `max_frames` *with the
  measurement in the comment*. If `Y` is *not* in the body, the original
  left the interval by a path you did not translate.
- `R != HitVSync` → an interrupt dispatch failed inside the walk; see §8.4.

**`WriteIoAtPc value mismatch pc=$P addr=$A twin=$T main=$M`**
Your value at instruction `P` is wrong. Look at the ALU chain feeding it in
the listing; typical: missing carry, wrong table index, register reused.

**`AtPc I/O: not an I/O-sized opcode pc=$P`** / `I/O cache addr mismatch`
Wrong `pc`: not the opcode of the instruction that touches `A`.

**`kb-check: FAIL frame F cycle C: <name> $A: main=$M twin=$T`**
- Find the *last* store to `$A` in your translation before this fence and
  the same in the listing. Missing store, wrong value, or a store the
  original made from an interrupt you have not translated.
- If the differing byte is chaotic (RNG, frame counter, input latch) and
  the fail is many frames after the last change you made → program lockstep
  (§9.3): count loop-head crossings with `WatchMark` before re-translating
  anything.
- If several bytes differ at once right after an interval exit → the exit
  PC is wrong or Twin left the interval elsewhere; check `twin_pc` in the
  `bounded` event.

**`screen mismatch` (frame F, bbox)**
- Small bbox in one sprite or one text row, kb equal → a handler store
  (§8.3) or a sprite X/Y RMW that must be AtPc. Use the bbox: HUD rows →
  screen/color RAM store in the ISR; sprite band → the `sp[n]` store.
- Whole-frame or many cells → the fence with `All()` is mid-frame or
  mid-transition; the mask is wrong for this fence, or the picture
  legitimately is not settled here. Move `All()` to the settled fence —
  keep it somewhere.

**`SID mismatch`**
- Voice control/frequency differs right after a music init → the driver's
  first tick ran at a different time: pair the CIA2 timer programming with
  an armed `.write()`, or the `All()` is too early.
- Otherwise: a translation bug in the driver body; `kb` on the driver's
  state bytes will usually show it too.

**`VIC FAIL` / `VIC state FAIL`**
- Config regs (`.vic`) after a sprite/scroll store → the AtPc write is
  wrong or missing. Sprite coords at an `All()` that is still mid-mux →
  the fence is too early; move `All()`, do not drop `.vic` or `screen`.
  Collision latches / `irq_flag` / lightpen are not this channel.
- `.vic_state` when config regs match → SC internals / status latches;
  too strict for a join unless that is the question.

**`CIA1 FAIL` / `CIA2 FAIL`**
- DDR / CRA / latch / mask differ → a programming write was missed or
  unpaired. Pair it with `Sync::AtPc`. Live timer phase is **not** this
  channel; that is §9.4.

**`IRQ schedule FAIL … silent`** → §8.4.

**`ExpectWatchDelta … delta=N expected=E`** → Twin completed N−E native
passes Main did not mirror. Find the interval that let Twin run: the fence
before it used a lockstep wait where a Main-only catch-up was needed, or a
`JoinAtPcBounded` is missing.

**Wall-clock timeout (exit 124)** → an unbounded C++ loop or a `--no-twin`
path with no duration. Never a frontier; fix the loop (poll
`host_.QuitRequested()` in long loops).

Forbidden responses to any of the above: read the value from Twin, walk
Twin past the code, drop a mask flag, raise `max_frames` without a
measurement, edit `src/revm/`.

---

## 13. Lint

`./rework lint stage3`. Rule table: `scripts/lint_s3.py --list-rules`.
Fixtures: `./rework selftest`. Zero errors before hand-off. Inline exception:
`// s3lint:allow S3-0NN — <reason>` on the offending line or the line
before.

| Rule | Sev | Catches |
|---|---|---|
| S3-000 | error | private host/Sync API from a plugin |
| S3-001 | error | `JoinAtPc` then RAM AtPc at the same PC in one function |
| S3-002 | error | timed `.write(x ± 1)` fake RMW |
| S3-003 | error | `None()`/`{}` join whose PC is not in `stage3-lint.json` |
| S3-004 | error | `JoinAtPcBounded` / `AdvanceCycles` / `AdvanceToVSync` inside an interrupt handler |
| S3-005 | warning | `Return*` without a preceding ICR/IRR ACK |
| S3-007 | warning | SID access outside `$D400–$D41F` |
| S3-008 | error | `sp[>7]` / `voice[>2]` literal index |
| S3-009 | warning | non-literal PC in a fence |
| S3-011 | warning | `Compare(None())` no-op |
| S3-012 | warning | AtPc inside an installed handler body (allowed with reason) |
| S3-013 | error | armed AtPc is double-armed, unconsumed, or non-adjacent |
| S3-014 | error | `host.diag()` outside a `REVM_LOG*` argument list |
| S3-015 | error | implicit mem/io bag read or write (use `.read()`/`.write()`) |

`Stage3/stage3-lint.json`:

```json
{ "none_mask_join_pcs": { "0xE5AB": "death anim interior fence; exit $E584 compares Kb()" } }
```

Lint cannot see: a skipped original instruction, an ordinary VIC access
that should be AtPc, a `JoinAtPc` used as a wait, a stub that Twin executes
natively. Those are review items; the checkpoint must not claim them clean.

---

## 14. Checkpoint and hand-off

`Stage3/NOTES.md` is overwritten each slice. Shape:

```markdown
# <Game> — Stage 3
Plugin: `Stage3/` → `bin/revm-s3-<Game>`   Review: `Stage3/REVIEW.md`

## Current checkpoint
- Lint: `./rework lint stage3` — 0 errors, N warnings (allowed: …)
- Last stable joined routine/PC:
- **Working frontier** (one): <play> frame <F> — `<exact message>`
- Temporary ignored checks: none
- Twin-state exceptions: <AlignMainCiaPhaseToTwin sites, if any>
- No-Twin status: no-twin1 to <F>, no-twin2 to <F>

| Play | Command | Last pass | First fail |
|---|---|---|---|
| original1 | `./rework test-play stage3 original1 --max-frames 1820` | 1819 exit 0, irq 612/0, kb 4102/0 | 1820 `kb-check FAIL … items_left` |
| original2 | `./rework test-play stage3 original2 --max-frames 1986` | 1986 exit 0 | — |

## Still open
Intervals with measured max_frames, debt (AlignMainCiaPhaseToTwin, no_twin_cycles=0), next slice.
```

Not in the checkpoint: history, closed slices, narrative, dated blocks,
landmines that are already in this file. Game-specific lessons with PCs go
in `docs/LANDMINES.md` (append-only) if the game needs one.

Hand-off message: commands, tallies, frontier, ignores, exceptions, KB
changes, and anything from §15. Review:
[`STAGE3-REVIEW.md`](STAGE3-REVIEW.md).

---

## 15. Forbidden, and when a helper looks wrong

Forbidden in a plugin, with the alternative:

| Do not | Because | Instead |
|---|---|---|
| Read Twin values into game logic (`diag()` outside logs) | Puppet; fails `--no-twin` | Translate the producer; use AtPc for timing |
| Stub a routine Twin executes natively | Green compares with Main doing nothing | Translate it, or `SoftQuit("not yet translated")` |
| Skip original instructions | Silent divergence later | Instruction-complete or a loud SoftQuit |
| `JoinAtPc` as a wait | A join claims position, not duration | `AdvanceCycles`, `AdvanceToVSync`, or `JoinAtPcBounded` |
| Raise `max_frames` past a measurement to hide a miss | Twin is somewhere else | Read `twin_pc`; translate that path |
| Drop `screen`/`sid`/`vic`/`cia*`/`kb` from a mask to pass | Hides the bug | Fix, or record as frontier |
| KB-watch VIC/SID/CIA registers | DRAM or a destructive chip read | `CompareMask` `.vic` / `.sid` / `.cia1` / `.cia2` (§10.1) |
| Leave `--ignore-checks` in a checkpoint command | Not a pass | Investigation only |
| Edit `src/revm/` | Not your slice; hides a translation gap | §15 bug case |

When a `Sync`/host helper looks wrong: it is almost always a missing or
misplaced translation. Before reporting, confirm (a) the PC is the opcode
of the right instruction, (b) the original actually reaches that PC on this
play (Twin's `twin_pc` says where it is), (c) the same fence works on the
other play. If it still looks like the helper, stop and report a
reproducible case:

1. exact command (`./rework test-play stage3 <play> --max-frames N --events - --report …`);
2. the SoftQuit line with `file:line`;
3. the plugin lines around the fence and the listing lines around the PC;
4. why you believe the helper, not the translation, is wrong.

The user decides. Do not patch `CpuMockHost` or `Sync` to unblock a slice.

---

## Appendix — FranticFreddie as a worked example

The in-tree `work/NewFreddie/Stage3/` tree (framework-development copy of
Frantic Freddie) is a complete translation that reproduces original1 end-to-end
(5938 frames) with byte-identical interrupt pairing. Use it to see the
patterns above in real code; do not copy its game facts or checkpoint
history.

| Pattern | File |
|---|---|
| Entry join then ordinary body; `goto` with PC labels | `play_move.cpp` (`entity_clip`, `rand_mix`) |
| Same-PC RAM AtPc as the fence, then `Compare` | `play.cpp` (`play_tick`, `$EC3C`) |
| Sprite X RMW with MSB fixup via AtPc | `play.cpp` (`player_input`, `$EA26`), `play_move.cpp` (`$EECD`) |
| IRQ body: ordinary bookkeeping, ACK, `ReturnIrq` | `play.cpp` (`irq_game`) |
| NMI music driver | `music.cpp` |
| Interval exits with `JoinAtPcBounded`, measured budgets | `game.cpp` (`main_after_music`, `main_loop`), `death.cpp`, `title.cpp` |
| Program lockstep with `WatchMark` | `game.cpp` (`main_loop`) |
| `--no-twin` siblings for data-dependent waits | `level.cpp` (`load_level`), `title.cpp` (fire wait) |
| `All()` only after a displayed frame | `game.cpp` (`main_loop` `picture_ready`), `title.cpp` (`$E58C`) |
| Filled `stage3-lint.json` with reasons | `work/NewFreddie/Stage3/stage3-lint.json` |
