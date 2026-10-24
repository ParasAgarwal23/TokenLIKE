#!/usr/bin/env python3
"""Turn a newline-delimited text file into a TokenLIKE raw column.

This is the dependency-free half of column preparation: it writes the raw byte
representation and the canonical parquet, leaving token encoding to encode.py.

    <prefix>.raw      uint8[total_bytes]
    <prefix>.rawoff   uint64[rows + 1]
    <prefix>.parquet  row_id BIGINT, value VARCHAR   (requires duckdb)
    <prefix>.meta.json

Row ids are the 0-based line index, so they are deterministic by construction.
A trailing newline (and a single preceding carriage return) is stripped; every
other byte is preserved exactly, including empty lines, which become empty
strings in both representations.

fetch_clickbench.py and gen_tpch.py reuse write_raw() from here so that every
dataset lands in exactly the same on-disk shape.

Usage:
    python scripts/encode/make_column.py --lines data/download/urls \
        --out data/dbtext/urls --limit 1000 --parquet
"""

import argparse
import hashlib
import json
import os
import sys
from array import array


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_array_le(path: str, arr: array) -> None:
    """Write an array little-endian, atomically."""
    tmp = path + ".tmp"
    if sys.byteorder != "little":
        arr = arr[:]
        arr.byteswap()
    with open(tmp, "wb") as f:
        arr.tofile(f)
    os.replace(tmp, path)


def write_raw(prefix: str, values) -> dict:
    """Write <prefix>.raw and <prefix>.rawoff from an iterable of bytes.

    Returns statistics including the hashes the pipeline uses to detect a
    column being regenerated underneath derived artifacts.
    """
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    raw_tmp = prefix + ".raw.tmp"
    rawoff = array("Q", [0])
    total = 0
    with open(raw_tmp, "wb") as f:
        for value in values:
            if not isinstance(value, (bytes, bytearray)):
                raise TypeError(f"row {len(rawoff) - 1}: expected bytes, got {type(value)}")
            f.write(value)
            total += len(value)
            rawoff.append(total)
    os.replace(raw_tmp, prefix + ".raw")
    write_array_le(prefix + ".rawoff", rawoff)

    rows = len(rawoff) - 1
    actual = os.path.getsize(prefix + ".raw")
    if actual != total:
        raise AssertionError(f"wrote {actual} raw bytes, accumulated {total}")
    return {
        "rows": rows,
        "raw_bytes": total,
        "mean_value_bytes": round(total / rows, 3) if rows else 0.0,
        "empty_values": sum(1 for i in range(rows) if rawoff[i] == rawoff[i + 1]),
        "raw_sha256": sha256_file(prefix + ".raw"),
        "rawoff_sha256": sha256_file(prefix + ".rawoff"),
    }


def read_lines(path: str, limit: int | None) -> list[bytes]:
    """Read up to `limit` lines as raw byte values, newline stripped."""
    values: list[bytes] = []
    with open(path, "rb") as f:
        for line in f:
            if line.endswith(b"\n"):
                line = line[:-1]
                if line.endswith(b"\r"):
                    line = line[:-1]
            values.append(line)
            if limit is not None and len(values) >= limit:
                break
    return values


def write_parquet(prefix: str, source_path: str, limit: int | None) -> dict:
    """Write the canonical parquet via duckdb, reading the same source file.

    The parquet is built from the original source rather than from our raw
    binaries, so the DuckDB ground truth stays an independent view of the data.
    A delimiter that cannot occur in the input (0x01) plus disabled quoting and
    escaping makes read_csv a faithful line reader for URLs containing commas
    and quotes.
    """
    try:
        import duckdb
    except ImportError:
        return {"parquet": None, "parquet_note": "duckdb unavailable; parquet skipped"}

    out = prefix + ".parquet"
    con = duckdb.connect()
    con.execute("SET threads=1")  # keep line order deterministic
    limit_clause = f"LIMIT {int(limit)}" if limit is not None else ""
    con.execute(
        f"""
        COPY (
            SELECT (row_number() OVER () - 1)::BIGINT AS row_id,
                   COALESCE(value, '') AS value
            FROM read_csv(
                     ?,
                     delim := chr(1),
                     quote := '',
                     escape := '',
                     header := false,
                     columns := {{'value': 'VARCHAR'}},
                     normalize_names := false
                 )
            {limit_clause}
        ) TO '{out}' (FORMAT PARQUET)
        """,
        [source_path],
    )
    (rows,) = con.execute(f"SELECT count(*) FROM read_parquet('{out}')").fetchone()
    con.close()
    return {"parquet": out, "parquet_rows": rows, "parquet_sha256": sha256_file(out)}


