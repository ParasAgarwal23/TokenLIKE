"""Benchmarks for the TokenLIKE DuckDB extension.

Compares, inside the same DuckDB process and through the same SQL front end:
  raw     original_url LIKE p              DuckDB on the VARCHAR column. In DuckDB's normal
                                           storage that column is itself compressed (FSST /
                                           dictionary, see storage_report.py); DuckDB rewrites
                                           the LIKE into prefix()/suffix()/contains()/=.
  decode  detokenize(encoded_url) LIKE p   token BLOB column, full decode, then native LIKE
  token   token_like(encoded_url, p)       TokenLIKE matcher on the token ids, no decoding

Sections (one JSON report; the committed reference is results/bench.json):
  grid             COUNT(*) per pattern x mode x scale (1k..1M) x threads (P-cores, 1); plus a
                   1M-row table stored with force_compression='uncompressed'
  cold_warm        per pattern on 1M rows: matcher build + first execution vs cached executions
  scan_floor       trivial filters on the same columns, to separate DuckDB scan/operator cost
                   and C-API call cost from matcher cost
  decode_selected  filter + GROUP BY on token BLOBs, detokenize only the 10 output rows
  tokens_read      tokens the matcher read per row (early termination)
  cli_reference    the standalone TokenLIKE CLI on the same 1M rows (single thread, scan only)
  cold_start       fresh process per mode: connect, LOAD, vocabulary, first query, peak memory

Methodology: 1 warm-up + N timed runs, median reported, timed from Python around the whole
statement (bind + plan + scan + aggregate + result). Every mode of every cell must return the same
count or the cell is marked FAIL. The process pins itself to the highest-efficiency-class cores
(P-cores on hybrid Intel CPUs) and opts out of Windows power throttling.

    .venv/Scripts/python -X utf8 integration/duckdb/python/bench_sql.py [--runs 7] [--quick]
        [--out FILE [--overwrite] | --update-reference]

Output: a new build/duckdb/results/bench-<UTC time>.json by default (git-ignored). An existing file
is never replaced without --overwrite; --update-reference replaces the committed results/bench.json
(results/bench_<tag>.json with TOKENLIKE_BENCH_TAG, results/bench_quick*.json with --quick).
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402
from tlsql import sql_str  # noqa: E402

PATTERNS = [  # (name from Person B's pattern files, class, pattern, concat)
    ("p_http", "prefix", "http://%", False),
    ("p_irr", "prefix", "http://irr.ru%", False),
    ("s_php", "suffix", "%.php", False),
    ("s_html", "suffix", "%.html", False),
    ("e_top_url", "exact", "http://irr.ru/index.php?showalbum/login-leniya7777294,938303130", False),
    ("i_search", "contains", "%/search%", False),
    ("u8_ru_cyr", "contains", "%ру%", False),
    ("w_four_under", "underscore", "http://irr.ru/____%", False),
    ("c_irr", "concat", "http://irr.ru%", True),
    ("c_yandex", "concat", "%yandex%", True),
]
SCALES = [("1k", "urls_1k", 1_000), ("10k", "urls_10k", 10_000), ("100k", "urls_100k", 100_000), ("1m", "urls", 1_000_000)]

DECODE_SELECTED = {
    "token_then_decode_10": """SELECT detokenize(encoded_url) AS url, n FROM (
        SELECT encoded_url, count(*) AS n FROM urls WHERE token_like(encoded_url, '%.php')
        GROUP BY encoded_url ORDER BY n DESC, encoded_url LIMIT 10)""",
    "decode_all_then_like": """SELECT url, count(*) AS n FROM (SELECT detokenize(encoded_url) AS url FROM urls)
        WHERE url LIKE '%.php' GROUP BY url ORDER BY n DESC, url LIMIT 10""",
    "raw_text": """SELECT original_url AS url, count(*) AS n FROM urls WHERE original_url LIKE '%.php'
        GROUP BY original_url ORDER BY n DESC, original_url LIMIT 10""",
}


def where(mode: str, pat: str, concat: bool) -> str:
    p = sql_str(pat)
    if concat:
        return {"raw": f"(original_url || original_referer) LIKE {p}",
                "decode": f"(detokenize(encoded_url) || detokenize(encoded_referer)) LIKE {p}",
                "token": f"token_like_concat(encoded_url, encoded_referer, {p})"}[mode]
    return {"raw": f"original_url LIKE {p}", "decode": f"detokenize(encoded_url) LIKE {p}",
            "token": f"token_like(encoded_url, {p})"}[mode]


def time_query(con, sql: str, runs: int, warmup: int = 1) -> tuple[list[float], object]:
    res = None
    for _ in range(warmup):
        res = con.execute(sql).fetchall()
    ts = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = con.execute(sql).fetchall()
        ts.append(time.perf_counter() - t0)
        if res is not None and r != res:
            raise AssertionError(f"result changed between runs: {sql}")
        res = r
    return ts, res[0][0]


def ms(ts) -> float:
    return round(statistics.median(ts) * 1e3, 4)


def ensure_uncompressed():
    if tlsql.UNC_DB.exists():
        return
    con = tlsql.connect(tlsql.UNC_DB)
    con.execute(f"ATTACH '{tlsql.DB.as_posix()}' AS src (READ_ONLY)")
    con.execute("PRAGMA force_compression='uncompressed'")
    con.execute("CREATE TABLE urls AS SELECT * FROM src.urls ORDER BY row_id")
    con.execute("CHECKPOINT")
    comp = con.execute("SELECT DISTINCT compression FROM pragma_storage_info('urls') WHERE segment_type IN ('BLOB','VARCHAR')").fetchall()
    con.close()
    if comp != [("Uncompressed",)]:
        raise SystemExit(f"uncompressed copy is compressed: {comp}")


def bench_grid(runs: int, scales, threads_list) -> list[dict]:
    rows = []
    targets = [(tlsql.DB, s) for s in scales] + [(tlsql.UNC_DB, ("1m-uncompressed", "urls", 1_000_000))]
    for db, (label, table, n) in targets:
        for threads in threads_list:
            con = tlsql.connect(db, read_only=True, threads=threads)
            for name, cls, pat, concat in PATTERNS:
                cell = {"scale": label, "rows": n, "threads": threads, "name": name,
                        "class": cls, "pattern": pat, "concat": concat}
                counts = {}
                for mode in ("raw", "decode", "token"):
                    ts, cnt = time_query(con, f"SELECT count(*) FROM {table} WHERE {where(mode, pat, concat)}", runs)
                    med = statistics.median(ts)
                    cell[mode] = {"median_ms": round(med * 1e3, 4), "min_ms": round(min(ts) * 1e3, 4),
                                  "ns_per_row": round(med * 1e9 / n, 3)}
                    counts[mode] = cnt
                cell["matches"] = counts["raw"]
                cell["verified"] = "pass" if len(set(counts.values())) == 1 else f"FAIL {counts}"
                cell["token_vs_decode"] = round(cell["decode"]["median_ms"] / cell["token"]["median_ms"], 2)
                cell["token_vs_raw"] = round(cell["raw"]["median_ms"] / cell["token"]["median_ms"], 2)
                rows.append(cell)
                print(f"{label:>15} t={threads:>2} {name:<13} raw={cell['raw']['median_ms']:9.3f} "
                      f"decode={cell['decode']['median_ms']:9.3f} token={cell['token']['median_ms']:9.3f} ms "
                      f"matches={cell['matches']} {cell['verified']}", flush=True)
            con.close()
    return rows


def bench_cold_warm(runs: int, threads: int) -> list[dict]:
    """Matcher construction + first execution (cache cleared) vs. cached executions, 1M rows.

    "cold" here means: the data is already in DuckDB's buffer pool (a warm-up scan ran), but the
    pattern's matcher is not in the cache, so bind builds it. The patterns are interleaved, so a
    stale matcher would show up as a wrong count against the raw-text count.
    """
    con = tlsql.connect(tlsql.DB, read_only=True, threads=threads)
    con.execute("SELECT count(*) FROM urls WHERE octet_length(encoded_url) >= 0 AND octet_length(encoded_referer) >= 0").fetchall()
    out = []
    for name, cls, pat, concat in PATTERNS:
        q = f"SELECT count(*) FROM urls WHERE {where('token', pat, concat)}"
        truth = con.execute(f"SELECT count(*) FROM urls WHERE {where('raw', pat, concat)}").fetchone()[0]
        cold = []
        for _ in range(5):
            con.execute("SELECT tokenlike_cache_clear(), tokenlike_reset_stats()").fetchall()
            t0 = time.perf_counter()
            cnt = con.execute(q).fetchone()[0]
            cold.append(time.perf_counter() - t0)
            st_cold = tlsql.stats(con)
        con.execute("SELECT tokenlike_reset_stats()").fetchall()
        warm, cw = time_query(con, q, runs, warmup=0)
        st_warm = tlsql.stats(con)
        info = json.loads(con.execute(f"SELECT tokenlike_info({sql_str(pat)})").fetchone()[0])
        cell = {"name": name, "pattern": pat, "kind": info["kind"], "states": info["states"],
                "table_kb": info["table_kb"], "dense_table_kb_transient": info["dense_table_kb"],
                "build_ms": info["build_ms"], "cold_ms": ms(cold), "warm_ms": ms(warm),
                "cold_minus_warm_ms": round(ms(cold) - ms(warm), 3),
                "cold_builds": st_cold["matcher_builds"], "warm_builds": st_warm["matcher_builds"],
                "warm_cache_hits": st_warm["cache_hits"], "warm_runs": runs,
                "verified": "pass" if cnt == cw == truth else f"FAIL cold={cnt} warm={cw} raw={truth}"}
        out.append(cell)
        print(f"coldwarm {name:<13} build={cell['build_ms']:8.2f} cold={cell['cold_ms']:8.2f} warm={cell['warm_ms']:8.2f} ms "
              f"builds cold/warm={cell['cold_builds']}/{cell['warm_builds']} hits={cell['warm_cache_hits']} {cell['verified']}", flush=True)
    con.close()
    return out


FLOOR = [  # (label, where-clause, expected count: None = all rows)
    ("count(*) without filter", None, None),
    ("octet_length(encoded_url) >= 0", "octet_length(encoded_url) >= 0", None),
    ("strlen(original_url) >= 0", "strlen(original_url) >= 0", None),
    ("tokenlike_floor_noop(encoded_url)", "tokenlike_floor_noop(encoded_url)", None),
    ("tokenlike_floor_copy(encoded_url)", "tokenlike_floor_copy(encoded_url)", None),
    ("token_like(encoded_url, '%.php')", "token_like(encoded_url, '%.php')", "s_php"),
    ("token_like(encoded_url, 'http://%')", "token_like(encoded_url, 'http://%')", "p_http"),
    ("token_like(encoded_url, '%/search%')", "token_like(encoded_url, '%/search%')", "i_search"),
    ("original_url LIKE '%.php'", "original_url LIKE '%.php'", "s_php"),
    ("detokenize(encoded_url) LIKE '%.php'", "detokenize(encoded_url) LIKE '%.php'", "s_php"),
]


def bench_scan_floor(runs: int, threads_list) -> list[dict]:
    out = []
    for db, label in ((tlsql.DB, "1m"), (tlsql.UNC_DB, "1m-uncompressed")):
        for threads in threads_list:
            con = tlsql.connect(db, read_only=True, threads=threads)
            n = con.execute("SELECT count(*) FROM urls").fetchone()[0]
            for name, w, _ in FLOOR:
                q = "SELECT count(*) FROM urls" + (f" WHERE {w}" if w else "")
                ts, cnt = time_query(con, q, runs)
                plan = con.execute("EXPLAIN " + q).fetchall()[0][1]
                fn = w.split("(")[0] if w else None
                cell = {"scale": label, "threads": threads, "query": name, "median_ms": ms(ts),
                        "ns_per_row": round(statistics.median(ts) * 1e9 / n, 3), "count": cnt,
                        "filter_in_plan": (fn in plan) if fn else None}
                if w and name.startswith(("octet_length", "strlen", "tokenlike_floor")):
                    cell["verified"] = "pass" if cnt == n and cell["filter_in_plan"] else f"FAIL count={cnt} in_plan={cell['filter_in_plan']}"
                out.append(cell)
                print(f"floor {label:>15} t={threads} {name:<40} {cell['median_ms']:9.3f} ms {cell['ns_per_row']:8.2f} ns/row", flush=True)
            con.close()
    return out


def bench_decode_selected(runs: int, threads: int) -> dict:
    con = tlsql.connect(tlsql.DB, read_only=True, threads=threads)
    res = {}
    results = {}
    for name, q in DECODE_SELECTED.items():
        r = con.execute(q).fetchall()
        times = []
        for _ in range(runs):
            t0 = time.perf_counter()
            r2 = con.execute(q).fetchall()
            times.append(time.perf_counter() - t0)
            assert r2 == r, name
        con.execute("SELECT tokenlike_reset_stats()").fetchall()
        con.execute(q).fetchall()
        st = tlsql.stats(con)
        results[name] = r
        res[name] = {"sql": " ".join(q.split()), "median_ms": ms(times), "rows_out": len(r),
                     "detokenize_rows": st["decoded_rows"], "token_like_rows": st["rows"]}
        print(f"decode-selected {name:<22} {res[name]['median_ms']:9.3f} ms detokenize rows={st['decoded_rows']:,} "
              f"token_like rows={st['rows']:,}", flush=True)
    a, b, c = (results[k] for k in ("token_then_decode_10", "decode_all_then_like", "raw_text"))
    full = dict(con.execute("SELECT original_url, count(*) FROM urls WHERE original_url LIKE '%.php' GROUP BY 1").fetchall())
    res["verified"] = ("pass" if all(full.get(u) == n for u, n in a) and [n for _, n in a] == [n for _, n in c]
                       and b == c and len(a) == 10 else "FAIL")
    res["note"] = ("The token query orders ties by the BLOB (token-id bytes), the raw query by text, so the URLs "
                   "inside a tie may differ; verification checks every decoded URL's count against the raw-text "
                   "GROUP BY and that the 10 counts are the 10 largest.")
    res["top10"] = [{"url": u, "n": n} for u, n in a]
    res["explain_analyze_token_then_decode_10"] = con.execute("EXPLAIN ANALYZE " + DECODE_SELECTED["token_then_decode_10"]).fetchall()[0][1]
    con.close()
    return res


def bench_tokens_read() -> list[dict]:
    """Tokens the matcher actually read vs. tokens present, on the uncompressed 1M table (every row evaluated)."""
    out = []
    con = tlsql.connect(tlsql.UNC_DB, read_only=True)
    for name, cls, pat, concat in PATTERNS:
        con.execute("SELECT tokenlike_reset_stats()").fetchall()
        con.execute(f"SELECT count(*) FROM urls WHERE {where('token', pat, concat)}").fetchall()
        st = tlsql.stats(con)
        out.append({"name": name, "rows_evaluated": st["rows"], "tokens_per_row_present": round(st["tokens_total"] / max(st["rows"], 1), 3),
                    "tokens_per_row_read": round(st["tokens_read"] / max(st["rows"], 1), 3)})
    con.close()
    return out


def bench_cli(runs: int) -> list[dict]:
    out = []
    if not tlsql.CLI.exists():
        print(f"cli: {tlsql.rel(tlsql.CLI)} not built; skipping the CLI reference", flush=True)
        return out
    url, ref = (tlsql.DATA / "clickbench" / c for c in ("URL", "Referer"))
    for name, cls, pat, concat in PATTERNS:
        cell = {"name": name, "pattern": pat}
        for mode in ("raw", "decode", "token"):
            cmd = [str(tlsql.CLI), "--mode", mode, "--pattern", pat, "--col", str(url), "--vocab", str(tlsql.VOCAB),
                   "--runs", str(runs), "--pin-cpu", str(tlsql.performance_cpus()[0])]
            if concat:
                cmd += ["--col2", str(ref)]
            r = subprocess.run(cmd, cwd=tlsql.ROOT, capture_output=True, text=True, encoding="utf-8")
            if r.returncode != 0:
                cell[mode] = {"error": r.stderr.strip()[-300:]}
                continue
            j = json.loads(r.stdout.strip().splitlines()[-1])
            cell[mode] = {"ns_per_row": j["ns_per_row"], "matches": j["matches"]}
        out.append(cell)
        print(f"cli   {name:<13} " + " ".join(f"{m}={cell[m].get('ns_per_row')}" for m in ("raw", "decode", "token")), flush=True)
    return out


PROBE = r"""
import json, sys, time
sys.path.insert(0, sys.argv[1])
t0 = time.perf_counter()
import duckdb, tlsql
mode = sys.argv[2]
t_import = time.perf_counter()
con = duckdb.connect(str(tlsql.DB), read_only=True, config={"allow_unsigned_extensions": "true"}); t1 = time.perf_counter()
con.execute(f"LOAD '{tlsql.EXT.as_posix()}'"); t2 = time.perf_counter()
con.execute(f"SELECT tokenlike_load_vocab('{tlsql.VOCAB.as_posix()}')").fetchall(); t3 = time.perf_counter()
from bench_sql import PATTERNS, where
first = None
for name, cls, pat, concat in PATTERNS:
    q0 = time.perf_counter(); con.execute(f"SELECT count(*) FROM urls WHERE {where(mode, pat, concat)}").fetchall()
    if first is None: first = time.perf_counter() - q0
