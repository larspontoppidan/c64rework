# Stage 4 — Detach state without dropping Twin

Stage 4 is a migration of the accepted Stage 3 replica, not a new runtime.
Main is still a `CpuMockHost` board, Twin still runs the original program, and
the same `Sync` fences still provide timing. What changes is ownership: game
state and assets move out of the C64 memory image and into ordinary C++.

To begin with, Stage 4 runs like Stage 3 and uses memory assets and register
values loaded from the BEGIN snapshot. At some point the Stage must switch
to **Stage 4.5**. Now `--main-blank` is passed revm and this disables any
loading of BEGIN state into Main. The purpose is to require the code to bring
all assets and state. Even the built-in C64 charset must be installed if it's 
required.

The exact time of introducing `--main-blank` and thus starting Stage 4.5 is
up to the agents. It makes sense to do when work on sprite resources is
started, but it could be done later or earlier.

Authority: the user → this file → `REVM_ROOT/src/revm/src/cpumock/Linked.hpp`,
`Sync.hpp`, `Mem.hpp`, and `CpuMockHost.hpp`. Keep the working frontier in
`Stage4/NOTES.md` at the game root.

Independent review procedure: [`STAGE4-REVIEW.md`](STAGE4-REVIEW.md).

Commands run from the **game root** via `./rework`.

## The model

Start Stage 4 from the passing Stage 3 source. Build it as another normal
cpu-mock/Twin plugin:

```bash
./rework start-stage4
./rework build stage4
```

`start-stage4` requires a pristine `Stage4/`, copies the accepted source, and
preserves the Stage 4 `NOTES.md` and `REVIEW.md` templates.

Keep:

- `CpuMockHost`, `IoMap`, `Sync::AtPc`, joins, handlers, and compare masks;
- the original PCs on all existing timing-critical operations;
- the same BEGIN snapshot, KB, and required plays.

Remove progressively:

- game state stored in `Mem`, `Mem16`, or `MemTable`;
- data or assets borrowed from the BEGIN memory image;
- generic `mem.*` access and `io.*.raw[]` in game code;
- machine-address arithmetic leaking into rules, graphics, or sound code;
- packed or encoded data. Compressed streams, run lists and records in the
  game's character codes become explicit assets in decoded, true form: text
  as legible strings, layouts as named records. Keep the decoder only when its
  output depends on game state. A named byte array a reader cannot read is
  still a blob;
- chip mirrors. By the end of Stage 4 there is no memory space and there are
  no mirrors, only chips: a store the original made to a mirrored address
  (a SID address from `$D420`, a VIC address from `$D040`) is a write to
  the chip register it reaches, through its named `IoMap` register. `raw[]`
  fails on an index past the register file, and a mirror is never a reason
  for `raw[]` or for an `s4lint:allow S4-003`. Twin's armed-write check
  already treats SID mirror addresses as the same register;
- C64 memory-map mechanisms once nothing depends on them, such as the 6510
  port at `$0000/$0001`. Its banking only decides where Main's own accesses
  land; when game state and assets no longer live in RAM, model nothing of it.

`Stage4/` is what the standalone export copies, file for file. Keep it to the
game's sources, headers and asset headers. Probe recordings, stress variants,
generator scripts and other tools belong elsewhere in the game root (for
example `tools/` or `plays/`), not in `Stage4/`.

Use named chip registers from `IoMap`. If a real hardware-facing RAM surface
is missing, add a small named bag for that surface; do not turn the game-state
memory map back into an API.

### Stage 4.5:

`--main-blank` never restores BEGIN into Main memory or chips wholesale. Twin
still restores BEGIN as the oracle. Declare Main's small CPU-mock origin
alongside the entry handler in `InstallGame`; take the PC, cycle, and frame
from the game's frozen Stage 1 BEGIN checkpoint. Register boot writes and
asset `install()` calls bring programmable chip and memory state. Live chip
phase that no register sequence can reconstruct (CIA timer counters, delay
pipeline, idle flags) is declared on the same start contract and applied by
the runtime at blank start:

