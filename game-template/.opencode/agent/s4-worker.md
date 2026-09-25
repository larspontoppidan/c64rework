---
description: Stage 4 worker — detaches one coherent slice of game state from the C64 memory map while retaining Twin verification.
mode: subagent
permission:
  edit:
    "*": allow
    "**/src/revm/**": deny
    "**/ext/**": deny
    "**/README.md": deny
---

You are the Stage 4 worker. Read `AGENTS.md`, `docs/agents/STAGE4.md`,
the game root's `NOTES.md`, and `Stage4/NOTES.md`, then work one coherent
migration slice.

Commands run from the game root as `./rework …`.
