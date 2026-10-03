# REVM standalone runtime

REVM is the Reverse Engineering friendly VM used in the C64-Rework project:
[C64rework](https://github.com/larspontoppidan/c64rework). The standalone
version seen here is a stripped down version with only the features needed to
support running the exported version of the reworked C64 game. REVM is based on
the Frodo V4 C64 emulator.

REVM standalone emulates the C64 visuals and infrastructure, except for the
game logic and the C64 CPU, which is provided by the reworked game code. REVM
does have a rudimentary SID emulation built-in, thanks to it's Frodo roots,
but reSID is recommended for better fidelity.

Certain features are preserved from the full REVM live recording and playing
back play sequences. Try running the binary with --help for more info.

## Game-facing facade

`src/gamehost/` is the dedicated, slim API exported game source binds to.
`gamehost::GameHost` is the Main-only host: I-flag control
(`IrqDisable`/`IrqEnable`), `Fail`, `ShouldQuit`, `AssertEntry`, chip-edge
handlers, and the clock/interrupt-completion methods. Register and video
surface types are re-exposed under the `gamehost` namespace. Free
opcode-mimicking helpers are rejected in Stage 4, while member RMW
operations remain on hardware bags.
`gamehost/Log.hpp` re-exposes the same logger under `GAME_LOG`/`GAME_LOG_TIMED`
so exported games keep their terminal trace. `revm::StandaloneRunner` constructs
a `GameHost` and calls the exported `gamehost::InstallGame(GameHost&)` once per
run.

Blank startup walks only to the declared PAL phase, then relocates the machine
counters to the game's BEGIN timestamp. Both standalone and Stage 4 rebase
the SID renderer clock after that untimed relocation. reSID retains its chip
state and PCM buffers; it must not synthesize audio for the omitted loader
interval. The VBlank watchdog remains active during gameplay.
