# TokenLIKE DuckDB extension (optional module)

SQL `LIKE` evaluated **on token-id columns inside DuckDB** by the shared TokenLIKE matcher,
packaged as a native loadable DuckDB extension.

```sql
LOAD 'build/duckdb/tokenlike.duckdb_extension';
SELECT tokenlike_load_vocab('data/vocab.bin');
SELECT COUNT(*) FROM urls WHERE token_like(encoded_url, '%.php');   -- 12,714 = original_url LIKE '%.php'
```

This module is **optional and self-contained**. The top-level TokenLIKE build (CLI, baselines,
`test_matcher`, `test_baselines`, `test_io`) does not include it and does not need DuckDB. It
compiles the repository's own `src/{pattern,nfa,dfa,lift,compress,scan,matcher,vocab}.cpp` in place,
so the extension always runs exactly the shared matcher. No matcher code is copied or changed.

Measured results: [RESULTS.md](RESULTS.md). Hand-off notes for integration:
[INTEGRATION_HANDOFF.md](INTEGRATION_HANDOFF.md).

## What it is (and is not)

* **Is:** a DuckDB extension built against DuckDB's **C extension API** (ABI `C_STRUCT`, API
  `v1.5.6`, stable surface only), which loads into the official DuckDB 1.5.6 binaries with `LOAD`.
  It registers scalar functions. DuckDB pushes a `WHERE token_like(col, 'p')` predicate into its
  `SEQ_SCAN` as a scan filter (see `EXPLAIN`).
* **Is not:** an optimizer rule or a custom physical operator. DuckDB never rewrites
  `col LIKE p` into `token_like` on its own; the query (or the application) calls `token_like`.

## SQL functions

