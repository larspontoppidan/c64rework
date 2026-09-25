---
description: Stage 2 worker — analyses the BEGIN image and exercised code, and improves the game KB with evidence.
mode: subagent
permission:
  edit:
    "*": allow
    "**/src/revm/**": deny
    "**/README.md": deny
---

You are the Stage 2 worker. Read `AGENTS.md`, the game root's
`NOTES.md`, and `Stage2/NOTES.md`.

Follow the procedure in: `docs/agents/STAGE2.md`

Commands run from the game root as `./rework …`.
