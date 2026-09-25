# Stage 2: analysis and knowledge base

Stage 2 builds the evidence needed for a faithful hand translation. Work from
the canonical BEGIN snapshot and representative recorded plays. The result is
not a perfect disassembly; it is a growing, evidence-based model of the code and
data exercised by the game.

Commands run from the **game root** via `./rework`. See `./rework help`.

## Two knowledge-base layers

The game owns `Stage2/<game>.kb.json`. It records game-specific routines, 
labels, variables, ranges, comments, watches, traces, and blobs. Its format 
is defined in `./docs/revm/KB.md`.

`REVM_ROOT/reference/c64.kb.json` in the framework is the shared C64 backdrop
knowledge base. `./rework kb-analysis` passes it as `--kb-support`. It supplies
fallback names for standard 6510, VIC, SID, CIA, BASIC, KERNAL, and 
character-ROM locations. It is deliberately support-only:

- the game KB wins when both files name the same address;
- support objects do not expand game listing ranges or add KB-only map rows;
- support blobs are not dumped as game assets;
- `bank: "chip"` names memory-mapped hardware;
- ROM names apply only in the matching bank; under-ROM game RAM remains game
  RAM and must not be mislabeled as KERNAL/BASIC.

Do not copy standard C64 definitions into every game KB. Add an entry to the
game KB when it describes game-owned meaning, overrides a fallback, marks a
watch/trace, or documents game RAM/code at an overlapping address.

## Games that call KERNAL (or BASIC) ROM

Many games `JSR` into KERNAL routines (`$FFD2` CHROUT, `$FFF0` PLOT, `$E8B3`
screen output) or chain their `$0314` IRQ handler into the KERNAL IRQ
(`$EA31`). That code is part of the exercised control flow and will be
hand-translated in Stage 3 like any other code. **Do not skip it or treat it as
an untouchable black box** — a replica that cannot reproduce what CHROUT/PLOT
do cannot certify a picture.

- Coverage already records ROM execution (`pc_rom` hits). The listing carries a
  `# KERNAL ROM` section built from real PC-ROM hits; its coverage ranges are
  the evidence of what the game actually invokes.
- For every invoked routine, add a game-KB `function` object with
  `bank: "kernal"` (or `"basic"`), the true entry PC and inclusive range, and a
  comment describing what the game uses it for. KB functions expand listing
  ranges, so these entries turn fragmented coverage slivers into readable whole
  bodies — including branches not yet executed. Keep support-KB fallback names;
  add only game-owned meaning (usage, state, side effects).
- Kernel zero-page temps the game path depends on (`$D1/$D3/$D6/$F3/$F6/$CB` …)
  become Main-owned state once Stage 3 translates through them: name them and
  set watches when a slice will translate across that state.
- Pull full function bodies with
  `./rework revm-tool snap begin/snap.bin --disasm --disasm-addr <pc>
  --disasm-bytes N`; covered slivers alone are not enough to translate a
  routine.
- Record which KERNAL entry points are invoked, from where, in the
  `Main control flow` section of `Stage2/NOTES.md`, so Stage 3 can plan
  translation slices (leaf output routines such as CHROUT/PLOT before the whole
  IRQ screen editor).

## Analysis loop

Coverage is regenerated when recorded plays change, not on every KB edit.
Day-to-day Stage 2 work is: edit the KB, then rebuild analysis.

1. When plays change, replay them from BEGIN while recording coverage:

   ```bash
   ./rework generate-cov original1 original2
   ```

   That writes `plays/original1_cov.bin`, `plays/original2_cov.bin`, and merges two or
   more into `plays/merged_cov.bin`.

2. Generate (and atomically replace) `kb-analysis/`: listing, address map,
   extracted blobs. If `plays/merged_cov.bin` exists, also the call graph and
   hot-UNDOC queue:

   ```bash
   ./rework kb-analysis
   ```

   The command warns if merged coverage is missing; listing and map still
   appear, without coverage-driven graphs.

3. Investigate the hottest unknown state and the control-flow path needed next.
4. Add only facts supported by disassembly, runtime observation, or data-format
   analysis.
5. Regenerate `kb-analysis/` and use watches, traces, and snapshot diffs to
   test the new interpretation. Extra REVM flags go through the launcher:

   ```bash
   ./rework test-play begin original1 --trace-calls --watch \
     --watch-timeline plays/original1.watch.jsonl
   ```

6. Document extracted blob layouts in `Stage2/FORMATS.md`.
7. Record coverage commands, listing rebuild, control flow, and analysis gaps
   in `Stage2/NOTES.md`.

`./rework test-play` is headless (`--headless`). Extra options after
`TARGET`/`PLAY` are passed to REVM. `./rework revm --help` and
`./rework revm-tool --help` remain authoritative for flags.

## Knowledge discipline

- Keep original addresses and inclusive ranges precise.
- Use `function` for callable original routines and `label` for internal entry
  points or branch targets.
- Use `watch: "yes"` for state expected to agree at Stage 3 VSYNC checks;
  reserve `watch: "verbose"` for useful optional detail.
- Use `trace: "yes"` selectively on routines whose runtime calls matter.
- Give each `blob` a real format documented in `Stage2/FORMATS.md`.
- Treat UNDOC counts as prioritization, not a completion score.
- Never add `unknown` objects, broad junk ranges, or empty blobs merely to make
  map coverage look better.

## Stage 2/3 feedback loop

Stage 2 does not end when Stage 3 begins. Translation commonly reveals an
incorrect function boundary, a hidden state variable, an alternate bank, or a
previously misunderstood table. Update the game KB and `Stage2/NOTES.md`,
and regenerate analysis outputs immediately so the C++ names and disassembly
remain aligned.

Stage 3 can start when the main exercised control flow is understood, most state
needed for the next translation slices is named, and required blob formats are
documented. Unknown code outside current plays may remain; invented knowledge
may not.

Do not modify `src/revm/` to add analysis features or "fix" listings. If a
tool command is wrong, stop and report the command plus expected vs actual.
