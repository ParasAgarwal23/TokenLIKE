#!/usr/bin/env python3
"""Generate DuckDB ground truth for LIKE patterns.

Evaluates each pattern with DuckDB against the canonical parquet and writes the
matching row ids as sorted little-endian uint32:

    data/truth/<dataset>/<name>.ids
    data/truth/<dataset>/truth.json    manifest: pattern, match count, hashes

This reads the canonical parquet and never our own binaries, so the oracle is
independent of both the encoder and Person A's matcher: a bug shared between
the encoder and the matcher cannot hide here.

Patterns are passed to DuckDB as bound parameters, never interpolated into SQL.

Usage:
    python scripts/verify/ground_truth.py patterns/core.txt
    python scripts/verify/ground_truth.py patterns/core.txt --force
    python scripts/verify/ground_truth.py --semantics
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import (  # noqa: E402
    Pattern,
    TRUTH_ROOT,
    backend_name,
    connect_duckdb,
    parse_pattern_file,
    read_ids,
    sha256_file,
    write_ids,
)

# DuckDB LIKE semantics that Person A's matcher has to reproduce. Each entry is
# (value, pattern); the probe prints DuckDB's verdict so the matcher is written
# against measured behaviour rather than an assumption about it.
SEMANTICS_PROBE = [
    ("", ""),
    ("", "%"),
    ("", "%%"),
    ("", "_"),
    ("a", ""),
    ("a", "_"),
    ("abc", "a%"),
    ("abc", "%c"),
    ("abc", "%b%"),
    ("abc", "abc"),
    ("abc", "a_c"),
    ("abc", "a__"),
    ("abc", "a___"),
    ("ABC", "abc"),
    ("é", "_"),          # one 2-byte codepoint: does _ mean byte or character?
    ("é", "__"),
    ("日本語", "___"),     # three 3-byte codepoints
    ("日本語", "日%"),
    ("日本語", "%語"),
    ("日本語", "_本_"),
    ("a%b", "a%b"),       # a literal % in the value
    ("a%b", "a\\%b"),     # escaped % matches the literal
    ("a_b", "a\\_b"),
    ("100%", "100%"),
    ("100%", "%\\%"),
]


def connect(prefer_cli: bool = False):
    """Open a DuckDB connection with deterministic ordering."""
    return connect_duckdb(prefer_cli=prefer_cli)


def parquet_for(prefix: str) -> str:
    path = prefix + ".parquet"
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"{path} is missing; generate the column first "
            f"(scripts/encode/make_column.py or fetch_clickbench.py)"
        )
    return path


def evaluate(con, pat: Pattern) -> list[int]:
    """Return the sorted matching row ids for one pattern."""
    if pat.is_concat:
        left, right = parquet_for(pat.column), parquet_for(pat.column2)
        rows = con.execute(
            """
            SELECT a.row_id
            FROM read_parquet(?) AS a
            JOIN read_parquet(?) AS b USING (row_id)
            WHERE (a.value || b.value) LIKE ?
            ORDER BY a.row_id
            """,
            [left, right, pat.pattern],
        ).fetchall()
    else:
        rows = con.execute(
            """
            SELECT row_id
            FROM read_parquet(?)
            WHERE value LIKE ?
            ORDER BY row_id
            """,
            [parquet_for(pat.column), pat.pattern],
        ).fetchall()
    return [r[0] for r in rows]


def manifest_path(dataset: str) -> str:
    return os.path.join(TRUTH_ROOT, dataset, "truth.json")


def load_manifest(dataset: str) -> dict:
    path = manifest_path(dataset)
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {}


def save_manifest(dataset: str, manifest: dict) -> None:
    path = manifest_path(dataset)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")


def input_fingerprint(pat: Pattern) -> str:
    """Hash of everything the truth depends on, so staleness is detectable."""
    parts = [sha256_file(parquet_for(pat.column))]
    if pat.is_concat:
        parts.append(sha256_file(parquet_for(pat.column2)))
    return "+".join(p[:16] for p in parts)


def ensure_truth(con, pat: Pattern, force: bool) -> tuple[int, bool]:
    """Generate truth for one pattern if missing or stale.

    Returns (match_count, regenerated).
    """
    manifest = load_manifest(pat.dataset)
    entry = manifest.get(pat.name)
    fingerprint = input_fingerprint(pat)

    fresh = (
        not force
        and entry is not None
        and entry.get("pattern") == pat.pattern
        and entry.get("column") == pat.column
        and entry.get("column2") == pat.column2
        and entry.get("input_fingerprint") == fingerprint
        and os.path.exists(pat.truth_path)
    )
    if fresh:
        # Trust the cache only if the id file still holds the stated count.
        if len(read_ids(pat.truth_path)) == entry["matches"]:
            return entry["matches"], False

    started = time.monotonic()
    ids = evaluate(con, pat)
    elapsed = time.monotonic() - started
    write_ids(pat.truth_path, ids)

    manifest[pat.name] = {
        "pattern": pat.pattern,
        "column": pat.column,
        "column2": pat.column2,
        "matches": len(ids),
        "ids_path": pat.truth_path,
        "ids_sha256": sha256_file(pat.truth_path),
        "input_fingerprint": fingerprint,
        "eval_seconds": round(elapsed, 4),
        "oracle": "duckdb",
    }
    save_manifest(pat.dataset, manifest)
    return len(ids), True


def probe_semantics(con) -> int:
    """Print DuckDB's LIKE verdicts on the cases most likely to bite.

    These are the authoritative answers the matcher must reproduce, in
    particular whether `_` advances one byte or one character.
    """
    print("DuckDB LIKE semantics (the behaviour the matcher must match):")
    print(f"  {'value':<12} {'pattern':<10} verdict")
    for value, pattern in SEMANTICS_PROBE:
        (result,) = con.execute("SELECT ? LIKE ?", [value, pattern]).fetchone()
        print(f"  {value!r:<12} {pattern!r:<10} {bool(result)}")

    (v,) = con.execute("SELECT version()").fetchone()
    print(f"\n  duckdb version: {v}")
    print(
        "  note: '_' advancing per character (not per byte) is visible in the\n"
        "        multi-byte rows above; Person A's matcher must agree."
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pattern_file", nargs="?", help="e.g. patterns/core.txt")
    ap.add_argument("--force", action="store_true", help="regenerate even if cached")
    ap.add_argument(
        "--semantics",
        action="store_true",
        help="print DuckDB's LIKE verdicts on the edge cases and exit",
    )
    ap.add_argument(
        "--cli",
        action="store_true",
        help="force the pinned DuckDB CLI backend instead of the Python module",
    )
    args = ap.parse_args()

    con = connect(prefer_cli=args.cli)
    print(f"  oracle backend: {backend_name(con)}")
    if args.semantics:
        return probe_semantics(con)
    if not args.pattern_file:
        ap.error("pattern_file is required unless --semantics is given")

    patterns = parse_pattern_file(args.pattern_file)
    print(f"ground truth for {len(patterns)} pattern(s) from {args.pattern_file}")

    total = 0
    zero_match = []
    for pat in patterns:
        matches, regenerated = ensure_truth(con, pat, args.force)
        total += matches
        tag = "generated" if regenerated else "cached"
        print(f"  {pat.name:<24} {matches:>9,} matches  ({tag})")
        if matches == 0:
            zero_match.append(pat.name)

    print(f"total matches across patterns: {total:,}")
    if zero_match:
        print(
            f"note: {len(zero_match)} pattern(s) match zero rows: "
            f"{', '.join(zero_match)}",
            file=sys.stderr,
        )
    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