def verify_parquet_matches_raw(prefix: str) -> dict:
    """Cross-check the oracle's view of the data against the encoded view.

    If these ever disagree, every downstream mismatch report would be noise,
    so this runs as part of column preparation rather than as a separate step.
    """
    parquet = prefix + ".parquet"
    if not os.path.exists(parquet):
        return {"parquet_matches_raw": None}
    try:
        import duckdb
    except ImportError:
        return {"parquet_matches_raw": None}

    rawoff = array("Q")
    with open(prefix + ".rawoff", "rb") as f:
        rawoff.frombytes(f.read())
    if sys.byteorder != "little":
        rawoff.byteswap()
    with open(prefix + ".raw", "rb") as f:
        raw = f.read()

    con = duckdb.connect()
    con.execute("SET threads=1")
    rows = con.execute(
        f"SELECT row_id, value FROM read_parquet('{parquet}') ORDER BY row_id"
    ).fetchall()
    con.close()

    if len(rows) != len(rawoff) - 1:
        raise AssertionError(
            f"{parquet} has {len(rows)} rows, raw column has {len(rawoff) - 1}"
        )
    for expected_id, (row_id, value) in enumerate(rows):
        if row_id != expected_id:
            raise AssertionError(
                f"row ids must be dense and 0-based: got {row_id} at position {expected_id}"
            )
        expected = raw[rawoff[row_id] : rawoff[row_id + 1]]
        if value.encode("utf-8") != expected:
            raise AssertionError(
                f"row {row_id}: parquet value {value!r} != raw bytes {expected!r}"
            )
    return {"parquet_matches_raw": True}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lines", required=True, help="newline-delimited source file")
    ap.add_argument("--out", required=True, help="output column prefix")
    ap.add_argument("--limit", type=int, default=None, help="take only the first N rows")
    ap.add_argument("--parquet", action="store_true", help="also write canonical parquet")
    ap.add_argument("--dataset", default=None, help="dataset label for meta.json")
    args = ap.parse_args()

    values = read_lines(args.lines, args.limit)
    stats = write_raw(args.out, values)

    meta = {
        "dataset": args.dataset or os.path.basename(os.path.dirname(args.out)),
        "column": os.path.basename(args.out),
        "source": {
            "kind": "lines",
            "path": args.lines,
            "sha256": sha256_file(args.lines),
            "limit": args.limit,
        },
        "row_id": "0-based line index of the source file",
        "null_policy": "empty line -> empty string (no NULLs in a line-based source)",
        "raw": stats,
    }
    if args.parquet:
        meta["raw"].update(write_parquet(args.out, args.lines, args.limit))
        meta["raw"].update(verify_parquet_matches_raw(args.out))

    with open(args.out + ".meta.json", "w") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")

    print(f"wrote {args.out}.raw / .rawoff / .meta.json")
    print(
        f"  rows={stats['rows']:,} raw_bytes={stats['raw_bytes']:,} "
        f"mean={stats['mean_value_bytes']} empty={stats['empty_values']}"
    )
    if args.parquet:
        note = meta["raw"].get("parquet_note")
        if note:
            print(f"  {note}")
        else:
            print(
                f"  parquet={meta['raw']['parquet']} rows={meta['raw']['parquet_rows']:,} "
                f"matches_raw={meta['raw']['parquet_matches_raw']}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
