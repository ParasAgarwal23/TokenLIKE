#!/usr/bin/env python3
"""DuckDB ground truth for the FSST-LIKE experiment.

DuckDB is the only authority on what SQL LIKE means here. It reads the original
TokenLIKE canonical parquet files (read-only) and never looks at anything this
experiment produced, so it cannot agree with the matcher by construction.

  python3 scripts/truth.py patterns/core.txt
  python3 scripts/truth.py --semantics
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (DATA, TRUTH, ColumnMeta, PatternSpec, backend_name, connect_duckdb,  # noqa: E402
                    load_column, parse_pattern_file, sha256_file, write_ids)

MANIFEST = TRUTH / "truth.json"

SEMANTICS = [
    ("'' LIKE ''", True),
    ("'' LIKE '%'", True),
    ("'' LIKE '_'", False),
    ("'abc' LIKE 'abc'", True),
    ("'abc' LIKE 'ABC'", False),
    ("'abc' LIKE 'a_c'", True),
    # '_' is one character, not one byte: three 3-byte characters match '___'.
    ("'日本語' LIKE '___'", True),
    ("'日本語' LIKE '_________'", False),
    ("'é' LIKE '_'", True),
    ("'\U0001F389' LIKE '_'", True),
    # Backslash is an ordinary byte without an explicit ESCAPE clause.
    ("'a%b' LIKE 'a\\%b'", False),
    ("'a\\%b' LIKE 'a\\%b'", True),
    ("'a%b' LIKE 'a%b'", True),
    ("'сезон' LIKE '%ез%'", True),
    ("'сезон' LIKE '%ЕЗ%'", False),
]


def fingerprint(col: ColumnMeta) -> str:
    return f"{sha256_file(col.parquet)[:16]}:{col.rows}"


def load_manifest() -> dict:
    if MANIFEST.exists():
        return json.loads(MANIFEST.read_text())
    return {}


def save_manifest(m: dict) -> None:
    TRUTH.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(m, indent=1, sort_keys=True) + "\n")


def evaluate(con, spec: PatternSpec) -> list[int]:
    col = load_column(spec.column)
    if not col.parquet.exists():
        raise SystemExit(f"{col.parquet} is missing; the ground truth needs the original canonical parquet")
    if spec.is_concat:
        col2 = load_column(spec.column2)
        rows = min(col.rows, col2.rows)
        sql = ("SELECT a.row_id FROM read_parquet(?) a JOIN read_parquet(?) b USING (row_id) "
               "WHERE a.row_id < ? AND (a.value || b.value) LIKE ? ORDER BY a.row_id")
        params = [str(col.parquet), str(col2.parquet), rows, spec.pattern]
    else:
        sql = "SELECT row_id FROM read_parquet(?) WHERE row_id < ? AND value LIKE ? ORDER BY row_id"
        params = [str(col.parquet), col.rows, spec.pattern]
    return [int(r[0]) for r in con.execute(sql, params).fetchall()]


def ensure_truth(specs: list[PatternSpec], force: bool = False, quiet: bool = False) -> dict:
    manifest = load_manifest()
    con = None
    fp_cache: dict[str, str] = {}
    built = 0
    for spec in specs:
        col = load_column(spec.column)
        key = spec.column + ("|" + spec.column2 if spec.is_concat else "")
        if key not in fp_cache:
            fp = fingerprint(col)
            if spec.is_concat:
                fp += "+" + fingerprint(load_column(spec.column2))
            fp_cache[key] = fp
        fp = fp_cache[key]
        entry = manifest.get(spec.name)
        fresh = (entry and entry.get("input") == fp and entry.get("pattern") == spec.pattern
                 and spec.truth_path.exists())
        if fresh and not force:
            continue
        if con is None:
            con = connect_duckdb()
            if not quiet:
                print(f"ground truth backend: {backend_name(con)}", flush=True)
        ids = evaluate(con, spec)
        write_ids(spec.truth_path, ids)
        manifest[spec.name] = {"pattern": spec.pattern, "column": spec.column,
                               "column2": spec.column2, "input": fp, "matches": len(ids),
                               "rows": col.rows}
        built += 1
        if not quiet:
            print(f"  {spec.name}: {len(ids)} matches of {col.rows} rows", flush=True)
    if built:
        save_manifest(manifest)
    elif not quiet:
        print("ground truth already current for every pattern")
    return manifest


def probe_semantics() -> int:
    con = connect_duckdb()
    print(f"DuckDB LIKE semantics, measured via {backend_name(con)}:")
    bad = 0
    for expr, expected in SEMANTICS:
        got = con.execute(f"SELECT {expr}").fetchall()[0][0]
        got = got in (True, "true", "True", 1, "1")
        flag = "ok " if got == expected else "DIFFERS"
        if got != expected:
            bad += 1
        print(f"  {flag} {expr} -> {got}")
    if bad:
        print(f"{bad} probe(s) disagree with what this experiment assumes; the matcher is built on these.")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("patterns", nargs="?", type=Path)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--semantics", action="store_true")
    args = ap.parse_args()
    if args.semantics:
        return probe_semantics()
    if not args.patterns:
        ap.error("give a pattern file, or --semantics")
    specs = parse_pattern_file(args.patterns)
    ensure_truth(specs, force=args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
