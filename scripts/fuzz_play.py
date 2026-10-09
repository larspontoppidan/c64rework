#!/usr/bin/env python3
"""fuzz_play.py — perturb recorded plays and replay the variants under Twin.

A recording that passes is a gate, but it only exercises the IRQ accept
boundaries that one input timeline happens to produce. Shifting or jittering
the input events moves every later accept onto other instruction boundaries,
which exposes stores that race an IRQ-reachable join, waits that test the
wrong edge, windows closed before their fence, and similar defects that no
recording hits. A variant is a PROBE, never a gate: each failure is either a
translation defect or a framework case (STAGE3-MANUAL.md §15).

Usage (from the game root, after `./rework build stage3` or `stage4`):

  ./rework fuzz-play TARGET [PLAY ...] [options] [-- REVM_ARGS ...]

  TARGET          stage3 or stage4
  PLAY            play names (plays/NAME.json) or paths. Default: every
                  plays/original*.json (plays/no-twin*.json with --no-twin).

Options:
  --out DIR       output directory for variants, failure dumps and
                  results.json (default: <fail dir>/fuzz-play)
  --shifts N      variants that shift ALL input events by +1..+N frames
                  (default 9; 0 disables)
  --jitters N     variants that move each input event independently by a
                  random amount (default 6; 0 disables)
  --jitter A:B    jitter range in frames, inclusive (default -2:3)
  --seed S        base seed for the jitters (default 0); a variant's seed is
                  derived from S, the play name and the jitter index
  --no-baseline   do not also replay each play unperturbed
  --tail N        replay every perturbed variant N frames past its last
                  input/end frame, so a defect triggered at the very end of
                  the recording is not cut off (default 50; baselines keep
                  the recording's own end_frame)
  --only NAMES    run only these variants (comma-separated, repeatable),
                  e.g. --only original1_s2,original3_j1
  --failed        rerun only the variants that failed in the previous
                  results.json in --out
  --jobs N        parallel replays (default: CPU count)
  --timeout SEC   per-replay wall-clock limit (default 1800)
  --no-twin       replay with --no-twin (stresses Main's own pacing instead)
  --main-blank    replay with --main-blank (Stage 4.5)
  --game-root DIR game root (default: current directory; ./rework sets it)
  -h, --help      show this text

With --only or --failed, plays are inferred from the variant names when no
PLAY is given, and a variant already saved in --out is replayed from that
file (the exact case that failed), not regenerated. A --failed rerun updates
those entries in results.json and leaves the others. A single variant can
also be replayed directly:

  ./rework test-play stage3 fail/fuzz-play/original1_s2.json --ignore-play-hashes

Everything after `--` is passed to the REVM binary unchanged. Variants are
replayed with --ignore-play-hashes because their end state legitimately
differs from the recording's. mod_events move with the input events in shift
variants and stay put in jitter variants.

Exit status: 0 when every replay passes, 1 when any fails, 2 on usage errors.
"""
from __future__ import annotations

import concurrent.futures as cf
import hashlib
import json
import os
import random
import re
import subprocess
import sys
import tomllib
from pathlib import Path

FAIL_LINE = re.compile(r"FAIL|mismatch|sync: [A-Z]|not yet translated|TwinRunToPc|SoftQuit|out of range")


def usage(code: int = 0, error: str | None = None) -> None:
    stream = sys.stderr if code else sys.stdout
    if error:
        print(f"fuzz_play: {error}\n", file=sys.stderr)
    print(__doc__.strip(), file=stream)
    sys.exit(code)


