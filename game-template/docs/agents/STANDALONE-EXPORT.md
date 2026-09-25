# Standalone export

Standalone export is a mechanical lowering pass over an accepted Stage 4
tree. It is not a fifth reverse-engineering stage and it must not contain
game-specific redesign. Stage 4 remains the reviewed source of truth.

The standalone runtime is a deliberately cut-down assembly of the existing
REVM/Frodo implementation. Reuse the proven chip simulation, register access,
reSID audio, display, input, and machine-clock modules directly. Do not write
parallel implementations of those facilities. Extract dependencies and add
narrow adapters only where an existing module is coupled to REVM's analysis,
recording, or Twin orchestration.

Its canonical runtime tree is `REVM_ROOT/src/revm-standalone/`. It begins as
a broad, literal REVM copy and is reduced through ordinary, reviewable C++ 
and CMake changes. The export command copies that tree; it must not synthesize
or patch the runtime while exporting a game. Game lowering is a separate
tree-sitter-cpp transformation over the copied `StandaloneExport/src/game/` tree.

The exported project's root files (CMake, `build.sh`, README) come from
`REVM_ROOT/src/template-standalone/`. Export copies that scaffold, then places
the lowered game, `REVM_ROOT/src/revm-standalone/` as `src/revm/`, and 
`src/resid/` beside it.

Runtime ownership is explicit; see `docs/revm/REVM-STANDALONE.md`. Mirrored files
are byte-identical to `src/revm`, intentional forks record their upstream base
hash and reason, and standalone-only files have no upstream counterpart. Do not
use a compile definition to give a mirrored file a hidden standalone personality.

The exporter produces `StandaloneExport/` in the **game root**.
`c64rework.version` records the C64 Rework release that produced it.

**As the standalone export step is still immature**, generally don't edit 
`Stage4/` to make an export build. Fix a general lowering problem in the
exporter or standalone runtime; send newly discovered game semantics back to
Stage 4 and re-review them there, ultimately correcting Stage 3 as well if
game code needed updating.

Commands run from the game root via `./rework`. Run `./rework setup-venv`
once so the Python lowering and audit tools can execute.

## Required input

- An accepted `Stage4/` checkpoint passing all the gates and code quality 
requirements.
- Relevant play recordings for testing. Standalone can only faithfully
playback no-twin type plays with no `mod_events`.

## Mechanical lowering

Preflight (`./rework stage4-diag` / `export`), lowering, and postflight
(`./rework check-export`) share one tree-sitter analyzer in
`scripts/lower_standalone_game.py`. Regular-expression C++ scanning is not
an authority. Comments, string literals, and similarly named game identifiers
are not constructs.

The generated tree must apply deterministic rules:

1. Copy the accepted Stage 4 sources and constexpr assets.
   `video.charset.install(...)` and `video.sprites.slot(...).install(...)`
   remain portable `gamehost` calls. They seed mutable charset and sprite
   surfaces; subsequent timed writes stay live operations. They are inventoried
   for a future asset manifest, not rewritten.
2. Select the no-Twin side of every supported `HasTwin()` / `NoTwin()` `if`
   and conditional — including `HasTwin() && …` (standalone-false) and
   `NoTwin() || …` (standalone-true). When the Twin predicate follows another
   operand, that operand must be side-effect-free; calls, assignments, and
   increments are export errors. `NoTwin() && …` and `HasTwin() || …` mixes
   are errors. Do not preserve a fake `NoTwin()` host API.
3. Remove `Sync::AtPc`, ordinary joins, compares, and watch probes. Rebind
   `AdvanceCycles`, `AdvanceToVSync`, `JoinAtPcBounded` (its `.no_twin_frames`
   wait), `ReturnIrq`, and `ReturnNmi` onto the host's Main-only clock and
   interrupt-completion methods. These are simulation-time operations, never
   wall-clock sleeps. IRQ/NMI returns are not timing no-ops.
4. Replace `LinkedByte<address>`, `LinkedWord<address>`, and
   `LinkedArray<address, size>` with address-free standalone state types.
   Preserve every initializer and provenance comment. Remove registrations.
   Lower their member RMW operations to ordinary wrapping C++ with explicit
   carry handling; do not introduce opcode-mimicking free helpers.
   `address()` has no standalone meaning and is an export error.
5. Bind `Sei`/`Cli`, handler installation, and checked quit paths to the
   `gamehost` names. Receivers must resolve to the declared host type or the
   `host()` accessor; a field merely named `host`/`host_` is not enough.
6. Rewrite REVM includes, `revm::` types, logging macros, and the
   `revm::cpumock` entry namespace onto the `gamehost` facade. Unknown
   qualified names, namespaces, and parse errors fail before any staged file
   is changed.
7. Generate the standalone entry point and CMake project. It links the curated
   standalone runtime assembled from existing REVM/Frodo/reSID modules, but
   must not link the REVM executable, Twin, snapshot/golden machinery,
   debugger/KB tooling, or other reverse-engineering-only orchestration.

Keep the existing register-shaped `IoMap`, video-asset surface, and input
semantics as the authoritative APIs. Remastering may replace these surfaces
later; export does not.

## Supported Stage 4 syntax

Accepted forms the analyzer will lower:

- Twin predicates: direct, negated, parenthesized, `HasTwin() && …`,
  `… && HasTwin()` when the left operand is side-effect-free, `NoTwin() || …`,
  and `… || NoTwin()` when the left operand is side-effect-free, in `if` and
  `?:` context.
