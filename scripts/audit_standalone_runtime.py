#!/usr/bin/env python3
"""Audit ownership and intentional divergence of the standalone runtime.

The manifest is deliberately small and human-editable. This tool checks that
every file in ``src/revm-standalone`` is either mirrored, an explicitly
recorded fork, or standalone-only. It also prevents a fork from silently
drifting: its recorded upstream SHA-256 must still describe the current bytes
in ``src/revm``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python 3.10 diagnostic
    tomllib = None  # type: ignore[assignment]


HASH_RE = re.compile(r"^[0-9a-f]{64}$")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_relative(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise ValueError(f"{field} must not be absolute or contain '.'/'..': {value!r}")
    return path.as_posix()


def safe_prefix(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a non-empty relative path")
    return safe_relative(value.rstrip("/"), field)


def path_in_prefix(path: str, prefix: str) -> bool:
    return path == prefix or path.startswith(prefix + "/")


def load_manifest(manifest_path: Path) -> dict[str, Any]:
    if tomllib is None:
        raise ValueError("Python 3.11 or newer is required (stdlib tomllib missing)")
    try:
        with manifest_path.open("rb") as stream:
            data = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ValueError(f"cannot read manifest {manifest_path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("format") != 1:
        raise ValueError("manifest format must be 1")
    return data


def _string_list(data: dict[str, Any], name: str, *, prefix: bool = False) -> list[str]:
    values = data.get(name, [])
    if not isinstance(values, list):
        raise ValueError(f"{name} must be an array")
    result = []
    for index, value in enumerate(values):
        field = f"{name}[{index}]"
        result.append(safe_prefix(value, field) if prefix else safe_relative(value, field))
    return result


def audit(repo_root: Path, manifest_path: Path) -> dict[str, Any]:
    """Return a JSON-friendly audit result; errors are reported, not raised."""
    errors: list[str] = []
    try:
        data = load_manifest(manifest_path)
        upstream_rel = safe_relative(data.get("upstream_root"), "upstream_root")
        standalone_rel = safe_relative(data.get("standalone_root"), "standalone_root")
        metadata_rel = safe_relative(data.get("manifest_file", "OWNERSHIP.toml"), "manifest_file")
        prefixes = _string_list(data, "mirrored_prefixes", prefix=True)
        mirrored = _string_list(data, "mirrored_files")
        standalone_only = _string_list(data, "standalone_only")
        forks_raw = data.get("forked", [])
        if not isinstance(forks_raw, list):
            raise ValueError("forked must be an array of tables")
    except ValueError as exc:
        return {"manifest": str(manifest_path), "files": 0, "errors": [str(exc)]}

    upstream_root = repo_root / upstream_rel
    standalone_root = repo_root / standalone_rel
    forked: dict[str, dict[str, str]] = {}
    for index, raw in enumerate(forks_raw):
        if not isinstance(raw, dict):
            errors.append(f"forked[{index}] must be a table")
            continue
        try:
            path = safe_relative(raw.get("path"), f"forked[{index}].path")
            upstream = safe_relative(raw.get("upstream", path), f"forked[{index}].upstream")
        except ValueError as exc:
            errors.append(str(exc))
            continue
        digest = raw.get("base_sha256")
        reason = raw.get("reason")
        if not isinstance(digest, str) or not HASH_RE.fullmatch(digest):
            errors.append(f"forked[{index}] {path}: base_sha256 must be 64 lowercase hex characters")
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"forked[{index}] {path}: reason must be non-empty")
        if path in forked:
            errors.append(f"duplicate fork entry: {path}")
        forked[path] = {"upstream": upstream, "base_sha256": str(digest), "reason": str(reason)}

    direct_classes = {"mirrored": set(mirrored), "standalone_only": set(standalone_only), "forked": set(forked)}
    for name, paths in direct_classes.items():
        for path in paths:
            if path == metadata_rel:
                errors.append(f"manifest metadata must not be listed as {name}: {path}")
    all_direct = [(path, name) for name, paths in direct_classes.items() for path in paths]
    seen: dict[str, str] = {}
    for path, name in all_direct:
        if path in seen and seen[path] != name:
            errors.append(f"path has multiple ownership classes: {path} ({seen[path]}, {name})")
        seen[path] = name
    for prefix in prefixes:
        for path in seen:
            if path_in_prefix(path, prefix):
                errors.append(f"path is both mirrored by prefix and explicitly listed: {path}")

    files: list[str] = []
    if not standalone_root.is_dir():
        errors.append(f"standalone root does not exist: {standalone_root}")
    else:
        files = sorted(
            path.relative_to(standalone_root).as_posix()
            for path in standalone_root.rglob("*")
            if path.is_file() and path.relative_to(standalone_root).as_posix() != metadata_rel
        )

    checked = {"mirrored": 0, "forked": 0, "standalone_only": 0}
    for rel in files:
        classes: list[str] = []
        if rel in direct_classes["mirrored"] or any(path_in_prefix(rel, prefix) for prefix in prefixes):
            classes.append("mirrored")
        if rel in direct_classes["standalone_only"]:
            classes.append("standalone_only")
        if rel in direct_classes["forked"]:
            classes.append("forked")
        if len(classes) != 1:
            errors.append(f"{rel}: expected exactly one ownership class, got {classes or ['unlisted']}")
            continue
        kind = classes[0]
        checked[kind] += 1
        standalone_path = standalone_root / rel
        if kind == "standalone_only":
            upstream_path = upstream_root / rel
            if upstream_path.exists():
                errors.append(f"{rel}: standalone-only file also exists upstream")
            continue
        if kind == "mirrored":
            upstream_path = upstream_root / rel
            if not upstream_path.is_file():
                errors.append(f"{rel}: mirrored upstream file is missing")
            elif standalone_path.read_bytes() != upstream_path.read_bytes():
                errors.append(f"{rel}: mirrored file differs from upstream")
            continue
        entry = forked[rel]
        upstream_path = upstream_root / entry["upstream"]
        if not upstream_path.is_file():
            errors.append(f"{rel}: fork upstream file is missing ({entry['upstream']})")
        else:
            current = sha256(upstream_path)
            if current != entry["base_sha256"]:
                errors.append(
                    f"{rel}: upstream changed since fork (base {entry['base_sha256']}, current {current})"
                )

    for path, kind in all_direct:
        if path not in files:
            errors.append(f"{path}: manifest {kind} entry has no standalone file")

    return {
        "manifest": str(manifest_path),
        "upstream_root": str(upstream_root),
        "standalone_root": str(standalone_root),
        "files": len(files),
        "checked": checked,
        "errors": errors,
        "forks": len(forked),
    }


def print_human(result: dict[str, Any]) -> None:
    print(
        f"standalone-ownership: checked {result['files']} files under "
        f"{result.get('standalone_root', '<unknown>')}"
    )
    checked = result.get("checked", {})
    if checked:
        print("  " + ", ".join(f"{name}={checked.get(name, 0)}" for name in ("mirrored", "forked", "standalone_only")))
    errors = result.get("errors", [])
    if errors:
        print(f"standalone-ownership: FAIL ({len(errors)} error(s))", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
    else:
        print("standalone-ownership: PASS")


def selftest() -> int:
    with tempfile.TemporaryDirectory(prefix="standalone-ownership-") as temp:
        root = Path(temp)
        upstream = root / "src/revm"
        standalone = root / "src/revm-standalone"
        (upstream / "shared").mkdir(parents=True)
        (standalone / "shared").mkdir(parents=True)
        (upstream / "shared/same.hpp").write_bytes(b"same\n")
        (standalone / "shared/same.hpp").write_bytes(b"same\n")
        (upstream / "fork.cpp").write_bytes(b"upstream\n")
        (standalone / "fork.cpp").write_bytes(b"standalone\n")
        (standalone / "README.md").write_bytes(b"only here\n")
        base = sha256(upstream / "fork.cpp")
        manifest = standalone / "OWNERSHIP.toml"
        manifest_contents = f"""format = 1
