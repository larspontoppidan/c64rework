# Modding the original game

A mod changes the **original program**: small 6502 patches applied to the
BEGIN image, packed into one PRG that loads and runs on a real C64 (or any
emulator) like an ordinary single-file release. It does not use the Stage 3
or Stage 4 translation; it uses what Stage 2 learnt about the original: the
KB names, the annotated disassembly and the free-RAM map.

Commands run from the game root through `./rework mod ...`. They need
`./rework build revm` (revm and revm-tool) and `64tass` (`apt install 64tass`).
A built-in packer can be used or `exomizer` if it is installed; (`exomizer`
on the PATH or `EXOMIZER=/path/to/exomizer`). Exomizer compresses better but
takes slightly longer to unpack. It can be found here:
https://bitbucket.org/magli143/exomizer/src/master/: (zlib license) clone it
and run `make` in `src/`.

## How it works

```
begin/snap.bin ──► BEGIN RAM + chip registers (revm-tool --dump-chips)
                    │
Mods/*.asm ──64tass──► patches ──checks──► patched BEGIN image
                                              │          │
                                              │          └─► build/mods/patched_snap.bin (tests)
                                              ▼
                            boot stub + packer ──► one PRG (BASIC SYS line at $0801)
```

At RUN the PRG decompresses the image in place, then the **boot stub**
recreates the BEGIN state: it copies back low memory (zero page, stack, the
KERNAL area, wherever the depacker and the KERNAL worked) and colour RAM from
copies kept in free RAM, writes the VIC, SID and CIA registers of the BEGIN
snapshot, sets the CPU port, stack pointer, registers and flags, and jumps to
the BEGIN PC. The game then starts exactly as the original does after its
loader. Loaders often leave state the game relies on, eg. Frantic Freddie's
copy-protection check samples `$D010 = $68` left by its loader, and the stub
restores it.

`./rework mod check-boot` proves this: it runs the PRG in revm to the BEGIN PC
and compares RAM and chip registers with the patched BEGIN image. They must be
identical apart from the stub area, the copy area, the CPU port `$00/$01`
(not RAM), and the one dead stack byte that carried the flags. What a stub cannot recreate is listed as notes: pending
interrupt flags (acknowledged), a stopped CIA timer whose counter differs from
its latch, and the exact phase of running timers. Check that the game does not
depend on them (it usually acknowledges and reprograms them itself).

While depacking, the KERNAL is banked out and the NMI vector at `$FFFA` is not
valid yet; the built-in loader blocks the RESTORE key with one unacknowledged
CIA2 NMI until the stub restores CIA2. exomizer's self-extractor does not:
RESTORE during its unpacking crashes the machine.

## Files in the game root

```
Mods/
  mods.toml           the mod: output name, boot areas, the patch list
  NOTES.md            what each patch does and why (keep it current)
  <patch>.asm         one 64tass source per patch
  tests/<name>.json   scenario tests
build/mods/           generated: game.inc, listings, patched image and snapshot,
                      boot stub sources, test runs (screens, logs)
```

`Mods/mods.toml`:

```toml
name = "GameName Mod"
base = "begin/snap.bin"
output = "GameNameMod.prg"

[boot]
stub_area = "$CC00-$CFFF"   # free RAM: depacker and boot stub (stays in RAM)
copy_area = "$D000-$DFFF"   # free RAM, page-aligned: copies of low memory and colour RAM
# exomizer_options = ["-x3"] # exomizer sfx options (default -x3: border flash while depacking; -x1/-x2 other flashes, -n none)

[[patch]]
name = "jukebox"
source = "jukebox.asm"
area = "$7100-$73FF"        # free RAM for this patch's code and data
description = "Title screen jukebox"
```

Every patch, the stub area and the copy area must lie in free RAM: RAM that no
recorded play executed, read, wrote or the VIC fetched
(`build/mods/free_map.txt`, made from `plays/merged_cov.bin`; run
`./rework generate-cov` with the plays first). Coverage proves only what the
plays reached. Prefer runs of one repeated byte, padding after known data, and
RAM under the I/O area for data the game never reads with its own code.

## Writing a patch

A patch is a 64tass source. `build/mods/game.inc` gives every KB name as a
label (`score_add`, `lives`, `music_init`) and, for ranges, `name_end`: the
KB's last address (inclusive). `AREA` and `AREA_END` are the patch's area from
`mods.toml`; `AREA_END` is the first address after it (exclusive).

```asm
        .include "game.inc"

;@expect $F54A A8 A2 05
        * = score_add                   ; was TAY / LDX #$05
        jmp score_add_2k

        * = AREA
score_add_2k
        ...
        .cerror * > AREA_END, "does not fit its area"
```

Rules:

1. **Every hook declares the bytes it replaces** with `;@expect $ADDR bytes`.
   The build fails if the image holds other bytes there. This catches a patch
   written for another version of the game, a wrong address, and two patches
   hooking the same place.
2. **Only hooks and the patch's own area are written.** The build fails on any
   other byte, on overlaps between patches and on areas outside free RAM.
3. **Re-execute what the hook displaced.** A `JSR` or `JMP` over an instruction
   must run that instruction in the new code (or jump to code that does).
   Note the register and flag state the following code expects; preserve what
   the original kept (a loop counter in X, the carry into a subtraction).
4. **Investigate first, with provenance.** Read the routine in
   `kb-analysis/disasm_begin.txt` and its Stage 3 translation; state in the
   source what the hook site does and why the patch is correct there. Quote
   addresses, not guesses.
5. **Respect the interrupt structure.** Know whether the hooked code runs with
   IRQs on, what the IRQ and NMI handlers touch, and never leave a chip
   register different from what the game expects (for example `$DC00` after a
   keyboard scan).