| Function | Meaning |
|---|---|
| `tokenlike_load_vocab(path VARCHAR) -> BIGINT` | Load the vocabulary (Person B's validating loader). Per database. Returns the token count. |
| `token_like(tokens BLOB, pattern VARCHAR) -> BOOLEAN` | `decode(tokens) LIKE pattern`, computed on the ids, **without decoding**. |
| `token_like_concat(a BLOB, b BLOB, pattern VARCHAR) -> BOOLEAN` | `(decode(a) ‖ decode(b)) LIKE pattern` (`Matcher::match_concat`). |
| `detokenize(tokens BLOB) -> VARCHAR` | Decode; validates UTF-8. Used for output and for the decode+LIKE baseline. |
| `tokenize(text VARCHAR) -> BLOB` | Greedy longest-match encoding for ad-hoc demos (a valid encoding, not necessarily Person B's segmentation). |
| `tokenlike_info(pattern) -> VARCHAR` | JSON: scan direction, DFA states, exception tokens, table size, build time. |
| `tokenlike_stats()`, `tokenlike_reset_stats()`, `tokenlike_cache_clear()` | Counters (matcher builds, cache hits, rows, tokens present/read, decoded rows) and cache control. |
| `tokenlike_floor_noop(BLOB)`, `tokenlike_floor_copy(BLOB) -> BOOLEAN` | Diagnostics for the SQL scan floor: TRUE for every non-NULL row; `floor_copy` also does `token_like`'s per-row copy and validation, without the matcher. |

Semantics are DuckDB's `LIKE`: case-sensitive, `%` any sequence, `_` exactly one Unicode code point,
no `ESCAPE` (backslash is literal). A NULL in any argument gives NULL.

**Token column format.** One BLOB per row holding the row's token ids as little-endian `uint16`,
so `octet_length(blob) = 2 × tokens`. The empty BLOB is the empty string. Odd lengths, ids
`>= vocab.n`, and (for `detokenize`) decoded bytes that are not valid UTF-8 raise SQL errors. VARCHAR
values are refused where a token BLOB is expected, so text is never read as token ids.

**Caching.** Constant patterns are folded at bind time and their matcher is built once per query
(invalid or too-complex patterns therefore fail at bind). Matchers are shared through a per-database
cache keyed by the exact pattern bytes within one vocabulary snapshot (≤ 64 entries, ≤ 256 MB of
tables). Non-constant patterns (`CASE`, joined pattern columns, prepared parameters) are resolved per
run of equal patterns within a chunk through the same cache. Reloading the vocabulary starts a new
snapshot with an empty cache; queries keep the snapshot they were bound against.

## Requirements

| | Tested |
|---|---|
| Compiler | MSYS2 UCRT64 GCC 16.2 (`C:\msys64\ucrt64\bin\g++.exe`); optional MSVC 19.51 (VS Build Tools 2026). **Not** `C:\MinGW` GCC 6.3. |
| Build | CMake ≥ 3.16, Ninja |
| DuckDB | Python package `duckdb==1.5.6` (official MSVC-built wheel, platform `windows_amd64`). No DuckDB source or library is needed to build: only the two vendored C API headers (`third_party/duckdb`, MIT). |
| Python | 3.13 with `requirements.txt` (`duckdb`, `pyarrow`, `pandas`) |
| Data | Person B's pipeline output in `data/` (see the top-level README). Only the smoke test runs without it. |

The extension is unsigned: open DuckDB with `allow_unsigned_extensions`
(Python: `duckdb.connect(db, config={"allow_unsigned_extensions": "true"})`; CLI: `duckdb -unsigned`).

## Build and run (Windows PowerShell, from the repository root)

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH

# Python environment (repo-local, git-ignored)
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r integration\duckdb\requirements.txt

# 1. Main TokenLIKE build and tests: unchanged, DuckDB not involved
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=-O3 -march=native" `
      -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure

# 2. The DuckDB extension
cmake -S integration\duckdb -B build\duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=-O3 -march=native" `
      -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DPython3_EXECUTABLE="$PWD\.venv\Scripts\python.exe"
cmake --build build\duckdb                         # -> build\duckdb\tokenlike.duckdb_extension
ctest --test-dir build\duckdb --output-on-failure  # data-free smoke test (synthetic vocabulary if data\ is absent)

# 3. Data: Person B's pipeline output in data\ (top-level README "Data"; edge, fuzz and TPC-H
#    fixtures: HANDOFF_B.md section 2 and STATUS.md). This module only reads it. Its own extra
#    fuzz set (seed 7) goes to data\duckdb\fixtures\, never to data\fuzz or patterns\:
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\make_fixtures.py --seed 7 --count 300

# 4. Database, tests, demo, measurements (reports go to new files in build\duckdb\results\)
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\build_db.py        # -> data\duckdb\tokenlike.duckdb
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\test_sql.py        # full suite (~2-4 min)
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\demo_sql.py        # live demo
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\run_sql.py integration\duckdb\examples\judge_demo.sql
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\storage_report.py  # compression accounting
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\bench_sql.py --runs 7   # ~15 min, pinned to P-cores
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\report_tables.py   # renders results\ as Markdown

# Optional MSVC build, tested with the same suite
integration\duckdb\tools\build_msvc.cmd
$env:TOKENLIKE_EXT = "build\duckdb-msvc\tokenlike.duckdb_extension"; $env:TOKENLIKE_TEST_RESULTS = "test_results_msvc.json"
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\test_sql.py        # -> build\duckdb\results\test_results_msvc-<UTC>.json
Remove-Item Env:TOKENLIKE_EXT, Env:TOKENLIKE_TEST_RESULTS
```

Use the `-X utf8` flag: the scripts print non-ASCII URLs and patterns.

## Outputs: nothing is replaced implicitly

| Script | Default output (git-ignored, a new file per run) | Committed reference, replaced only with `--update-reference` |
|---|---|---|
| `test_sql.py` (full, `--quick`) | `build/duckdb/results/test_results[_quick]-<UTC>.json` | `results/test_results.json` (`TOKENLIKE_TEST_RESULTS` sets the name) |
| `bench_sql.py` | `build/duckdb/results/bench[_quick][_<tag>]-<UTC>.json` | `results/bench.json` (`bench_<tag>.json` with `TOKENLIKE_BENCH_TAG`) |
| `storage_report.py` | `build/duckdb/results/storage-<UTC>.json` | `results/storage.json` |
| `build_db.py` (build report) | `build/duckdb/results/build_db-<UTC>.json` | `results/build_db.json` |
| `report_tables.py` | `build/duckdb/results/RESULTS-<UTC>.md` | `RESULTS.md` |
| `make_fixtures.py` | `data/duckdb/fixtures/fuzz_s<seed>/values.*` and `data/duckdb/fixtures/fuzz_s<seed>.txt` | none |

* Every measurement script also takes `--out FILE`. An existing file is refused unless `--overwrite`
  is given too. `--update-reference` replaces the committed file on purpose, for example
  `bench_sql.py --runs 7 --update-reference`. The check runs before any work starts, and a file that
  appears while a script runs is not replaced either. If a default name is taken (two runs in one
  second), a `-1`, `-2`, … suffix is added.
* `report_tables.py [BENCH.json ...] [--results-dir DIR]` renders the committed `results/` by
  default. To render new runs, pass their files.
* `make_fixtures.py` runs Person B's own `scripts/verify/fuzz.py --random` and
  `scripts/encode/encode.py`, unchanged, with explicit output paths in `data/duckdb/fixtures/`. It
  refuses an output directory inside Person B's data or the tracked tree, and an existing fixture
  without `--overwrite`. Afterwards it checks that every file of Person B's data and of `patterns/` is
  unchanged (size, mtime, SHA-256). `build_db.py` loads each fixture as table `fuzz_s<seed>`, and
  `test_sql.py` runs its patterns.
* Do not run `fuzz.py` without both `--out` and `--patterns`. Its defaults are Person B's official
  `data/fuzz/values` (seed 1, 20,016 rows) and the tracked `patterns/generated_fuzz.txt`.
* The database (`data/duckdb/tokenlike.duckdb`) is this module's own derived file. `build_db.py`
  builds it in a temporary file and then replaces it.
* `ctest --test-dir build\duckdb` also runs `duckdb_output_safety` (`python/test_outputs.py`), which
  checks all of the above without data, in a temporary directory.

Environment overrides (all optional): `TOKENLIKE_DATA` (data directory), `TOKENLIKE_EXT` (extension
file), `TOKENLIKE_DUCKDB` (database file), `TOKENLIKE_CLI` (CLI used as the benchmark reference),
`TOKENLIKE_BENCH_TAG` (benchmark output suffix), `TOKENLIKE_TEST_RESULTS` (test output file name),
`TOKENLIKE_RESULTS_DIR` (default output directory, `build/duckdb/results`).

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Standalone project: core static library from `../../src`, the extension DLL, the metadata footer, the CTest smoke test. |
| `src/tokenlike_duckdb.cpp` | The extension. |
| `tools/append_metadata.py` | Appends DuckDB's 534-byte extension footer (independent implementation; byte-identical output to DuckDB's own tool, checked). |
| `tools/build_msvc.cmd` | MSVC build. |
| `third_party/duckdb/` | `duckdb.h`, `duckdb_extension.h` (v1.5.6, unmodified, MIT) and provenance. |
| `python/` | `tlsql.py` (paths, connection, output policy), `build_db.py`, `test_sql.py`, `demo_sql.py`, `run_sql.py`, `bench_sql.py`, `storage_report.py`, `report_tables.py`, `make_fixtures.py`, `test_outputs.py`. |
| `examples/judge_demo.sql` | Plain-SQL demo. |
| `results/` | Committed reference JSON from the reference machine; `RESULTS.md` renders them. Scripts replace them only with `--update-reference`. |

Generated files go to git-ignored places only: `build/duckdb*` (binaries, scratch, script outputs in
`build/duckdb/results/`), `data/duckdb/` (databases, `fixtures/`), `.venv/`.

## Database schema (`data/duckdb/tokenlike.duckdb`)

`urls` (1,000,000 ClickBench rows): `row_id BIGINT`, `original_url VARCHAR`, `original_referer
VARCHAR` (raw text, for truth and baselines only), `encoded_url BLOB`, `encoded_referer BLOB`
(Person B's token encoding, loaded zero-copy from `.tok`/`.off`), `traffic_source_id SMALLINT` (the
real ClickBench column `TraficSourceID`, row-aligned and verified against the canonical URL/Referer).
Also `urls_1k/10k/100k` (prefixes), `edge`, `fuzz`, `tpch` (Person B's fixtures, optional) and
`fuzz_s<seed>` (this module's `make_fixtures.py` output, optional). After loading, every encoded
column is checked with `detokenize(encoded) IS DISTINCT FROM original` (0 rows).

Storage note: DuckDB compresses **both** the VARCHAR and the BLOB columns itself (Dictionary and FSST
segments, see `results/storage.json`). Native `LIKE` therefore does not scan an uncompressed text
column: it reads DuckDB's compressed segments, and DuckDB rewrites it into `prefix()` / `suffix()` /
`contains()` / `=`. How each segment is decoded during the scan (dictionary vectors versus FSST
decompression) is DuckDB-internal and was not instrumented here.
