# REVM hints

Practical details for building, running, and diagnosing REVM. These are useful
facts rather than a workflow; `./rework revm --help`, 
`./rework revm-tool --help`, and the source headers remain authoritative.

## Building

```bash
./rework build revm      # bin/revm and bin/revm-tool (from the game root)
./rework build stage3    # bin/revm-s3-<Game>
./rework build stage4    # bin/revm-s4-<Game>
./rework clean           # remove build/, bin/, and compile_commands.json
```

A CMake cache from another absolute path is discarded automatically for that
target. Use `./rework clean` when you want a full wipe.

REVM requires CMake 3.16 or newer, a C++20 compiler, SDL2, and Perl for reSID
wave-table generation. Video export also requires `ffmpeg` on `PATH`.

reSID is the `REVM_ROOT/src/resid` submodule. `./rework build revm` initializes
it when needed. Builds enable `CMAKE_EXPORT_COMPILE_COMMANDS` and refresh the
merged `compile_commands.json` in the game root. Build the normal REVM
target and any Stage 3 plugin you are editing once so clangd can see them.

## Command-line behavior

The `revm` CLI is flat: recording, playback, headless execution, media output,
and diagnostics are options rather than subcommands.

- `--headless` runs without real-time 50 Hz pacing.
- `--load-d64 FILE` mounts a D64 in drive 8 with 1541 processor + GCR
  emulation. `--disk-auto-load` issues `LOAD"*",8,1` then `RUN`.
  `--disk-warp` skips 50 Hz pacing while the 1541 CPU is running (Frodo's
  DOS idle park is off). Cycles and play hashes are unchanged (headless is
  already uncapped). The host
  file is a read-only seed; snapshots in this mode append a `REVMdrv1`
  trailer with 1541 CPU/RAM and GCR tracks so later disk loads do not
  remount the file. PRG snapshots stay C64-POD-only (existing hashes
  unchanged). `./rework play-game original` only supplies `--load-d64` when
  `original_load` ends in `.d64`; pass `--disk-auto-load` and `--disk-warp` on
  that command if you want Kernal `LOAD"*",8,1` and an uncapped drive wait.
  D64 recordings store `source.auto_load_d64` so replay preserves whether
  loading started automatically. Older plays without this field default to
  autoload enabled.
- A supervisor polls Main's VBlank generation once per second. If it sees no
  progress while the game is not paused, it sends `SIGABRT` to the emulation
  thread so a core dump can preserve the stuck stack. Detection takes between
  one and two seconds. Debugger/process suspension is not treated as a stall.
- `--max-seconds N` uses the same supervisor as a wall-clock guard. It requests
  a clean quit with exit code 124, then aborts if shutdown does not complete
  within two seconds.
- `--resid` selects reSID 6581 emulation. `--save-video` always requires
  `--resid` (Frodo SID is not used for MP4 audio). Headless `--save-audio`
  also requires `--resid`; Frodo SID only emits the sample tap through SDL
  speakers. `--save-video` writes 2× nearest-neighbor H.264 and muxes reSID
  PCM as 192 kbit/s AAC. `--no-audio` cannot be combined with either media
  flag. `--save-screen FRAME FILE` writes a Pepto P6 PPM of Main at that
  VBLANK (same frame counter as `--max-frames`).
- Record further canonical plays from the BEGIN snapshot with
  `--load-snapshot` and `--record-play`. There is no separate BEGIN-cycle
  option.
- Stage 3 `--ignore-checks` keeps comparisons running but does not abort on
  failure. It is for investigation, never checkpoint evidence.
- Stage 3 `--no-twin` omits Twin setup and implies ignored checks, assertions,
  and play hashes. It validates Main as a standalone execution path; it does
  not promise the same later trajectory as a Twin-backed recording.
- Twin-backed recordings are `plays/originalN.json`. `--no-twin` recordings
  are `plays/no-twinN.json` and store `"no-twin": true`. Replaying a play on
  the other kind of system prints a warning; playback is not expected to
  match. Stage 4 does not support `mod_events` and warns if a play contains
  them.
- `--dump-fail DIR` writes expected, actual, and diff PPM files for screen
  mismatches. `--dump-fail-limit N` caps how many mismatch sets are written:
  the default `1` matches the legacy aborting-run behavior; `0` writes every
  mismatch, which is how per-fail evidence is collected under
  `--ignore-checks`.
