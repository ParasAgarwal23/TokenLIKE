#!/usr/bin/env python3
"""Download and prepare the ClickBench URL column.

ClickBench's `hits` table is published as 100 equal partitions. We take one
partition, which is ~1M rows in a 117 MB download, instead of the 14.8 GB
single-file export.

Produces, for each requested column:

    data/clickbench/canonical.parquet   row_id + every requested column
    data/clickbench/<col>.parquet       row_id BIGINT, value VARCHAR
    data/clickbench/<col>.raw/.rawoff   raw byte representation
    data/clickbench/<col>.meta.json

The canonical parquet is the single source of truth for row ids. It is built
once, with threads=1 and a materialized row_id, so row ids never depend on
scan order; every other artifact is derived from it. All columns come from the
same canonical file, so a multi-column pattern is row-aligned by construction.

NULL becomes the empty string here and only here, so the encoded data and the
DuckDB reference queries cannot disagree about it.

Token encoding is a separate step: run scripts/encode/encode.py afterwards.

Usage:
    python scripts/encode/fetch_clickbench.py --rows 1000
    python scripts/encode/fetch_clickbench.py --rows 1000000 --columns URL,Referer
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_column import sha256_file, write_raw  # noqa: E402

PARTITION_URL = (
    "https://datasets.clickhouse.com/hits_compatible/athena_partitioned/hits_{part}.parquet"
)
# Observed Content-Length of partition 0, used as a download completeness check.
PART0_BYTES = 122446530
COLUMN_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
FETCH_BATCH = 50_000


def connect():
    try:
        import duckdb
    except ImportError as e:
        raise SystemExit(
            "duckdb is required.\n"
            "  install: .venv/bin/python -m pip install 'duckdb==1.5.6'\n"
            "  then run this script with .venv/bin/python"
        ) from e
    con = duckdb.connect()
    # Deterministic row order is the whole point of the canonical parquet.
    con.execute("SET threads=1")
    con.execute("SET preserve_insertion_order=true")
    return con


def download(part: int, dest_dir: str) -> str:
    """Download one ClickBench partition, skipping if already complete."""
    os.makedirs(dest_dir, exist_ok=True)
    path = os.path.join(dest_dir, f"hits_{part}.parquet")
    url = PARTITION_URL.format(part=part)

    if os.path.exists(path):
        have = os.path.getsize(path)
        if part != 0 or have == PART0_BYTES:
            print(f"using existing {path} ({have:,} bytes)")
            return path
        print(f"{path} is {have:,} bytes, expected {PART0_BYTES:,}; re-downloading")

    print(f"downloading {url}")
    started = time.monotonic()
    tmp = path + ".part"
    with urllib.request.urlopen(url, timeout=120) as resp, open(tmp, "wb") as out:
        total = int(resp.headers.get("Content-Length") or 0)
        done = 0
        while chunk := resp.read(1 << 20):
            out.write(chunk)
            done += len(chunk)
            if total and done % (1 << 24) < (1 << 20):
                print(f"  {done / total:.0%} ({done:,}/{total:,} bytes)", flush=True)
    os.replace(tmp, path)
    print(f"  downloaded {os.path.getsize(path):,} bytes in {time.monotonic() - started:.1f}s")
    return path


def validate_columns(con, source: str, columns: list[str]) -> None:
    """Reject column names that are not plain identifiers present in the file.

    Column names reach SQL as quoted identifiers (bound parameters cannot name
    a column), so they are checked against a strict pattern and against the
    file's actual schema before being interpolated.
    """
    for name in columns:
        if not COLUMN_NAME_RE.match(name):
            raise ValueError(f"refusing column name {name!r}: not a plain identifier")
    available = {
        r[0] for r in con.execute("DESCRIBE SELECT * FROM read_parquet(?)", [source]).fetchall()
    }
    missing = [c for c in columns if c not in available]
    if missing:
        raise ValueError(f"column(s) {missing} not present in {source}")


def assert_bytes_preserved(con, source: str, canonical: str, columns: list[str], rows: int | None) -> None:
    """Assert the canonical parquet holds the source bytes unchanged.

    ClickBench stores URL and Referer as bare BYTE_ARRAY with no UTF8 logical
    type, so DuckDB types them BLOB, while the frozen format calls for
    `value VARCHAR` and LIKE is not defined on BLOB. The conversion therefore
    has to be exact, and the obvious spelling is not:

        CAST(blob AS VARCHAR)   renders the blob's escaped representation, so
                                'сезон' becomes the 24-character text
                                '\\xD1\\x81\\xD0\\xB5...'. It inflated the URL
                                column from 88,562,192 to 100,529,987 bytes and
                                corrupted every non-ASCII URL. TRY_CAST does not
                                reject it either, because escaping never fails.

        decode(blob)            reinterprets the bytes as UTF-8, byte-exact,
                                and raises on genuinely invalid UTF-8.

    So the build uses decode(), and this check compares total byte length
    column by column against the source. strlen() counts bytes; length() counts
    characters, which would hide exactly this bug.
    """
    limit = f"LIMIT {int(rows)}" if rows else ""
    for column in columns:
        (src,) = con.execute(
            f'SELECT sum(octet_length("{column}")) FROM (SELECT "{column}" FROM read_parquet(?) {limit})',
            [source],
        ).fetchone()
        (got,) = con.execute(
            f'SELECT sum(strlen("{column}")) FROM read_parquet(?)', [canonical]
        ).fetchone()
        if (src or 0) != (got or 0):
            raise AssertionError(
                f"column {column!r}: source holds {src:,} bytes but the canonical "
                f"parquet holds {got:,}; the BLOB to VARCHAR conversion is not byte-exact"
            )


def build_canonical(con, source: str, out: str, columns: list[str], rows: int | None) -> int:
    """Materialize the canonical parquet with explicit row ids."""
    # decode(), not CAST: see assert_bytes_preserved().
    select = ",\n                   ".join(
        f'COALESCE(decode("{c}"), \'\') AS "{c}"' for c in columns
    )
    limit = f"LIMIT {int(rows)}" if rows else ""
    print(f"building {out}")
    con.execute(
        f"""
        COPY (
            SELECT (row_number() OVER () - 1)::BIGINT AS row_id,
                   {select}
            FROM read_parquet(?)
            {limit}
        ) TO '{out}' (FORMAT PARQUET)
        """,
        [source],
    )
    (n,) = con.execute("SELECT count(*) FROM read_parquet(?)", [out]).fetchone()

    # Row ids must be dense, 0-based and unique, or nothing downstream aligns.
    (lo, hi, distinct) = con.execute(
        "SELECT min(row_id), max(row_id), count(DISTINCT row_id) FROM read_parquet(?)", [out]
    ).fetchone()
    if n and (lo != 0 or hi != n - 1 or distinct != n):
        raise AssertionError(
            f"{out}: row ids are not dense 0..{n - 1} (min={lo} max={hi} distinct={distinct})"
        )
    print(f"  {n:,} rows, row_id 0..{hi}")
    return n


def export_column(con, canonical: str, prefix: str, column: str) -> dict:
    """Write <prefix>.parquet and the raw binaries for one column."""
    parquet = prefix + ".parquet"
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    con.execute(
        f"""
        COPY (
            SELECT row_id, "{column}" AS value
            FROM read_parquet(?)
            ORDER BY row_id
        ) TO '{parquet}' (FORMAT PARQUET)
        """,
        [canonical],
    )

    # Stream the values out in row_id order and write the raw representation.
    cur = con.execute("SELECT value FROM read_parquet(?) ORDER BY row_id", [parquet])

    def values():
        # The canonical parquet stores VARCHAR, so values arrive as str; the
        # bytes branch stays as a guard in case a column is ever exported
        # without the cast, since .raw must hold the original bytes unmodified.
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

    # The oracle reads the parquet and the matcher reads the binaries; if those
    # two views ever disagree, every mismatch report downstream is noise.
    (parquet_rows, parquet_bytes) = con.execute(
        "SELECT count(*), sum(strlen(value)) FROM read_parquet(?)", [parquet]
    ).fetchone()
    if parquet_rows != stats["rows"]:
        raise AssertionError(
            f"{parquet} has {parquet_rows} rows but {prefix}.raw holds {stats['rows']}"
        )
    if (parquet_bytes or 0) != stats["raw_bytes"]:
        raise AssertionError(
            f"{parquet} holds {parquet_bytes} value bytes but {prefix}.raw holds {stats['raw_bytes']}"
        )
    stats["parquet_matches_raw"] = True
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rows", type=int, default=1_000_000, help="row limit (0 for all)")
    ap.add_argument("--columns", default="URL,Referer")
    ap.add_argument("--part", type=int, default=0, help="ClickBench partition index")
    ap.add_argument("--download-dir", default="data/download")
    ap.add_argument("--out-dir", default="data/clickbench")
    args = ap.parse_args()

    columns = [c.strip() for c in args.columns.split(",") if c.strip()]
    rows = args.rows or None

    source = download(args.part, args.download_dir)
    con = connect()
    validate_columns(con, source, columns)

    os.makedirs(args.out_dir, exist_ok=True)
    canonical = os.path.join(args.out_dir, "canonical.parquet")
    n = build_canonical(con, source, canonical, columns, rows)
    assert_bytes_preserved(con, source, canonical, columns, rows)

    summary = {}
    for column in columns:
        prefix = os.path.join(args.out_dir, column)
        started = time.monotonic()
        stats = export_column(con, canonical, prefix, column)
        stats["export_seconds"] = round(time.monotonic() - started, 2)
        summary[column] = stats

        meta = {
            "dataset": "clickbench",
            "column": column,
            "source": {
                "kind": "clickbench-partition",
                "url": PARTITION_URL.format(part=args.part),
                "path": source,
                "sha256": None,  # 117 MB; hashed on request, not on every run
                "partition": args.part,
                "row_limit": rows,
            },
            "canonical_parquet": canonical,
            "canonical_sha256": sha256_file(canonical),
            "row_id": "0-based position in the canonical parquet",
            "null_policy": "NULL -> empty string, applied once when building canonical.parquet",
            "raw": stats,
        }
        with open(prefix + ".meta.json", "w") as f:
            json.dump(meta, f, indent=2, sort_keys=True)
            f.write("\n")

        print(f"exported {prefix}")
        print(
            f"  rows={stats['rows']:,} raw_bytes={stats['raw_bytes']:,} "
            f"mean={stats['mean_value_bytes']} empty={stats['empty_values']:,} "
            f"in {stats['export_seconds']}s"
        )

    con.close()
    print(f"\ncanonical row count: {n:,}")
    print("next:")
    for column in columns:
        prefix = os.path.join(args.out_dir, column)
        print(f"  python scripts/encode/encode.py {prefix}")
        print(f"  python scripts/encode/roundtrip.py {prefix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
