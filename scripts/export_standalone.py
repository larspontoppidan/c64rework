#!/usr/bin/env python3
"""Validate standalone play JSON copied into an export.

C++ Stage 4 inventory, lowering, and leftover checking live in
``lower_standalone_game.py``. This script only gates play recordings.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def check_play(path: Path) -> int:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"standalone-export: invalid play {path}: {exc}", file=sys.stderr)
        return 1
    if not isinstance(value, dict):
        print(f"standalone-export: invalid play {path}: root must be an object",
              file=sys.stderr)
        return 1
    end_frame = value.get("end_frame")
    if isinstance(end_frame, bool) or not isinstance(end_frame, int) or end_frame <= 0:
        print(f"standalone-export: invalid play {path}: positive end_frame is required",
              file=sys.stderr)
        return 1
    mod_events = value.get("mod_events", {})
    if mod_events:
        print(f"standalone-export: rejected play with mod_events: {path}",
              file=sys.stderr)
        return 3
    return 0


def selftest() -> int:
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        good = root / "ok.json"
        good.write_text('{"end_frame": 10, "mod_events": {}}\n', encoding="utf-8")
        if check_play(good) != 0:
            print("standalone-export selftest: valid play rejected", file=sys.stderr)
            return 1
        missing = root / "missing-end.json"
        missing.write_text("{}\n", encoding="utf-8")
        if check_play(missing) != 1:
            print("standalone-export selftest: missing end_frame accepted",
                  file=sys.stderr)
            return 1
        mods = root / "mods.json"
        mods.write_text('{"end_frame": 4, "mod_events": {"1": []}}\n', encoding="utf-8")
        if check_play(mods) != 3:
            print("standalone-export selftest: mod_events play not rejected",
                  file=sys.stderr)
            return 1
    print("standalone-export selftest: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("play-check", "selftest"))
    parser.add_argument("path", nargs="?")
    args = parser.parse_args()
    if args.command == "selftest":
        return selftest()
    if not args.path:
        parser.error("play-check requires a play JSON path")
    root = Path(args.path)
    if not root.exists():
        parser.error(f"path does not exist: {root}")
    return check_play(root)


if __name__ == "__main__":
    raise SystemExit(main())