```cpp
void InstallGame(CpuMockHost &host) {
	host.InstallMainStart({
		.entry_pc = 0x1234,
		.cycle = 12345678,
		.frame = 628,
		.cpu_port_ddr = 0xFF,
		.cpu_port = 0x35,
		.interrupts_disabled = true,
		// Dump these from begin/snap.bin; do not guess. Omit when every
		// required timer is fully programmed by boot writes before first use.
		.cia1 = CiaLivePhase{
			.ta = {.counter = 0x1234, .latch = uint16_t((0x40 << 8) | 0x25), .count_delay = 0xFF},
			.tb = {.idle = true},
		},
	});
	host.SetEntryHandler([&host] {
		// Construct and run the translated game.
	});
}
```

The cycle and frame fields establish the play/Twin timeline and the PC checks
the Twin entry. They do not restore the BEGIN memory image. The processor-port
and interrupt fields are the mock-CPU state retained because they affect
Main's hardware register access and interrupt delivery. Optional `cia1` /
`cia2` live-phase fields seed in-flight timers after the PAL phase walk so
blank Main matches a BEGIN-restored no-Twin Main; they are not a wholesale
CIA snapshot restore. REVM rejects `--main-blank` if the declaration is
absent or disagrees with Twin or the play origin. Main otherwise starts with
canonical fresh chips and zero RAM/color; the translated startup must
configure every required chip register and install every required asset.

`--no-twin` play recordings (`play/no-twinN.json`) differ from Twin/original
ones (`play/originalN.json`). (see `AGENTS.md` and `STAGE3-MANUAL.md` §11). 
Both types of recordings must exist as Stage 3 concluded. In Stage 4 the
original plays must continue to play exactly like in Stage 3. This ALSO
applies to Stage 4.5 with `--main-blank`. No-twin plays must keep running
without failures and feel the same, but need not replay frame-identically:
Stage 4 may change no-twin timing (for example a shorter boot). Before
standalone export, record no-twin plays without `mod_events` on Stage 4.5
(`--main-blank --no-twin`); they are the export's evidence.

Note that stage 4 will fail recordings with `mod_events` if the modified 
addresses are not registered Linked values.

Graphics that the game consumes through the VIC address space belong behind
the `video` API. Lift their initial bytes explicitly at setup time:

```cpp
video.charset.install(assets::charset);

video.sprites.slot(pointer).install(assets::sprite1);
video.sprites.slot(pointer2).install(assets::sprite2);
```

Here `pointer` is the sprite pointer value used by the game, and each sprite
asset is one complete VIC sprite surface. `charset.install()` seeds the active
character page. `install()` copies the bytes into Main's mutable surface without
advancing emulated time. The game may modify the charset and sprite data for
animation or effects after initial installation.

Live changes may use the surface's timed write API with original `Sync::AtPc`:

```cpp
Sync::AtPc(host_, 0x4567);
video.charset.glyph(ch).row(row).write(value);
```

Do not call `install()` more than necessary.

## Linked local state

`LinkedByte`, `LinkedWord`, and `LinkedArray` are local C++ storage carrying an
original address. The address exists only for Twin timing, KB comparison,
diagnostics, and recorded `mod_events`; it never initializes the value.

```cpp
struct GameState {
	LinkedByte<0x003F> input_armed{0};
	LinkedWord<0x0040> score{0};
	LinkedArray<0x0050, 4> counters{};

	void Register(CpuMockHost &host) {
		input_armed.Register(host.Links(), "input_armed");
		score.Register(host.Links(), "score");
		counters.Register(host.Links(), "counters");
	}
};
```

## Verification-only code

Code that exists only to check Main against Twin (a declared-PC table scan, a
flag cross-check, a compare helper) must sit under `HasTwin()`, or start with
`if (host_.NoTwin()) return;`. The export then removes it with its callers
(STANDALONE-EXPORT.md, step 7). Unmarked checking code survives into the
export as dead weight that a remaster team has to read and understand.

