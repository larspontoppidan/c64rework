# Stage 3 — Make Main the game

Stage 3 replaces Main's 6510 with a hand-translated C++ plugin. Twin continues
to run the original game from the same BEGIN snapshot and receives the same
inputs. It supplies timing and checks the result while Main progressively takes
ownership of the complete game.

This file is the working procedure. Exact synchronization contracts, APIs,
translation patterns, and failure diagnosis are in `STAGE3-MANUAL.md`. 
Commands run from the **game root** via `./rework` (`./rework help`). 
Source, headers, and `--help` remain the authority for API details.

The review procedure is `STAGE3-REVIEW.md`.

## The model

Main is the emerging replica. It must perform the original control flow,
calculations, state changes, and hardware programming itself. Twin is a live
oracle with two jobs:

- tell synchronization fences when the original reached a particular
  instruction;
- compare Main's state, picture, and sound with the original.

Twin never supplies gameplay values and never performs missing work for Main.
A green comparison is meaningful only when Main earned the result through an
honest translation. The replica must also remain capable of running with
`--no-twin`.

Hand translation means preserving the original program's behaviour and order,
not rewriting it into attractive modern C++. Keep routine boundaries,
branches, flags, loop counts, state changes, and hardware accesses explicit.
Stage 4 will reshape the result after fidelity has been established.

## Workflow

Stage 3 start: template code files should be in the `Stage3/` folder. The
starter deliberately compiles and lints but exits with “Stage 3 not configured.”
Its `$1000`/`$0002` addresses are illustrative only: replace them with the
frozen BEGIN PC, KB-backed state, and disassembled instructions before setting
`kConfigured` to true.

Alternate between work slice and review:

### Work one frontier

Material for the worker to read includes:

- the current checkpoint in `Stage3/NOTES.md`;
- the relevant original disassembly, KB entries, and Stage 2 findings;
- the parts of [`STAGE3-MANUAL.md`](STAGE3-MANUAL.md) needed by the current
   path. If very early in the Stage 3 process, read the entire manual to start
   things right.
- review findings `Stage3/REVIEW.md`

Steps:

1. Build the plugin and reproduce the checkpoint with its recorded commands. The
same last pass and first failure should appear. If they do not, investigate the
baseline before translating further.

   ```bash
   ./rework build stage3
   ./rework test-play stage3 <play> --max-frames <N>
   ```

2. Work the frontier. Stage 3 advances through small slices. A slice is the
next untranslated or incorrect piece of original execution responsible for
the earliest failure. It may be one routine, one branch, an interrupt path,
or one timing-critical access. Small is good when the acceptance evidence is clear.

3. Translating a slice. Use one function for each original routine and begin
it with the corresponding `JoinAtPc`. Keep the listing's execution order visible.
Every relevant original instruction must have an equivalent; leave a loud `SoftQuit`
at a genuine frontier instead of silently skipping work or installing a no-op stub.

Use named memory and I/O bags. Give functions and variables names that express
their understood role. Machine-like control flow is acceptable; unexplained
state and guessed semantics are not.

For synchronization:

- routine entries meet at `JoinAtPc` and compare the state the routine owns;
- chip-register and interrupt-shared accesses that depend on exact timing use
  the appropriate `AtPc` operation at the original opcode PC;
- interrupt bodies are translated C++ handlers driven by Main's own chips;
- when Main completes a translated body instantly while Twin must execute it,
  use an honest bounded interval and compare at its real exit;
- use `CompareMask::None()` only for justified interior fences, never to hide
  divergence;
- never copy RAM, registers, chip state, or computed outcomes from Twin.

4. Verify the advance

Build and run the narrow acceptance pair first:

- the previous last-pass frame must remain green;
- the previous first-fail frame must now pass or expose a later, intelligible
  frontier.

Run a useful bounded `--no-twin` smoke. It proves that Main can operate without
oracle state; it need not reproduce the recording's later trajectory. Then run
the required play table and Stage 3 lint. Do not use `--ignore-checks`, a weaker
mask, a larger unexplained interval, or a later comparison point to manufacture
a pass.

   ```bash
   ./rework build stage3
   ./rework test-play stage3 <play> --max-frames <last_pass>
   ./rework test-play stage3 <play> --max-frames <first_fail>
   ./rework test-play stage3 <play> --no-twin --max-frames <N>
   ./rework lint stage3
   ```

`./rework test-play` is headless. Extra flags after `TARGET`/`PLAY` go to REVM
(`--max-frames`, `--no-twin`, `--events - --report FILE`, `--dump-fail` is
already defaulted). Windowed replay for a human: `./rework show-play stage3
<play>`.

5. Record the checkpoint

Overwrite `Stage3/NOTES.md`. It is current state, not a diary. Record:

- exact commands and verification tallies;
- lint errors, warnings, and justified exceptions;
- the last stable joined routine or PC;
- one earliest working frontier with play, frame, cycle, and exact failure;
- every required play's last pass and first failure;
- useful `--no-twin` smoke status;
- temporary ignored checks, synchronization exceptions, and open interval debt.

Keep history out of the checkpoint. Detailed shape and examples are in the
reference manual.

### Review

Hand the slice to a fresh reviewer using `docs/agents/STAGE3-REVIEW.md`.
The worker owns the mechanical build, lint, and play evidence. The reviewer
studies the original and the translation to decide whether Main honestly earned
the new frontier.

Stage 3 slices may be very short. Review them at their natural scale and then
continue from the next earliest cause. Fix fidelity or synchronization findings
before translating beyond them.

## Non-negotiable rules

- Main owns all game state and control flow; Twin supplies timing and evidence.
- Do not skip original instructions or let Twin execute untranslated work on
  Main's behalf.
- Do not borrow Twin values for gameplay, even temporarily.
- Do not weaken comparisons, lint, policy, or recorded gates to make progress.
- Do not use a join as a generic wait or disguise an unbounded interval.
- Preserve a viable `--no-twin` path.
- Keep translation discoveries aligned with the Stage 2 KB.
- Do not edit `REVM_ROOT/src/revm/` during a game translation.

## Done

Stage 3 is complete when every required recorded play passes against Twin, the
translation and synchronization survive independent review, lint has no
unresolved findings, and Main is fully playable with Twin removed. 

The user performs the final interactive play-feel check and records the no-Twin
plays needed by Stage 4 `plays/no-twinN.json`.
