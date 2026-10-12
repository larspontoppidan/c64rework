# c64rework releases

## v0.9.4

- 0.9.4 changes the timing of armed RAM accesses (Stage 3/4 `mem.*` and
Linked values after `Sync::AtPc`). Twin now executes the armed instruction and
Main's read or write lands on that instruction's own bus cycle, as armed I/O
already did; before, it landed at the opcode fetch, 2-6 Φ2 early, and the next
fetch's VIC stall was not walked. The PC must name the load, store or RMW
itself: an implied, immediate or control-flow opcode SoftQuits, and an arm
whose instruction on Twin does not touch the byte Main accesses fails the run
as a check (`--ignore-checks` runs past it).
Games that armed the instruction after a store to make up for the early store
must arm the store itself.
- 0.9.4 changes `--no-twin` timing (Stage 3/4 and the standalone export): an
interrupt handler now takes the interrupted code's time, as on the real CPU.
After `ReturnIrq` the main line waits 7 + `no_twin_cycles` + 8 Φ2 (and
`ReturnNmi`'s `no_twin_cycles`, which Twin runs now ignore, as they do
for `ReturnIrq`) before it continues; `AdvanceCycles` counts
main-line time only. Declare each handler's body length; pacing constants
calibrated from Twin wall time must drop the interrupt share. Existing
no-twin recordings may follow a different course and should be re-recorded.
- Recordings note how they were made: every recorder (REVM, Stage 3/4 and the
standalone export) writes `"recorded_on": {"no_twin", "main_blank",
"details"}` after `source`. It replaces the root `"no-twin"` field, which is
still read in older plays. `details` names the program and framework version
and is never checked. No-twin plays also store `git`, the game repository's
HEAD at record time (`./rework` passes `--record-git`; the standalone export
bakes it in at export).
- Four recording classes are documented: Load, Original, No-twin and No-twin
blank (Stage 4.5 `--main-blank --no-twin` or the standalone export).
- Replaying a play on another kind of system (no-twin differs, or no-twin
blank vs no-twin from BEGIN) warns and continues. The standalone warns on
plays with `mod_events` or of another class, and ignores the `mod_events`;
such plays are not export evidence.
- Stage 3/4: Main's mock CPU now stands at the PC the translation declares
(joins and armed accesses), not at the BEGIN PC. The VIC reads the byte at
the CPU's PC for its DMA-delay "garbage" fetch, so Main now draws the same
bytes there as the original. Not with `--main-blank`.
- New `--state-hashes FILE` on REVM, the Stage 3/4 builds and the standalone
export: one CRC32 per frame of the screen, the SID writes (with their timing
in the frame), VIC, CIA1 and CIA2, about 20 bytes per frame.
`./rework hash-compare A B` shows the first differing frame per channel.
Records are made to prove something and thrown away, never stored or gated.
- The standalone export's parity check now compares state hashes of the
export and Stage 4.5 `--main-blank --no-twin` over whole plays, sound
included, instead of PNGs at sampled frames.
- `--save-screen FRAME FILE` can be given several times (REVM, Stage 3/4 and
the standalone export); before, only the last one was kept.
- Stage 3/4: new CPU-flag compare channel. `CompareMask::Flags(bits, main)`
or `.WithFlags(bits, main)` compares the flags the translation models (D, C,
...) with Twin's P at a fence and fails with `CPU flag FAIL ... C main=0
twin=1`. It needs no `s3lint:allow S3-014`; events and `--report` gain a
`flags` channel.
- Stage 4: `video.charset.page(n)` is the charset surface fixed to character
page `n` (0..7, bank offset `n * $800`) of the VIC bank, with the same timed
writes. Use it when the original writes an absolute glyph page while a raster
IRQ flips `$D018`; `video.charset` still follows the live `$D018`.

## v0.9.3

- REVM performance: faster KB compares with Linked values and cheaper play
hash checks.
- REVM saves the play json even on crashes and hangs
- REVM watch window now scrollable with mouse wheel
- Lots of bug fixes in REVM and tools.
- New `./rework fuzz-play`: replays shifted and jittered variants of the
recorded plays under Twin to find timing defects no single recording hits.
A probe, not a gate.
- Stage 4.5 has a new deliverable, `Stage4/REMASTERING.md`: a guide to the
game and its code for remasterers. The standalone export copies it to its root.
- Stage 1: BEGIN must not have an interrupt about to be taken. `revm-tool snap`
prints the interrupt state, and `--check-begin` fails on a bad BEGIN.
- Stage 4: Linked values can be declared `revm::uninitialized`; reading one
before it is written fails loudly. The standalone export lowers it to
zero-initialised storage.
- KB json new watch level: "stage3" that compares in Stage 3 only. Useful to
allow eliminating values in Stage 4.
- Standalone export fixes, now supports base-class members and Linked values
reached through references.
- Numerous process and policy improvements

Author: Lars Ole Pontoppidan, 2026-10-10

## v0.9.2

- Support for d64 images: REVM can now load d64 disk images with --load-d64
using true 1541-II emulation. --disk-auto-load does LOAD"*",8,1 and RUN 
automatically, --disk-warp speeds up emulation while 1541 CPU is running.
- SID emulation improvements: REVM can now emulate specific SID versions
using --sid-model. Fastmem resampling HQ mode with --resid-hq.
- REVM now stores all screenshots in png format. ppm was used before.
- REVM performance speed-ups
- REVM improved event history and failure context
- Various document and process improvements
- rework.toml must now specify `c64rework_version` instead of `minimum_c64rework_version`

Author: Lars Ole Pontoppidan, 2026-10-03

## v0.9.1

- KB (knowledge base) watches now never follow CPU I/O map, bank must be ram
(the default) or color (to watch values in color ram).
- REVM compares gained vic, cia1 and cia2. Before, only SID registers were
checked at the compare ritual (along with KB watches and the visual frame).
The compares only look at config registers, not full state of the chips.
The All() shorthand was added vic, cia1 and cia2 so it now properly checks
all chip config.
- rework script now requires exact VERSION match.

Author: Lars Ole Pontoppidan, 2026-09-29

## v0.9.0

First public release.

Author: Lars Ole Pontoppidan, 2026-09-25