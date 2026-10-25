#!/usr/bin/env python3
"""Compares every matching mode against the DuckDB ground truth.

  python3 scripts/check.py patterns/core.txt
  python3 scripts/check.py patterns/core.txt --modes raw-opt,fsst,fsst-rev2

Exits non-zero if any mode gets any pattern wrong, and also if nothing was
actually verified -- so a run where everything was skipped can never read as a
pass.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (DATA, PatternSpec, load_column, parse_pattern_file, read_ids,  # noqa: E402
                    read_raw_values, resolve_cli)
from truth import ensure_truth  # noqa: E402

ALL_MODES = ["raw", "raw-opt", "ref", "decode", "fsst", "fsst-compact",
             "fsst-rev", "fsst-rev2"]


def run_mode(cli: str, spec: PatternSpec, mode: str, out: Path, timeout: int) -> tuple[str, str, dict]:
    argv = [cli, "--mode", mode, "--col", str(DATA / spec.column), "--pattern", spec.pattern,
            "--rowids", str(out), "--runs", "1", "--warmup", "0", "--json"]
    if spec.is_concat:
        argv += ["--col2", str(DATA / spec.column2)]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return "FAIL", f"timed out after {timeout}s", {}
    if proc.returncode == 2:
        return "SKIP", (proc.stderr.strip() or "unsupported"), {}
    if proc.returncode != 0:
        return "FAIL", f"exit {proc.returncode}: {proc.stderr.strip()}", {}
    info = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                info = json.loads(line)
            except json.JSONDecodeError:
                pass
    return "OK", "", info


def describe(spec: PatternSpec, missing: list[int], extra: list[int]) -> None:
    """Prints the first few differing rows with their actual bytes."""
    wanted = (missing[:3] + extra[:3])
    if not wanted:
        return
    vals = read_raw_values(DATA / spec.column, wanted)
    vals2 = read_raw_values(DATA / spec.column2, wanted) if spec.is_concat else {}
    for r in wanted:
        tag = "DuckDB says match, we did not" if r in missing else "we said match, DuckDB did not"
        v = vals.get(r, b"")
        print(f"      row {r}: {tag}")
        print(f"        value  = {v!r}")
        if spec.is_concat:
            print(f"        value2 = {vals2.get(r, b'')!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("patterns", type=Path)
    ap.add_argument("--modes", default=",".join(ALL_MODES))
    ap.add_argument("--cli", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for m in modes:
        if m not in ALL_MODES:
            raise SystemExit(f"unknown mode {m!r}; known: {', '.join(ALL_MODES)}")
    cli = resolve_cli(args.cli) if args.cli else resolve_cli()
    if not Path(cli).is_file():
        raise SystemExit(f"no CLI at {cli}; configure the repository with "
                         "-DTOKENLIKE_WITH_FSST=ON and build fsst_like, or set FSSTLIKE_BUILD")

    specs = parse_pattern_file(args.patterns)
    manifest = ensure_truth(specs, force=args.force, quiet=True)
    print(f"{args.patterns.name}: {len(specs)} patterns x {len(modes)} modes   cli={cli}")

    t0 = time.time()
    passed = failed = skipped = 0
    skip_reasons: dict[str, int] = {}
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "ids.bin"
        for spec in specs:
            truth = set(read_ids(spec.truth_path))
            stated = manifest[spec.name]["matches"]
            if len(truth) != stated:
                raise SystemExit(f"{spec.name}: truth file holds {len(truth)} ids but the manifest says {stated}")
            for mode in modes:
                status, detail, info = run_mode(cli, spec, mode, out, args.timeout)
                if status == "SKIP":
                    skipped += 1
                    skip_reasons[detail] = skip_reasons.get(detail, 0) + 1
                    continue
                if status == "FAIL":
                    failed += 1
                    failures.append(f"{spec.name} [{mode}]: {detail}")
                    print(f"  FAIL {spec.name} [{mode}]: {detail}")
                    continue
                got = set(read_ids(out))
                if got == truth:
                    passed += 1
                    continue
                failed += 1
                missing = sorted(truth - got)
                extra = sorted(got - truth)
                failures.append(f"{spec.name} [{mode}]: {len(missing)} missing, {len(extra)} extra")
                print(f"  FAIL {spec.name} [{mode}] pattern={spec.pattern!r}")
                print(f"      truth={len(truth)} got={len(got)} missing={len(missing)} extra={len(extra)}")
                describe(spec, missing, extra)

    dt = time.time() - t0
    print(f"  {passed} passed, {failed} failed, {skipped} skipped in {dt:.1f}s")
    for reason, n in sorted(skip_reasons.items()):
        print(f"    skipped x{n}: {reason}")
    if failed:
        print(f"{args.patterns.name}: FAILED ({failed} checks)")
        return 1
    if passed == 0:
        print(f"{args.patterns.name}: nothing was verified (every check skipped) -- treating as failure")
        return 1
    print(f"{args.patterns.name}: all {passed} checks match DuckDB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
