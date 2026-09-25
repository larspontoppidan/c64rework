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
- machine-address arithmetic leaking into rules, graphics, or sound code.

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
Both types of recordings must exist as Stage 3 concluded. And in Stage 4 both
types must continue to play successfully and exactly like in Stage 3. This
ALSO applies to Stage 4.5 with `--main-blank`.

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

The rule fixtures are part of the tool and must remain green:

```bash
./rework selftest
```

## Done

Stage 4 as a whole, including Stage 4.5, is complete when:

- Every original play passes with `--main-blank`
- Every no-twin play passes with `--main-blank --no-twin`
- No lint findings except for valid and documented exceptions
- Review based on STAGE4-REVIEW.md accepts the quality of the code.
