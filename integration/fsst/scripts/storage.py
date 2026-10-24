#!/usr/bin/env python3
"""Physical storage accounting, on identical rows.

Counts every byte a reader would have to keep on disk, not just the compressed
payload: the codes, the row offsets, the symbol table, and the reverse-scan
sidecar if one is used. Then does the same for the LLM-token-table
representation of the *same rows*, from the original TokenLIKE pipeline, so the
two schemes can be compared without mixing datasets.

  python3 scripts/storage.py                  # -> bench/v2-integration/storage.{md,json}
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DATA, ROOT  # noqa: E402

TOKENLIKE_DIR = Path(os.environ.get(
    "TOKENLIKE_DIR", os.path.expanduser("~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE")))

# experiment column -> the original project's column with the same rows
PAIRS = [
    ("clickbench/URL_1m", "clickbench/URL"),
    ("clickbench/Referer_1m", "clickbench/Referer"),
    ("tpch/l_comment_1m", "tpch/l_comment"),
    ("tpch/o_comment_1m", "tpch/o_comment"),
    ("dbtext/urls_100k", "dbtext/urls"),
    ("clickbench/URL_100k", None),
    ("clickbench/URL_10k", None),
    ("clickbench/URL_1k", None),
]


def size(p: Path) -> int:
    return p.stat().st_size if p.exists() else 0


def main() -> int:
    out = {"tokenlike_dir": str(TOKENLIKE_DIR), "columns": []}
    md: list[str] = []
    vocab = size(TOKENLIKE_DIR / "data" / "vocab.bin")

    md.append("### Physical storage, identical rows\n")
    md.append("Every byte a reader has to keep, not just the payload. `.fsst.off` is stored as "
              "`uint64` per row boundary, which is what the loader reads; the `u32 offsets` column "
              "is what the same data would cost with 4-byte offsets, which is sound for any payload "
              "under 4 GiB and is what a real engine would do. The sidecar is **no longer required** "
              "— mode `fsst-rev2` reverse-scans without it (see FINAL_REPORT.md §H) — so it is "
              "listed but excluded from the totals, with the sidecar total shown for comparison.\n")
    md.append("| column | rows | raw bytes | codes | offsets (u64) | offsets (u32) | symbol table "
              "| sidecar (optional) | total, u32 off, no sidecar | ratio, payload only "
              "| ratio, total | ratio, total + sidecar |")
    md.append("|" + "|".join(["---"] * 12) + "|")

    for name, _ in PAIRS:
        meta_p = Path(str(DATA / name) + ".fsst.meta.json")
        if not meta_p.exists():
            continue
        m = json.loads(meta_p.read_text())
        rows, raw = m["rows"], m["raw_bytes"]
        codes = size(Path(str(DATA / name) + ".fsst.cmp"))
        off64 = size(Path(str(DATA / name) + ".fsst.off"))
        off32 = 4 * (rows + 1)
        sym = size(Path(str(DATA / name) + ".fsst.sym"))
        side = size(Path(str(DATA / name) + ".fsst.rev"))
        total = codes + off32 + sym
        md.append(f"| `{name}` | {rows:,} | {raw:,} | {codes:,} | {off64:,} | {off32:,} | {sym:,} "
                  f"| {side:,} | {total:,} | {raw / codes:.3f}x | {raw / total:.3f}x "
                  f"| {raw / (total + side):.3f}x |")
        out["columns"].append({"column": name, "rows": rows, "raw_bytes": raw, "codes": codes,
                               "offsets_u64": off64, "offsets_u32": off32, "symbol_table": sym,
                               "sidecar": side, "total_u32_no_sidecar": total,
                               "ratio_payload": raw / codes, "ratio_total": raw / total,
                               "ratio_total_with_sidecar": raw / (total + side)})
    md.append("")

    md.append("### Same rows, FSST versus the LLM token table\n")
    md.append(f"The token-table columns come from the original TokenLIKE pipeline over the identical "
              f"canonical parquet rows. Its vocabulary (`data/vocab.bin`, {vocab:,} B) is **shared by "
              f"every column**, so it is shown separately rather than charged to each one; FSST's "
              f"symbol table is per column and is charged. Both use `u32` offsets here, so the "
              f"comparison is of payload plus per-column metadata.\n")
    md.append("| column | rows | raw bytes | FSST codes + table | token ids (`uint16`) "
              "| FSST total (u32 off) | token total (u32 off) | FSST ratio | token ratio | FSST / token |")
    md.append("|" + "|".join(["---"] * 10) + "|")
    for name, tl in PAIRS:
        if not tl:
            continue
        meta_p = Path(str(DATA / name) + ".fsst.meta.json")
        tok_p = TOKENLIKE_DIR / "data" / (tl + ".tok")
        if not meta_p.exists() or not tok_p.exists():
            continue
        m = json.loads(meta_p.read_text())
        rows, raw = m["rows"], m["raw_bytes"]
        codes = size(Path(str(DATA / name) + ".fsst.cmp"))
        sym = size(Path(str(DATA / name) + ".fsst.sym"))
        off32 = 4 * (rows + 1)
        tok = size(tok_p)
        fsst_total = codes + sym + off32
        tok_total = tok + off32
        md.append(f"| `{name}` | {rows:,} | {raw:,} | {codes + sym:,} | {tok:,} | {fsst_total:,} "
                  f"| {tok_total:,} | {raw / fsst_total:.3f}x | {raw / tok_total:.3f}x "
                  f"| {tok_total / fsst_total:.3f}x |")
        out.setdefault("vs_token_table", []).append(
            {"column": name, "tokenlike_column": tl, "rows": rows, "raw_bytes": raw,
             "fsst_codes": codes, "fsst_symbol_table": sym, "token_ids": tok,
             "offsets_u32": off32, "fsst_total": fsst_total, "token_total": tok_total,
             "shared_vocab_bytes": vocab})
    md.append("")
    md.append(f"The token table's {vocab:,}-byte vocabulary is amortised across however many columns "
              f"a deployment has. Charged in full to a single 1M-row column it would add about "
              f"{100.0 * vocab / (size(Path(str(DATA / 'clickbench/URL_1m') + '.fsst.cmp')) or 1):.1f}% "
              f"to that column's FSST-equivalent footprint; across the four 1M-row columns measured "
              f"here it is under 0.4% each. FSST's per-column tables total "
              f"{sum(c['symbol_table'] for c in out['columns']):,} B for all "
              f"{len(out['columns'])} columns.")
    md.append("")

    dest = ROOT / "bench" / "v2-integration"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "storage.md").write_text("\n".join(md) + "\n")
    (dest / "storage.json").write_text(json.dumps(out, indent=1) + "\n")
    print("\n".join(md))
    print(f"wrote {dest/'storage.md'} and {dest/'storage.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