print(json.dumps({"mode": mode, "import_ms": (t_import - t0) * 1e3, "connect_ms": (t1 - t_import) * 1e3,
                  "load_extension_ms": (t2 - t1) * 1e3, "load_vocab_ms": (t3 - t2) * 1e3,
                  "first_query_1m_ms": first * 1e3, "total_10_queries_ms": (time.perf_counter() - t3) * 1e3,
                  **tlsql.memory_mb()}))
"""


def bench_cold_start() -> list[dict]:
    out = []
    for mode in ("raw", "decode", "token"):
        r = subprocess.run([sys.executable, "-c", PROBE, str(Path(__file__).resolve().parent), mode],  # inherits P-core affinity
                           capture_output=True, text=True, encoding="utf-8")
        if r.returncode != 0:
            out.append({"mode": mode, "error": r.stderr[-500:]})
            continue
        j = json.loads(r.stdout.strip().splitlines()[-1])
        out.append({k: (round(v, 2) if isinstance(v, float) else v) for k, v in j.items()})
        print("cold start", out[-1], flush=True)
    return out


def cpu_name() -> str:
    if sys.platform == "win32":
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
        except OSError:
            pass
    return platform.processor()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--quick", action="store_true", help="100k and 1M only, fewer sections")
    tag = os.environ.get("TOKENLIKE_BENCH_TAG", "")
    tlsql.add_output_args(ap, "results/bench*.json")
    args = ap.parse_args()
    stem = ("bench_quick" if args.quick else "bench") + (f"_{tag}" if tag else "")
    out = tlsql.output(args, f"results/{stem}.json")  # checked now, before ~15 minutes of measurement
    for f in (tlsql.DB, tlsql.EXT, tlsql.VOCAB):
        if not f.exists():
            print(f"ERROR: {tlsql.rel(f)} not found (see integration/duckdb/README.md)")
            return 2
    pin = tlsql.pin_to_performance_cores()
    print("pinning:", pin, flush=True)
    pcores = len(pin["pinned_cpus"])
    ensure_uncompressed()
    scales = SCALES[2:] if args.quick else SCALES
    t0 = time.perf_counter()
    report = {
        "meta": {"timestamp": time.strftime("%Y-%m-%d %H:%M:%S"), "cpu": cpu_name(), "logical_cpus": os.cpu_count(),
                 "os": platform.platform(), "python": platform.python_version(),
                 "duckdb": __import__("duckdb").__version__, "runs": args.runs,
                 "extension": tlsql.rel(tlsql.EXT), "extension_build": os.environ.get("TOKENLIKE_BENCH_TAG", "mingw-gcc"),
                 "note": "statement time from Python, median of runs after 1 warm-up",
                 "pinning": pin},
        "cold_warm": bench_cold_warm(args.runs, pcores),
        "scan_floor": bench_scan_floor(args.runs, [pcores, 1]),
        "decode_selected": bench_decode_selected(args.runs, pcores),
        "grid": bench_grid(args.runs, scales, [pcores, 1]),
        "tokens_read": bench_tokens_read(),
        "cli_reference_1m_single_thread": bench_cli(5),
        "cold_start": bench_cold_start(),
    }
    report["meta"]["seconds"] = round(time.perf_counter() - t0, 1)
    out.write(json.dumps(report, indent=2, ensure_ascii=False))
    bad = ([c for c in report["grid"] if c["verified"] != "pass"] + [c for c in report["cold_warm"] if c["verified"] != "pass"]
           + [c for c in report["scan_floor"] if c.get("verified", "pass") != "pass"]
           + ([1] if report["decode_selected"]["verified"] != "pass" else []))
    print(f"wrote {tlsql.rel(out.path)}; {len(report['grid'])} grid cells, {len(bad)} unverified")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