- Failing checks report the frame they failed in, and the run prints how to
  reproduce them: fence misses and compare aborts raised during frame `N` are
  shown by `--max-frames N`, while screen-check fails report at `N` but
  reproduce with `--max-frames N+1` (the compare runs inside frame `N`, so a
  run capped at `N` quits first). Screen mismatches also emit a `net_fail`
  event row with the reproducing command; see [`EVENTS.md`](EVENTS.md).

For machine-readable diagnostics, see [`EVENTS.md`](EVENTS.md).

## Logging

REVM diagnostics use `REVM_ROOT/src/revm/src/util/Log.hpp` and go to stderr. 
Modules set their name once with `REVM_LOG_MODULE`.

- Default output includes Info and Error.
- `--log-debug CYCLE` enables Debug from the given Main cycle.
- `--log-verbose CYCLE` enables Verbose and Debug from that cycle.
- Cycle `0` enables early startup messages. A higher threshold remains quiet
  until a clock source has been registered.
- Timing stamps are produced by `REVM_LOG_TIMED`; ordinary log calls do not add
  them.
- PC probes use the `watch-main` and `watch-twin` modules and include `PC=` in
  the message. A Stage 3 Main has no opcode stream, so Twin PC probes use
  `host.diag().WatchTwinPc`.
- Multi-line messages prefix only their first line.
- Mismatch details are emitted even when Debug and Verbose logging are off.

`SoftQuit` unwinds the Stage 3 entry handler while allowing tallies, event
flush, and reports to finish. Assertion and comparison helpers may become
non-fatal under their corresponding ignore options; this does not make the run
a valid gate.

## Input configuration

REVM reads `revm.cfg` from the working directory and then next to the binary,
unless `--config PATH` is supplied.

- `[joystick1]` and `[joystick2]` define `up`, `down`, `left`, `right`, and
  `fire` keys.
- Joystick-bound keys are suppressed from the C64 keyboard matrix.
- `[cheats]` configures increment and decrement keys for the selected KB watch
  variable; the defaults are Page Up and Page Down.
- Ctrl+Tab swaps joystick ports. Tab alone remains the C64 Ctrl alias.
- F9 records a timestamp, F10 pauses or resumes, and F12 performs a clean exit.
  These host shortcuts are fixed.
- `--rom-dir DIR` supplies BASIC, KERNAL, and character ROM images. `./rework`
  defaults this to `<revm_root>/roms/` and passes it on every launch; set
  `rom_dir` in `rework.toml` to override. REVM accepts case-insensitive
  `basic*.bin`, `kernal*.bin`, `chargen*.bin` (8 KiB, 8 KiB, 4 KiB) and optional
  `dos1541ii*.bin` (16 KiB), and records their SHA-256 in new play JSON (`roms`)
  without checking those hashes on replay. ROMs are not bundled with the
  framework.

The shipped laptop configuration maps joystick port 2 to the arrow keys with
Left Shift as fire.

## Snapshots, memory, and analysis

`revm-tool snap` can inspect a snapshot, disassemble around an address, and
write two different 64 KiB views:

- `--dump-ram` writes raw DRAM, including RAM hidden under ROM;
- `--dump-cpuview` writes the banked memory image visible to the 6510.

Do not confuse those views when investigating under-ROM code or data.

`revm-tool selftest` runs the analysis, coverage, linked-registry, and
boot-snapshot self-tests. Coverage from multiple plays should be merged before
generating a listing or map. A map can combine RAM, BASIC, and KERNAL regions;
unnamed memory hits appear as `UNDOC`.

The game KB supplies names, watches, traces, ranges, and blobs. Load
`REVM_ROOT/reference/c64.kb.json` as the support KB rather than copying standard
C64 definitions into each game. The KB format is documented in [`KB.md`](KB.md).

## CpuMock plugins

Stage 3 and Stage 4 plugins use the public contracts in
`REVM_ROOT/src/revm/src/cpumock/`. Game work must not change those helpers.

- [`STAGE3-MANUAL.md`](../agents/STAGE3-MANUAL.md) explains `Sync`, memory and
  I/O bags, interrupt dispatch, timed accesses, comparisons, and no-Twin
  behavior.
- [`STAGE4.md`](../agents/STAGE4.md) explains linked local state.
- The corresponding headers are authoritative for exact signatures and current
  behavior.

If a helper appears wrong during game work, stop with a reproducible case. Do
not patch the framework to accommodate one game's translation.
