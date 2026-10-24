"""Execute a .sql file statement by statement against data/tokenlike.duckdb and print results.

    .venv/Scripts/python -X utf8 integration/duckdb/python/run_sql.py integration/duckdb/examples/judge_demo.sql

The file's own LOAD / tokenlike_load_vocab statements are executed as written; their paths are
relative to the repository root, which is used as the working directory.
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import duckdb

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402

ROOT = tlsql.ROOT


def statements(text: str):
    buf, out = [], []
    for line in text.splitlines():
        if line.strip().startswith("--") or not line.strip():
            continue
        buf.append(line)
        if line.rstrip().endswith(";"):
            out.append("\n".join(buf).rstrip().rstrip(";"))
            buf = []
    if buf:
        out.append("\n".join(buf))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--db", default=str(tlsql.DB))
    args = ap.parse_args()
    sql_file = Path(args.file).resolve()
    os.chdir(ROOT)
    con = duckdb.connect(args.db, read_only=True, config={"allow_unsigned_extensions": "true"})
    for st in statements(sql_file.read_text(encoding="utf-8")):
        print(f"\nD> {st};")
        t0 = time.perf_counter()
        rel = con.sql(st)
        if rel is None:
            print(f"   ok  [{(time.perf_counter() - t0) * 1e3:.1f} ms]")
            continue
        if st.lstrip().upper().startswith("EXPLAIN"):
            print(rel.fetchall()[0][1])
            continue
        rel.show(max_width=160)
        print(f"   [{(time.perf_counter() - t0) * 1e3:.1f} ms]")
    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