- Sync fences `AtPc`, `JoinAtPc`, `Compare`, `ExpectWatchDelta` as expression
  statements; `WatchMark` as a value that becomes `uint64_t{0}`.
- Clocks `AdvanceCycles`, `AdvanceToVSync`; `JoinAtPcBounded` with a decimal
  `.no_twin_frames`; `ReturnIrq` / `ReturnNmi`.
- Linked scalar and array `read`/`write`/`inc`/`dec`/`asl`/`lsr`/`rol`/`ror`,
  including template parameters resolved from call sites across the copied
  tree. Receivers are matched by declared type path, not terminal names.
- Host `Sei`/`Cli`/`SoftQuit`/`AssertBegin`/`QuitRequested` and
  `InstallIrqHandler`/`InstallNmiHandler` on a typed host or `host()`.
- Facade includes, `revm::cpumock` types in the rewrite map, `REVM_LOG*`
  macros, and `namespace revm::cpumock` as the game entry.

Deliberate rejections: mixed Twin predicates with side effects; linked
`address()`; free `asl`/`lsr`/`rol`/`ror`/`inc`/`dec` helpers; unknown
`revm::` names; `Sei()` on a non-host; parse errors other than the known
functional-cast-vs-relational grammar defect (`unsigned(x) >= …` in a
condition); already-lowered trees passed to `lower`.

Prefer a Stage 4 ordinary-C++ rewrite over expanding the transformer.

## Runtime contract

The shared runtime retains the existing implementations of all semantics
formerly supplied by Main's no-Twin board:

- PAL simulation clock: 63 cycles per line, 312 lines per frame;
- raster progress and VSYNC-time input latching;
- CIA timer/TOD progression and ordered IRQ/music callback delivery;
- screen matrix, color surface, VIC registers, sprites and collision latches;
- mutable charset and sprite surfaces, including checked asset installation;
- SID register events and audio production;
- loud checked failures for invalid asset, screen, sprite, and register use.

`advance_cycles(n)` must advance every device and dispatch events occurring
inside the interval. A once-per-frame callback is not an equivalent
implementation: music, raster waits, collision behavior, and timer-derived
randomness depend on sub-frame progress.

## Visual assets

Do not ship the BEGIN memory image as an unnamed asset. Export the complete
2-KiB character page selected by the accepted VIC setup. Compute the closure
of every sprite pointer the translated code can produce, including animation
offsets and table-derived values, then export the corresponding aligned
64-byte slots with source range and digest metadata. The Stage 4 install calls
are the source-level inventory for these initial surfaces; dynamic updates
remain ordinary mutable writes and must not be flattened into one-time assets.

Screen RAM, color RAM, and the sprite-pointer table are mutable runtime
surfaces unless read-before-write evidence says otherwise.

## Verification

Game export from the game root, after Stage 4.5 is accepted:

1. `./rework export` (or `./rework export --replace`) to generate
   `StandaloneExport/`. Preflight runs the tree-sitter `audit` on `Stage4/`
   and audits runtime ownership. Optional: `./rework stage4-diag` for that
   same inventory without generating.
2. `./rework check-export` (the same analyzer in
   `check` mode on `StandaloneExport/src/game`)
3. `./rework build export`
4. verify the generated project does not link `revm_lib`, Twin, snapshots/goldens,
   or debug tools
5. replay every applicable play from the accepted Stage 4 table with the
   exported binary (`StandaloneExport/bin/game --use-play plays/…`); recordings
   containing `mod_events` must be rejected and are not standalone evidence.
   Standalone `--use-play` is the same machine as Stage 4
   `--main-blank --no-twin`: timed gameplay events (pickups, collect, level
   clear, death) must match that no-Twin recording.
6. as a human diagnostic, inspect representative terminal output beside the
   Stage 4 `--main-blank --no-twin` run; matching frame/cycle stamps and
   gameplay events (pickups, collect, level clear, death) are required
   evidence that standalone is the same machine. Logs still omit video/audio
   state, so they do not replace play-feel or SID/frame checks.
7. compare frame observations and public SID traces with the accepted Stage 4
   no-Twin run
8. ask the user for the final interactive play-feel check

Framework tool fixtures (lint, tree-sitter lowering analyzer, ownership auditor)
are `./rework selftest`. That is not a game-export step.

The exported runtime already exposes `--use-play FILE` for no-Twin playback
from the game root's `plays/` directory. Snapshot binaries are not part of
the export. Playback applies input events and the random seed, and defaults
to `end_frame`. It is the same blank-Main machine as Stage 4
`--main-blank --no-twin`.

An unsupported construct or missing asset is an export error. Never patch
generated game logic by hand.
Likewise, never make a new chip, I/O, audio, display, or input implementation
merely to avoid disentangling the existing one.

## Framework checkpoint

Bootstrap packaging, intrinsic startup, JSON playback, unified tree-sitter
audit/lower/check, Twin-branch and fence lowering, clock/interrupt rebinding
onto the gamehost facade, explicit runtime ownership, removal of standalone
`Sync.hpp`, lowering of `Linked*` state to plain storage, and a direct Frodo
SC/reSID hardware probe are complete. Regex C++ preflight is gone; charset
and sprite `install()` calls remain through `gamehost` and are inventoried
for the asset manifest. The runtime no longer carries `LinkedRegistry` or
memory-modification support. GameHost owns the Main-only clock and interrupt
dispatch; StandaloneRunner owns launch and JSON replay. Preserve the existing
`IoMap`, `VideoAssets`, input, and chip implementations; prune unused
subsystems only behind parity probes.
