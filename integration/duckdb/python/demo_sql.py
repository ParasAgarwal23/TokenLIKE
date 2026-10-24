"""TokenLIKE inside DuckDB: live SQL demonstration.

    .venv/Scripts/python -X utf8 integration/duckdb/python/demo_sql.py [--table urls] [--runs 5]

Everything shown is executed now against data/duckdb/tokenlike.duckdb; nothing is pre-recorded
except the routing table, which is read from integration/duckdb/results/bench.json (produced by
bench_sql.py) and is labelled as such.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402
from tlsql import sql_str  # noqa: E402


def hr(title: str):
    print("\n" + "=" * 100 + f"\n{title}\n" + "=" * 100)


def timed(con, sql: str, runs: int):
    res = con.execute(sql).fetchall()
    ts = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = con.execute(sql).fetchall()
        ts.append(time.perf_counter() - t0)
        assert r == res
    return res, statistics.median(ts) * 1e3


def classify(pattern: str, concat: bool) -> str:
    if concat:
        return "concat"
    body = pattern.strip("%")
    if "_" in pattern:
        return "underscore"
    if "%" in body:
        return "interior"
    s, e = pattern.startswith("%"), pattern.endswith("%")
    return {(False, True): "prefix", (True, False): "suffix", (False, False): "exact", (True, True): "contains"}[(s, e)]


def load_policy() -> tuple[dict, str]:
    """Per pattern class: measured median ms of raw LIKE, token_like and detokenize+LIKE at 1M rows
    (normal DuckDB storage, all pinned threads), from results/bench.json."""
    f = tlsql.RESULTS / "bench.json"
    if not f.exists():
        return {}, "no results/bench.json (run bench_sql.py); default: token_like"
    b = json.loads(f.read_text(encoding="utf-8"))
    pol: dict[str, dict] = {}
    for c in b["grid"]:
        if c["scale"] != "1m" or c["threads"] == 1 or c["verified"] != "pass":
            continue
        d = pol.setdefault(c["class"], {"token_ms": [], "decode_ms": [], "raw_ms": [], "names": []})
        for m in ("token", "decode", "raw"):
            d[m + "_ms"].append(c[m]["median_ms"])
        d["names"].append(c["name"])
    for d in pol.values():
        for m in ("token", "decode", "raw"):
            d[m] = statistics.median(d[m + "_ms"])
    pinned = b["meta"].get("pinning", {}).get("pinned_cpus")
    where = f"{len(pinned)} threads pinned to CPUs {pinned}" if pinned else f"{b['meta']['logical_cpus']} threads"
    return pol, f"results/bench.json, measured {b['meta']['timestamp']} on {b['meta'].get('cpu', '?')} ({where})"


PLANS = {"token_like": "token", "detokenize+LIKE": "decode", "raw LIKE": "raw"}


def route(pattern: str, concat: bool, pol: dict, raw_available: bool) -> tuple[str, str]:
    """Experimental APPLICATION-LEVEL routing demonstration (not a DuckDB optimizer rule; DuckDB never
    rewrites LIKE into token_like by itself). Picks the plan with the lowest measured median for the
    pattern's class. Candidates: token_like and detokenize+LIKE when only the token column is stored;
    additionally raw LIKE when the original text column is stored too."""
    cls = classify(pattern, concat)
    d = pol.get(cls)
    if not d:
        return "token_like", f"class={cls}: no measurement, default token_like"
    cands = ["token_like", "detokenize+LIKE"] + (["raw LIKE"] if raw_available else [])
    choice = min(cands, key=lambda c: d[PLANS[c]])
    return choice, f"class={cls}: " + ", ".join(f"{c} {d[PLANS[c]]:.1f} ms" for c in cands) + f" (median over {', '.join(d['names'])})"


def predicate(plan: str, pattern: str, concat: bool) -> str:
    p = sql_str(pattern)
    if concat:
        return {"token_like": f"token_like_concat(encoded_url, encoded_referer, {p})",
                "detokenize+LIKE": f"(detokenize(encoded_url) || detokenize(encoded_referer)) LIKE {p}",
                "raw LIKE": f"(original_url || original_referer) LIKE {p}"}[plan]
    return {"token_like": f"token_like(encoded_url, {p})", "detokenize+LIKE": f"detokenize(encoded_url) LIKE {p}",
            "raw LIKE": f"original_url LIKE {p}"}[plan]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--table", default="urls")
    ap.add_argument("--runs", type=int, default=5)
    args = ap.parse_args()
    T = args.table

    hr("1. Open DuckDB and load the native TokenLIKE extension")
    t0 = time.perf_counter()
    con = tlsql.connect(tlsql.DB, read_only=True)
    print(f"database        : {tlsql.rel(tlsql.DB)}")
    print(f"duckdb          : {con.execute('SELECT version()').fetchone()[0]}  platform={con.execute('PRAGMA platform').fetchone()[0]}")
    ext_row = con.execute("SELECT extension_name, loaded, install_mode FROM duckdb_extensions() WHERE extension_name='tokenlike'").fetchone()
    print(f"extension       : {ext_row}")
    print(f"integration     : NATIVE loadable DuckDB extension (C extension API, C_STRUCT ABI) -> {tlsql.rel(tlsql.EXT)}")
    print("                  token_like() runs the unmodified TokenLIKE C++ Matcher inside DuckDB's scan; no Python per row")
    st = tlsql.stats(con)
    print(f"vocabulary      : {st['vocab_n']} tokens from {tlsql.rel(tlsql.VOCAB)}  (connect+LOAD+vocab {1e3 * (time.perf_counter() - t0):.0f} ms)")
    fns = con.execute("""SELECT DISTINCT function_name FROM duckdb_functions()
                         WHERE function_name LIKE 'token%' OR function_name LIKE '%tokenize' ORDER BY 1""").fetchall()
    print("functions       :", ", ".join(f[0] for f in fns))

    hr("2. The table: token ids stored as BLOB (little-endian uint16), raw text kept only for truth/baselines")
    for r in con.execute(f"DESCRIBE {T}").fetchall():
        print(f"   {r[0]:<18} {r[1]}")
    rid, url, blob = con.execute(f"SELECT row_id, original_url, encoded_url FROM {T} WHERE strlen(original_url) BETWEEN 30 AND 70 ORDER BY row_id LIMIT 1").fetchone()
    ids = [int.from_bytes(blob[i:i + 2], "little") for i in range(0, len(blob), 2)]
    print(f"   row {rid}: original_url = {url}")
    print(f"           encoded_url  = {len(blob)} bytes = {len(ids)} token ids {ids[:12]}{' ...' if len(ids) > 12 else ''}")
    toks = con.execute(f"SELECT detokenize(encoded_url) FROM {T} WHERE row_id = {rid}").fetchone()[0]
    print(f"           detokenize() = {toks}  (round trip {'OK' if toks == url else 'MISMATCH'})")

    hr("3. SQL queries, executed now (median of %d runs)" % args.runs)
    queries = [
        ("A  native LIKE on raw text", f"SELECT COUNT(*) FROM {T} WHERE original_url LIKE '%.php'"),
        ("B  TokenLIKE on token ids", f"SELECT COUNT(*) FROM {T} WHERE token_like(encoded_url, '%.php')"),
        ("   decode + LIKE baseline", f"SELECT COUNT(*) FROM {T} WHERE detokenize(encoded_url) LIKE '%.php'"),
        ("C  TokenLIKE prefix", f"SELECT COUNT(*) FROM {T} WHERE token_like(encoded_url, 'http://%')"),
        ("   native LIKE prefix", f"SELECT COUNT(*) FROM {T} WHERE original_url LIKE 'http://%'"),
        ("D  TokenLIKE concatenation", f"SELECT COUNT(*) FROM {T} WHERE token_like_concat(encoded_url, encoded_referer, '%yandex%')"),
        ("   native LIKE concatenation", f"SELECT COUNT(*) FROM {T} WHERE (original_url || original_referer) LIKE '%yandex%'"),
    ]
    got = {}
    for label, q in queries:
        res, ms = timed(con, q, args.runs)
        got[label] = res[0][0]
        print(f"\n{label}\n   {q}\n   -> {res[0][0]:,} rows   [{ms:.2f} ms]")
    print(f"\n   A == B: {got['A  native LIKE on raw text'] == got['B  TokenLIKE on token ids']}   "
          f"C == native: {got['C  TokenLIKE prefix'] == got['   native LIKE prefix']}   "
          f"D == native: {got['D  TokenLIKE concatenation'] == got['   native LIKE concatenation']}")

    q_e = f"""SELECT traffic_source_id AS category, COUNT(*) AS n