## Comments

Stage 4 should read like a game. Stage 3 stays the annotated translation and
keeps the instruction-level trail; the KB, `Stage2/FORMATS.md` and
`docs/LANDMINES.md` keep the understanding; git history and `NOTES.md` keep
how it was found. In Stage 4:

- give each function one provenance line, e.g.
  `// Original: $3C14 unpack_stream (Stage3/title.cpp)`;
- explain game behaviour and why the code has its shape, including quirks the
  game depends on;
- do not echo original instructions (`// $3C1E INY`) or keep investigation
  narratives; move anything still needed to LANDMINES.md or NOTES.md.

Lint rule S4-008 warns about comments that echo an original instruction. PCs in
`Sync` calls are code, not comments, and stay.

A KB watch exists to catch drift in game variables. If Stage 4 finds itself
keeping a RAM copy of data the game no longer reads from RAM only because a
watch covers it, the watch is wrong, not the code: switch it off in the KB
with a note saying why (see STAGE2.md), and keep the screen and SID compares
as the check for assets and display memory.

Registration is its own method. The standalone exporter deletes every
`Register` method, so a function that calls `host.Links()` must be named
`Register…` and do nothing but register: no other initialisation in it, no
registration in a constructor, and no host pointer kept by a state type.

Access is deliberately explicit and symmetric with the I/O bags:

```cpp
const uint8_t old = state.input_armed.read();
state.input_armed.write(1);
state.input_armed.inc();
state.counters[x].dec();

Sync::AtPc(host, 0xF574);
state.input_armed.write(1);
```

The member RMW operations above remain valid because they preserve a bag's
read/modify/write and Twin semantics. Free opcode-mimicking helpers such as
`asl(value, carry)` or `inc(value)` are Stage 3 translation residue and are a
Stage 4 lint error. Rewrite those as explicit C++ value and carry operations.

Memory values that have a preceding `Sync::AtPc` must be converted to a Linked
type for the sync scaffolding to keep working. Values that are checked
by kb also need to be converted to Linked for the kb check to keep working.

### Initial values: declare, or say `uninitialized`

A Linked value either has a genuine starting value or none:

```cpp
LinkedByte<0x0090> mode{0x40};                    // set by boot / a constant
LinkedArray<0x0400, 256> roster{revm::uninitialized};  // game fills it first
```

Give a declared initial value only when the value is real: boot code cleared
or set it, it is a constant table, or the original program reads it before it
has written it and the value it sees is part of the game's behaviour.

Use `revm::uninitialized` (also `revm::cpumock::uninitialized`; accepted by
`LinkedByte`, `LinkedWord` and `LinkedArray`) when the game itself initialises
the state before anything reads it: a level builder that clears a table, a
work buffer written before use, a slot table the game fills when it spawns.

Do **not** seed such state from the BEGIN image. The KB compare starts at the
first join, long before the game writes its tables, so a mismatch against
Twin's RAM tempts you to copy whatever the packer or BASIC left there
(a roster page full of BASIC screen text, say). That is cargo: it encodes
leftovers the game never depends on, it hides a missing or late write, and it
blocks an honest `--main-blank` because the seed data has to come from
somewhere. `uninitialized` states the truth instead:

- every byte is tracked until the game has written it (per byte for arrays,
  per object for scalars; once all bytes are written tracking switches off
  and the value costs nothing);
- `read()`, an RMW (`inc`, `asl`, ...) or a recorded `mod_event` on an unwritten
  byte stops the run (`SoftQuit`, works in Twin and `--no-twin`) naming the
  object, index, address, frame and the last Sync fence: that is a real bug
  in the translation, or state that does have a starting value;
- the KB compare takes Twin's value for unwritten bytes (they are skipped),
  and Main's own value for written ones, so Twin still verifies everything
  the game has produced;
