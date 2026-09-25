# C64 REWORK

[C64 Rework](https://github.com/larspontoppidan/c64rework) reverse-engineers
Commodore 64 games and re-implements them as standalone C++/SDL2 applications 
that behave like the originals. The result is a faithful replica suitable for
later remastering with better assets and features. Remastering itself is out 
of scope.

This file is the spirit of the project: what we are optimizing for and why.
It contains no commands, API names, or procedures, so it does not go stale.
Operational detail lives in `AGENTS.md` and `docs/agents/`. Never modify 
this file.

## Two pillars

**REVM, the reverse-engineering-friendly VM.** A cycle-accurate PAL C64
emulator built on Frodo, usable as a playable machine, a library, a *play
sequence* recorder and a deterministic playback machine. Cycle and frame
counts are kept faithfully from boot, snapshots can be frozen at any cycle,
and plays store snapshot hashes so a replay proves the machine is in exactly
the expected state. User input is applied at the next VSYNC, never mid-frame,
which is what lets plays reference frames only and still be exact.

During translation REVM runs two boards from the same BEGIN state and the
same inputs. **Main** runs the developing C++ replica with no 6510. **Twin**
runs the original program. Because both see identical inputs, Twin is a live
oracle rather than a golden file: every recorded play is a test bench, and a
divergence is caught at the instruction where it appears instead of frames
later on the screen.

**The agent workflow.** Reworking is done by agents, and agents drift:
they translate ahead of evidence, paper over timing with retries, and copy
state from the oracle to make a check pass. The workflow is therefore part of
the framework, not an afterthought. A worker advances one bounded slice with
observable acceptance evidence and records a checkpoint; an independent,
adversarial reviewer with fresh context checks the slice against policy and
sends the worker back when it has strayed. Nothing moves forward past an
unresolved review finding.

## What "faithful" means

The replica executes the original's control flow with the original's state,
and produces the original's screen, SID output and timing. It is not a
re-creation that looks the same. Every translated routine corresponds to an
original routine, every original instruction is accounted for, and where the
original's behaviour depends on exact timing (a raster register read, a CIA
timer sample, a hardware-driven random number), the replica performs that
access at the same instruction, in lockstep with Twin, so it gets the same
value for the same reason the original did. There is no legitimate reason to
lift state from Twin into game logic; lockstep makes the honest translation
possible, and copying is always the wrong fix.

Twin exists to verify and to supply the hardware's timing of external events
such as interrupts and frame edges. Main owns all game state and all control
flow. When Twin is absent the replica must run exactly the same game.

## The stage arc

Stages are the order of work, not watertight compartments. Later stages
routinely send knowledge back to earlier ones.

**Stage 1 — capture.** The original PRG runs under full emulation. The user
records representative plays; these are the test benches for everything that
follows. The exact cycle where loading and decrunching are over and the game
proper begins is found and frozen as the BEGIN snapshot. Done when BEGIN is
reproducible and the plays replay with matching hashes.

**Stage 2 — analyse.** The BEGIN image and the code the plays actually
exercise are disassembled and understood. A knowledge base names routines,
variables, ranges and binary formats, with provenance for every entry; it
annotates listings and lets REVM watch named state live. Done when the main
control flow, the state Stage 3 will need first, and the binary formats are
documented well enough to start translating. The knowledge base keeps growing
through Stage 3.

**Stage 3 — replicate.** Main's 6510 is removed and the original routines
are translated by hand into C++ that keeps their structure, names and
execution order. Main and Twin meet at the entry of every translated routine
and at every timing-critical access; at those meetings screen, SID and named
memory are compared. A translation that is not yet complete may be declared
as a bounded interval and worked through, but missing joins, hidden drift and
borrowed state are never acceptable. Done when all recorded plays pass with
Twin, policy is satisfied, and the game is fully playable with Twin removed.

**Stage 4 — reshape.** The faithful Stage 3 translation is turned into a 
well-structured C++ game while Twin remains attached as its verification oracle. 
Game state and assets leave the C64 memory map, machine-address arithmetic 
disappears from game logic, and the code is organized around clear 
responsibilities such as gameplay, input, graphics, sound, and assets. The 
original behavior and timing remain unchanged; synchronization scaffolding 
stays only so every refactor can be checked against the original. Done when 
all plays still pass and the code provides a clean, understandable foundation 
for future remastering. **Stage 4.5** names the point where `--main-blank`
is set in REVM. Now Main will not be loaded with any state from the BEGIN 
image. The code must now bring and install all assets and state, including 
C64 ROMs if needed.

**Standalone export.** After Stage 4, the rework is complete. The code is
now mechanically processed to remove all twin references, synchronization,
and other scaffolding. The standalone equivalents for synchronization are
inserted making the game essentially play as --no-twin. The export produces
a game project that builds and plays on its own. The rework concludes.

## Non-goals

- Remastering, new features, or improving on the original.
- A generic recompiler. Translation is hand work informed by understanding;
  mechanical instruction-for-instruction output is not the goal.
- Passing checks. Green plays are evidence of fidelity, never a substitute
  for it; weakening a compare or a rule to obtain them defeats the project.


## File layout

This file is at the root of a game folder for a game being reworked.

- `rework` — Small trampoline into the configured framework's `scripts/rework`.
- `rework.toml` — Specifies the game, its paths, and minimum framework version.
- `$REVM_ROOT/VERSION` — Authoritative C64 Rework framework version.
- `$REVM_ROOT/src/revm/` — REVM, its tools and the Stage 3 host headers.
- `$REVM_ROOT/src/resid/` — external reSID submodule; never modify.
- `$REVM_ROOT/reference/c64.kb.json` — support knowledge base for standard chip registers and ROM.
- `docs/agents/`, `AGENTS.md` — stage manuals and agent instructions, don't modify
- `docs/revm/` — useful information about revm and revm-tool
- `Stage2` .. `Stage4` — source code and other files generated at given stages
- `NOTES.md` — compact overall state and current stage
- `Stage1/NOTES.md` .. `Stage4/NOTES.md` — per-stage working checkpoints