FROM {T} WHERE token_like(encoded_url, '%.php')
GROUP BY category ORDER BY category"""
    q_e_raw = q_e.replace("token_like(encoded_url, '%.php')", "original_url LIKE '%.php'")
    e_tok, ms = timed(con, q_e, args.runs)
    e_raw = con.execute(q_e_raw).fetchall()
    print(f"\nE  filter, then aggregate (category = ClickBench source column TraficSourceID)\n   {q_e.replace(chr(10), chr(10) + '   ')}")
    for cat, n in e_tok:
        print(f"      {cat:>4}  {n:>7,}")
    print(f"   [{ms:.2f} ms]   identical to the same query with original_url LIKE: {e_tok == e_raw}")

    hr("4. Row ids, not only counts: TokenLIKE vs native LIKE")
    for p in ["%.php", "http://irr.ru/%", "%ру%", "http://irr.ru/____%"]:
        extra = con.execute(f"SELECT count(*) FROM (SELECT row_id FROM {T} WHERE token_like(encoded_url, {sql_str(p)}) EXCEPT SELECT row_id FROM {T} WHERE original_url LIKE {sql_str(p)})").fetchone()[0]
        missing = con.execute(f"SELECT count(*) FROM (SELECT row_id FROM {T} WHERE original_url LIKE {sql_str(p)} EXCEPT SELECT row_id FROM {T} WHERE token_like(encoded_url, {sql_str(p)}))").fetchone()[0]
        first = con.execute(f"SELECT list(row_id ORDER BY row_id)[1:5] FROM {T} WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0]
        print(f"   {p:<22} extra={extra} missing={missing}  first row ids {first}")

    hr("5. Where it runs (EXPLAIN): token_like is evaluated as a filter inside DuckDB's TABLE_SCAN")
    plan = con.execute(f"EXPLAIN SELECT COUNT(*) FROM {T} WHERE token_like(encoded_url, '%.php')").fetchall()[0][1]
    print("\n".join("   " + line for line in plan.splitlines() if line.strip()))
    print("   Note: this is a scalar function DuckDB pushes into the scan. DuckDB does NOT rewrite plain LIKE")
    print("   into token_like; there is no optimizer rule (DuckDB rewrites LIKE '%.php' into suffix() itself).")

    hr("6. What the matcher did: build once per query, read only the tokens it needs")
    for p in ["http://%", "%.php", "%/search%"]:
        info = json.loads(con.execute("SELECT tokenlike_info(?)", [p]).fetchone()[0])
        con.execute("SELECT tokenlike_reset_stats()").fetchall()
        con.execute(f"SELECT COUNT(*) FROM {T} WHERE token_like(encoded_url, {sql_str(p)})").fetchall()
        s = tlsql.stats(con)
        print(f"   {p:<12} scan={info['kind']:<8} states={info['states']:<3} table={info['table_kb']:.1f} KB "
              f"build={info['build_ms']:.2f} ms | rows evaluated={s['rows']:,} tokens present={s['tokens_total']:,} "
              f"read={s['tokens_read']:,} ({100 * s['tokens_read'] / max(s['tokens_total'], 1):.1f}%) "
              f"matcher builds this query={s['matcher_builds']} cache hits={s['cache_hits']}")
    print("   (rows evaluated < table rows when DuckDB evaluates the filter once per dictionary entry of a")
    print("    dictionary-compressed segment; this applies to every mode equally.)")

    hr("7. Filter and group in token space, decode only the output rows")
    q_sel = f"""SELECT detokenize(encoded_url) AS url, n FROM (
    SELECT encoded_url, count(*) AS n FROM {T} WHERE token_like(encoded_url, '%.php')
    GROUP BY encoded_url ORDER BY n DESC, encoded_url LIMIT 10)"""
    q_raw = f"""SELECT original_url, count(*) AS n FROM {T} WHERE original_url LIKE '%.php'
    GROUP BY original_url ORDER BY n DESC, original_url LIMIT 10"""
    con.execute("SELECT tokenlike_reset_stats()").fetchall()
    top = con.execute(q_sel).fetchall()
    decoded = tlsql.stats(con)["decoded_rows"]
    _, ms_sel = timed(con, q_sel, args.runs)
    raw_top, ms_raw = timed(con, q_raw, args.runs)
    full = dict(con.execute(f"SELECT original_url, count(*) FROM {T} WHERE original_url LIKE '%.php' GROUP BY 1").fetchall())
    print("   " + q_sel.replace(chr(10), chr(10) + "   "))
    for u, n in top:
        print(f"      {n:>6,}  {u[:90]}")
    ok = all(full.get(u) == n for u, n in top) and [n for _, n in top] == [n for _, n in raw_top]
    print(f"   [{ms_sel:.2f} ms]  detokenize() ran on {decoded} rows (the output), not on the {len(full):,} matching groups"
          f" or the {con.execute(f'SELECT count(*) FROM {T}').fetchone()[0]:,} rows")
    print(f"   raw-text equivalent [{ms_raw:.2f} ms]; every decoded URL's count equals its raw-text GROUP BY count and the"
          f" 10 counts are the 10 largest: {ok}")
    print("   (Ties are ordered by token bytes here and by text in the raw query, so URLs inside a tie may differ.)")

    hr("8. Experimental application-level routing demonstration (NOT a DuckDB optimizer)")
    pol, src = load_policy()
    print(f"   policy source: {src}")
    print("   DuckDB does not rewrite LIKE into token_like; this script picks a predicate from measured medians.")
    demo = [("http://%", False), ("%.php", False), ("%/search%", False),
            ("http://irr.ru/index.php?showalbum/login-leniya7777294,938303130", False), ("%yandex%", True)]
    choices = {False: [], True: []}
    for raw_available in (False, True):
        print(f"\n   storage: {'token column AND original text' if raw_available else 'token column only'}")
        for p, concat in demo:
            choice, why = route(p, concat, pol, raw_available)
            choices[raw_available].append(choice)
            res, ms = timed(con, f"SELECT COUNT(*) FROM {T} WHERE {predicate(choice, p, concat)}", args.runs)
            truth = con.execute(f"SELECT COUNT(*) FROM {T} WHERE {predicate('raw LIKE', p, concat)}").fetchone()[0]
            print(f"   ROUTE {p[:40]:<40} -> {choice:<16} {res[0][0]:>8,} rows {ms:8.2f} ms  verified={res[0][0] == truth}\n         {why}")
    if not pol:
        print("\n   No measurements loaded: every route defaulted to token_like. Run bench_sql.py first.")
    else:
        n_dec = choices[False].count("detokenize+LIKE")
        n_raw = choices[True].count("raw LIKE")
        print(f"\n   Token column only: detokenize+LIKE chosen {n_dec} of {len(demo)} times"
              + (" - with these measurements this case always picks token_like, so it is a demonstration, not a decision."
                 if n_dec == 0 else "."))
        print(f"   Token column and text: raw LIKE chosen {n_raw} of {len(demo)} times, "
              f"token_like {choices[True].count('token_like')} times.")

    hr("9. Performance notes")
    print("   * token_like vs detokenize()+LIKE: both read the token column; see section 3 timings.")
    print("   * token_like vs native LIKE: DuckDB stores the VARCHAR column compressed too (FSST/dictionary, see")
    print("     results/storage.json) and rewrites LIKE into prefix()/suffix()/contains()/= kernels that work on it.")
    print("   * Full numbers: integration/duckdb/results/bench.json and RESULTS.md.")
    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
