#!/usr/bin/env python3
"""Produces the measured tables that FINAL_REPORT.md quotes.

Writes bench/tables.md and bench/analysis.json. Everything in it comes from a
process run here; nothing is transcribed by hand.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import BUILD, DATA, ROOT  # noqa: E402

CLI = str(BUILD / "fsst_like")
COLUMNS = ["clickbench/URL_1k", "clickbench/URL_10k", "clickbench/URL_100k", "clickbench/URL_1m",
           "clickbench/Referer_1m", "tpch/l_comment_1m", "tpch/o_comment_1m",
           "dbtext/urls_100k", "fuzz/values", "sample/urls_1k", "edge/values"]
TABLE_PATTERNS = [("prefix", "http://%"), ("suffix", "%.php"), ("contains", "%yandex%"),
                  ("underscore", "%/v_/%"), ("complex", "http%irr%php%"),
                  ("long literal", "http://irr.ru/index.php?showalbum/login-leniya7777294,938303130")]
REV_PATTERNS = [("%.php", "suffix"), ("%.html", "suffix"), ("%.m3u8", "suffix"),
                ("http://irr.ru/index.php?showalbum/login-leniya7777294,938303130", "exact"),
                ("%yandex%", "contains")]


def q(mode: str, col: str, pattern: str, extra: list[str] | None = None) -> dict:
    argv = [CLI, "--mode", mode, "--col", str(DATA / col), "--pattern", pattern,
            "--runs", "7", "--warmup", "2", "--json"] + (extra or [])
    p = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise SystemExit(f"{' '.join(argv)}\n{p.stderr}")
    return json.loads([l for l in p.stdout.splitlines() if l.startswith("{")][0])


def main() -> int:
    out: dict = {}
    md: list[str] = []

    # ---- compression, per column -------------------------------------------
    md.append("### Compression on real data (upstream FSST, one symbol table per column)\n")
    md.append("| column | rows | original B | codes B | symbol table B | .rev B | ratio (codes) "
              "| ratio (codes+table+.rev) | symbols | escape units | % escape units | rows with escape |")
    md.append("|" + "|".join(["---"] * 12) + "|")
    cols = {}
    for name in COLUMNS:
        mp = Path(str(DATA / name) + ".fsst.meta.json")
        if not mp.exists():
            continue
        m = json.loads(mp.read_text())
        units = m["symbol_units"] + m["escape_units"]
        total = m["cmp_bytes"] + m["sym_bytes"] + m["rev_bytes"]
        cols[name] = m
        md.append(f"| `{name}` | {m['rows']:,} | {m['raw_bytes']:,} | {m['cmp_bytes']:,} | "
                  f"{m['sym_bytes']:,} | {m['rev_bytes']:,} | {m['ratio']:.3f}x | "
                  f"{m['raw_bytes'] / total:.3f}x | {m['nsymbols']} | {m['escape_units']:,} | "
                  f"{100.0 * m['escape_units'] / max(units, 1):.2f}% | {m['rows_with_escape']:,} |")
    out["columns"] = cols
    md.append("")

    # ---- dense vs compact ---------------------------------------------------
    md.append("### Dense versus compact lifted tables (clickbench/URL_1m)\n")
    md.append("The DFA's own 256-wide byte table is in both figures, since the escape path needs it.\n")
    md.append("| pattern | states | dense ns/row | compact ns/row | dense total table B "
              "| compact total table B | compact/dense size | compact/dense time |")
    md.append("|" + "|".join(["---"] * 8) + "|")
    dc = []
    for label, pat in TABLE_PATTERNS:
        d = q("fsst", "clickbench/URL_1m", pat)
        c = q("fsst-compact", "clickbench/URL_1m", pat)
        dc.append({"pattern": pat, "label": label, "dense": d, "compact": c})
        md.append(f"| `{pat if len(pat) < 40 else pat[:37] + '...'}` ({label}) | {d['dfa_states']} | "
                  f"{d['ns_per_row']:.2f} | {c['ns_per_row']:.2f} | {d['table_bytes']:,} | "
                  f"{c['table_bytes']:,} | {c['table_bytes'] / d['table_bytes']:.2f}x | "
                  f"{c['ns_per_row'] / d['ns_per_row']:.2f}x |")
    out["dense_vs_compact"] = dc
    md.append("")

    # ---- reverse-scan metadata ---------------------------------------------
    m = cols["clickbench/URL_1m"]
    md.append("### Cost of making the backward scan exact (clickbench/URL_1m)\n")
    md.append(f"The 4-byte-per-row `.fsst.rev` sidecar is {m['rev_bytes']:,} B, "
              f"{100.0 * m['rev_bytes'] / m['cmp_bytes']:.1f}% on top of the "
              f"{m['cmp_bytes']:,} B of codes. A 1-bit-per-row has-escape flag would be "
              f"{(m['rows'] + 7) // 8:,} B, {100.0 * ((m['rows'] + 7) // 8) / m['cmp_bytes']:.2f}%, "
              f"but then every one of the {m['rows_with_escape']:,} rows with an escape "
              f"({100.0 * m['rows_with_escape'] / m['rows']:.1f}%) has to be scanned forward instead.\n")
    md.append("| pattern | shape | 4-byte sidecar ns/row | 1-bit flag ns/row | **no sidecar** ns/row "
              "| sidecar fallbacks | 1-bit fallbacks | no-sidecar fallbacks | sidecar units/row "
              "| 1-bit units/row | no-sidecar units/row |")
    md.append("|" + "|".join(["---"] * 11) + "|")
    rev = []
    for pat, shape in REV_PATTERNS:
        a = q("fsst-rev", "clickbench/URL_1m", pat, ["--rev-mode", "offset"])
        b = q("fsst-rev", "clickbench/URL_1m", pat, ["--rev-mode", "bit"])
        c = q("fsst-rev2", "clickbench/URL_1m", pat)
        rev.append({"pattern": pat, "shape": shape, "offset": a, "bit": b, "none": c})
        md.append(f"| `{pat if len(pat) < 40 else pat[:37] + '...'}` | {shape} | {a['ns_per_row']:.2f} | "
                  f"{b['ns_per_row']:.2f} | {c['ns_per_row']:.2f} | {a['fallback_rows']:,} | "
                  f"{b['fallback_rows']:,} | {c['fallback_rows']:,} | {a['units_per_row']:.2f} | "
                  f"{b['units_per_row']:.2f} | {c['units_per_row']:.2f} |")
    out["rev_modes"] = rev
    md.append("")

    # ---- encoder cost -------------------------------------------------------
    md.append("### Compression cost (upstream FSST, single-threaded)\n")
    md.append("| column | rows | fsst_create ms | fsst_compress ms | MB/s compressed |")
    md.append("|" + "|".join(["---"] * 5) + "|")
    for name, m in cols.items():
        if m["rows"] < 100000:
            continue
        mb = m["raw_bytes"] / 1e6
        md.append(f"| `{name}` | {m['rows']:,} | {m['fsst_create_ms']:.0f} | "
                  f"{m['fsst_compress_ms']:.0f} | {mb / (m['fsst_compress_ms'] / 1000.0):,.0f} |")
    md.append("")

    # ---- peak RSS -----------------------------------------------------------
    bj = ROOT / "bench" / "v2-integration" / "bench.json"
    if bj.exists():
        rows = json.loads(bj.read_text())["rows"]
        per_mode: dict[str, list[int]] = {}
        for r in rows:
            if r.get("peak_rss_kb"):
                per_mode.setdefault(r["mode"], []).append(r["peak_rss_kb"])
        if per_mode:
            md.append("### Peak resident set size, whole process (clickbench/URL_1m unless noted)\n")
            md.append("| mode | min MiB | median MiB | max MiB | what it has to hold |")
            md.append("|" + "|".join(["---"] * 5) + "|")
            notes = {"raw": "88 MB raw column + offsets",
                     "decode": "50 MB codes + offsets + one decode buffer",
                     "fsst": "50 MB codes + offsets + lifted table",
                     "fsst-compact": "50 MB codes + offsets + compact table",
                     "fsst-rev": "50 MB codes + offsets + .rev + two lifted tables"}
            for mode, vs in per_mode.items():
                vs = sorted(vs)
                md.append(f"| {mode} | {vs[0] / 1024:.1f} | {vs[len(vs) // 2] / 1024:.1f} | "
                          f"{vs[-1] / 1024:.1f} | {notes.get(mode, '')} |")
            out["peak_rss_kb"] = per_mode
            md.append("")

    dest = ROOT / "bench" / "v2-integration"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "tables.md").write_text("\n".join(md) + "\n")
    (dest / "analysis.json").write_text(json.dumps(out, indent=1) + "\n")
    print("\n".join(md))
    print(f"\nwrote {dest / 'tables.md'} and {dest / 'analysis.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
