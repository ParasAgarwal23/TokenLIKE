#!/usr/bin/env python3
"""Benchmark runner for the FSST-LIKE experiment.

Each (pattern, mode) is one fsst_like process reporting --json. The CLI itself
does the warmup/repeat loop and reports the best and mean wall time of its
scans, so process startup and column loading are outside the measurement.
Automaton construction is reported separately, and also added back in for the
end-to-end column.

  python3 scripts/bench.py --patterns patterns/bench.txt --out bench/bench.json

Peak RSS is measured with /usr/bin/time -l on macOS (-v on Linux) for one run
per mode, since it covers the whole process, not just the scan.
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DATA, ROOT, parse_pattern_file, resolve_cli  # noqa: E402

ALL_MODES = ["raw", "raw-opt", "decode", "fsst", "fsst-compact", "fsst-rev", "fsst-rev2"]


def run(cli: str, col: str, pattern: str, mode: str, runs: int, warmup: int, col2: str | None) -> dict:
    argv = [cli, "--mode", mode, "--col", str(DATA / col), "--pattern", pattern,
            "--runs", str(runs), "--warmup", str(warmup), "--json"]
    if col2:
        argv += ["--col2", str(DATA / col2)]
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode == 2:
        return {"mode": mode, "skipped": proc.stderr.strip()}
    if proc.returncode != 0:
        return {"mode": mode, "error": proc.stderr.strip()}
    for line in proc.stdout.splitlines():
        if line.strip().startswith("{"):
            return json.loads(line)
    return {"mode": mode, "error": "no JSON line in output"}


def peak_rss_kb(cli: str, col: str, pattern: str, mode: str) -> int | None:
    """Peak resident set size of one whole fsst_like process, in KiB."""
    if platform.system() == "Darwin":
        argv = ["/usr/bin/time", "-l", cli, "--mode", mode, "--col", str(DATA / col),
                "--pattern", pattern, "--runs", "1", "--json"]
        key = r"(\d+)\s+maximum resident set size"
        scale = 1 / 1024  # macOS reports bytes
    else:
        argv = ["/usr/bin/time", "-v", cli, "--mode", mode, "--col", str(DATA / col),
                "--pattern", pattern, "--runs", "1", "--json"]
        key = r"Maximum resident set size \(kbytes\):\s*(\d+)"
        scale = 1
    try:
        proc = subprocess.run(argv, capture_output=True, text=True)
    except FileNotFoundError:
        return None
    m = re.search(key, proc.stderr)
    return int(int(m.group(1)) * scale) if m else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--patterns", type=Path, default=ROOT / "patterns" / "bench.txt")
    ap.add_argument("--modes", default=",".join(ALL_MODES))
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--rss", action="store_true", help="also measure peak RSS (one extra process per row)")
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "bench.json")
    args = ap.parse_args()

    cli = resolve_cli()
    if not Path(cli).is_file():
        raise SystemExit(f"no CLI at {cli}")
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    specs = parse_pattern_file(args.patterns)

    env = {
        "machine": platform.platform(),
        "processor": platform.processor() or platform.machine(),
        "python": platform.python_version(),
        "when": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "cli": cli,
        "runs": args.runs,
        "warmup": args.warmup,
        "note": "best-of-N wall time of the scan only; column load and automaton build are outside it",
    }
    if platform.system() == "Darwin":
        env["cpu"] = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                    capture_output=True, text=True).stdout.strip()

    rows = []
    for spec in specs:
        for mode in modes:
            r = run(cli, spec.column, spec.pattern, mode, args.runs, args.warmup, spec.column2)
            r["name"] = spec.name
            r["pattern_text"] = spec.pattern
            if args.rss and "error" not in r and "skipped" not in r and not spec.column2:
                r["peak_rss_kb"] = peak_rss_kb(cli, spec.column, spec.pattern, mode)
            rows.append(r)
            tag = ("SKIP" if "skipped" in r else "ERR" if "error" in r
                   else f"{r['ns_per_row']:9.3f} ns/row  m={r['matches']}")
            print(f"  {spec.name:22s} {mode:13s} {tag}", flush=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"env": env, "rows": rows}, indent=1) + "\n")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
