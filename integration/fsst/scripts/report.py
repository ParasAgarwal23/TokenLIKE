#!/usr/bin/env python3
"""Turns bench.py's JSON into the markdown tables used in FINAL_REPORT.md."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

MODES = ["raw", "raw-opt", "decode", "fsst", "fsst-compact", "fsst-rev", "fsst-rev2"]


def fmt(x, nd=2):
    return "-" if x is None else f"{x:,.{nd}f}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("json", type=Path)
    ap.add_argument("--title", default="Per-pattern performance")
    args = ap.parse_args()
    doc = json.loads(args.json.read_text())
    rows = doc["rows"]
    by = {}
    order = []
    for r in rows:
        if r["name"] not in by:
            by[r["name"]] = {}
            order.append(r["name"])
        by[r["name"]][r.get("mode", "?")] = r

    env = doc["env"]
    print(f"### {args.title}\n")
    print(f"Machine: {env.get('cpu', env['processor'])}, {env['machine']}  ")
    print(f"Best of {env['runs']} runs after {env['warmup']} warmups; {env['note']}.\n")

    head = (["pattern", "matches"] + [f"{m} ns/row" for m in MODES]
            + ["best compressed", "fair raw = min(raw, raw-opt)", "compressed vs fair raw",
               "compressed vs decode"])
    print("| " + " | ".join(head) + " |")
    print("|" + "|".join(["---"] * len(head)) + "|")
    for name in order:
        g = by[name]
        ref = g.get("fsst", {})
        pat = ref.get("pattern_text", g[list(g)[0]].get("pattern_text", ""))
        cells = [f"`{pat}`" if pat else "`` (empty)", fmt(ref.get("matches"), 0)]
        vals = {}
        for m in MODES:
            r = g.get(m, {})
            if "skipped" in r or "error" in r or "ns_per_row" not in r:
                cells.append("n/a")
                continue
            vals[m] = r["ns_per_row"]
            cells.append(fmt(r["ns_per_row"]))
        dec = vals.get("decode")
        # "best compressed" excludes fsst-compact, which the measurements show is
        # strictly worse; a planner would never choose it.
        comp = {k: v for k, v in vals.items() if k.startswith("fsst") and k != "fsst-compact"}
        best = min(comp.values(), default=None)
        bestname = min(comp, key=comp.get) if comp else None
        fair = min([v for k, v in vals.items() if k in ("raw", "raw-opt")], default=None)
        cells.append(f"{fmt(best)} ({bestname})" if best else "-")
        cells.append(fmt(fair) if fair else "-")
        cells.append(fmt(fair / best, 2) + "x" if best and fair else "-")
        cells.append(fmt(dec / best, 2) + "x" if best and dec else "-")
        print("| " + " | ".join(cells) + " |")

    print("\n### Automaton construction and table size\n")
    head2 = ["pattern", "subset states", "minimal states", "canonical states (fwd)",
             "canonical states (rev)", "DFA build us", "lift build us", "table bytes",
             "units/row (fsst)", "early-exit rows", "fallback rows (fsst-rev)",
             "fallback rows (fsst-rev2)"]
    print("| " + " | ".join(head2) + " |")
    print("|" + "|".join(["---"] * len(head2)) + "|")
    for name in order:
        g = by[name]
        f = g.get("fsst", {})
        rv = g.get("fsst-rev", {})
        rv2 = g.get("fsst-rev2", {})
        if "dfa_states" not in f:
            continue
        print("| " + " | ".join([
            f"`{f.get('pattern_text','')}`" or "``",
            fmt(f.get("dfa_states_subset"), 0), fmt(f.get("dfa_states_minimal"), 0),
            fmt(f["dfa_states"], 0), fmt(rv.get("rev_dfa_states"), 0),
            fmt(f["dfa_build_ns"] / 1000.0, 1), fmt(f["lift_build_ns"] / 1000.0, 1),
            fmt(f["table_bytes"], 0), fmt(f["units_per_row"], 2), fmt(f["early_rows"], 0),
            fmt(rv.get("fallback_rows"), 0), fmt(rv2.get("fallback_rows"), 0),
        ]) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
