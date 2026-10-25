"""Correctness suite for the TokenLIKE DuckDB extension.

Every check compares the TokenLIKE SQL result with DuckDB's own LIKE over the original text. A
check passes only if it ran and produced the expected answer: an exception where a result was
expected is a failure, and so is a missing error where one was expected.

    .venv/Scripts/python -X utf8 integration/duckdb/python/test_sql.py [--quick | --smoke] [--ext FILE]
        [--out FILE [--overwrite] | --update-reference]

The full and quick runs write a JSON report: a new build/duckdb/results/test_results-<UTC time>.json
by default (TOKENLIKE_TEST_RESULTS=<name>.json changes the name); --update-reference replaces the
committed results/<name>.json. An existing file is never replaced without --overwrite. The smoke run
writes no report.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import struct
import sys
import threading
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402
from tlsql import sql_str  # noqa: E402

import duckdb  # noqa: E402

RESULTS: dict[str, dict] = {}
FAILURES: list[str] = []
CURRENT = ["?"]


def group(name):
    CURRENT[0] = name
    RESULTS.setdefault(name, {"pass": 0, "fail": 0})


def check(cond: bool, what: str):
    r = RESULTS[CURRENT[0]]
    if cond:
        r["pass"] += 1
    else:
        r["fail"] += 1
        FAILURES.append(f"[{CURRENT[0]}] {what}")
        if len(FAILURES) <= 40:
            print("  FAIL", FAILURES[-1], flush=True)


def expect_error(con, sql: str, needle: str, what: str, params=None):
    try:
        con.execute(sql, params) if params else con.execute(sql)
        con.fetchall()
    except Exception as e:  # noqa: BLE001
        check(needle.lower() in str(e).lower(), f"{what}: wrong error: {e}")
        return str(e)
    check(False, f"{what}: no error raised")
    return None


# --------------------------------------------------------------------- vocabulary (Python side)

def read_vocab(path: Path) -> list[bytes]:
    b = path.read_bytes()
    n = struct.unpack_from("<I", b)[0]
    off = struct.unpack_from(f"<{n + 1}I", b, 4)
    base = 4 + 4 * (n + 1)
    return [b[base + off[i]: base + off[i + 1]] for i in range(n)]


class Encoder:
    """Independent Python encoders used to produce unusual (but valid) token segmentations."""

    def __init__(self, toks: list[bytes]):
        self.ids: dict[bytes, int] = {}
        for i, t in enumerate(toks):
            if t and t not in self.ids:
                self.ids[t] = i
        self.maxlen = max(len(t) for t in toks)

    def single_bytes(self, s: str) -> bytes:
        return b"".join(struct.pack("<H", self.ids[bytes([c])]) for c in s.encode("utf-8"))

    def random_split(self, s: str, rng: random.Random) -> bytes:
        data, out, i = s.encode("utf-8"), [], 0
        while i < len(data):
            opts = [L for L in range(1, min(self.maxlen, len(data) - i) + 1) if data[i:i + L] in self.ids]
            L = rng.choice(opts)
            out.append(struct.pack("<H", self.ids[data[i:i + L]]))
            i += L
        return b"".join(out)

    def shortest_first_long(self, s: str) -> bytes:
        """Prefers the shortest token at even positions and the longest at odd ones: token
        boundaries fall in places a greedy encoder never produces (e.g. inside UTF-8 sequences)."""
        data, out, i, k = s.encode("utf-8"), [], 0, 0
        while i < len(data):
            opts = [L for L in range(1, min(self.maxlen, len(data) - i) + 1) if data[i:i + L] in self.ids]
            L = opts[0] if k % 2 == 0 else opts[-1]
            out.append(struct.pack("<H", self.ids[data[i:i + L]]))
            i += L
            k += 1
        return b"".join(out)


# --------------------------------------------------------------------- fixtures

FIXTURE_STRINGS = [
    "", "a", "_", "%", "%%", "\\", "\\\\", "a\\b", "a_b", "a%b", "a\\_b", "%25", "100%", "50%25off",
    "http://example.com/index.php", "https://example.com/index.php?x=1", "http://example.com/a.php5",
    "HTTP://EXAMPLE.COM/", "http://irr.ru/index.php?showalbum/login-leniya7777294,938303130",
    "http://komme%2F27.0.1453.116", "/static/js/app.js", "/static/", "static/", "/v1/x", "/v2/y/z",
    "é", "e\u0301", "ÉCOLE", "école", "Москва", "москва", "https://секрет.рф/путь", "日本語", "例え.テスト",
    "😀", "a😀b", "😀😀", "\u00a0", "tab\there", "new\nline", "x" * 300, "ab" * 200 + "z",
    "yandex.ru/search?text=%D0%BF", "http://www.yandex.ru/", "mail.yandex.ru",
]

FIXTURE_PATTERNS = [
    "", "%", "%%", "_", "__", "___", "a", "a%", "%a", "%a%", "_%", "%_", "%_%", "a_b", "a\\_b", "a\\b",
    "%\\%", "\\%", "%\\\\%", "a%b", "%25", "%25%", "%\\%25%", "%%%25", "100%", "%%%", "http://%", "https://%",
    "http%", "htt%", "http://example.com/index.php", "%.php", "%.php_", "%.php%", "%index%", "%example%php%",
    "http://%.php", "h%p://%/%.%", "/static/%", "%/v_/%", "/v_/%", "_____", "%é%", "_cole", "__cole", "%cole",
    "É%", "%Моск%", "%моск%", "Москва", "_осква", "%рф/%", "___", "日本_", "%テ%", "_", "😀", "_😀_", "a_b",
    "%😀%", "%\u00a0%", "%\t%", "%\n%", "x%x", "%" + "x" * 100 + "%", "%" + "ab" * 50 + "z", "%yandex%",
    "%yandex.ru%", "yandex%", "%/search?text=%",
]


def build_fixture(con, enc: Encoder, rng: random.Random):
    rows = []
    rid = 0
    for s in FIXTURE_STRINGS:
        for scheme in ("greedy", "single", "random", "alternating"):
            if scheme == "greedy":
                blob = None  # filled by SQL tokenize()
            elif scheme == "single":
                blob = enc.single_bytes(s)
            elif scheme == "random":
                blob = enc.random_split(s, rng)
            else:
                blob = enc.shortest_first_long(s)
            rows.append((rid, s, blob, scheme))
            rid += 1
    rows.append((rid, None, None, "null"))
    con.execute("CREATE OR REPLACE TABLE fx (row_id BIGINT, s VARCHAR, enc BLOB, scheme VARCHAR)")
    con.executemany("INSERT INTO fx VALUES (?, ?, ?, ?)", rows)
    con.execute("UPDATE fx SET enc = tokenize(s) WHERE scheme = 'greedy'")
    con.execute("CREATE OR REPLACE TABLE pats (pid BIGINT, p VARCHAR)")
    con.executemany("INSERT INTO pats VALUES (?, ?)", [(i, p) for i, p in enumerate(FIXTURE_PATTERNS)] +
                    [(len(FIXTURE_PATTERNS), None)])


# --------------------------------------------------------------------- tests

def test_fixture_constant(con):
    group("fixture: constant patterns (bind-time matcher)")
    bad = con.execute("SELECT count(*) FROM fx WHERE detokenize(enc) IS DISTINCT FROM s").fetchone()[0]
    check(bad == 0, f"fixture encodings do not decode back ({bad})")
    for p in FIXTURE_PATTERNS:
        q = f"""SELECT count(*) FILTER (WHERE token_like(enc, {sql_str(p)}) IS DISTINCT FROM (s LIKE {sql_str(p)})),
                       count(*) FILTER (WHERE s LIKE {sql_str(p)}) FROM fx"""
        mism, _ = con.execute(q).fetchone()
        check(mism == 0, f"pattern {p!r}: {mism} rows differ from LIKE (incl. NULL)")
        a = con.execute(f"SELECT list(row_id ORDER BY row_id) FROM fx WHERE token_like(enc, {sql_str(p)})").fetchone()[0]
        b = con.execute(f"SELECT list(row_id ORDER BY row_id) FROM fx WHERE s LIKE {sql_str(p)}").fetchone()[0]
        check(a == b, f"WHERE row ids differ for {p!r}")


def test_fixture_runtime(con):
    group("fixture: per-row patterns (runtime cache path)")
    q = """SELECT count(*) FILTER (WHERE token_like(fx.enc, pats.p) IS DISTINCT FROM (fx.s LIKE pats.p)), count(*)
           FROM fx, pats"""
    mism, total = con.execute(q).fetchone()
    check(mism == 0, f"{mism} of {total} (string, pattern) pairs differ")
    # Patterns interleaved row by row (worst case for run detection).
    q = """WITH x AS (SELECT fx.row_id, fx.enc, fx.s, pats.p FROM fx, pats ORDER BY random())
           SELECT count(*) FILTER (WHERE token_like(enc, p) IS DISTINCT FROM (s LIKE p)) FROM x"""
    check(con.execute(q).fetchone()[0] == 0, "shuffled pairs differ")


def test_fixture_concat(con):
    group("fixture: token_like_concat")
    pats = ["%", "", "%a%b%", "%php%", "http%", "%/%", "_%_", "%😀%", "%é%", "%\\%", "%25%", "a%", "%ab", "__",
            "%yandex%", "%.ruhttp%", "%ru/http%", "%Москва%"]
    for p in pats:
        q = f"""SELECT count(*) FILTER (WHERE token_like_concat(a.enc, b.enc, {sql_str(p)})
                                       IS DISTINCT FROM ((a.s || b.s) LIKE {sql_str(p)})), count(*)
                FROM fx a JOIN fx b ON (a.row_id * 7 + b.row_id) % 11 = 0"""
        mism, total = con.execute(q).fetchone()
        check(mism == 0 and total > 100, f"concat {p!r}: {mism} of {total} differ")
    q = """SELECT count(*) FILTER (WHERE token_like_concat(a.enc, b.enc, p.p) IS DISTINCT FROM ((a.s || b.s) LIKE p.p))
           FROM fx a JOIN fx b ON (a.row_id + b.row_id) % 37 = 0, pats p"""
    check(con.execute(q).fetchone()[0] == 0, "concat with per-row patterns differs")


def test_real_data(con, files, quick):
    group("real data: pattern files vs DuckDB LIKE")
    colmap = {
        "data/clickbench/URL": ("urls", "original_url", "encoded_url"),
        "data/clickbench/Referer": ("urls", "original_referer", "encoded_referer"),
        "data/edge/values": ("edge", "original", "encoded"),
        "data/fuzz/values": ("fuzz", "original", "encoded"),
        "data/tpch/l_comment": ("tpch", "l_comment", "encoded_l"),
        "data/tpch/o_comment": ("tpch", "o_comment", "encoded_o"),
    }
    # This module's own fixtures (make_fixtures.py): table fuzz_s<N>, column prefix in <data>/duckdb/fixtures.
    tables = {r[0] for r in con.execute("SELECT table_name FROM duckdb_tables()").fetchall()}
    for name in sorted(t for t in tables if t.startswith("fuzz_s")):
        colmap[tlsql.column_key(tlsql.FIXTURES / name / "values")] = (name, "original", "encoded")
    summary = []
    for pf in files:
        for name, col, pat, col2 in tlsql.read_patterns(pf):
            if col not in colmap or (col2 and col2 not in colmap):
                summary.append({"file": Path(pf).name, "name": name, "status": "skipped (dataset not in DB)", "column": col})
                continue
            table, raw, enc = colmap[col]
            if quick and table == "urls":
                table = "urls_100k"
            if col2:
                _, raw2, enc2 = colmap[col2]
                tl = f"token_like_concat({enc}, {enc2}, {sql_str(pat)})"
                lk = f"({raw} || {raw2}) LIKE {sql_str(pat)}"
            else:
                tl = f"token_like({enc}, {sql_str(pat)})"
                lk = f"{raw} LIKE {sql_str(pat)}"
            try:
                n_tl = con.execute(f"SELECT count(*) FROM {table} WHERE {tl}").fetchone()[0]
                n_lk = con.execute(f"SELECT count(*) FROM {table} WHERE {lk}").fetchone()[0]
                d1 = con.execute(f"SELECT count(*) FROM (SELECT row_id FROM {table} WHERE {tl} EXCEPT SELECT row_id FROM {table} WHERE {lk})").fetchone()[0]
                d2 = con.execute(f"SELECT count(*) FROM (SELECT row_id FROM {table} WHERE {lk} EXCEPT SELECT row_id FROM {table} WHERE {tl})").fetchone()[0]
                ok = n_tl == n_lk and d1 == 0 and d2 == 0
                check(ok, f"{Path(pf).name}:{name} {pat!r}: token={n_tl} like={n_lk} extra={d1} missing={d2}")
                summary.append({"file": Path(pf).name, "name": name, "table": table, "pattern": pat,
                                "concat": bool(col2), "matches": n_lk, "status": "pass" if ok else "FAIL"})
            except Exception as e:  # noqa: BLE001
                check(False, f"{Path(pf).name}:{name} {pat!r}: exception {e}")
                summary.append({"file": Path(pf).name, "name": name, "pattern": pat, "status": f"ERROR {e}"})
    return summary


def test_caching(con):
    group("caching and pattern changes")
    con.execute("SELECT tokenlike_cache_clear(), tokenlike_reset_stats()").fetchall()
    pats = ["http://%", "%.php", "https://%", "%.html", "%yandex%", "http://irr.ru/%", "%/", "%"]
    expected = {p: con.execute(f"SELECT count(*) FROM urls_100k WHERE original_url LIKE {sql_str(p)}").fetchone()[0] for p in pats}
    for rnd in range(3):
        for p in (pats if rnd % 2 == 0 else reversed(pats)):
            got = con.execute(f"SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0]
            check(got == expected[p], f"round {rnd} pattern {p!r}: {got} != {expected[p]}")
    st = tlsql.stats(con)
    check(st["matcher_builds"] == len(pats), f"expected {len(pats)} builds, got {st['matcher_builds']}")
    check(st["cache_hits"] >= 2 * len(pats), f"expected cache hits on repeated queries, got {st['cache_hits']}")

    # Prepared statement, parameter changes between executions.
    for _ in range(2):
        for p in pats:
            got = con.execute("SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, ?)", [p]).fetchone()[0]
            check(got == expected[p], f"prepared parameter {p!r}: {got} != {expected[p]}")
    stmt_sql = "SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, $1)"
    for p in pats + pats[::-1]:
        got = con.execute(stmt_sql, [p]).fetchone()[0]
        check(got == expected[p], f"$1 parameter {p!r}: {got} != {expected[p]}")

    # Two different constant patterns in one query, and the same pattern twice.
    a, b = con.execute("""SELECT count(*) FILTER (WHERE token_like(encoded_url, 'http://%')),
                                 count(*) FILTER (WHERE token_like(encoded_url, '%.php')) FROM urls_100k""").fetchone()
    check((a, b) == (expected["http://%"], expected["%.php"]), "two patterns in one query")

    # Eviction: more distinct patterns than the cache holds, then re-check early ones.
    for i in range(80):
        con.execute(f"SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, {sql_str('%' + str(i) + '%')})").fetchone()
    for p in pats:
        got = con.execute(f"SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0]
        check(got == expected[p], f"after eviction {p!r}")
    check(tlsql.stats(con)["cached_matchers"] <= 64, "cache exceeded its bound")

    # Reloading the vocabulary starts a new snapshot with an empty cache.
    g0 = tlsql.stats(con)["vocab_generation"]
    con.execute("SELECT tokenlike_load_vocab(?)", [tlsql.VOCAB.as_posix()]).fetchall()
    st = tlsql.stats(con)
    check(st["vocab_generation"] == g0 + 1 and st["cached_matchers"] == 0, f"vocab reload: {st}")
    for p in pats:
        got = con.execute(f"SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0]
        check(got == expected[p], f"after vocab reload {p!r}")
    check(con.execute("SELECT tokenlike_cache_clear()").fetchone()[0] == len(pats), "cache_clear count")
    # After a clear, the same queries rebuild their matchers once each and still agree with LIKE.
    con.execute("SELECT tokenlike_reset_stats()").fetchall()
    for _ in range(2):
        for p in pats:
            got = con.execute(f"SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0]
            check(got == expected[p], f"after cache clear {p!r}")
    st = tlsql.stats(con)
    check(st["matcher_builds"] == len(pats) and st["cache_hits"] >= len(pats),
          f"after clear: one rebuild per pattern, then hits ({st['matcher_builds']} builds, {st['cache_hits']} hits)")


def test_sql_features(con):
    group("SQL features: joins, group by, mixed expressions")
    t = "urls_100k"
    q1 = lambda w: con.execute(f"SELECT traffic_source_id, count(*) FROM {t} WHERE {w} GROUP BY 1 ORDER BY 1").fetchall()
    check(q1("token_like(encoded_url, '%.php')") == q1("original_url LIKE '%.php'"), "group by after filter")
    q2 = lambda w: con.execute(f"""SELECT a.traffic_source_id, count(*) FROM {t} a JOIN {t} b ON a.row_id = b.row_id + 1
                                   WHERE {w} GROUP BY 1 ORDER BY 1""").fetchall()
    check(q2("token_like(a.encoded_url, 'http://%') AND token_like(b.encoded_referer, '%yandex%')") ==
          q2("a.original_url LIKE 'http://%' AND b.original_referer LIKE '%yandex%'"), "join + filters on both sides")
    mixed_tl = "(token_like(encoded_url, 'http://%') AND NOT token_like(encoded_url, '%.html')) OR (row_id % 97 = 0 AND token_like(encoded_referer, ''))"
    mixed_lk = "(original_url LIKE 'http://%' AND NOT original_url LIKE '%.html') OR (row_id % 97 = 0 AND original_referer LIKE '')"
    check(con.execute(f"SELECT list(row_id ORDER BY row_id) FROM {t} WHERE {mixed_tl}").fetchone() ==
          con.execute(f"SELECT list(row_id ORDER BY row_id) FROM {t} WHERE {mixed_lk}").fetchone(), "AND/OR/NOT mix")
    check(con.execute(f"""SELECT sum(CASE WHEN token_like(encoded_url, '%.php') THEN 1 WHEN token_like(encoded_url, 'https://%') THEN 10 ELSE 100 END) FROM {t}""").fetchone() ==
          con.execute(f"""SELECT sum(CASE WHEN original_url LIKE '%.php' THEN 1 WHEN original_url LIKE 'https://%' THEN 10 ELSE 100 END) FROM {t}""").fetchone(), "CASE")
    check(con.execute(f"SELECT traffic_source_id FROM {t} GROUP BY 1 HAVING count(*) FILTER (WHERE token_like(encoded_url, 'http://%')) > 100 ORDER BY 1").fetchall() ==
          con.execute(f"SELECT traffic_source_id FROM {t} GROUP BY 1 HAVING count(*) FILTER (WHERE original_url LIKE 'http://%') > 100 ORDER BY 1").fetchall(), "HAVING / FILTER")
    check(con.execute(f"WITH m AS (SELECT row_id FROM {t} WHERE token_like(encoded_url, '%/')) SELECT count(*) FROM m JOIN {t} u USING (row_id) WHERE token_like(u.encoded_referer, 'http://%')").fetchone() ==
          con.execute(f"WITH m AS (SELECT row_id FROM {t} WHERE original_url LIKE '%/') SELECT count(*) FROM m JOIN {t} u USING (row_id) WHERE u.original_referer LIKE 'http://%'").fetchone(), "CTE + join")
    check(con.execute(f"SELECT row_id FROM {t} WHERE token_like(encoded_url, '%.php') ORDER BY row_id DESC LIMIT 5").fetchall() ==
          con.execute(f"SELECT row_id FROM {t} WHERE original_url LIKE '%.php' ORDER BY row_id DESC LIMIT 5").fetchall(), "ORDER BY / LIMIT")
    check(con.execute(f"SELECT count(*) FROM (SELECT row_id FROM {t} WHERE token_like(encoded_url, '%.php') UNION SELECT row_id FROM {t} WHERE token_like(encoded_url, '%.html'))").fetchone() ==
          con.execute(f"SELECT count(*) FROM {t} WHERE original_url LIKE '%.php' OR original_url LIKE '%.html'").fetchone(), "UNION")
    check(con.execute(f"SELECT count(*) FROM {t} WHERE token_like(encoded_url, 'http://' || 'irr.ru/%')").fetchone() ==
          con.execute(f"SELECT count(*) FROM {t} WHERE original_url LIKE 'http://irr.ru/%'").fetchone(), "folded constant expression as pattern")
    check(con.execute(f"SELECT count(*) FROM {t} WHERE token_like(encoded_url, CASE WHEN row_id % 2 = 0 THEN '%.php' ELSE 'http://%' END)").fetchone() ==
          con.execute(f"SELECT count(*) FROM {t} WHERE original_url LIKE CASE WHEN row_id % 2 = 0 THEN '%.php' ELSE 'http://%' END").fetchone(), "row-dependent pattern")
    check(con.execute(f"SELECT count(*) FROM {t} WHERE token_like(tokenize(original_url), '%.php')").fetchone() ==
          con.execute(f"SELECT count(*) FROM {t} WHERE original_url LIKE '%.php'").fetchone(), "token_like over SQL tokenize() (different segmentation)")
    check(con.execute(f"SELECT count(*) FROM {t} WHERE detokenize(encoded_url) LIKE '%.php'").fetchone() ==
          con.execute(f"SELECT count(*) FROM {t} WHERE original_url LIKE '%.php'").fetchone(), "decode+LIKE baseline")
    w_tl = con.execute(f"SELECT traffic_source_id, sum(token_like(encoded_url, '%.php')::INT) OVER (PARTITION BY traffic_source_id) s FROM {t} QUALIFY row_number() OVER (PARTITION BY traffic_source_id ORDER BY row_id) = 1 ORDER BY 1").fetchall()
    w_lk = con.execute(f"SELECT traffic_source_id, sum((original_url LIKE '%.php')::INT) OVER (PARTITION BY traffic_source_id) s FROM {t} QUALIFY row_number() OVER (PARTITION BY traffic_source_id ORDER BY row_id) = 1 ORDER BY 1").fetchall()
    check(w_tl == w_lk, "window aggregate over token_like")


def test_nulls_and_empty(con):
    group("NULLs, empty strings, empty tables")
    r = con.execute("SELECT token_like(NULL::BLOB, '%'), token_like(tokenize('a'), NULL), token_like(''::BLOB, ''), token_like(''::BLOB, '%'), token_like(''::BLOB, '_'), token_like_concat(NULL, ''::BLOB, '%'), token_like_concat(''::BLOB, ''::BLOB, '')").fetchone()
    check(r == (None, None, True, True, False, None, True), f"NULL/empty semantics: {r}")
    check(con.execute("SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, NULL)").fetchone()[0] == 0, "NULL pattern filters everything")
    check(con.execute("SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, NULL) IS NULL").fetchone()[0] == 1000, "NULL pattern yields NULL")
    con.execute("CREATE OR REPLACE TEMP TABLE empty_t (row_id BIGINT, encoded BLOB)")
    check(con.execute("SELECT count(*) FROM empty_t WHERE token_like(encoded, '%')").fetchone()[0] == 0, "empty table")
    check(con.execute("SELECT count(*) FROM urls_1k WHERE false AND token_like(encoded_url, '%')").fetchone()[0] == 0, "short-circuited filter")
    # Empty-string rows in real data (78,775 Referers are empty).
    check(con.execute("SELECT count(*) FROM urls WHERE token_like(encoded_referer, '')").fetchone() ==
          con.execute("SELECT count(*) FROM urls WHERE original_referer LIKE ''").fetchone(), "empty pattern on real Referer")
    s = "x" * 10
    con.execute("CREATE OR REPLACE TEMP TABLE nul_t AS SELECT 'a' || chr(0) || 'b' AS s")
    check(con.execute("SELECT token_like(tokenize(s), 'a_b'), s LIKE 'a_b', token_like(tokenize(s), 'a' || chr(0) || '%'), s LIKE 'a' || chr(0) || '%' FROM nul_t").fetchone() ==
          con.execute("SELECT s LIKE 'a_b', s LIKE 'a_b', s LIKE 'a' || chr(0) || '%', s LIKE 'a' || chr(0) || '%' FROM nul_t").fetchone(), "embedded NUL byte in data and pattern")
    del s


def test_errors(con, small_vocab: Path):
    group("errors and invalid inputs")
    expect_error(con, "SELECT token_like('\\x01'::BLOB, '%')", "odd length", "odd-length blob")
    # A VARCHAR column has no implicit cast to BLOB, so DuckDB's binder rejects it; a string literal
    # could be cast to BLOB, and the extension's bind refuses it instead of reading text as token ids.
    expect_error(con, "SELECT token_like(original_url, '%') FROM urls_1k", "No function matches", "VARCHAR column as token column")
    expect_error(con, "SELECT token_like('ab', '%')", "must be BLOBs", "string literal as token column")
    expect_error(con, "SELECT token_like_concat(tokenize('a'), 'ab', '%')", "must be BLOBs", "string literal as concat column")
    expect_error(con, "SELECT detokenize('\\xFF\\x00'::BLOB)", "not valid utf-8", "decoding to invalid UTF-8")
    # Byte tokens (id b = byte b) forming invalid UTF-8, incl. after an ASCII run (fast path exit).
    for bs in [b"\xc3", b"\x80", b"\xe2\x82", b"\xf0\x9f\x98", b"abcdefgh\xc3", b"abcdefghijklmnop\xed\xa0\x80",
               b"\xc0\xaf", b"\xf5\x80\x80\x80"]:
        hexs = "".join(f"{x:02X}00" for x in bs)
        expect_error(con, f"SELECT detokenize(from_hex('{hexs}'))", "not valid utf-8", f"invalid UTF-8 {bs!r}")
    for s in ["abcdefgh", "abcdefghé", "x" * 7 + "é" + "y" * 20, "a" * 16 + "日本", "😀x" * 9]:
        check(con.execute("SELECT detokenize(tokenize(?)) = ?", [s, s]).fetchone()[0] is True, f"round trip {s!r}")
    long_pat = "%" + "_a" * 300 + "%"
    err = expect_error(con, f"SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, {sql_str(long_pat)})", "states", "TooComplex pattern")
    check(err is None or "DFA states" in err, f"TooComplex message names the limit: {err}")
    expect_error(con, "SELECT tokenlike_load_vocab('C:/definitely/missing/vocab.bin')", "vocab", "missing vocab file")
    check(tlsql.stats(con)["vocab_loaded"], "failed load keeps the previous vocabulary")
    expect_error(con, "SELECT token_like(from_hex('FF'), '%')", "odd length", "odd blob from from_hex")
    # BLOB -> VARCHAR casts escape non-UTF-8 bytes ('\xFF' text), so SQL cannot produce an invalid
    # UTF-8 pattern; the escaped text must still behave exactly like LIKE.
    check(con.execute(r"SELECT token_like(tokenize('\xFF'), from_hex('FF')::VARCHAR), '\xFF' LIKE from_hex('FF')::VARCHAR").fetchone() == (True, True),
          "escaped BLOB->VARCHAR pattern behaves like LIKE")
    # The connection keeps working after errors.
    check(con.execute("SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, 'http://%')").fetchone() ==
          con.execute("SELECT count(*) FROM urls_1k WHERE original_url LIKE 'http://%'").fetchone(), "connection usable after errors")

    # Out-of-vocabulary ids: a 1,000-token vocabulary makes ids >= 1000 invalid.
    con2 = tlsql.connect(None, vocab=small_vocab)
    con2.execute("CREATE TABLE t AS SELECT '\\x41\\x00'::BLOB AS ok, '\\xE8\\x03'::BLOB AS bad")  # ids 65, 1000
    check(con2.execute("SELECT token_like(ok, 'A') FROM t").fetchone()[0] is True, "small vocab valid id")
    expect_error(con2, "SELECT token_like(bad, '%') FROM t", "outside the vocabulary", "id >= vocab.n in token_like")
    expect_error(con2, "SELECT token_like_concat(ok, bad, '%') FROM t", "outside the vocabulary", "id >= vocab.n in concat")
    expect_error(con2, "SELECT detokenize(bad) FROM t", "outside the vocabulary", "id >= vocab.n in detokenize")
    con2.close()

    con3 = tlsql.connect(None, vocab=None)
    expect_error(con3, "SELECT token_like(''::BLOB, '%')", "no vocabulary loaded", "query before vocabulary is loaded")
    expect_error(con3, "SELECT detokenize(''::BLOB)", "no vocabulary loaded", "detokenize before vocabulary")
    con3.close()


def test_large_rows(con):
    group("large rows and long patterns")
    big = ("abc/" * 250_000) + "end.php"           # 1,000,007 bytes
    con.execute("CREATE OR REPLACE TEMP TABLE big AS SELECT ? AS s", [big])
    con.execute("ALTER TABLE big ADD COLUMN e BLOB")
    con.execute("UPDATE big SET e = tokenize(s)")
    for p in ["%.php", "abc/%", "%end%", "%abc/end%", "%_.php", "abc/abc/%abc/end.ph_", "%x%"]:
        check(con.execute(f"SELECT token_like(e, {sql_str(p)}) = (s LIKE {sql_str(p)}) FROM big").fetchone()[0], f"1 MB row {p!r}")
    ntok = con.execute("SELECT octet_length(e) // 2 FROM big").fetchone()[0]
    check(ntok > 100_000, f"big row has {ntok} tokens")
    # The longest pattern in Person B's fuzz set family (365 bytes) already runs in the real-data group;
    # here: a long literal prefix that is just inside the state limit.
    p = "http://" + "x" * 400 + "%"
    check(con.execute(f"SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0] == 0, "400-byte literal prefix")


def test_concurrency(con_path: Path):
    group("concurrent queries")
    base = tlsql.connect(con_path, read_only=True)
    pats = ["http://%", "%.php", "%yandex%", "https://%", "%.html", "%/", "http://irr.ru/%", "%_%_%"]
    exp = {p: base.execute(f"SELECT count(*) FROM urls_100k WHERE original_url LIKE {sql_str(p)}").fetchone()[0] for p in pats}
    errors, mism = [], []

    def worker(k):
        cur = base.cursor()
        rng = random.Random(k)
        for _ in range(12):
            p = rng.choice(pats)
            try:
                got = cur.execute(f"SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, {sql_str(p)})").fetchone()[0]
                if got != exp[p]:
                    mism.append((k, p, got, exp[p]))
            except Exception as e:  # noqa: BLE001
                errors.append((k, p, str(e)))
        cur.close()

    th = [threading.Thread(target=worker, args=(k,)) for k in range(6)]
    for t in th:
        t.start()
    for t in th:
        t.join()
    check(not errors, f"errors in concurrent queries: {errors[:3]}")
    check(not mism, f"wrong concurrent results: {mism[:3]}")
    base.close()


def test_reopen(db: Path):
    group("close / reopen / repeated statements")
    exp = None
    for i in range(3):
        con = tlsql.connect(db, read_only=True, threads=1 if i == 1 else None)
        got = con.execute("SELECT count(*) FROM urls WHERE token_like(encoded_url, '%.php')").fetchone()[0]
        if exp is None:
            exp = con.execute("SELECT count(*) FROM urls WHERE original_url LIKE '%.php'").fetchone()[0]
        check(got == exp, f"reopen {i}: {got} != {exp}")
        con.execute(f"LOAD '{tlsql.EXT.as_posix()}'")  # second LOAD on the same database is harmless
        check(con.execute("SELECT count(*) FROM urls WHERE token_like(encoded_url, '%.php')").fetchone()[0] == exp, "after second LOAD")
        con.close()
    con = tlsql.connect(db, read_only=True)
    for _ in range(50):
        check(con.execute("SELECT count(*) FROM urls_10k WHERE token_like(encoded_url, 'http://%')").fetchone() ==
              con.execute("SELECT count(*) FROM urls_10k WHERE original_url LIKE 'http://%'").fetchone(), "repeated statement")
    con.close()


def test_bug_hunt(db: Path, small_vocab: Path):
    group("bug hunting: reload races, interrupts, multiple databases")
    # 1. Vocabulary reloads racing with running queries (same file: results must not change).
    base = tlsql.connect(db, read_only=True)
    exp = base.execute("SELECT count(*) FROM urls_100k WHERE original_url LIKE '%.php'").fetchone()[0]
    exp_d = base.execute("SELECT count(*) FROM urls_100k WHERE original_url LIKE 'http://%'").fetchone()[0]
    stop, errs, bad = threading.Event(), [], []

    def reloader():
        cur = base.cursor()
        while not stop.is_set():
            try:
                cur.execute(f"SELECT tokenlike_load_vocab({sql_str(tlsql.VOCAB.as_posix())})").fetchall()
            except Exception as e:  # noqa: BLE001
                errs.append(str(e))
        cur.close()

    def querier(k):
        cur = base.cursor()
        for i in range(10):
            try:
                if (i + k) % 2:
                    got = cur.execute("SELECT count(*) FROM urls_100k WHERE token_like(encoded_url, '%.php')").fetchone()[0]
                    want = exp
                else:
                    got = cur.execute("SELECT count(*) FROM urls_100k WHERE detokenize(encoded_url) LIKE 'http://%'").fetchone()[0]
                    want = exp_d
                if got != want:
                    bad.append((k, i, got, want))
            except Exception as e:  # noqa: BLE001
                errs.append(str(e))
        cur.close()

    r = threading.Thread(target=reloader)
    r.start()
    qs = [threading.Thread(target=querier, args=(k,)) for k in range(4)]
    for t in qs:
        t.start()
    for t in qs:
        t.join()
    stop.set()
    r.join()
    check(not errs, f"errors during reload race: {errs[:3]}")
    check(not bad, f"wrong results during reload race: {bad[:3]}")
    check(tlsql.stats(base)["vocab_generation"] > 2, "reloads actually happened")

    # 2. Interrupt a long-running token_like query from another thread.
    cur = base.cursor()
    big_q = """SELECT count(*) FROM urls a, (SELECT range FROM range(200)) r
               WHERE token_like_concat(a.encoded_url, a.encoded_referer, '%' || (r.range % 7)::VARCHAR || '%yandex%')"""
    timer = threading.Timer(0.5, cur.interrupt)
    timer.start()
    t0 = time.perf_counter()
    try:
        cur.execute(big_q).fetchall()
        interrupted = False
    except Exception as e:  # noqa: BLE001
        interrupted = "interrupt" in str(e).lower()
    timer.cancel()
    check(interrupted and time.perf_counter() - t0 < 30, "long query was interrupted cleanly")
    check(cur.execute("SELECT count(*) FROM urls_1k WHERE token_like(encoded_url, '%.php')").fetchone() ==
          cur.execute("SELECT count(*) FROM urls_1k WHERE original_url LIKE '%.php'").fetchone(), "cursor usable after interrupt")
    cur.close()
    base.close()

    # 3. Two independent databases in one process: separate extension state.
    a = tlsql.connect(None)
    b = tlsql.connect(None, vocab=None)
    check(tlsql.stats(a)["vocab_loaded"] and not tlsql.stats(b)["vocab_loaded"], "per-database extension state")
    check(a.execute("SELECT token_like(tokenize('x.php'), '%.php')").fetchone()[0] is True, "db A works")
    expect_error(b, "SELECT token_like(''::BLOB, '%')", "no vocabulary", "db B has no vocabulary")
    b.execute(f"SELECT tokenlike_load_vocab({sql_str(small_vocab.as_posix())})").fetchall()
    check(tlsql.stats(a)["vocab_n"] == 65536 and tlsql.stats(b)["vocab_n"] == 1000, "vocabularies stay separate")
    a.close()
    b.close()

    # 4. Runtime (non-constant) pattern that exceeds the automaton limit: clean execution error.
    m = tlsql.connect(None)
    m.execute("CREATE TABLE p AS SELECT * FROM (VALUES ('%a%'), (?)) v(p)", ["%" + "_a" * 300 + "%"])
    expect_error(m, "SELECT token_like(tokenize('aaa'), p) FROM p", "DFA states", "TooComplex on the runtime path")
    check(m.execute("SELECT token_like(tokenize('bab'), '%a%')").fetchone()[0] is True, "usable after runtime error")
    m.close()


def synthetic_vocab(path: Path) -> list[bytes]:
    """A small vocabulary for data-free runs: all 256 single bytes plus 2..6-byte substrings of the
    fixture strings (including substrings that cut UTF-8 sequences)."""
    toks = [bytes([b]) for b in range(256)]
    seen = set(toks)
    for s in FIXTURE_STRINGS + FIXTURE_PATTERNS:
        data = s.replace("%", "").replace("_", "").encode("utf-8")
        for L in range(2, 7):
            for i in range(0, max(len(data) - L + 1, 0)):
                t = data[i:i + L]
                if t not in seen and len(toks) < 6000:
                    seen.add(t)
                    toks.append(t)
    write_vocab(path, toks)
    return toks


def write_vocab(path: Path, toks: list[bytes]) -> None:
    offs = [0]
    for t in toks:
        offs.append(offs[-1] + len(t))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<I", len(toks)) + struct.pack(f"<{len(toks) + 1}I", *offs) + b"".join(toks))


def test_smoke_extra(mem, small_vocab: Path):
    group("smoke: NULLs, errors, cache, diagnostics (no dataset)")
    r = mem.execute("SELECT token_like(NULL::BLOB, '%'), token_like(tokenize('a'), NULL), token_like(''::BLOB, ''), "
                    "token_like(''::BLOB, '_'), token_like_concat(NULL, ''::BLOB, '%'), "
                    "token_like_concat(''::BLOB, ''::BLOB, '')").fetchone()
    check(r == (None, None, True, False, None, True), f"NULL/empty semantics: {r}")
    expect_error(mem, r"SELECT token_like('\x01'::BLOB, '%')", "odd length", "odd-length blob")
    expect_error(mem, "SELECT token_like('ab', '%')", "must be BLOBs", "string literal as token column")
    expect_error(mem, r"SELECT detokenize('\xFF\x00'::BLOB)", "not valid utf-8", "invalid UTF-8 decode")
    expect_error(mem, "SELECT token_like(tokenize('a'), " + sql_str("%" + "_a" * 300 + "%") + ")", "DFA states",
                 "too-complex pattern")
    # Pattern changes and cache clears on the same connection.
    mem.execute("SELECT tokenlike_cache_clear(), tokenlike_reset_stats()").fetchall()
    pats = ("%a%", "a%", "%a", "_", "%é%")
    for rnd in range(3):
        for p in pats:
            a = mem.execute(f"SELECT list(row_id ORDER BY row_id) FROM fx WHERE token_like(enc, {sql_str(p)})").fetchone()[0]
            b = mem.execute(f"SELECT list(row_id ORDER BY row_id) FROM fx WHERE s LIKE {sql_str(p)}").fetchone()[0]
            check(a == b, f"round {rnd} pattern {p!r}")
        if rnd == 1:
            check(mem.execute("SELECT tokenlike_cache_clear()").fetchone()[0] == len(pats), "cache held one matcher per pattern")
    st = tlsql.stats(mem)
    check(st["matcher_builds"] == 2 * len(pats), f"each pattern built once before and once after the clear: {st['matcher_builds']}")
    # Diagnostics functions.
    n_nn = mem.execute("SELECT count(enc) FROM fx").fetchone()[0]
    got = mem.execute("SELECT count(*) FILTER (WHERE tokenlike_floor_noop(enc)), "
                      "count(*) FILTER (WHERE tokenlike_floor_copy(enc)) FROM fx").fetchone()
    check(got == (n_nn, n_nn), f"floor functions are TRUE exactly for non-NULL rows: {got} vs {n_nn}")
    check(mem.execute("SELECT tokenlike_floor_copy(NULL::BLOB)").fetchone()[0] is None, "floor_copy NULL")
    expect_error(mem, r"SELECT tokenlike_floor_copy('\x01'::BLOB)", "odd length", "floor_copy validates like token_like")
    # Out-of-vocabulary ids with a 1,000-token vocabulary.
    c2 = tlsql.connect(None, vocab=small_vocab)
    c2.execute(r"CREATE TABLE t AS SELECT '\x41\x00'::BLOB AS ok, '\xE8\x03'::BLOB AS bad")  # ids 65, 1000
    check(c2.execute("SELECT token_like(ok, 'A') FROM t").fetchone()[0] is True, "small vocab valid id")
    expect_error(c2, "SELECT token_like(bad, '%') FROM t", "outside the vocabulary", "id >= vocab.n")
    expect_error(c2, "SELECT detokenize(bad) FROM t", "outside the vocabulary", "id >= vocab.n detokenize")
    c2.close()
    c3 = tlsql.connect(None, vocab=None)
    expect_error(c3, "SELECT token_like(''::BLOB, '%')", "no vocabulary loaded", "query before vocabulary is loaded")
    c3.close()


def test_floors(con):
    group("scan-floor diagnostics")
    n_nn = con.execute("SELECT count(encoded_url) FROM urls").fetchone()[0]
    a, b = con.execute("SELECT count(*) FILTER (WHERE tokenlike_floor_noop(encoded_url)), "
                       "count(*) FILTER (WHERE tokenlike_floor_copy(encoded_url)) FROM urls").fetchone()
    check(a == n_nn and b == n_nn, f"floors select every non-NULL row ({a}, {b}, {n_nn})")
    plan = con.execute("EXPLAIN SELECT count(*) FROM urls WHERE tokenlike_floor_copy(encoded_url)").fetchall()[0][1]
    check("tokenlike_floor_copy" in plan, "floor function stays in the plan (not optimised away)")


DECODE_SELECTED_SQL = """SELECT detokenize(encoded_url) AS url, n FROM (
    SELECT encoded_url, count(*) AS n FROM urls WHERE token_like(encoded_url, '%.php')
    GROUP BY encoded_url ORDER BY n DESC, encoded_url LIMIT 10)"""


def test_decode_selected(con):
    group("filter + group in token space, decode only the output")
    con.execute("SELECT tokenlike_reset_stats()").fetchall()
    top_tok = con.execute(DECODE_SELECTED_SQL).fetchall()
    decoded = tlsql.stats(con)["decoded_rows"]
    check(len(top_tok) == 10, f"10 output rows ({len(top_tok)})")
    check(decoded == 10, f"detokenize ran on exactly the 10 output rows (decoded_rows={decoded})")
    full = dict(con.execute("SELECT original_url, count(*) FROM urls WHERE original_url LIKE '%.php' GROUP BY 1").fetchall())
    check(all(full.get(u) == n for u, n in top_tok), "every decoded URL has its raw-text count")
    counts = sorted(full.values(), reverse=True)
    check([n for _, n in top_tok] == counts[:10], "the 10 counts are the 10 largest raw-text group counts")
    tok_groups = con.execute("SELECT count(DISTINCT encoded_url) FROM urls WHERE token_like(encoded_url, '%.php')").fetchone()[0]
    check(tok_groups == len(full), f"same number of groups ({tok_groups} vs {len(full)})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="real-data patterns on urls_100k instead of 1M rows")
    ap.add_argument("--smoke", action="store_true", help="data-free run: fixtures + error/cache checks only")
    ap.add_argument("--ext", help="extension file to test (default: TOKENLIKE_EXT or build/duckdb)")
    tlsql.add_output_args(ap, "results/test_results*.json")
    args = ap.parse_args()
    report = None
    if not args.smoke:  # checked before the run, which takes minutes
        name = "test_results_quick.json" if args.quick else os.environ.get("TOKENLIKE_TEST_RESULTS", "test_results.json")
        report = tlsql.output(args, f"results/{name}")
    elif args.out is not None or args.update_reference:
        print("ERROR: --smoke writes no report; --out / --update-reference apply to the full and quick runs")
        return 2
    if args.ext:
        tlsql.EXT = Path(args.ext).resolve()
    t0 = time.perf_counter()
    rng = random.Random(20261025)
    tlsql.SCRATCH.mkdir(parents=True, exist_ok=True)

    vocab = tlsql.VOCAB
    if args.smoke and not vocab.exists():
        vocab = tlsql.SCRATCH / "vocab_synthetic.bin"
        n = len(synthetic_vocab(vocab))
        print(f"smoke: {tlsql.rel(tlsql.VOCAB)} not found, using a synthetic {n}-token vocabulary", flush=True)
    elif not vocab.exists():
        print(f"ERROR: {tlsql.rel(vocab)} not found. Generate data (scripts/encode) or run with --smoke.")
        return 2
    toks = read_vocab(vocab)
    enc = Encoder(toks)
    small = tlsql.SCRATCH / "vocab_1000.bin"
    write_vocab(small, toks[:1000])
    if not args.smoke and not tlsql.DB.exists():
        print(f"ERROR: {tlsql.rel(tlsql.DB)} not found. Run integration/duckdb/python/build_db.py or use --smoke.")
        return 2

    mem = tlsql.connect(None, vocab=vocab)
    build_fixture(mem, enc, rng)
    real_summary = []
    steps = [lambda: test_fixture_constant(mem), lambda: test_fixture_runtime(mem), lambda: test_fixture_concat(mem),
             lambda: test_smoke_extra(mem, small)]
    con = None
    if not args.smoke:
        con = tlsql.connect(tlsql.DB, read_only=True)
        files = [tlsql.PATTERNS / f for f in ("core.txt", "tier1.txt", "tier2.txt", "generated_edge.txt", "generated_fuzz.txt")]
        extra = sorted(tlsql.FIXTURES.glob("fuzz_s*.txt"))  # optional: make_fixtures.py --seed 7
        legacy = tlsql.SCRATCH / "fuzz_s7.txt"  # earlier layout: seed-7 patterns over the data/fuzz column
        if not extra and legacy.exists():
            print(f"note: using the earlier {tlsql.rel(legacy)}; make_fixtures.py replaces it", flush=True)
            extra = [legacy]
        files += extra
        steps += [
            lambda: real_summary.extend(test_real_data(con, files, args.quick)),
            lambda: test_caching(con), lambda: test_sql_features(con), lambda: test_nulls_and_empty(con),
            lambda: test_errors(con, small), lambda: test_large_rows(con), lambda: test_floors(con),
            lambda: test_decode_selected(con), lambda: test_concurrency(tlsql.DB),
            lambda: test_reopen(tlsql.DB), lambda: test_bug_hunt(tlsql.DB, small),
        ]
    for s in steps:
        try:
            s()
        except Exception:  # noqa: BLE001
            RESULTS.setdefault(CURRENT[0], {"pass": 0, "fail": 0})
            check(False, "uncaught exception:\n" + traceback.format_exc())
        print(f"{CURRENT[0]}: {RESULTS[CURRENT[0]]}", flush=True)
    if con is not None:
        con.close()
    mem.close()

    total_pass = sum(r["pass"] for r in RESULTS.values())
    total_fail = sum(r["fail"] for r in RESULTS.values())
    skipped = [s for s in real_summary if s["status"].startswith("skipped")]
    mode = "smoke" if args.smoke else ("quick" if args.quick else "full")
    out = {"extension": tlsql.rel(tlsql.EXT), "duckdb": duckdb.__version__, "mode": mode,
           "vocabulary": tlsql.rel(vocab), "seconds": round(time.perf_counter() - t0, 1), "groups": RESULTS,
           "total_pass": total_pass, "total_fail": total_fail, "failures": FAILURES[:200],
           "real_data_patterns": len(real_summary) - len(skipped), "real_data_skipped": skipped,
           "real_data": real_summary}
    if report is not None:
        report.write(json.dumps(out, indent=2, ensure_ascii=False))
        print("wrote", tlsql.rel(report.path))
    print(f"\n{'ALL PASSED' if total_fail == 0 else 'FAILURES'} ({mode}): {total_pass} checks passed, {total_fail} failed, "
          f"{len(real_summary) - len(skipped)} real-data patterns, {len(skipped)} skipped, {out['seconds']} s")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
