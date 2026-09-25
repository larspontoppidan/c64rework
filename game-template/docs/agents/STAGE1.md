
# Stage 1 — BEGIN and plays

Record a draft find-BEGIN play from the cold PRG. Play through the loader.
Press F9 when the real game first becomes visible. The printed cycle/frame is
a draft upper bound because the key was pressed after the true game entry.
Quit cleanly so the play JSON is finalized.

```bash
./rework play-game original --record-play plays/find-begin.json
```

Inspect the draft and locate the stable entry PC after the loader/decruncher.
Probe a candidate during deterministic replay (`test-play` is already
headless):

```bash
DRAFT_CYCLE=12345678

./rework test-play original find-begin \
  --save-snapshot "$DRAFT_CYCLE" begin/draft.bin \
  --max-cycles $((DRAFT_CYCLE + 64))

./rework revm-tool snap begin/draft.bin --disasm

./rework test-play original find-begin \
  --watch-main-pc $ENTRY_PC --max-cycles $DRAFT_CYCLE
```

The first-fetch cycle printed for the correct entry PC may be the exact BEGIN
cycle. Be careful and run further analysis to be sure ENTRY_PC and BEGIN is
correct. When done record the PC, cycle, and frame in `Stage1/NOTES.md`. Freeze it:

```bash
BEGIN_CYCLE=12345000

./rework test-play original find-begin \
  --save-snapshot "$BEGIN_CYCLE" begin/snap.bin \
  --max-cycles $((BEGIN_CYCLE + 64))

./rework revm-tool snap begin/snap.bin \
  --dump-ram begin/ram64k.bin \
  --dump-cpuview begin/cpuview.bin \
  --disasm --disasm-addr 8000
```

Ask user to record representative plays from BEGIN, then verify stored snapshot hashes:

```bash
./rework play-game begin --record-play plays/original1.json
./rework test-play begin original1
```
