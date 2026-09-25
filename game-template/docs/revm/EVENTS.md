# REVM structured events and reports

REVM can emit machine-readable runtime evidence alongside its human logs. The
event stream is JSON Lines for chronological analysis; the report is one JSON
object summarizing the run. Both are disabled by default.

Implementation authority:
`REVM_ROOT/src/revm/src/util/Event.hpp` and `REVM_ROOT/src/revm/src/util/Event.cpp`.
Event payloads are additive: consumers must ignore fields and event kinds they 
do not recognize.

## Enable output

```bash
./rework test-play stage3 original1 \
  --events /tmp/events.jsonl \
  --report /tmp/report.json
```

- `--events PATH` appends one JSON object per line to `PATH`.
- `--events -` writes lines prefixed with `EVENT ` to stderr.
- `--report PATH` writes one aggregate JSON object at shutdown, replacing the
  file.
- Either option enables event collection. `--report` works without an event
  stream.
- When neither is enabled, an event site costs only one boolean check.

## Event envelope

Every record begins with:

```json
{"v":1,"ev":"fence","frame":12,"cyc":345678,"twin_frame":12,"twin_cyc":345680}
```

| Field | Meaning |
|---|---|
| `v` | Event format version. Currently `1`. |
| `ev` | Event kind. |
| `frame`, `cyc` | Main frame and cycle sampled when the record is created. |
| `twin_frame`, `twin_cyc` | Twin position, omitted when Twin clocks are unavailable. |

Kind-specific payload fields follow the envelope. Addresses and PCs emitted
through the event `Hex` type are JSON strings such as `"0xEC3C"`.

## Standard event kinds

| Kind | Principal payload | Meaning |
|---|---|---|
| `run_start` | `mode`, `play`, `snap`, `kb`, run options | Configuration at the beginning of the run. Fields vary slightly by run mode. |
| `fence` | `op`, `pc`, `result`, `extra_vsync`, `nested` | A synchronization operation. |
| `accept` | `phase`, `kind`, optional `boundary_cyc` | IRQ/NMI detection, resolution, dispatch, silence, or repair. |
| `skew` | `kind`, `paired`, `phi2` | Main/Twin interrupt pairing and Φ2 difference. |
| `handler` | `kind`, `phase` | IRQ/NMI handler entry or exit. |
| `compare` | `screen`, `sid`, `kb` | Per-channel result; `-1` means the channel was not compared. |
| `compare_fail` | `channel`, `addr`, `main`, `twin`, or `overflow` | A differing byte. At most 32 are emitted per comparison before an overflow marker. |
| `screen_fail` | pixel count, bounding box, optional context | Visual mismatch and failure-oriented screen diagnosis. |
| `extra_vsync` | `site` | An extra VBLANK crossed at a `join`, RAM fence, or nested fence. |
| `watch_hit` | `pc`, `label`, `total` | Running hit count for a watched Twin PC. |
| `watch_delta` | `pc`, `what`, `expected`, `got` | Twin crossed a watched point a different number of times than expected. |
| `tally_window` | VSYNC and per-channel compare/fail counts | Periodic comparison coverage window. |
| `softquit` | `code`, `msg` | A soft failure. Failure records are flushed immediately. |
| `run_end` | `result`, `frames_run` | Terminal result and executed frame count. Flushed immediately. |

Fence `op` values include `join`, `bounded`, `ram_read`, `ram_write`,
`ram_rmw`, `io_read`, `io_write`, and `io_rmw`. Results include `ok`, `miss`,
`hit_vsync`, and `timeout`.

The interrupt `kind` is `irq` or `nmi`. Accept phases currently include
`detected`, `resolved`, `dispatched`, `silent`, and `repair`.

### Screen-failure context

A `screen_fail` always reports `pixels` and the inclusive bounding box `x0`,
`y0`, `x1`, `y1`. When both boards' chip state can be captured it also carries:

- `context`: decoded VIC configuration, mode, bases, scrolling, or disagreeing
  registers;
- `cells`: up to 16 implicated video-matrix cells, including Main/Twin character
  and color values;
- `sprites`: sprites whose boxes intersect the mismatch;
- `background_involved` and `border_involved`.

The nested values are JSON objects or arrays, not encoded strings. Their fields
may grow as diagnostics improve.

## Aggregate report

The report contains this top-level shape:

```json
{
  "v": 1,
  "channels": {
    "screen": {"compared": 0, "failed": 0},
    "sid": {"compared": 0, "failed": 0},
    "kb": {"compared": 0, "failed": 0}
  },
  "skew": {
    "irq": {"paired": 0, "silent": 0, "min": 0, "max": 0},
    "nmi": {"paired": 0, "silent": 0, "min": 0, "max": 0}
  },
  "handlers": {
    "irq": {"enter": 0, "exit": 0},
    "nmi": {"enter": 0, "exit": 0}
  },
  "parity": {"irq": "n/a", "nmi": "n/a"},
  "extra_vsync": {"join": 0, "ram": 0, "nested": 0},
  "watch_hits": [],
  "softquits": [],
  "first_fail": null,
  "run_end": null,
  "kinds": {},
  "totals": {"events": 0}
}
```

- `parity` is `pass`, `fail`, or `n/a`. It compares paired accepts with handler
  entries and exits; it is meaningful only when interrupt skew was observed.
- `watch_hits` contains `{pc, label, total}` rows and is capped at 64 rows with
  an overflow marker.
- `softquits` retains at most 32 `{code, msg}` records.
- `first_fail` is the first retained soft quit as `{code, msg}`.
- `run_end` is null if no terminal record was set.
- `kinds` counts every emitted event kind, including plugin-defined kinds.
- `totals.events` counts every emitted record.

The channel, skew, handler, parity, extra-VSYNC, watch, and failure summaries are
maintained by REVM's named event emitters. Arbitrary plugin events appear in
`kinds` and the total count but are not otherwise aggregated.

## Plugin events

Plugins may add game-specific records with `REVM_EVENT`:

```cpp
REVM_EVENT("level_loaded", "level", level, "entry_pc",
           revm::event::Hex{0xC000});
```

Use stable descriptive kinds and fields. Plugin events share the standard
envelope and must remain diagnostic: they must not change game behavior or
synchronization.
