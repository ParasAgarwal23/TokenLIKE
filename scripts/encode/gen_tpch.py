#!/usr/bin/env python3
"""Generate the optional TPC-H concatenation dataset.

Builds row-aligned l_comment and o_comment columns so that

    (l_comment || o_comment) LIKE '%regular%'

can be verified. Row alignment is the whole difficulty here: lineitem and
orders have different cardinalities, so the two columns are produced by a
single join, in one deterministic order, and both are written from the same
canonical parquet. Pairing them any other way would silently misalign row ids
and make every concatenation mismatch meaningless.

Requires the duckdb Python module and its bundled tpch extension.

Usage:
    python scripts/encode/gen_tpch.py --scale 1
    python scripts/encode/gen_tpch.py --scale 0.1 --rows 200000
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_column import sha256_file, write_raw  # noqa: E402

FETCH_BATCH = 50_000


def connect():
    try:
        import duckdb
    except ImportError as e:
        raise SystemExit(
            "gen_tpch.py needs the duckdb Python module (the CLI backend cannot "
            "stream rows).\n  .venv/bin/python -m pip install 'duckdb==1.5.6'"
        ) from e
    con = duckdb.connect()
    con.execute("SET threads=1")
    con.execute("SET preserve_insertion_order=true")
    try:
        con.execute("INSTALL tpch")
    except Exception:
        pass  # already installed, or offline with it already present
    con.execute("LOAD tpch")
    return con


def build_canonical(con, out: str, scale: float, rows: int | None) -> int:
    """Generate TPC-H and materialize the row-aligned comment pair."""
    print(f"generating TPC-H at scale factor {scale}")
    started = time.monotonic()
    con.execute(f"CALL dbgen(sf={scale})")
    print(f"  dbgen finished in {time.monotonic() - started:.1f}s")

    limit = f"LIMIT {int(rows)}" if rows else ""
    # One join, one deterministic order, one materialized row_id: both columns
    # are then slices of the same table and cannot drift out of alignment.
    con.execute(
        f"""
        COPY (
            SELECT (row_number() OVER () - 1)::BIGINT AS row_id,
                   l.l_comment AS a,
                   o.o_comment AS b
            FROM (
                SELECT l_orderkey, l_linenumber, l_comment
                FROM lineitem
                ORDER BY l_orderkey, l_linenumber
            ) l
            JOIN orders o ON o.o_orderkey = l.l_orderkey
            {limit}
        ) TO '{out}' (FORMAT PARQUET)
        """
    )
    (n, lo, hi, distinct) = con.execute(
        "SELECT count(*), min(row_id), max(row_id), count(DISTINCT row_id) FROM read_parquet(?)",
        [out],
    ).fetchone()
    if n and (lo != 0 or hi != n - 1 or distinct != n):
        raise AssertionError(f"{out}: row ids are not dense 0..{n - 1}")
    print(f"  {n:,} row-aligned comment pairs")
    return n


def export_column(con, canonical: str, prefix: str, field: str) -> dict:
    """Write <prefix>.parquet and the raw binaries for one side of the pair."""
    parquet = prefix + ".parquet"
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    con.execute(
        f"""
        COPY (
            SELECT row_id, "{field}" AS value FROM read_parquet(?) ORDER BY row_id
        ) TO '{parquet}' (FORMAT PARQUET)
        """,
        [canonical],
    )
    cur = con.execute("SELECT value FROM read_parquet(?) ORDER BY row_id", [parquet])

    def values():
        while batch := cur.fetchmany(FETCH_BATCH):
            for (value,) in batch:
                if value is None:
                    yield b""
                elif isinstance(value, bytes):
                    yield value
                else:
                    yield value.encode("utf-8")

    stats = write_raw(prefix, values())
    stats["parquet"] = parquet
    stats["parquet_sha256"] = sha256_file(parquet)

    (prows, pbytes) = con.execute(
        "SELECT count(*), sum(strlen(value)) FROM read_parquet(?)", [parquet]
    ).fetchone()
    if prows != stats["rows"] or (pbytes or 0) != stats["raw_bytes"]:
        raise AssertionError(
            f"{parquet} holds {prows} rows / {pbytes} bytes but {prefix}.raw holds "
            f"{stats['rows']} rows / {stats['raw_bytes']} bytes"
        )
    stats["parquet_matches_raw"] = True
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scale", type=float, default=1.0, help="TPC-H scale factor")
    ap.add_argument("--rows", type=int, default=1_000_000, help="row limit (0 for all)")
    ap.add_argument("--out-dir", default="data/tpch")
    args = ap.parse_args()

    rows = args.rows or None
    os.makedirs(args.out_dir, exist_ok=True)
    canonical = os.path.join(args.out_dir, "canonical.parquet")

    con = connect()
    n = build_canonical(con, canonical, args.scale, rows)

    for field, column in (("a", "l_comment"), ("b", "o_comment")):
        prefix = os.path.join(args.out_dir, column)
        stats = export_column(con, canonical, prefix, field)
        meta = {
            "dataset": "tpch",
            "column": column,
            "source": {"kind": "duckdb-tpch-dbgen", "scale_factor": args.scale, "row_limit": rows},
            "canonical_parquet": canonical,
            "canonical_sha256": sha256_file(canonical),
            "row_id": "0-based position in the canonical parquet",
            "row_alignment": (
                "l_comment and o_comment come from one lineitem-orders join in a "
                "single deterministic order, so both columns share row ids"
            ),
            "null_policy": "NULL -> empty string",
            "raw": stats,
        }
        with open(prefix + ".meta.json", "w") as f:
            json.dump(meta, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"exported {prefix}: rows={stats['rows']:,} raw_bytes={stats['raw_bytes']:,}")

    con.close()
    print(f"\n{n:,} rows. next:")
    for column in ("l_comment", "o_comment"):
        print(f"  python scripts/encode/encode.py {args.out_dir}/{column}")
        print(f"  python scripts/encode/roundtrip.py {args.out_dir}/{column}")
    print("  python scripts/verify/ground_truth.py patterns/tpch.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