- `--events` emits `linked_init` (name, addr, frame) when an object becomes
  fully written, and the end of the run logs `linked: uninitialized NAME ...
  never fully written` for tracked objects that never completed (dead state,
  or state the game only partly fills: consider a declared value or deleting
  it). Partly-written objects are fine as long as unwritten bytes are never read.

A write that only some paths perform is not an excuse to seed: if the game
reads a byte it has not written on some path, that read *is* a starting-value
dependency and the object needs a declared value (from a provenance you can
name: the original's boot code, not the BEGIN dump).

Standalone export lowers an `uninitialized` value to plain zero-initialised
storage (`uint8_t x{}`, `std::array<uint8_t, N> x{}`) with no tracking.

A `LinkedWord` operation has one logical fence at its base address. It does
not represent two 6510 byte accesses. When the original timing depends on the
individual low/high accesses, represent them as linked bytes or array elements.

## Workflow

Stage 4 start: Make a direct copy of the Stage3 source tree into Stage4.

Alternate between work slice and review:

### Work slice

1. Reproduce the Stage 3 plays, both original and no-twin types, they must
pass.  When in Stage 4.5, all plays must pass with `--main-blank`, which is a
gate of Stage 4.5.

   ```bash
   ./rework build stage4
   ./rework test-play stage4 original1
   ./rework test-play stage4 no-twin1 --no-twin
   ./rework test-play stage4 original1 --main-blank
   ./rework test-play stage4 no-twin1 --main-blank --no-twin
   ```

2. Run the lint check: `./rework lint stage4` and note number of findings.
There will be a lot in the beginning of Stage 4.
3. Work on the next slice.
4. Run play tests and lint checks when relevant to check work.
5. Update `Stage4/NOTES.md` with a short summary of the work slice.
Include a summary of latest play tests and lint check results. It must include
the lint counts at the start and the end of the slice.

### Review

A fresh review agent should follow the instructions in: `docs/agents/STAGE4-REVIEW.md`.

## About the lint check

Run with:

```bash
./rework lint stage4
```

It reports `Mem`/`Mem16`/`MemTable`, generic `mem.` access, `io.*.raw[]`,
implicit or overloaded value access, `goto`, and raw machine-address literals.
`Sync` PCs and `Linked*<address>` provenance are allowed. The initial copied
tree is expected to fail; fix findings in coherent slices and keep the exact
counts in `Stage4/NOTES.md`. Use a nearby
`// s4lint:allow S4-xxx -- reason` only for a real translation exception.

Run `./rework stage4-diag` alongside lint. It is the standalone exporter's
preflight over `Stage4/` and stops at the first construct the export cannot
lower; it is cheaper to fix those during Stage 4 than to discover them at
export time. Its C++ parser does not accept an attribute between `enum class`
and the name (`enum class [[nodiscard]] Flow`); write `enum class Flow`.

The rule fixtures are part of the tool and must remain green:

```bash
./rework selftest
```

## REMASTERING.md

The last Stage 4 deliverable is `Stage4/REMASTERING.md`, a guide for the people
who will remaster the standalone export. `start-stage4` keeps the template's
headlines (game, architecture, timing, assets, hardware surfaces, gotchas,
changing the game safely, carried debt); fill them in once the code has
settled. Explain the game and the code as they are, in game terms: what each
module owns, what must stay frame-exact, which quirks of the original the game
depends on. Leave the rework history to NOTES.md and the instruction-level
detail to Stage 3, the KB and LANDMINES.md. The export copies the file to its
root.

## Done

Stage 4 as a whole, including Stage 4.5, is complete when:

- Every original play passes with `--main-blank`
- Every no-twin play passes with `--main-blank --no-twin`
- At least one no-twin play without `mod_events` is recorded on Stage 4.5
- No lint findings except for valid and documented exceptions
- `Stage4/REMASTERING.md` describes the finished code
- Review based on STAGE4-REVIEW.md accepts the quality of the code.
