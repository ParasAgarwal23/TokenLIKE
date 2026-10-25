"""Render results/bench.json (+ storage.json, test_results.json) as integration/duckdb/RESULTS.md.

    .venv/Scripts/python -X utf8 integration/duckdb/python/report_tables.py [BENCH.json ...] [--results-dir DIR]
        [--out FILE [--overwrite] | --update-reference]

Output: a new build/duckdb/results/RESULTS-<UTC time>.md by default; --update-reference replaces the
committed integration/duckdb/RESULTS.md. An existing file is never replaced without --overwrite.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402


def f(x, d=2):
    return f"{x:,.{d}f}"


def grid_tables(b) -> list[str]:
    out = []
    threads = sorted({c["threads"] for c in b["grid"]}, reverse=True)
    for t in threads:
        out += [f"### COUNT(*) query time, ms, threads={t}", "",
                "| scale | pattern | class | matches | raw LIKE | detokenize+LIKE | token_like | token vs decode | token vs raw |",
                "|---|---|---|---:|---:|---:|---:|---:|---:|"]
        for c in b["grid"]:
            if c["threads"] != t:
                continue
            out.append(f"| {c['scale']} | `{c['name']}` | {c['class']} | {c['matches']:,} | {f(c['raw']['median_ms'], 3)} | "
                       f"{f(c['decode']['median_ms'], 3)} | {f(c['token']['median_ms'], 3)} | {c['token_vs_decode']:.2f}x | "
                       f"{c['token_vs_raw']:.2f}x |")
        out.append("")
    out += ["`token vs decode` = decode time / token time (>1: token_like faster). "
            "`token vs raw` = raw time / token time (<1: native LIKE on the VARCHAR column faster). "
            "`1m-uncompressed` is a copy of the table stored with `force_compression='uncompressed'`, so every "
            "row reaches the function (DuckDB evaluates pushed-down filters once per dictionary entry on "
            "dictionary-compressed segments).", ""]
    return out


def render_bench(path: Path) -> list[str]:
    b = json.loads(path.read_text(encoding="utf-8"))
    m = b["meta"]
    out = [f"## Benchmarks: `{path.name}`", "",
           f"{m['cpu']}, {m['os']}, DuckDB {m['duckdb']}, Python {m['python']}, extension `{m['extension']}` "
           f"({m['extension_build']}), pinned to CPUs {m['pinning'].get('pinned_cpus')} with power throttling disabled, "
           f"{m['timestamp']}. Median of {m['runs']} runs after 1 warm-up; statement time measured from Python.", ""]

    out += ["### Cold vs warm (1M rows, all pinned threads)", "",
            "Cold = matcher cache cleared, so bind builds the matcher, then the query runs (data already in "
            "DuckDB's buffer pool). Warm = cached matcher. Builds/hits are the extension's own counters.", "",
            "| pattern | scan | states | table KB | build ms | cold ms | warm ms | cold − warm | builds cold / warm | warm cache hits | verified |",
            "|---|---|---:|---:|---:|---:|---:|---:|---|---:|---|"]
    for c in b["cold_warm"]:
        out.append(f"| `{c['name']}` | {c['kind']} | {c['states']} | {c['table_kb']} | {c['build_ms']} | {c['cold_ms']} | "
                   f"{c['warm_ms']} | {c['cold_minus_warm_ms']} | {c['cold_builds']} / {c['warm_builds']} | "
                   f"{c['warm_cache_hits']} ({c['warm_runs']} runs) | {c['verified']} |")
    out.append("")

    out += ["### SQL scan floor (1M rows)", "",
            "| storage | threads | query | ms | ns/row |", "|---|---:|---|---:|---:|"]
    for c in b["scan_floor"]:
        out.append(f"| {c['scale']} | {c['threads']} | `{c['query']}` | {c['median_ms']} | {c['ns_per_row']} |")
    out.append("")

    d = b["decode_selected"]
    out += ["### Filter + GROUP BY on token ids, decode only the output", "",
            "| query | ms | output rows | detokenize() rows | token_like rows |", "|---|---:|---:|---:|---:|"]
    for k in ("token_then_decode_10", "decode_all_then_like", "raw_text"):
        r = d[k]
        out.append(f"| `{k}` | {r['median_ms']} | {r['rows_out']} | {r['detokenize_rows']:,} | {r['token_like_rows']:,} |")
    out += ["", f"Verified: {d['verified']}. {d['note']}", "", "```sql", DECODE_SQL(d), "```", "",
            "EXPLAIN ANALYZE of the token query (DuckDB's own plan):", "", "```", d["explain_analyze_token_then_decode_10"].strip(), "```", ""]

    out += grid_tables(b)

    out += ["### Tokens read by the matcher (uncompressed 1M table, every row evaluated)", "",
            "| pattern | rows evaluated | tokens/row present | tokens/row read |", "|---|---:|---:|---:|"]
    for c in b["tokens_read"]:
        out.append(f"| `{c['name']}` | {c['rows_evaluated']:,} | {c['tokens_per_row_present']} | {c['tokens_per_row_read']} |")
    out.append("")

    sql1 = {c["name"]: c for c in b["grid"] if c["scale"] == "1m-uncompressed" and c["threads"] == 1}
    if b["cli_reference_1m_single_thread"]:
        out += ["### Standalone CLI vs SQL, 1M rows, single thread, ns/row", "",
                "CLI: `build/tokenlike.exe` scan-only median over in-memory columns. SQL: whole statement on the "
                "uncompressed table, threads=1.", "",
                "| pattern | CLI raw | CLI decode | CLI token | SQL raw | SQL decode | SQL token | SQL token / CLI token |",
                "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for c in b["cli_reference_1m_single_thread"]:
            s = sql1.get(c["name"])
            ct = c["token"].get("ns_per_row")
            if s and ct:
                out.append(f"| `{c['name']}` | {c['raw'].get('ns_per_row')} | {c['decode'].get('ns_per_row')} | {ct} | "
                           f"{s['raw']['ns_per_row']} | {s['decode']['ns_per_row']} | {s['token']['ns_per_row']} | "
                           f"{s['token']['ns_per_row'] / ct:.2f}x |")
        out.append("")

    out += ["### Cold start (fresh process per mode; 10 queries on 1M rows)", "",
            "| mode | import ms | connect ms | LOAD ms | load vocab ms | first query ms | 10 queries ms | peak working set MB |",
            "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for c in b["cold_start"]:
        if "error" in c:
            out.append(f"| {c['mode']} | error | | | | | | |")
            continue
        out.append(f"| {c['mode']} | {c['import_ms']} | {c['connect_ms']} | {c['load_extension_ms']} | {c['load_vocab_ms']} | "
                   f"{c['first_query_1m_ms']} | {c['total_10_queries_ms']} | {c['peak_working_set_mb']} |")
    out.append("")
    return out


def DECODE_SQL(d) -> str:
    return d["token_then_decode_10"]["sql"].replace(" FROM ( ", " FROM (\n  ").replace(" GROUP BY", "\n  GROUP BY")


def render_storage(path: Path) -> list[str]:
    s = json.loads(path.read_text(encoding="utf-8"))
    L, P = s["logical"], s["physical_single_column"]
    out = ["## Compression accounting (same 1,000,000 ClickBench rows)", "",
           "| column | text bytes | token bytes | tokens/row | bytes/token | tokens ÷ text | (tokens + offsets) ÷ (text + offsets) | + vocabulary |",
           "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for col in ("URL", "Referer"):
        c = L[col]
        out.append(f"| {col} | {c['text_bytes']:,} | {c['token_bytes']:,} | {c['tokens_per_row']} | {c['bytes_per_token']} | "
                   f"{c['ratio_tokens_to_text']} | {c['ratio_with_offsets']} | {c['ratio_with_offsets_and_vocab']} |")
    out += ["", f"Offsets: uint64 per row (+1), as in Person B's `.off` / `.rawoff` files ({L['URL']['offsets_uint64_bytes']:,} B "
            f"per column). Vocabulary: `{L['vocabulary']['file']}`, {L['vocabulary']['bytes']:,} B, shared by all encoded columns; "
            "the \"+ vocabulary\" column charges all of it to that single column.", "",
            "### Inside DuckDB (each column stored alone; used blocks × block size)", "",
            "| column | default storage MB | segments by compression | uncompressed MB |", "|---|---:|---|---:|"]
    for c in ("original_url", "encoded_url", "original_referer", "encoded_referer"):
        dflt, unc = P[f"{c}/default"], P[f"{c}/uncompressed"]
        mix = ", ".join(f"{x['compression']} {x['segments']}" for x in dflt["segments"] if x["type"] != "VALIDITY")
        out.append(f"| `{c}` | {dflt['used_mb']} | {mix} | {unc['used_mb']} |")
    out += ["", f"Token column ÷ text column in DuckDB's default storage: URL {L['URL']['duckdb_default_ratio_tokens_to_text']}, "
            f"Referer {L['Referer']['duckdb_default_ratio_tokens_to_text']}. Block granularity is "
            f"{P['original_url/default']['block_size'] // 1024} KB.", "", f"FSST: {s['person_b_fsst']}", ""]
    out += ["Compression mix of the `urls` table itself (`pragma_storage_info('urls')`):", "",
            "| column | type | compression | segments | rows |", "|---|---|---|---:|---:|"]
    for x in s["urls_table_compression_mix"]:
        out.append(f"| `{x['column']}` | {x['type']} | {x['compression']} | {x['segments']} | {x['rows']:,} |")
    out.append("")
    return out


def render_tests(path: Path) -> list[str]:
    t = json.loads(path.read_text(encoding="utf-8"))
    out = [f"## Correctness: `{path.name}`", "",
           f"Extension `{t['extension']}`, DuckDB {t['duckdb']}, mode {t['mode']}: **{t['total_pass']} checks passed, "
           f"{t['total_fail']} failed**, {t['real_data_patterns']} real-data patterns, {len(t['real_data_skipped'])} skipped, "
           f"{t['seconds']} s.", "", "| group | pass | fail |", "|---|---:|---:|"]
    for g, r in t["groups"].items():
        out.append(f"| {g} | {r['pass']} | {r['fail']} |")
    out.append("")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Render result JSON files as RESULTS.md")
    ap.add_argument("benches", nargs="*", type=Path, help="bench JSON files (default: <results-dir>/bench.json)")
    ap.add_argument("--results-dir", type=Path, default=tlsql.RESULTS,
                    help="directory with test_results*.json and storage.json (default: the committed results/)")
    tlsql.add_output_args(ap, "RESULTS.md")
    args = ap.parse_args()
    out_file = tlsql.output(args, "RESULTS.md")
    res = args.results_dir.resolve()
    benches = args.benches or [res / "bench.json"]
    out = ["# TokenLIKE DuckDB integration: measured results", "",
           "Generated by `integration/duckdb/python/report_tables.py` from the JSON files in this directory. "
           "Every number below was measured on the machine named in each section.", ""]
    for name in ("test_results.json", "test_results_msvc.json"):
        if (res / name).exists():
            out += render_tests(res / name)
    if (res / "storage.json").exists():
        out += render_storage(res / "storage.json")
    for b in benches:
        if b.exists():
            out += render_bench(b)
    out_file.write("\n".join(out) + "\n")
    print("wrote", tlsql.rel(out_file.path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
