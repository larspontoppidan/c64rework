# c64rework releases

## v0.9.5

## v0.9.4

Breaking:
- Armed RAM accesses (`Sync::AtPc`) now land on the instruction's own bus
cycle, not at its opcode fetch. Arm the load/store itself; an arm that
doesn't match Twin's access fails the run.
- No-twin: interrupt handlers take the interrupted code's time
(`ReturnIrq`/`ReturnNmi` `no_twin_cycles`).

New:
- Plays record how they were made (`recorded_on`); replaying on another kind
of system warns.
- State-hash records (`--state-hashes`) and `./rework hash-compare`: per-frame
proof that two runs look and sound the same. The export parity check uses them.
- CPU-flag compare at joins (`CompareMask::Flags`).
- Stage 4 `video.charset.page(n)` for a fixed glyph page.
- REVM now supports various fixes for the SID's ADSR delay bug when running
reSID. It can be fully fixed, fixed only for attack, or fixed only 80% for the
attacks, to keep some variability. Game code can configure reSID with 
`host.SetResid(...)` and `host.SetResidAdsr(...)`. See: `docs/revm/SOUND.md`.

Improved:
- reSID speaker output: no crackle, latency ~180 ms → ~50 ms.
- Watchdog counts CPU time, so load or a debugger no longer kills runs; the
standalone has none.
- `--save-screen` can be repeated; disk images hint at `--disk-auto-load`;
`update-docs` refreshes all docs; lint understands armed `Mem16` accesses.
- Docs: four recording classes, fat-slice rules, release workflow.

Fixed:
- Main's mock CPU stands at the translation's PC, so VIC garbage fetches match.
- Stage 3/4 builds ignored `--resid-hq` and `--sid-model`.
- Loading a disk-game snapshot with reSID could stall up to a minute.
- Standalone export: `$DD00` IEC input bits, shared Linked names, leftover
staging folders.

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
