# Standalone Export

This is a faithful standalone C++/SDL2 reimplementation produced with the
[C64rework](https://github.com/larspontoppidan/c64rework) framework.

Layout:

- `src/game/` — reimplemented game code and packaged assets.
- `src/revm/` — slim REVM runtime and Frodo V4 C64 chip emulation.
- `src/resid/` — vendored reSID sound chip emulation (not a git submodule).

Build requires CMake ≥ 3.16, a C++20 compiler, SDL2, pkg-config, and perl
(wave-table generation). Then:

```bash
./build.sh
bin/game
```

The rework was human-directed and utilized AI-assisted code generation. The
original game and its assets remain subject to the rights of their respective
owners.

## License

The REVM runtime and its Frodo and reSID components are licensed under the GNU
General Public License version 2 or later. The bundled JSON for Modern C++
header is MIT-licensed. See `src/revm/LICENSE` and the notices retained in the
source files. The license of the reimplemented game code and original game
assets must be determined separately.