6. **Text** uses the game's own font: find how the game's texts are encoded
   (often ASCII codes or screen codes) and pick `.text` (source characters as
   they are) or `.enc "screen"`.
7. **One feature per patch**, each with its own area, so patches can be built,
   tested and shipped separately (`./rework mod build --only jukebox`).

Keyboard scanning on CIA1: write a row mask to `$DC00`, read the columns on
`$DC01` (0 = down). Joystick 2 shares `$DC00` and joystick 1 shares `$DC01`;
check `$DC01` with no row selected (`$FF`) and read `$DC00` back after
selecting a row to ignore joystick lines. Restore `$DC00` to the value the game
left there.

## Testing

Interactive play checks are the user's. Agents test with scenarios: small JSON
files in `Mods/tests/` that drive a headless revm run and check the result.

```json
{
 "description": "At 1990 points the next pickup reaches 2000 and gives a fourth life",
 "source": "snapshot",
 "patches": ["extra_life"],
 "inputs_from": "original1",
 "pokes": {"6370": {"$D0": 1, "$D1": 9, "$D2": 9}},
 "inputs": {"6400": ["K SPACE"], "6404": []},
 "end_frame": 6430,
 "screens": [6420],
 "expect": [
  {"frame": 6390, "var": "lives", "is": 4},
  {"frame": 6390, "addr": "$4BCB", "bytes": "50 49 43 4B"},
  {"pc": "$7600", "hit": true, "after": 6380, "before": 6390}
 ]
}
```

- `source`: `snapshot` runs the patched BEGIN snapshot (frames count from the
  BEGIN frame, as in the recorded plays); `prg` runs the packed PRG from
  power-on (frames from 0), which tests the whole delivery.
- `patches`: build the snapshot with only these patches. Use it whenever a test
  replays recorded inputs: any patch that changes timing (an extra poll, a
  longer routine) changes the CIA-based randomness, and the recorded inputs
  stop matching the game after that point. Without the key, the test uses the
  full build.
- `inputs_from`: a recorded play whose input events and `mod_events` are used.
  `inputs` adds the scenario's own inputs on top (`"K <key>"` with Frodo key
  names, `"J2 fire"`): each value is the set the scenario holds from that frame
  on, and `[]` releases only the scenario's inputs.
- `pokes`: memory increments (negative: decrements) at a frame, by address or
  KB name; the play's `mod_events`. Use them to reach a state fast (a score
  just below a threshold, the last pickup of a level, a later level).
- Frames are PAL frames (revm emulates a PAL C64: 19656 cycles each).
- `expect`: `var` (a KB name) or `addr` with `is` (one byte) or `bytes` (hex)
  at a frame, read from a snapshot taken whole frames after the start (at the
  start's raster position; $D800-$DBFF reads colour RAM); `pc` with `hit` true
  or false within `after`/`before` frames.
- `screens`: PNGs of those frames in `build/mods/tests/<name>/`. Look at them.
- `audio` / `video` (true): `audio.wav` or `video.mp4` (with sound) of the run,
  for the user to judge sound and feel without playing.

Every patch gets tests for its effect, for the normal path staying as before
(the interlude still plays without SPACE; 1000 points still gives no life),
and for its edge cases. One `prg` test runs the packed delivery end to end.

## The loop

1. Investigate the original: the routine, its callers, its state, the
   interrupts. Record facts in `Mods/NOTES.md`.
2. Write the patch and its tests.
3. `./rework mod build`, `./rework mod check-boot`, `./rework mod test`.
4. Look at the screens; give the user `video` runs to watch and listen to.
5. Report what was verified and what needs the user's play check.

The PRG is a build artifact (not committed); the sources, `mods.toml`, the
tests and `NOTES.md` are.

## Sharing a mod

The packed PRG and the patched snapshot contain the whole game. Sharing such
binaries most likely break property rights. Share the recipe instead, the
receiver can rebuild the mod from their own copy.

Export (a folder or archive):

- `Mods/` (mods.toml, patch sources, tests, NOTES.md) and the KB
  (`Stage2/<game>.kb.json`, for `game.inc`);
- `plays/find-begin.json` and the plays the tests use (inputs only; they
  name the original by its SHA-256);
- a short README: the original's file name and SHA-256, the BEGIN cycle, the
  SHA-256 of `begin/snap.bin`, and the framework version.

Not exported: the original image, ROMs, `begin/`, `build/`, the PRG.

Apply (the receiver has C64 Rework, 64tass and optionally exomizer, their own
original and C64 ROMs):

1. Set up a game root for the original (`rework.toml`, the image) and copy
   the export in.
2. `./rework build revm`; check the image's SHA-256.
3. Recreate BEGIN as in `docs/agents/STAGE1.md` ("Freeze BEGIN image"):
   `./rework test-play original find-begin --save-snapshot <cycle>
   begin/snap.bin --max-cycles <cycle + 64>`. Emulation is deterministic: the
   snapshot's SHA-256 must equal the README's, else stop (another dump or
   framework version).
4. `./rework generate-cov` with the plays (else the build warns that it
   skips the free-RAM check); then `./rework mod build`,
   `./rework mod check-boot`, `./rework mod test`.

## Limits

- The base is the BEGIN snapshot. Programs that load more files after BEGIN
  (levels from disk) need those files patched and shipped as well; the
  single-PRG packing does not cover that.
- Recorded original plays will not replay on a patched build once a patch
  changes timing. They remain the specification of the unpatched game.
- The built-in packer compresses less than exomizer and depacks slower (about
  4 s for a full image, screen blanked). Use exomizer for a release.
