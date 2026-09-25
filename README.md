# About C64 Rework

C64-Rework is a framework for reverse-engineering Commodore 64 games, producing
C++ code that replicates the original behaviour faithfully. The framework
provides assets and procedures intended for AI agents to follow with limited
need for human supervision.

The output is a standalone C++ application that behaves exactly like the original,
replacing the machine code with legible and structured code. While the 6510 CPU
is fully eliminated, the other chips of the C64 are provided in emulated form
using code from the Frodo v4 emulator. The binary targets SDL2 standalone.

The repository provides the following:

- Comprehensive documentation of four rework stages. Some stages require brief
human interaction. Most are able to be run by agents automonously.
- `revm` The Reverse Engineering friendly Virtual Machine. This is the technical
heart of the rework process. It's an adaptation of the Frodo v4 emulator adding
specialized features to support reverse engineering.
- `game-template`. The starting folder structure to use for reworking a game.
- `rework`. A script that provides easy access to frequently used commands in the
process.
- Lint tooling scripts for stage 3 and stage 4.
- `standalone-export`. A minimal `revm` version and support structure for 
exportoring the final product as a standalone C++/SDL2 application.

The rework process is intended to be run on Linux platforms. MacOS may be supported
with minor adjustments.

## Getting started

Locate a `prg` loader file of the game to rework. Multiload games in files like
`d64` or `crt` are not supported.

Locate **C64 ROMs** preferably matching the filenames and SHA-256 checksums seen
below.

### Steps

1. Copy the `game-template` folder to an external folder and name it reflecting
the game to be reworked. Remember to include hidden files, eg.: 
`cp -a /path/to/c64rework/game-template/. /path/to/MyGame/`
2. Place the loader `.prg` file in the folder.
3. Rename `gamename.kb.json` in the Stage2 folder to reflect the game name.
4. Set the paths and minimum framework version in `rework.toml`.
5. Place C64 ROM dumps in the framework `roms/` directory (gitignored), see below.
6. Run `./rework doctor` to validate the game, framework, and version.
7. Recommended, initialize a git repository in the root.
8. Open an agent harness in the folder and ask it to lead the rework process.

### Prompt inspiration

Stage 1: Read the project documentation and start up rework of GAME. Tell me what
to do.

Stage 2: Read the project documentation and work on Stage 2.

Stage 3/4: Read the project documentation and orchestrate alternating worker and
review agents for stage 3/4 of the project.

## Overview of the stages:

### Stage 1: Identifying BEGIN and recording play sequences

The original game prg is run under full emulation while the first **play** sequence 
recording is done. As soon as the game loading has finished, and the game has 
started, **F9** is pushed to record a timestamp. This will be used as a reference 
point to determine the exact time and code location for the actual starting point 
of the game, named BEGIN. Now several game play recordings are made starting from
the BEGIN snapshot, with the purpose of capturing representative game play.

### Stage 2: Building the Knowledge Base 

The C64 memory map at the BEGIN snapshot is analysed supported by the
recorded play sequences. Findings are gathered in a knowledge-base (kb) file 
documenting memory locations. The kb enables **revm-tool** to generate 
disassembly listings with symbol names and more.

### Stage 3: Building the C++ CPU replica

With a well annotated disassembly, this stage is about replicating the C64
CPU behaviour with hand-translated C++ code. The replica CPU is run on the
**Main** board along with emulated C64 chips. To keep the process honest,
a **Twin** board, with its own CPU, is run alongside the Main board to serve as 
a live oracle. The Twin also provides the exact cycle timing to make it possible
to keep perfect lockstep between Main and Twin. Stage 3 concludes when all 
recorded plays can be played back with Main and Twin in perfect unison.

### Stage 4: Lifting the replica from the memory map

The Stage 3 game code is duplicated and refactored to detach the
game state from the C64 memory map and to not rely on assets from the BEGIN
snapshot. The code is refactored and polished to look more like a sensible
C++/SDL2 game than converted 6510 machine code. The Twin oracle is still 
available throughout the process and must be kept in perfect lockstep such 
that correctness of the refactored code can be checked.

### Standalone Export

This is not regarded a seperate stage as no work should have to be done here.
The export procedure mechanically processes the Stage 4 code to remove 
synchronization statements and other scaffolding. The simplified code is 
plugged into a standalone version of REVM resulting in the promised standalone
version of the C64 game.

## Read more

Read more about the process in
[game-template/C64REWORK.md](game-template/C64REWORK.md). The `game-template`
is where the documentation lives.

The framework release is recorded in `VERSION`. A game's small `./rework`
trampoline checks its required minimum, then runs the implementation in
`scripts/rework` from the configured framework.


## C64 ROM images

C64 ROM images are not bundled. They must be provided in the `roms/` folder or
in the `rom_dir` specified in `rework.toml`.

The following images were used during development of REVM:

```text
roms/basic-901226-01.bin (8192 bytes)
roms/kernal-901227-03.bin (8192 bytes)
roms/chargen-901225-01.bin (4096 bytes)
roms/dos1541ii-251968-03.bin (16384 bytes)
```

SHA-256 values:

```json
{
  "basic": "89878cea0a268734696de11c4bae593eaaa506465d2029d619c0e0cbccdfa62d",
  "kernal": "83c60d47047d7beab8e5b7bf6f67f80daa088b7a6a27de0d7e016f6484042721",
  "chargen": "fd0d53b8480e86163ac98998976c72cc58d5dd8eb824ed7b829774e74213b420",
  "dos1541ii": "326c289c38753323d7e8167897447cf61ef35189d82eb8d75210ece949adda7c"
}
```

These exact files are not required. REVM looks for `basic*.bin`, `kernal*.bin`,
`chargen*.bin`, and `dos1541ii*.bin`, then checks their sizes. The 1541 ROM is
optional unless processor-level drive emulation is enabled.

## License

The c64rework framework, including the original REVM code, is licensed under
the GNU General Public License version 2 or later. REVM links against Frodo and
reSID, which use the same license family. See [LICENSE](LICENSE).

Bundled third-party components:

- **[Frodo V4](https://github.com/cebix/frodo4)** — Copyright Christian Bauer;
GPL version 2 or later. See [src/revm/COPYING.Frodo](src/revm/COPYING.Frodo).
The Frodo bundled C64 ROMs have been removed here.
- **[reSID](https://github.com/libsidplayfp/resid)** — Copyright Dag Lem; GPL
version 2 or later. It is included as the `src/resid` git submodule and carries
license notices in its source files.
- **[Spleen fonts](https://github.com/fcambus/spleen)** — Copyright Frederic
Cambus; BSD 2-Clause License. See
[src/revm/src/debug/LICENSE.spleen](src/revm/src/debug/LICENSE.spleen).
- **[JSON for Modern C++](https://github.com/nlohmann/json)** — Copyright
Niels Lohmann and contributors; MIT License. The license notice is retained
in the bundled `json.hpp`.

The legality and license of program code generated by the c64rework process
is not specified here and is up to the user to determine on a case by case
basis.

## Author

The C64 rework framework and REVM were designed by Lars Ole Pontoppidan starting
in July 2026. AI assisted code and document generation was utilized to create this
project.
