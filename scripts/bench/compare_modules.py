#!/usr/bin/env python3
"""Same-machine comparison of TokenLIKE and FSST-LIKE on identical rows.

    python scripts/bench/compare_modules.py --out results/integration-v4/<cpu>/compare.json

Both tools scan the same 1,000,000 ClickBench URL rows with the same patterns.
Every process is created suspended, pinned to the same logical CPU and then
resumed (identical, external pinning for both tools). Each (tool, mode,
pattern) runs in --repeats separate processes; every process's row ids must be
byte-identical to the DuckDB truth file, or the cell is reported as failed and
not timed. Statistics are kept per process:

  tokenlike CLI : median and min (best) over --runs warm scans after 1 warmup
  fsst_like     : min (best) and mean over --runs scans after 1 warmup

so "best" is the like-for-like statistic between the two tools.
Storage is computed from the files on disk.
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))
import run_bench as rb  # noqa: E402

PATTERNS = [  # (name in data/truth/clickbench, pattern, class)
    ("p_http", "http://%", "prefix"),
    ("p_irr", "http://irr.ru%", "prefix"),
    ("s_php", "%.php", "suffix"),
    ("s_html", "%.html", "suffix"),
    ("e_top_url", "http://irr.ru/index.php?showalbum/login-leniya7777294,938303130", "exact"),
    ("i_yandex", "%yandex%", "contains"),
    ("i_v_under", "%/v_/%", "contains + _"),
    ("u8_sezon", "%сезон%", "contains, Cyrillic"),
]
TOKEN_MODES = ["token", "decode", "raw"]
FSST_MODES = ["fsst", "fsst-rev2", "decode", "raw-opt"]


def run_pinned(cmd: list[str], cpu: int) -> subprocess.CompletedProcess:
    """Start suspended, pin the whole process to `cpu`, resume (Windows); plain run elsewhere."""
    if os.name != "nt" or cpu < 0:
        return subprocess.run(cmd, capture_output=True, timeout=900)
    CREATE_SUSPENDED = 0x00000004
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=CREATE_SUSPENDED)
    handle = int(p._handle)  # noqa: SLF001
    if not ctypes.windll.kernel32.SetProcessAffinityMask(handle, ctypes.c_size_t(1 << cpu)):
        p.kill()
        raise SystemExit(f"cannot pin to CPU {cpu}")
    ctypes.windll.ntdll.NtResumeProcess(handle)
    out, err = p.communicate(timeout=900)
    return subprocess.CompletedProcess(cmd, p.returncode, out, err)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measure(tool: str, mode: str, pattern: str, truth: Path, args, tmp: Path) -> dict:
    ids = tmp / "out.ids"
    procs = []
    for _ in range(args.repeats):
        if ids.exists():
            ids.unlink()
        if tool == "tokenlike":
            cmd = [str(args.tokenlike), "--mode", mode, "--pattern", pattern, "--col", str(ROOT / "data/clickbench/URL"),
                   "--vocab", str(ROOT / "data/vocab.bin"), "--runs", str(args.runs), "--warmup", "1", "--rowids", str(ids)]
        else:
            cmd = [str(args.fsst), "--mode", mode, "--pattern", pattern, "--col",
                   str(ROOT / "integration/fsst/data/clickbench/URL_1m"), "--runs", str(args.runs), "--warmup", "1",
                   "--rowids", str(ids), "--json"]
        p = run_pinned(cmd, args.cpu)
        if p.returncode != 0:
            return {"status": "error", "exit": p.returncode, "stderr": p.stderr.decode("utf-8", "replace")[-300:]}
        js = json.loads(p.stdout.decode("utf-8").strip().splitlines()[-1])
        if sha(ids) != sha(truth):
            return {"status": "MISMATCH", "matches": js.get("matches")}
        procs.append(js)
    r = {"status": "ok", "matches": procs[0]["matches"], "processes": len(procs)}
    if tool == "tokenlike":
        r["median_ns_per_row"] = statistics.median(p["ns_per_row"] for p in procs)
        r["best_ns_per_row"] = min(p["ns_per_row_min"] for p in procs)
        r["first_scan_ns_per_row"] = statistics.median(p["first_scan_ns"] / p["rows"] for p in procs)
        r["build_us"] = statistics.median(p["build_us"] for p in procs)
        r["cold_total_ms"] = statistics.median(p["cold_total_ms"] for p in procs)
        r["units_per_row"] = procs[0]["tokens_per_row"]
        r["process_median_ns_per_row"] = [p["ns_per_row"] for p in procs]
    else:
        r["best_ns_per_row"] = min(p["best_ns"] / p["rows"] for p in procs)
        r["mean_ns_per_row"] = statistics.median(p["mean_ns"] / p["rows"] for p in procs)
        r["build_us"] = statistics.median((p["dfa_build_ns"] + p["lift_build_ns"]) / 1000 for p in procs)
        r["units_per_row"] = procs[0]["units_per_row"]
        r["table_bytes"] = procs[0]["table_bytes"]
        r["process_best_ns_per_row"] = [p["best_ns"] / p["rows"] for p in procs]
    return r


def storage() -> dict:
    def size(p: str) -> int:
        f = ROOT / p
        return f.stat().st_size if f.exists() else 0
    rows = size("data/clickbench/URL.off") // 8 - 1
    raw, rawoff = size("data/clickbench/URL.raw"), size("data/clickbench/URL.rawoff")
    tok, tokoff, vocab = size("data/clickbench/URL.tok"), size("data/clickbench/URL.off"), size("data/vocab.bin")
    f = "integration/fsst/data/clickbench/URL_1m.fsst"
    cmp_, foff, sym, rev = size(f + ".cmp"), size(f + ".off"), size(f + ".sym"), size(f + ".rev")
    return {
        "rows": rows,
        "raw_text_bytes": raw,
        "raw_text_plus_u64_offsets": raw + rawoff,
        "token_ids_bytes": tok,
        "token_ids_plus_u64_offsets": tok + tokoff,
        "token_ids_plus_u64_offsets_plus_vocab": tok + tokoff + vocab,
        "fsst_codes_bytes": cmp_,
        "fsst_codes_plus_u64_offsets_plus_symbol_table": cmp_ + foff + sym,
        "fsst_with_rev_sidecar": cmp_ + foff + sym + rev,
        "notes": "offsets as stored on disk (uint64, rows+1). vocab.bin is shared by every token column; "
                 "the FSST symbol table is per column. The .fsst.rev sidecar is only needed by fsst-rev.",
    }


def main() -> int:
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--cpu", type=int, default=2, help="logical CPU for every process (-1 = no pinning)")
    exe = ".exe" if os.name == "nt" else ""
    ap.add_argument("--tokenlike", type=Path, default=ROOT / "build" / f"tokenlike{exe}")
    ap.add_argument("--fsst", type=Path, default=ROOT / "build-fsst" / f"fsst_like{exe}")
    args = ap.parse_args()
    out = ROOT / args.out
    if out.exists() and not args.overwrite:
        raise SystemExit(f"{args.out} exists; refusing to overwrite (use --overwrite or a new --out)")

    results = []
    with tempfile.TemporaryDirectory(prefix="tl-compare-") as tmpd:
        tmp = Path(tmpd)
        for name, pattern, cls in PATTERNS:
            truth = ROOT / "data/truth/clickbench" / f"{name}.ids"
            row = {"name": name, "pattern": pattern, "class": cls, "truth_matches": truth.stat().st_size // 4}
            for tool, modes in (("tokenlike", TOKEN_MODES), ("fsst_like", FSST_MODES)):
                for mode in modes:
                    r = measure(tool, mode, pattern, truth, args, tmp)
                    row[f"{tool}:{mode}"] = r
                    shown = f"best {r['best_ns_per_row']:.2f}" if r["status"] == "ok" else r["status"]
                    print(f"  {name:<10} {tool + ':' + mode:<20} {shown}", flush=True)
            results.append(row)

    meta = {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "git_commit": rb.git("rev-parse", "HEAD"),
        "git_dirty": bool(rb.git("status", "--porcelain", "--untracked-files=no", "--", ".", ":!results")),
        "cpu": rb.cpu_name(), "logical_cpus": os.cpu_count(), "os": platform.platform(),
        "pinned_cpu": args.cpu, "runs": args.runs, "repeats": args.repeats,
        "tokenlike_build": rb.cmake_cache(ROOT / "build"),
        "fsst_build": rb.cmake_cache(ROOT / "build-fsst"),
        "method": __doc__.strip(),
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"meta": meta, "storage": storage(), "results": results}, indent=2,
                              ensure_ascii=False) + "\n", encoding="utf-8")
    bad = [(r["name"], k) for r in results for k, v in r.items() if isinstance(v, dict) and v.get("status") != "ok"]
    print(f"written {args.out}; non-ok cells: {bad or 'none'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
