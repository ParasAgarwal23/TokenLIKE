#!/usr/bin/env python3
"""Same machine, same rows: FSST-LIKE against the original TokenLIKE.

Both systems are run here, in this session, on this laptop, over columns derived
from the same raw bytes -- so the two sets of numbers are comparable in a way
that numbers copied from a different machine would not be.

TokenLIKE is built from a read-only `git archive` snapshot of the original
repository (third_party/tokenlike-snapshot) and reads its own token columns out
of the original data/ directory. It is never written to.

Both CLIs are asked for their *minimum* per-row time over the same number of
runs, since that is the only statistic both report.

  python3 scripts/compare_tokenlike.py --out bench/vs_tokenlike.json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import BUILD, DATA, ROOT, parse_pattern_file, read_ids  # noqa: E402

# experiment column -> original TokenLIKE column prefix (relative to its data/)
COLUMN_MAP = {
    "clickbench/URL_1m": "clickbench/URL",
    "clickbench/Referer_1m": "clickbench/Referer",
    "tpch/l_comment_1m": "tpch/l_comment",
    "tpch/o_comment_1m": "tpch/o_comment",
}
FSST_MODES = ["raw", "raw-opt", "decode", "fsst", "fsst-rev", "fsst-rev2"]
TL_MODES = ["raw", "decode", "token"]


def run_json(argv: list[str]) -> dict | None:
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        return {"error": f"exit {proc.returncode}: {proc.stderr.strip()[:300]}"}
    for line in proc.stdout.splitlines():
        if line.strip().startswith("{"):
            return json.loads(line)
    return {"error": "no JSON line"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--patterns", type=Path, default=ROOT / "patterns" / "bench.txt")
    import os
    ap.add_argument("--tokenlike", default=os.environ.get(
        "TOKENLIKE_DIR", os.path.expanduser("~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE")))
    # The TokenLIKE CLI built from this very branch, so both systems come from
    # one source tree and one compiler invocation.
    ap.add_argument("--tl-cli", default=str(BUILD / "tokenlike"))
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "vs_tokenlike.json")
    args = ap.parse_args()

    fsst_cli = str(BUILD / "fsst_like")
    tl_cli = args.tl_cli
    tl_data = Path(args.tokenlike) / "data"
    for p in (fsst_cli, tl_cli):
        if not Path(p).is_file():
            raise SystemExit(f"{p} is not built")
    vocab = tl_data / "vocab.bin"
    if not vocab.is_file():
        raise SystemExit(f"{vocab} is missing; TokenLIKE cannot run without its vocabulary")

    specs = [s for s in parse_pattern_file(args.patterns) if s.column in COLUMN_MAP and not s.is_concat]
    rows = []
    agree = disagree = 0
    with tempfile.TemporaryDirectory() as td:
        a_ids, b_ids = Path(td) / "a.bin", Path(td) / "b.bin"
        for spec in specs:
            tl_col = tl_data / COLUMN_MAP[spec.column]
            rec = {"name": spec.name, "pattern": spec.pattern, "column": spec.column,
                   "fsst": {}, "tokenlike": {}}
            sets: dict[str, set] = {}
            for mode in FSST_MODES:
                r = run_json([fsst_cli, "--mode", mode, "--col", str(DATA / spec.column),
                              "--pattern", spec.pattern, "--runs", str(args.runs),
                              "--warmup", str(args.warmup), "--rowids", str(a_ids), "--json"])
                if r and "error" not in r:
                    rec["fsst"][mode] = {"ns_per_row": r["ns_per_row"], "matches": r["matches"],
                                         "units_per_row": r["units_per_row"],
                                         "dfa_states": r["dfa_states"],
                                         "build_us": (r["dfa_build_ns"] + r["lift_build_ns"]) / 1000.0,
                                         "table_bytes": r["table_bytes"]}
                    sets["fsst:" + mode] = set(read_ids(a_ids))
                else:
                    rec["fsst"][mode] = r
            for mode in TL_MODES:
                r = run_json([tl_cli, "--mode", mode, "--col", str(tl_col), "--vocab", str(vocab),
                              "--pattern", spec.pattern, "--runs", str(args.runs),
                              "--warmup", str(args.warmup), "--rowids", str(b_ids)])
                if r and "error" not in r:
                    # TokenLIKE reports median in ns_per_row and the minimum separately.
                    rec["tokenlike"][mode] = {"ns_per_row": r.get("ns_per_row_min", r["ns_per_row"]),
                                              "ns_per_row_median": r["ns_per_row"],
                                              "matches": r["matches"],
                                              "tokens_per_row": r.get("tokens_per_row"),
                                              "table_kb": r.get("table_kb"),
                                              "build_us": r.get("build_us"),
                                              "loaders": r.get("loaders")}
                    sets["tokenlike:" + mode] = set(read_ids(b_ids))
                else:
                    rec["tokenlike"][mode] = r
            keys = list(sets)
            same = all(sets[k] == sets[keys[0]] for k in keys) if keys else False
            rec["row_ids_identical_across_both_systems"] = same
            rec["modes_compared"] = keys
            if same:
                agree += 1
            else:
                disagree += 1
                rec["set_sizes"] = {k: len(v) for k, v in sets.items()}
            rows.append(rec)
            fb = rec["fsst"].get("fsst", {}).get("ns_per_row")
            fr = rec["fsst"].get("fsst-rev", {}).get("ns_per_row")
            tb = rec["tokenlike"].get("token", {}).get("ns_per_row")
            best = min([x for x in (fb, fr) if x is not None], default=None)
            ratio = f"{tb / best:5.2f}x" if best and tb else "  -  "
            print(f"  {spec.name:22s} fsst={fb if fb is None else round(fb,2):>8} "
                  f"fsst-rev={fr if fr is None else round(fr,2):>8} "
                  f"tokenlike-token={tb if tb is None else round(tb,2):>8}  "
                  f"best-fsst vs token {ratio}  ids-identical={same}", flush=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"patterns": len(rows), "agree": agree, "disagree": disagree,
                                    "runs": args.runs, "rows": rows}, indent=1) + "\n")
    print(f"\n{agree} of {len(rows)} patterns: every mode of both systems returned the same row ids")
    if disagree:
        print(f"{disagree} pattern(s) DISAGREE -- see {args.out}")
    print(f"wrote {args.out}")
    return 1 if disagree else 0


if __name__ == "__main__":
    raise SystemExit(main())
