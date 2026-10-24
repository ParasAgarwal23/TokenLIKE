#!/usr/bin/env python3
"""End-to-end demonstration of LIKE matching on FSST-compressed data.

    python3 demo_fsst.py                      # 100,000 ClickBench URLs
    python3 demo_fsst.py --rows 1000000       # the full column
    python3 demo_fsst.py --no-duckdb          # skip the DuckDB cross-check

It compresses a real column with the upstream FSST encoder in this run (not a
pre-built artifact), then evaluates the same patterns three ways -- over the
compressed codes, over fully decompressed strings, and over the original raw
strings -- and compares the row-id sets it actually gets back. Nothing here
prints a verdict it did not compute.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "scripts"))
from common import DATA, ENCODER, ROOT, read_ids, resolve_cli  # noqa: E402

PATTERNS = [
    ("prefix", "http://%"),
    ("prefix, long", "http://irr.ru%"),
    ("suffix", "%.php"),
    ("suffix", "%.html"),
    ("contains", "%yandex%"),
    ("one Unicode character wildcard", "%/v_/%"),
    ("Cyrillic contains", "%сезон%"),
    ("mixed wildcards", "http%irr%php%"),
]
MODES = ["raw", "raw-opt", "decode", "fsst", "fsst-rev2"]


def sh(argv: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True)


def human(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(n) < 1024 or unit == "GiB":
            return f"{n:,.1f} {unit}" if unit != "B" else f"{int(n):,} B"
        n /= 1024
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--source", default="clickbench/URL",
                    help="raw column under the original TokenLIKE data/ directory")
    ap.add_argument("--tokenlike", default=os.environ.get(
        "TOKENLIKE_DIR", os.path.expanduser("~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE")))
    ap.add_argument("--no-duckdb", action="store_true")
    args = ap.parse_args()

    cli = resolve_cli()
    enc = Path(ENCODER)
    for p in (cli, enc):
        if not Path(p).is_file():
            raise SystemExit(f"{p} is not built; configure the repository with "
                             f"-DTOKENLIKE_WITH_FSST=ON and build it")

    src = Path(args.tokenlike) / "data" / args.source
    if not Path(str(src) + ".raw").is_file():
        raise SystemExit(f"{src}.raw not found; set --tokenlike or --source")

    col = "demo/urls"
    (DATA / "demo").mkdir(parents=True, exist_ok=True)

    print("=" * 78)
    print(f"1. compressing {src}.raw with upstream FSST ({args.rows:,} rows)")
    print("=" * 78)
    t0 = time.perf_counter()
    proc = sh([str(enc), "--raw", str(src), "--out", str(DATA / col), "--rows", str(args.rows)])
    if proc.returncode != 0:
        print(proc.stdout)
        raise SystemExit(f"fsst_encode failed:\n{proc.stderr}")
    meta = json.loads(proc.stdout)
    wall = time.perf_counter() - t0

    raw_b, cmp_b, sym_b = meta["raw_bytes"], meta["cmp_bytes"], meta["sym_bytes"]
    rev_b = meta["rev_bytes"]
    units = meta["symbol_units"] + meta["escape_units"]
    print(f"   rows                 {meta['rows']:,}")
    print(f"   original             {human(raw_b)}")
    print(f"   compressed codes     {human(cmp_b)}")
    print(f"   symbol table         {human(sym_b)}  ({meta['nsymbols']} symbols)")
    print(f"   reverse-scan sidecar {human(rev_b)}  (4 bytes/row, see fsstcol.h)")
    print(f"   compression ratio    {raw_b / cmp_b:.3f}x  on codes alone")
    print(f"                        {raw_b / (cmp_b + sym_b):.3f}x  including the symbol table")
    print(f"                        {raw_b / (cmp_b + sym_b + rev_b):.3f}x  including the sidecar")
    print(f"   escapes              {meta['escape_units']:,} of {units:,} units "
          f"({100.0 * meta['escape_units'] / max(units, 1):.2f}%), "
          f"{meta['rows_with_escape']:,} rows affected")
    print(f"   round-trip           every row decoded and compared: "
          f"{meta['roundtrip_mismatches']} mismatches")
    print(f"   encode wall time     {wall:.2f} s "
          f"(fsst_create {meta['fsst_create_ms']:.0f} ms, fsst_compress {meta['fsst_compress_ms']:.0f} ms)")

    truth = {}
    if not args.no_duckdb:
        try:
            from common import backend_name, connect_duckdb

            con = connect_duckdb()
            pq = str(src) + ".parquet"
            print(f"\n   DuckDB cross-check enabled via {backend_name(con)} on {Path(pq).name}")
            for _, pat in PATTERNS:
                ids = [int(r[0]) for r in con.execute(
                    "SELECT row_id FROM read_parquet(?) WHERE row_id < ? AND value LIKE ? ORDER BY row_id",
                    [pq, meta["rows"], pat]).fetchall()]
                truth[pat] = set(ids)
        except SystemExit as e:
            print(f"\n   DuckDB cross-check unavailable: {e}")

    print()
    print("=" * 78)
    print("2. the same patterns, three ways, over the same rows")
    print("=" * 78)
    hdr = f"{'pattern':44s} {'matches':>9s} " + " ".join(f"{m:>12s}" for m in MODES)
    print(hdr)
    print(f"{'':44s} {'':>9s} " + " ".join(f"{'ns/row':>12s}" for _ in MODES))
    print("-" * len(hdr))

    disagreements = []
    details = {}
    for label, pat in PATTERNS:
        ids_by_mode = {}
        timings = {}
        info_by_mode = {}
        for mode in MODES:
            out = DATA / "demo" / f"ids_{mode}.bin"
            p = sh([str(cli), "--mode", mode, "--col", str(DATA / col), "--pattern", pat,
                    "--rowids", str(out), "--runs", "5", "--warmup", "1", "--json"])
            if p.returncode != 0:
                timings[mode] = None
                continue
            info = json.loads([l for l in p.stdout.splitlines() if l.startswith("{")][0])
            info_by_mode[mode] = info
            timings[mode] = info["ns_per_row"]
            ids_by_mode[mode] = set(read_ids(out))
        details[pat] = info_by_mode

        sets = list(ids_by_mode.values())
        agree = all(s == sets[0] for s in sets) if sets else False
        n = len(sets[0]) if sets else 0
        if not agree:
            disagreements.append((pat, {m: len(s) for m, s in ids_by_mode.items()}))
        if pat in truth and sets and sets[0] != truth[pat]:
            disagreements.append((pat, f"DuckDB says {len(truth[pat])}, we say {n}"))

        # Truncate on characters, never mid-codepoint, and never on a pattern
        # short enough to print whole -- the earlier version cut
        # "one Unicode character wildcard: '%/v_/%'" to "...: '%" and lost it.
        shown = f"{label}: {pat!r}"
        if len(shown) > 44:
            shown = shown[:41] + "..."
        print(f"{shown:44s} {n:9,d} " +
              " ".join(f"{timings[m]:12.2f}" if timings.get(m) is not None else f"{'n/a':>12s}"
                       for m in MODES))

    print()
    print("=" * 78)
    print("3. do the modes agree?")
    print("=" * 78)
    if disagreements:
        print(f"   NO -- {len(disagreements)} disagreement(s):")
        for pat, d in disagreements:
            print(f"     {pat!r}: {d}")
    else:
        checked = len(PATTERNS) * len(MODES)
        extra = f" and with DuckDB on all {len(truth)} patterns" if truth else " (DuckDB check skipped)"
        print(f"   yes -- all {len(MODES)} modes returned identical row-id sets for every one of "
              f"the {len(PATTERNS)} patterns ({checked} scans){extra}")

    print()
    print("=" * 78)
    print("4. which optimisations were actually active")
    print("=" * 78)
    for label, pat in PATTERNS:
        f = details.get(pat, {}).get("fsst")
        rv = details.get(pat, {}).get("fsst-rev2")
        if not f:
            continue
        opts = []
        if f["dead_reachable"]:
            opts.append("early reject")
        if f["asink_reachable"]:
            opts.append("early accept")
        if f["has_underscore"]:
            opts.append("UTF-8 '_' sub-automaton")
        if f["early_rows"]:
            opts.append(f"{100.0 * f['early_rows'] / f['rows']:.0f}% of rows exited early")
        if rv is not None:
            fb = rv.get("fallback_rows", 0)
            # This is a property of the *reverse* scan of this pattern, not of
            # the forward scan whose state counts are printed above. The old
            # wording implied the forward scan had fallen back.
            if fb:
                opts.append(f"fsst-rev2 fell back to a forward scan on {fb:,} rows")
            else:
                opts.append("fsst-rev2 needed no fallback and no sidecar")
        print(f"   {pat!r}")
        print(f"     {f['dfa_states']} canonical states "
              f"({f.get('dfa_states_subset')} from subset construction, "
              f"{f.get('dfa_states_minimal')} minimal), "
              f"lifted table {f['table_bytes']:,} B, built in "
              f"{(f['dfa_build_ns'] + f['lift_build_ns']) / 1000.0:.0f} us")
        print(f"     {f['units_per_row']:.2f} symbol units read per row vs "
              f"{raw_b / meta['rows']:.2f} raw bytes per row")
        print(f"     {', '.join(opts) if opts else 'no early exit available for this shape'}")
    return 1 if disagreements else 0


if __name__ == "__main__":
    raise SystemExit(main())