def parse_args(argv: list[str]) -> dict:
    opts = {
        "target": None, "plays": [], "out": None, "shifts": 9, "jitters": 6,
        "jitter": (-2, 3), "seed": 0, "baseline": True, "jobs": os.cpu_count() or 4,
        "timeout": 1800, "no_twin": False, "main_blank": False,
        "game_root": Path.cwd(), "revm_args": [], "tail": 50, "only": [],
        "failed": False,
    }
    if "--" in argv:
        i = argv.index("--")
        argv, opts["revm_args"] = argv[:i], argv[i + 1:]
    value_opts = {"--out", "--shifts", "--jitters", "--jitter", "--seed", "--jobs",
                  "--timeout", "--game-root", "--tail", "--only"}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            usage(0)
        if a in value_opts:
            if i + 1 >= len(argv):
                usage(2, f"{a} needs a value")
            v = argv[i + 1]
            i += 2
            try:
                if a == "--out":
                    opts["out"] = Path(v)
                elif a == "--game-root":
                    opts["game_root"] = Path(v)
                elif a == "--only":
                    names = [n for n in v.split(",") if n]
                    if not names:
                        raise ValueError
                    opts["only"] += names
                elif a == "--jitter":
                    lo, hi = (int(x) for x in v.split(":"))
                    if lo > hi:
                        raise ValueError
                    opts["jitter"] = (lo, hi)
                else:
                    n = int(v)
                    if n < 0:
                        raise ValueError
                    opts[a[2:]] = n
            except ValueError:
                usage(2, f"bad value for {a}: {v!r}")
            continue
        if a == "--no-baseline":
            opts["baseline"] = False
        elif a == "--failed":
            opts["failed"] = True
        elif a == "--no-twin":
            opts["no_twin"] = True
        elif a == "--main-blank":
            opts["main_blank"] = True
        elif a.startswith("-"):
            usage(2, f"unknown option {a}")
        elif opts["target"] is None:
            opts["target"] = a
        else:
            opts["plays"].append(a)
        i += 1
    if opts["target"] not in ("stage3", "stage4"):
        usage(2, "TARGET must be stage3 or stage4")
    if opts["only"] and opts["failed"]:
        usage(2, "--only and --failed are mutually exclusive")
    if opts["jobs"] < 1 or opts["timeout"] < 1:
        usage(2, "--jobs and --timeout must be positive")
    return opts


def rework_paths(game_root: Path) -> dict[str, str]:
    launcher = game_root / "rework"
    if not launcher.exists():
        usage(2, f"{game_root} is not a game root (no ./rework launcher)")
    out = subprocess.run([str(launcher), "paths"], cwd=game_root,
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"fuzz_play: `rework paths` failed:\n{out.stderr}")
    return dict(line.split("=", 1) for line in out.stdout.splitlines() if "=" in line)


def resolve_play(name: str, game_root: Path) -> Path:
    p = Path(name)
    for cand in (p, game_root / p, game_root / "plays" / name, game_root / "plays" / f"{name}.json"):
        if cand.is_file():
            return cand.resolve()
    usage(2, f"play not found: {name}")
    raise AssertionError


def make_variants(name: str, base: dict, o: dict) -> list[tuple[str, dict]]:
    events = {int(f): v for f, v in base.get("input_events", {}).items()}
    mods = {int(f): v for f, v in base.get("mod_events", {}).items()}
    first = int(base.get("start_frame", 0)) + 1
    out: list[tuple[str, dict]] = []
    if o["baseline"]:
        out.append((f"{name}_base", base))
    for k in range(1, o["shifts"] + 1):
        d = json.loads(json.dumps(base))
        d["input_events"] = {str(f + k): v for f, v in sorted(events.items())}
        if mods:
            d["mod_events"] = {str(f + k): v for f, v in sorted(mods.items())}
        d["end_frame"] = int(base["end_frame"]) + k + o["tail"]
        out.append((f"{name}_s{k}", d))
    lo, hi = o["jitter"]
    for j in range(1, o["jitters"] + 1):
        digest = hashlib.sha256(f"{o['seed']}:{name}:{j}".encode()).digest()
        rnd = random.Random(int.from_bytes(digest[:8], "little"))
        moved: dict[int, object] = {}
        for f, v in sorted(events.items()):
            nf = max(f + rnd.randint(lo, hi), first)
            while nf in moved:
                nf += 1
            moved[nf] = v
        d = json.loads(json.dumps(base))
        d["input_events"] = {str(f): v for f, v in sorted(moved.items())}
        last = max(moved) + 1 if moved else 0
        d["end_frame"] = max(int(base["end_frame"]), last) + o["tail"]
        out.append((f"{name}_j{j}", d))
    return out


