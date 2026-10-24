"""Compression accounting and DuckDB storage inspection for the same 1M ClickBench rows.

  A. Logical sizes: raw text bytes + row offsets vs. token-id bytes + row offsets, with the shared
     vocabulary counted separately (it is paid once per dataset, not per row).
  B. Physical sizes inside DuckDB: each column stored alone in a fresh database, with DuckDB's
     default compression and with force_compression='uncompressed', measured as used blocks
     (PRAGMA database_size), plus the per-segment compression mix from pragma_storage_info.

    .venv/Scripts/python -X utf8 integration/duckdb/python/storage_report.py   -> results/storage.json
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402

COLUMNS = [("URL", "original_url", "encoded_url"), ("Referer", "original_referer", "encoded_referer")]


def fsize(p: Path) -> int | None:
    return p.stat().st_size if p.exists() else None


def logical() -> dict:
    out = {"vocabulary": {"file": tlsql.rel(tlsql.VOCAB), "bytes": fsize(tlsql.VOCAB),
                          "note": "shared by every encoded column; paid once, not per row"}}
    con = tlsql.connect(tlsql.DB, read_only=True)
    for col, raw, enc in COLUMNS:
        rows, text, tok, empty = con.execute(
            f"SELECT count(*), sum(strlen({raw})), sum(octet_length({enc})), count(*) FILTER (WHERE {raw} = '') FROM urls").fetchone()
        prefix = tlsql.DATA / "clickbench" / col
        files = {ext: fsize(Path(str(prefix) + ext)) for ext in (".raw", ".rawoff", ".tok", ".off")}
        off64 = 8 * (rows + 1)
        out[col] = {
            "rows": rows, "empty_rows": empty, "text_bytes": text, "token_bytes": tok, "tokens": tok // 2,
            "bytes_per_token": round(text / (tok // 2), 4), "tokens_per_row": round((tok // 2) / rows, 3),
            "offsets_uint64_bytes": off64,
            "text_plus_offsets": text + off64, "tokens_plus_offsets": tok + off64,
            "ratio_tokens_to_text": round(tok / text, 4),
            "ratio_with_offsets": round((tok + off64) / (text + off64), 4),
            "ratio_with_offsets_and_vocab": round((tok + off64 + out["vocabulary"]["bytes"]) / (text + off64), 4),
            "person_b_files": files,
            "files_consistent": files[".raw"] == text and files[".tok"] == tok and files[".rawoff"] == off64 == files[".off"],
        }
    con.close()
    return out


def physical() -> dict:
    out = {}
    scratch = tlsql.SCRATCH / "storage"
    scratch.mkdir(parents=True, exist_ok=True)
    for col, raw, enc in COLUMNS:
        for c in (raw, enc):
            for mode in ("default", "uncompressed"):
                p = scratch / f"{c}_{mode}.duckdb"
                for q in (p, Path(str(p) + ".wal")):
                    if q.exists():
                        q.unlink()
                con = tlsql.connect(p, load=False)
                con.execute(f"ATTACH '{tlsql.DB.as_posix()}' AS src (READ_ONLY)")
                if mode == "uncompressed":
                    con.execute("PRAGMA force_compression='uncompressed'")
                con.execute(f"CREATE TABLE t AS SELECT {c} FROM src.urls ORDER BY row_id")
                con.execute("CHECKPOINT")
                bs, used = con.execute("SELECT block_size, used_blocks FROM pragma_database_size() WHERE database_name = current_database()").fetchone()
                mix = con.execute(f"""SELECT segment_type, compression, count(*) AS segments, sum(count) AS rows
                                      FROM pragma_storage_info('t') WHERE column_name = '{c}' GROUP BY ALL ORDER BY ALL""").fetchall()
                con.close()
                out[f"{c}/{mode}"] = {"used_bytes": bs * used, "used_mb": round(bs * used / 2**20, 2), "block_size": bs,
                                      "segments": [{"type": t, "compression": cm, "segments": s, "rows": r} for t, cm, s, r in mix],
                                      "file_bytes": p.stat().st_size}
                print(f"{c:<18} {mode:<12} {bs * used / 2**20:8.2f} MB  " + ", ".join(f"{cm}:{s}" for t, cm, s, r in mix if t != 'VALIDITY'), flush=True)
    return out


def main_table_mix() -> list[dict]:
    con = tlsql.connect(tlsql.DB, read_only=True, load=False)
    rows = con.execute("""SELECT column_name, segment_type, compression, count(*) AS segments, sum(count) AS rows
                          FROM pragma_storage_info('urls')
                          WHERE column_name IN ('original_url', 'encoded_url', 'original_referer', 'encoded_referer')
                            AND segment_type <> 'VALIDITY'
                          GROUP BY ALL ORDER BY ALL""").fetchall()
    con.close()
    return [{"column": c, "type": t, "compression": cm, "segments": s, "rows": r} for c, t, cm, s, r in rows]


def main() -> int:
    for f in (tlsql.DB, tlsql.VOCAB):
        if not f.exists():
            print(f"ERROR: {tlsql.rel(f)} not found")
            return 2
    rep = {"duckdb": __import__("duckdb").__version__, "rows_compared": "the same 1,000,000 ClickBench rows in every measurement",
           "logical": logical(), "physical_single_column": physical(), "urls_table_compression_mix": main_table_mix(),
           "person_b_fsst": "No Person B FSST results exist in the repository at this commit; DuckDB's own FSST "
                            "(physical_single_column */default) is the FSST comparison available here."}
    ph = rep["physical_single_column"]
    for col, raw, enc in COLUMNS:
        rep["logical"][col]["duckdb_default_text_mb"] = ph[f"{raw}/default"]["used_mb"]
        rep["logical"][col]["duckdb_default_tokens_mb"] = ph[f"{enc}/default"]["used_mb"]
        rep["logical"][col]["duckdb_default_ratio_tokens_to_text"] = round(ph[f"{enc}/default"]["used_bytes"] / ph[f"{raw}/default"]["used_bytes"], 4)
    tlsql.RESULTS.mkdir(exist_ok=True)
    (tlsql.RESULTS / "storage.json").write_text(json.dumps(rep, indent=2), encoding="utf-8")
    for col, *_ in COLUMNS:
        L = rep["logical"][col]
        print(f"{col}: text {L['text_bytes']:,} B, tokens {L['token_bytes']:,} B ({L['ratio_tokens_to_text']}x); "
              f"with uint64 offsets {L['ratio_with_offsets']}x; + vocabulary {L['ratio_with_offsets_and_vocab']}x; "
              f"DuckDB default storage: text {L['duckdb_default_text_mb']} MB vs tokens {L['duckdb_default_tokens_mb']} MB "
              f"({L['duckdb_default_ratio_tokens_to_text']}x); Person B files consistent: {L['files_consistent']}")
    print("wrote", tlsql.rel(tlsql.RESULTS / "storage.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
