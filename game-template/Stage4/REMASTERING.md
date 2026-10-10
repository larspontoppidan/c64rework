# GameName — remastering guide

<!-- Written at the end of Stage 4 / 4.5 for the people who will remaster the
standalone export. Explain the game and the code, not the rework process.
The export copies this file to its root. -->

## The game

<!-- What the game is and its flow: title, play, levels, game over. -->

## Architecture

<!-- Module map: each file/type and its responsibility (rules, presentation,
sound, input, assets), who owns which state, and the top-level control flow. -->

## Timing

<!-- What runs per frame and in which interrupt; pacing constants; what must
stay frame-exact for the game to feel the same. -->

## Assets

<!-- Where assets live, how they are generated, their formats. -->

## Hardware surfaces

<!-- The IoMap, video-surface and SID calls a remaster would replace, and the
seams to replace them at. -->

## Gotchas

<!-- Quirks of the original the game depends on, in game terms. -->

## Changing the game safely

<!-- What keeps Stage 4 Twin parity, what only the export checks, declared
no-twin divergences. -->

## Carried debt

<!-- What is still shaped by the C64 and where. -->