def main(argv: list[str]) -> int:
    o = parse_args(argv)
    game_root = o["game_root"].resolve()
    paths = rework_paths(game_root)
    game = tomllib.loads((game_root / "rework.toml").read_text())["game"]
    exe = Path(paths["BIN"]) / f"revm-s{o['target'][-1]}-{game}"
    if not exe.is_file():
        usage(2, f"missing {exe}; run ./rework build {o['target']} first")

    out_dir = (o["out"] or Path(paths["FAIL"]) / "fuzz-play").resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    results_path = out_dir / "results.json"
    previous: dict[str, dict] = {}
    if results_path.is_file():
        previous = {r["variant"]: r for r in json.loads(results_path.read_text())}

    wanted: list[str] = list(o["only"])
    if o["failed"]:
        if not previous:
            usage(2, f"--failed needs a previous {results_path}")
        wanted = sorted(n for n, r in previous.items() if r["exit"] != 0)
        if not wanted:
            print("fuzz_play: no failed variants in the previous results.json")
            return 0

    plays = o["plays"]
    if not plays and wanted:
        plays = sorted({n.rsplit("_", 1)[0] for n in wanted})
    if not plays:
        plays = sorted(p.stem for p in (game_root / "plays").glob(
            "no-twin*.json" if o["no_twin"] else "original*.json"))
    if not plays:
        usage(2, "no plays given and none found in plays/")

    variants: list[str] = []
    for play in plays:
        path = resolve_play(play, game_root)
        base = json.loads(path.read_text())
        if bool(base.get("no-twin")) != o["no_twin"]:
            kind = "no-twin" if base.get("no-twin") else "original"
            print(f"fuzz_play: warning: {path.name} is a {kind} recording", file=sys.stderr)
        for vname, data in make_variants(path.stem, base, o):
            if wanted and vname not in wanted:
                continue
            saved = out_dir / f"{vname}.json"
            if not (wanted and saved.is_file()):   # selected reruns reuse the saved case
                saved.write_text(json.dumps(data))
            variants.append(vname)
    if wanted:
        missing = sorted(set(wanted) - set(variants))
        if missing:
            usage(2, "unknown variant(s): " + ", ".join(missing))

    def run(vname: str) -> tuple[str, int, str]:
        cmd = [str(exe), "--load-snapshot", paths["BEGIN_SNAP"], "--kb", paths["KB_JSON"],
               "--use-play", str(out_dir / f"{vname}.json"), "--headless", "--no-audio",
               "--ignore-play-hashes", "--rom-dir", paths["ROM_DIR"],
               "--dump-fail", str(out_dir / "fail" / vname), "--dump-fail-limit", "1"]
        if o["no_twin"]:
            cmd.append("--no-twin")
        if o["main_blank"]:
            cmd.append("--main-blank")
        cmd += o["revm_args"]
        try:
            r = subprocess.run(cmd, cwd=game_root, capture_output=True, text=True,
                               timeout=o["timeout"])
        except subprocess.TimeoutExpired:
            return vname, 124, f"timeout after {o['timeout']} s"
        text = r.stdout + r.stderr
        (out_dir / f"{vname}.log").write_text(text)
        first = next((ln for ln in text.splitlines()
                      if FAIL_LINE.search(ln) and "JoinAtPcBounded" not in ln), "")
        return vname, r.returncode, first.strip()[:200]

    print(f"fuzz_play: {len(variants)} replays of {len(plays)} play(s) on {o['target']}, "
          f"{o['jobs']} jobs -> {out_dir}", flush=True)
    with cf.ThreadPoolExecutor(o["jobs"]) as ex:
        results = sorted(ex.map(run, variants))

    failed = [r for r in results if r[1] != 0]
    merged = dict(previous) if wanted else {}
    for n, rc, f in results:
        merged[n] = {"variant": n, "exit": rc, "first_failure": f}
    results_path.write_text(json.dumps([merged[n] for n in sorted(merged)], indent=1))
    print(f"pass {len(results) - len(failed)}  fail {len(failed)}")
    for n, rc, first in failed:
        print(f"  {n:<24} exit={rc}  {first}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
