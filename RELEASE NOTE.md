# c64rework releases

## v0.9.4

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