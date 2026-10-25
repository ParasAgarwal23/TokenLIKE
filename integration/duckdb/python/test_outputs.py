"""Regression tests for the DuckDB module's output safety (data-free; CTest duckdb_output_safety).

1. Measurement scripts (bench_sql, storage_report, build_db, test_sql, report_tables) never replace
   an existing file implicitly: the default is a new timestamped file in TOKENLIKE_RESULTS_DIR, an
   existing --out is refused before any work unless --overwrite is given, and the committed reference
   results are replaced only with --update-reference.
2. Fixture generation (make_fixtures.py) writes only to <data>/duckdb/fixtures (or another allowed
   place), refuses Person B's data directories and the tracked tree, and leaves Person B's official
   fixture files byte-for-byte unchanged.
3. The module's documentation contains no fuzz.py command that writes to Person B's default paths.

Everything runs in a temporary directory with a synthetic data directory; the repository's tracked
results and pattern files are hashed before and after and must be unchanged.

    .venv/Scripts/python -X utf8 integration/duckdb/python/test_outputs.py
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TMP = Path(tempfile.mkdtemp(prefix="tokenlike_duckdb_outputs_"))
ENV = dict(os.environ, TOKENLIKE_DATA=str(TMP / "data"), TOKENLIKE_RESULTS_DIR=str(TMP / "runs"),
           TOKENLIKE_DUCKDB=str(TMP / "data" / "duckdb" / "absent.duckdb"), PYTHONIOENCODING="utf-8")
ENV.pop("TOKENLIKE_TEST_RESULTS", None)
ENV.pop("TOKENLIKE_BENCH_TAG", None)
os.environ.update({k: ENV[k] for k in ("TOKENLIKE_DATA", "TOKENLIKE_RESULTS_DIR", "TOKENLIKE_DUCKDB")})

sys.path.insert(0, str(HERE))
import tlsql  # noqa: E402  (after the environment above: paths are resolved at import)

sys.path.insert(0, str(tlsql.ROOT / "scripts" / "encode"))
from make_column import write_raw  # noqa: E402  Person B's column writer, unchanged

PASS, FAIL = [0], []


def check(cond: bool, what: str) -> None:
    if cond:
        PASS[0] += 1
    else:
        FAIL.append(what)
        print("FAIL:", what, flush=True)


def sha(p: Path) -> tuple:
    st = p.stat()
    return st.st_size, st.st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest()


def tree(*dirs: Path) -> dict[str, tuple]:
    return {str(p): sha(p) for d in dirs if d.exists() for p in sorted(d.rglob("*")) if p.is_file()}


def py(script: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-X", "utf8", str(HERE / script), *args], env=ENV, cwd=tlsql.ROOT,
                          capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)


def ns(**kw) -> argparse.Namespace:
    return argparse.Namespace(**{"out": None, "overwrite": False, "update_reference": False, **kw})


def test_output_policy() -> None:
    runs = tlsql.RUNS
    check(runs == (TMP / "runs").resolve(), "TOKENLIKE_RESULTS_DIR sets the default output directory")
    o = tlsql.output(ns(), "results/bench.json")
    check(o.path.parent == runs and re.fullmatch(r"bench-\d{8}T\d{6}Z\.json", o.path.name) is not None and not o.replace,
          f"default output is a new timestamped file in the runs directory ({o.path.name})")
    o.write("first")
    o2 = tlsql.output(ns(), "results/bench.json")
    check(o2.path != o.path and o2.path.parent == runs, "a second default run gets another name (same second too)")
    check(o.path.read_text(encoding="utf-8") == "first", "the earlier run is kept")
    o2.write("second")

    existing = TMP / "existing.json"
    existing.write_text("keep me", encoding="utf-8")
    for kw, what in ((dict(out=existing), "--out of an existing file without --overwrite"),
                     (dict(update_reference=True, out=TMP / "x.json"), "--update-reference with --out"),
                     (dict(out=TMP), "--out naming a directory")):
        try:
            tlsql.output(ns(**kw), "results/bench.json")
            check(False, f"{what} is refused")
        except SystemExit as e:
            check("refusing" in str(e) or "mutually exclusive" in str(e) or "directory" in str(e), f"{what} is refused ({e})")
    check(existing.read_text(encoding="utf-8") == "keep me", "a refused output leaves the file unchanged")

    o = tlsql.output(ns(out=existing, overwrite=True), "results/bench.json")
    check(o.replace and o.path == existing.resolve(), "--overwrite allows replacing --out")
    o.write("replaced")
    check(existing.read_text(encoding="utf-8") == "replaced", "--overwrite replaces the file")
    check(not list(TMP.glob("existing.json.tmp-*")), "replacement leaves no temporary file")

    ref = tlsql.output(ns(update_reference=True), "results/bench.json")  # resolved, never written here
    check(ref.path == tlsql.MODULE / "results" / "bench.json" and ref.replace, "--update-reference targets the committed file")
    try:
        tlsql.output(ns(out=tlsql.MODULE / "results" / "bench.json"), "results/bench.json")
        check(False, "--out of the committed reference without --overwrite is refused")
    except SystemExit as e:
        check("--update-reference" in str(e), "refusing the committed reference names --update-reference")

    late = TMP / "late.json"
    o = tlsql.output(ns(out=late), "results/bench.json")
    late.write_text("appeared meanwhile", encoding="utf-8")
    try:
        o.write("new")
        check(False, "a file that appears during the run is not replaced")
    except SystemExit:
        check(late.read_text(encoding="utf-8") == "appeared meanwhile", "a file that appears during the run is not replaced")


def test_scripts_refuse_existing() -> None:
    target = TMP / "keep.json"
    target.write_text("recorded result", encoding="utf-8")
    runs_before = tree(tlsql.RUNS)
    for script in ("bench_sql.py", "storage_report.py", "build_db.py", "test_sql.py", "report_tables.py"):
        r = py(script, "--out", str(target))
        check(r.returncode != 0 and "refusing to overwrite" in (r.stdout + r.stderr),
              f"{script} --out <existing> is refused (exit {r.returncode}: {(r.stdout + r.stderr).strip()[-200:]})")
        check(target.read_text(encoding="utf-8") == "recorded result", f"{script} left the existing file unchanged")
        r = py(script, "--update-reference", "--out", str(TMP / "new.json"))
        check(r.returncode != 0 and not (TMP / "new.json").exists(), f"{script} --update-reference --out is refused")
    for script, ref in (("bench_sql.py", "results/bench.json"), ("storage_report.py", "results/storage.json"),
                        ("build_db.py", "results/build_db.json"), ("test_sql.py", "results/test_results.json"),
                        ("report_tables.py", "RESULTS.md")):
        r = py(script, "--out", str(tlsql.MODULE / ref))
        check(r.returncode != 0 and "--update-reference" in (r.stdout + r.stderr),
              f"{script} refuses the committed {ref} without --update-reference")
    # Without data the measurement scripts stop at their prerequisites and write nothing.
    for script in ("bench_sql.py", "storage_report.py", "test_sql.py"):
        r = py(script)
        check(r.returncode == 2, f"{script} without data exits 2 (got {r.returncode})")
    check(tree(tlsql.RUNS) == runs_before, "scripts that did no work wrote no output")
    r = py("test_sql.py", "--smoke", "--out", str(TMP / "smoke.json"))
    check(r.returncode == 2 and not (TMP / "smoke.json").exists(), "--smoke rejects --out (it writes no report)")


def test_report_tables() -> None:
    before = set(tlsql.RUNS.glob("RESULTS-*.md")) if tlsql.RUNS.exists() else set()
    r1, r2 = py("report_tables.py"), py("report_tables.py")
    new = set(tlsql.RUNS.glob("RESULTS-*.md")) - before
    check(r1.returncode == 0 and r2.returncode == 0 and len(new) == 2,
          f"report_tables.py writes a new file per run ({len(new)} new; {r1.stderr.strip()[-200:]})")
    check(all(p.read_text(encoding="utf-8").startswith("# TokenLIKE DuckDB integration") for p in new),
          "the generated report renders the committed results")
    out = TMP / "report.md"
    out.write_text("old", encoding="utf-8")
    r = py("report_tables.py", "--out", str(out))
    check(r.returncode != 0 and out.read_text(encoding="utf-8") == "old", "report_tables.py refuses an existing --out")
    r = py("report_tables.py", "--out", str(out), "--overwrite")
    check(r.returncode == 0 and out.read_text(encoding="utf-8").startswith("# TokenLIKE"), "report_tables.py --overwrite replaces it")
    r = py("report_tables.py", str(tlsql.RESULTS / "bench.json"), "--results-dir", str(tlsql.RESULTS), "--out", str(TMP / "pos.md"))
    check(r.returncode == 0 and (TMP / "pos.md").exists(), "positional bench files and --results-dir still work")


def fake_official(prefix: Path, label: str) -> None:
    prefix.parent.mkdir(parents=True, exist_ok=True)
    for ext in (".raw", ".rawoff", ".tok", ".off", ".parquet", ".meta.json"):
        Path(str(prefix) + ext).write_bytes(f"Person B official {label} {ext}\n".encode())


def test_fixtures() -> None:
    data = TMP / "data"
    urls = [f"http://www.example{i % 37}.com/path/{i}/index{i % 5}.html?q={i * 7 % 101}".encode() for i in range(400)]
    urls += ["https://ру.example/путь".encode(), b"", b"100%_literal"]
    write_raw(str(data / "clickbench" / "URL"), urls)
    sys.path.insert(0, str(HERE))
    from test_sql import write_vocab  # noqa: E402
    write_vocab(data / "vocab.bin", [bytes([b]) for b in range(256)] + [b"http", b"://", b"www.", b".com", b"/index", b".html"])
    fake_official(data / "fuzz" / "values", "fuzz")
    fake_official(data / "edge" / "values", "edge")
    official = tree(data / "fuzz", data / "edge", data / "clickbench") | {str(data / "vocab.bin"): sha(data / "vocab.bin")}

    args = ("--seed", "7", "--rows", "200", "--count", "25")
    r = py("make_fixtures.py", *args)
    check(r.returncode == 0, f"make_fixtures.py runs (exit {r.returncode}: {(r.stdout + r.stderr).strip()[-400:]})")
    fx = data / "duckdb" / "fixtures"
    prefix, pfile = fx / "fuzz_s7" / "values", fx / "fuzz_s7.txt"
    check(all(Path(str(prefix) + e).exists() for e in (".raw", ".rawoff", ".tok", ".off", ".parquet", ".meta.json")),
          "the fixture column is written under <data>/duckdb/fixtures/fuzz_s7")
    pats = tlsql.read_patterns(pfile) if pfile.exists() else []
    check(len(pats) == 25 and {c for _, c, _, _ in pats} == {tlsql.column_key(prefix)},
          "every generated pattern names the isolated fixture column")
    check(not any(c == "data/fuzz/values" for _, c, _, _ in pats), "no generated pattern names Person B's data/fuzz/values")
    check(tree(data / "fuzz", data / "edge", data / "clickbench") | {str(data / "vocab.bin"): sha(data / "vocab.bin")} == official,
          "Person B's official fuzz/edge files, source column and vocabulary are unchanged")
    first = tree(fx)

    r = py("make_fixtures.py", *args)
    check(r.returncode == 2 and "refusing to overwrite" in r.stderr, "an existing fixture is not replaced without --overwrite")
    check(tree(fx) == first, "the refused run left the fixture unchanged")
    r = py("make_fixtures.py", *args, "--overwrite")
    check(r.returncode == 0, "--overwrite regenerates the fixture")
    check(sha(Path(str(prefix) + ".tok"))[2] == first[str(prefix) + ".tok"][2], "regeneration is deterministic (same .tok)")

    for d, what in ((data / "fuzz", "Person B's data/fuzz"), (data / "edge", "Person B's data/edge"),
                    (data, "the data directory root"), (tlsql.PATTERNS, "the tracked patterns/"),
                    (tlsql.RESULTS, "the committed results/")):
        before = tree(d)
        r = py("make_fixtures.py", *args, "--out-dir", str(d))
        check(r.returncode == 2 and "refusing to write fixtures there" in r.stderr and tree(d) == before,
              f"--out-dir {what} is refused and nothing is written there")
    check(tree(data / "fuzz", data / "edge", data / "clickbench") | {str(data / "vocab.bin"): sha(data / "vocab.bin")} == official,
          "Person B's official files are still unchanged")
    other = TMP / "elsewhere"
    r = py("make_fixtures.py", *args, "--out-dir", str(other))
    check(r.returncode == 0 and Path(str(other / "fuzz_s7" / "values") + ".tok").exists(), "--out-dir outside the repository works")


def test_protected_paths() -> None:
    d = tlsql.DATA
    for p, ok in ((d / "fuzz" / "values", False), (d / "edge", False), (d / "clickbench" / "URL", False), (d, False),
                  (d / "duckdb" / "fixtures", True), (d / "duckdb", True), (tlsql.ROOT / "build" / "x", True),
                  (tlsql.ROOT / "patterns", False), (tlsql.MODULE / "results", False), (tlsql.ROOT / "data" / "fuzz", False),
                  (TMP / "elsewhere", True)):
        check((tlsql.protected_reason(p) is None) == ok, f"protected_reason({p}) -> {'allowed' if ok else 'refused'}")


def test_docs() -> None:
    """No documented fuzz.py command may write Person B's default outputs (data/edge, data/fuzz, patterns/)."""
    n = 0
    for doc in (tlsql.MODULE / "README.md", tlsql.MODULE / "INTEGRATION_HANDOFF.md"):
        fenced = False
        for line in doc.read_text(encoding="utf-8").splitlines():
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            m = re.search(r"scripts[/\\]verify[/\\]fuzz\.py(.*)", line)
            if not fenced or not m or line.lstrip().startswith("#"):  # commands only, not prose or comments
                continue
            n += 1
            flags = dict(re.findall(r"(--out|--patterns)\s+(\S+)", m.group(1).replace("\\", "/")))
            safe = all(f in flags and flags[f].startswith(("build/", "data/duckdb/")) for f in ("--out", "--patterns"))
            check(safe, f"{doc.name}: documented fuzz.py command writes only isolated paths: {line.strip()}")
    for doc in (tlsql.MODULE / "README.md", tlsql.MODULE / "INTEGRATION_HANDOFF.md"):
        text = doc.read_text(encoding="utf-8")
        check("make_fixtures.py" in text, f"{doc.name} documents make_fixtures.py for the extra fuzz fixture")
    print(f"docs: {n} direct fuzz.py command(s) checked", flush=True)


def main() -> int:
    tracked = tree(tlsql.RESULTS, tlsql.PATTERNS) | {str(tlsql.MODULE / "RESULTS.md"): sha(tlsql.MODULE / "RESULTS.md")}
    for t in (test_output_policy, test_protected_paths, test_scripts_refuse_existing, test_report_tables, test_fixtures, test_docs):
        try:
            t()
        except Exception as e:  # noqa: BLE001
            check(False, f"{t.__name__}: uncaught {type(e).__name__}: {e}")
        print(f"{t.__name__}: done ({PASS[0]} passed, {len(FAIL)} failed so far)", flush=True)
    after = tree(tlsql.RESULTS, tlsql.PATTERNS) | {str(tlsql.MODULE / "RESULTS.md"): sha(tlsql.MODULE / "RESULTS.md")}
    check(after == tracked, "the committed results, RESULTS.md and tracked pattern files are unchanged")
    print(f"\n{'ALL PASSED' if not FAIL else 'FAILURES'}: {PASS[0]} checks passed, {len(FAIL)} failed (temp dir {TMP})")
    if not FAIL:
        import shutil
        shutil.rmtree(TMP, ignore_errors=True)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
