"""Generate this module's extra random SQL fixtures in an isolated, git-ignored place.

The full SQL suite (test_sql.py) can run a second random fuzz set (seed 7 by default) in addition to
Person B's official fixtures (data/edge, data/fuzz, patterns/generated_*.txt). This script makes it
with Person B's own generator and encoder, unchanged (scripts/verify/fuzz.py --random and
scripts/encode/encode.py), but always passes explicit output paths, so it never writes Person B's
data or the tracked pattern files:

  <data>/duckdb/fixtures/fuzz_s<seed>/values.{raw,rawoff,parquet,tok,off,meta.json}
  <data>/duckdb/fixtures/fuzz_s<seed>.txt      patterns; their column prefix is the path above

    .venv/Scripts/python -X utf8 integration/duckdb/python/make_fixtures.py [--seed 7] [--count 300] [--rows 5000]
        [--from data/clickbench/URL] [--vocab data/vocab.bin] [--out-dir DIR] [--overwrite]

Then rebuild the database (build_db.py) to load it as table fuzz_s<seed>.

Safety: the output directory must not lie inside Person B's data or the repository's tracked tree
(only <data>/duckdb/, build/ and places outside the repository are accepted); an existing fixture is
replaced only with --overwrite; and afterwards every file in the data directory outside
<data>/duckdb/ and every tracked pattern file is checked to be unchanged (size, mtime, SHA-256).
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlsql  # noqa: E402

FUZZ = tlsql.ROOT / "scripts" / "verify" / "fuzz.py"
ENCODE = tlsql.ROOT / "scripts" / "encode" / "encode.py"


def snapshot() -> dict[str, tuple]:
    """Person B's data (everything in <data> except <data>/duckdb) and the tracked pattern files."""
    own = tlsql.DATA / "duckdb"
    files = [p for p in tlsql.DATA.rglob("*") if p.is_file() and own not in p.parents] if tlsql.DATA.exists() else []
    files += [p for p in tlsql.PATTERNS.rglob("*") if p.is_file()]
    snap = {}
    for p in files:
        st = p.stat()
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        snap[str(p)] = (st.st_size, st.st_mtime_ns, h.hexdigest())
    return snap


def run(cmd: list[str]) -> None:
    print("  $", " ".join(cmd[1:]), flush=True)
    r = subprocess.run(cmd, cwd=tlsql.ROOT)
    if r.returncode != 0:
        raise SystemExit(f"error: command failed with exit code {r.returncode}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the DuckDB module's extra fuzz fixtures (isolated paths)")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--count", type=int, default=300, help="number of random patterns")
    ap.add_argument("--rows", type=int, default=5000, help="rows sampled from the source column")
    ap.add_argument("--from", dest="source", type=Path, default=tlsql.DATA / "clickbench" / "URL",
                    help="source column prefix (read only; default <data>/clickbench/URL)")
    ap.add_argument("--vocab", type=Path, default=tlsql.VOCAB, help="vocabulary (read only; default <data>/vocab.bin)")
    ap.add_argument("--out-dir", type=Path, default=tlsql.FIXTURES,
                    help=f"fixture directory (default {tlsql.show(tlsql.FIXTURES)})")
    ap.add_argument("--overwrite", action="store_true", help="replace an existing fixture of the same seed")
    args = ap.parse_args()

    out_dir = args.out_dir.resolve()
    why = tlsql.protected_reason(out_dir)
    if why:
        print(f"error: refusing to write fixtures there: {why}", file=sys.stderr)
        return 2
    name = f"fuzz_s{args.seed}"
    fixture, pfile = out_dir / name, out_dir / f"{name}.txt"
    prefix = fixture / "values"
    existing = [p for p in (fixture, pfile) if p.exists()]
    if existing and not args.overwrite:
        print(f"error: {', '.join(tlsql.show(p) for p in existing)} exists; refusing to overwrite "
              "(use --overwrite to regenerate it, or another --seed / --out-dir)", file=sys.stderr)
        return 2
    for f in (Path(str(args.source.resolve()) + ".raw"), args.vocab.resolve()):
        if not f.exists():
            print(f"error: {tlsql.show(f)} not found (Person B's pipeline, see the top-level README)", file=sys.stderr)
            return 2

    before = snapshot()
    if args.overwrite:
        if fixture.exists():
            shutil.rmtree(fixture)
        pfile.unlink(missing_ok=True)
    fixture.mkdir(parents=True)
    print(f"generating {name} into {tlsql.show(out_dir)}/ (Person B's generator and encoder, explicit outputs)")
    run([sys.executable, str(FUZZ), "--random", "--from", tlsql.column_key(args.source), "--seed", str(args.seed),
         "--rows", str(args.rows), "--count", str(args.count),
         "--out", tlsql.column_key(prefix), "--patterns", str(pfile)])
    run([sys.executable, str(ENCODE), tlsql.column_key(prefix), "--vocab", str(args.vocab.resolve())])

    after = snapshot()
    changed = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    if changed:
        print("error: files outside the fixture directory changed:\n  " +
              "\n  ".join(tlsql.show(c) for c in changed), file=sys.stderr)
        return 1
    pats = tlsql.read_patterns(pfile)
    cols = {c for _, c, _, _ in pats}
    if cols != {tlsql.column_key(prefix)} or not Path(str(prefix) + ".tok").exists():
        print(f"error: unexpected fixture output (pattern columns {sorted(cols)})", file=sys.stderr)
        return 1
    print(f"wrote {tlsql.show(prefix)}.* and {tlsql.show(pfile)} ({len(pats)} patterns); "
          f"{len(before)} files of Person B's data and tracked patterns unchanged")
    print("next: integration/duckdb/python/build_db.py loads it as table", name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
