# C64 Rework — agent instructions

## Where to read

Find REVM_ROOT, the framework root, in `./rework.toml` under `revm_root`.
The manifest also states the minimum compatible C64 Rework version.

1. The user's current instruction.
2. `C64REWORK.md` — architecture and intent. Never modify it.
3. The game root's `NOTES.md` — the compact overall state and current stage.
4. `Stage<n>/NOTES.md` — the facts and current checkpoint for that stage;
   update this during game work.
5. The stage guides are in: `docs/agents/STAGE2.md`,
   `docs/agents/STAGE3.md`, `docs/agents/STAGE4.md`. Stage 3's detailed
   technical reference is `docs/agents/STAGE3-MANUAL.md`. After Stage 4, 
   standalone export is governed by `docs/agents/STANDALONE-EXPORT.md`.
6. Review processes: `docs/agents/STAGE3-REVIEW.md` for Stage 3 and
`docs/agents/STAGE4-REVIEW.md` for Stage 4.
7. Source, `--help`, and headers under `REVM_ROOT/src/revm/src/cpumock/`
   are the authority for how to call an API. Never modify files in 
   REVM_ROOT tree.

All game-side commands run from the **game root** through `./rework`
(`./rework help`). 
`obsolete/` and other stale markdown are clues only; verify before reusing
anything from them.

## Game root

A game is a directory that contains `rework.toml` and the `rework` trampoline
(copied from `game-template/rework`). The trampoline checks compatibility and
invokes `scripts/rework` in the configured framework. The game directory may
live **anywhere**. It should generally **not** be inside this framework
repository. An in-tree example under `work/` exists only while the framework
itself is being developed.

`rework.toml` names the game, the original PRG, the game KB, the framework
root (`revm_root`, absolute or relative to the game root), and the minimum
framework version. C64 ROM dumps default to `<revm_root>/roms/`; set `rom_dir`
to override. Run `./rework doctor` if a path or version looks wrong.

Typical layout:

```
<game-root>/                 # not inside c64rework
  rework                     # launcher (framework trampoline)
  rework.toml
  <game>.prg
  Stage1/NOTES.md            # Progress notes for stage 1
  begin/snap.bin
  plays/*.json
  Stage2/NOTES.md            # Progress notes for stage 2
  Stage2/<game>.kb.json
  Stage2/FORMATS.md
  kb-analysis/               # generated
  Stage3/                    # source code, NOTES.md and REVIEW.md
  Stage4/                    # source code, NOTES.md and REVIEW.md
  NOTES.md                   # Top level progress notes, identifying current stage
```

## Preparation

Before starting work in a game folder, in any stage, go through the checks in:
`docs/agents/PREPARATION.md`.

## Stages

| Stage | Goal | Manual |
|---|---|---|
| 1 | Run the PRG under full emulation, record plays, freeze the BEGIN snapshot and entry PC | `docs/agents/STAGE1.md` |
| 2 | Analyse the BEGIN image and exercised code; build the KB and binary-format notes | `docs/agents/STAGE2.md` |
| 3 | Replace Main's 6510 with a hand-translated C++ plugin; Twin is the timing oracle | `docs/agents/STAGE3.md` |
| 4 | Detach game state from the C64 memory map while retaining Stage 3 Sync/Twin verification. Stage 4.5 adds `--main-blank` | `docs/agents/STAGE4.md` |

After Stage 4, standalone export mechanically lowers the accepted tree into a
plain application. It is not another game-work stage. If issues are found in
the exported product they must be fixed in Stage 4 code and backported to 
Stage 3 for consistency.

Stages 2 and 3 overlap: translation discoveries go back into the KB with
provenance. Never add placeholder KB entries.
Stage 4 should play exactly like Stage 3. Stage 4.5 adds `--main-blank` and 
must still play recordings identically.

Stage 3, 4 and 4.5 all support two modes of operation: original and `no-twin`. 
The original mode perfectly matches the original gameplay, while no-twin 
diverges slightly as there is no timing oracle to align with. The game play
should still feel the same to the human player, but the exact timing and things
like RNG may be different. The no-twin behaviour must be identical between 
stage 3, 4, 4.5 and standalone export, however.

For this reason two types of recordings will be made: original and no-twin,
reflected in the name: `plays/originalN.json` and `plays/no-twinN.json`. There
is a field in the json storing the type: `"no-twin": <bool>`. Replaying a
play on the other kind of system warns that playback will not be faithful.

Recorded plays may contain `mod_events`: deliberate increments or decrements of
KB variables used to reach otherwise costly states (for example, skipping 
through levels). 

The Standalone Export game will support only no-twin plays without `mod_events`,
thus it is important to have recorded such plays in the rework process.

## Repository rules

- Never edit any file in REVM_ROOT during game work (analysis, translation, a failing
  frontier). If a helper looks wrong, stop and report a reproducible case
  (`docs/agents/STAGE3-MANUAL.md` §15).
- Game artifacts live in the game root. Do not scatter them into the
  framework tree.
- `NOTES.md` never grows. `Stage<n>/NOTES.md` holds facts; the checkpoint is 
  overwritten with one working frontier, never appended.
- Never weaken a compare, mask, golden, lint rule, or policy to make a result
  pass.
- Interactive recording and play-feel checks need the user. Agents run
  deterministic headless verification only (`./rework test-play`).
- Standalone export never edits the accepted `Stage4/` source. General
  lowering belongs in the exporter/runtime; semantic discoveries go back
  through Stage 4 review.
- Stage 4 asset lifting uses the named `video` surfaces. `install()` seeds a
  mutable charset or sprite surface; live changes must use the corresponding
  timed write API so the Twin comparison retains original timing.

## Working loop (all game-work stages)

1. Reproduce the last checkpoint before changing anything.
2. Pick a naturally sized slice, not too small, not too large
   (Stage 3: `docs/agents/STAGE3.md` “Work one frontier”).
3. Investigate the original before implementing; keep provenance.
4. Implement only that slice in the game root and the KB.
5. Verify: narrow test, then the play gates, then lint.
6. Update the owning `Stage<n>/NOTES.md`; report exact commands, frontier, first
   failure, ignores, and exceptions. Do not commit unless asked.