upstream_root = \"src/revm\"
standalone_root = \"src/revm-standalone\"
manifest_file = \"OWNERSHIP.toml\"
mirrored_prefixes = [\"shared\"]
mirrored_files = []
standalone_only = [\"README.md\"]
[[forked]]
path = \"fork.cpp\"
base_sha256 = \"{base}\"
reason = \"test fork\"
"""
        manifest.write_text(
            manifest_contents,
            encoding="utf-8",
        )
        result = audit(root, manifest)
        if result["errors"]:
            print("selftest: expected clean manifest to pass", file=sys.stderr)
            print_human(result)
            return 1
        (standalone / "shared/same.hpp").write_bytes(b"changed mirror\n")
        result = audit(root, manifest)
        if not any("mirrored file differs" in error for error in result["errors"]):
            print("selftest: changed mirror was not detected", file=sys.stderr)
            return 1
        (standalone / "shared/same.hpp").write_bytes(b"same\n")
        (upstream / "fork.cpp").write_bytes(b"changed upstream\n")
        result = audit(root, manifest)
        if not any("upstream changed since fork" in error for error in result["errors"]):
            print("selftest: changed upstream was not detected", file=sys.stderr)
            return 1
    print("standalone-ownership selftest: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, help="ownership manifest (default: src/revm-standalone/OWNERSHIP.toml)")
    parser.add_argument("--repo-root", type=Path, help="repository root (default: repository containing this script)")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--selftest", action="store_true", help="run the audit self-test")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    repo_root = (args.repo_root or Path(__file__).resolve().parents[1]).resolve()
    manifest = (args.manifest or (repo_root / "src/revm-standalone/OWNERSHIP.toml")).resolve()
    result = audit(repo_root, manifest)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_human(result)
    return 1 if result["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
