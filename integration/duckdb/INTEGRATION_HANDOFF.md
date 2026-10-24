# Hand-off: optional DuckDB SQL integration (Person A → Person C)

## Branch

* Branch: `feature/duckdb-sql`, based on `origin/main` = `v3-freeze` = `c58d066`.
* Implementation commit: see "Verified commit" at the end of this file (recorded in a follow-up
  commit, since a commit cannot contain its own SHA).
* **Not merged.** Person C decides whether and how to merge. `main` and `v3-freeze` are untouched.

## What it adds

A native, loadable DuckDB extension (`tokenlike.duckdb_extension`) that runs the shared TokenLIKE
matcher inside DuckDB queries: `WHERE token_like(encoded_url, '%.php')`,
`token_like_concat(url, referer, p)`, `detokenize()`, `tokenize()`. It is built against DuckDB's C
extension API v1.5.6 and loads into the official DuckDB 1.5.6 binaries. DuckDB evaluates it as a
scan filter. There is **no** optimizer rule, so plain `LIKE` is never rewritten. Details:
[README.md](README.md). Numbers: [RESULTS.md](RESULTS.md).

## Files

**Added (all under `integration/duckdb/`):**

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Standalone CMake project (not included by the top-level build) |
| `src/tokenlike_duckdb.cpp` | The extension (C API callbacks, bind-time matcher construction, cache, token BLOB handling) |
| `tools/append_metadata.py` | Writes DuckDB's extension footer (independent implementation, byte-identical to DuckDB's tool) |
| `tools/build_msvc.cmd` | Optional MSVC build |
| `third_party/duckdb/{duckdb.h, duckdb_extension.h, LICENSE, README.md}` | DuckDB v1.5.6 C API headers, unmodified, MIT, with SHA-256 provenance |
| `python/*.py` | `tlsql` (paths/connection), `build_db`, `test_sql`, `demo_sql`, `run_sql`, `bench_sql`, `storage_report`, `report_tables` |
| `examples/judge_demo.sql` | Plain-SQL demo |
| `requirements.txt` | `duckdb==1.5.6`, `pyarrow`, `pandas` |
| `results/{test_results.json, test_results_msvc.json, bench.json, storage.json, build_db.json}` | Measurements from the reference machine |
| `README.md`, `RESULTS.md`, `INTEGRATION_HANDOFF.md` | Documentation |

**Modified: none.** No file outside `integration/duckdb/` changed: not the top-level
`CMakeLists.txt`, README, `.gitignore`, `src/`, `include/`, `tests/`, `scripts/` or `patterns/`.
The matcher and vocabulary loader are compiled **in place** from `src/*.cpp`; nothing is copied.
(The reconciliation was done against the earlier local experiment: all 53 tracked files at
`c58d066` were identical, ignoring line endings.)

**Not committed (git-ignored by the existing `.gitignore`):** `build/duckdb*` (DLLs, extension files,
scratch), `data/duckdb/` (DuckDB databases, ~505 MB), `data/` (Person B's datasets), `.venv/`.

## Dependencies

* Build: CMake ≥ 3.16, Ninja, MSYS2 UCRT64 GCC 16.2 (tested), or MSVC 19.51 (tested). Not `C:\MinGW` GCC 6.3.
* Run/test: Python 3.13 + `integration/duckdb/requirements.txt`. These are the same `duckdb==1.5.6`
  pin and the same `pyarrow` that the project already uses. No DuckDB C++ library and no DuckDB
  source build.
* Data: Person B's `data/` (vocab + ClickBench URL/Referer; edge, fuzz and TPC-H are optional).

## Commands (Windows PowerShell, repository root)

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\.venv\Scripts\python.exe -m pip install -r integration\duckdb\requirements.txt

# build
cmake -S integration\duckdb -B build\duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=-O3 -march=native" `
      -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DPython3_EXECUTABLE="$PWD\.venv\Scripts\python.exe"
cmake --build build\duckdb

# tests
ctest --test-dir build\duckdb --output-on-failure                            # smoke, no data required
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\build_db.py      # needs data\ from Person B's pipeline
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\test_sql.py      # full suite

# demo
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\demo_sql.py
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\run_sql.py integration\duckdb\examples\judge_demo.sql

# measurements
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\storage_report.py
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\bench_sql.py --runs 7
.\.venv\Scripts\python.exe -X utf8 integration\duckdb\python\report_tables.py
```

The existing project is unaffected: `cmake -B build …; cmake --build build; ctest --test-dir build`
passes 6/6 on this branch, the same as on `main`.

## Verification on this branch (Intel Core Ultra 9 288V, Windows 11, DuckDB 1.5.6)

| Check | Result |
|---|---|
| Top-level build + CTest (CLI, `test_matcher`, `test_baselines`, `test_io` on real data) | 6/6 pass, no warnings |
| Module build (GCC 16.2), warnings | 0 |
| Module CTest smoke (no dataset needed) | 1/1 pass |
| Full SQL suite, GCC build | **1,116 checks, 0 failures**, 694 real-data patterns (core 22, tier1 13, tier2 23 incl. TPC-H, generated_edge 36, generated_fuzz 300, extra fuzz 300), 0 skipped |
| Full SQL suite, MSVC build | **1,116 checks, 0 failures** |
| Benchmark cells (100) whose three modes disagree | 0 |
| DuckDB loads the extension and executes `token_like` | yes (`duckdb_extensions()`: loaded; every test executes it) |

The suite covers: different patterns in consecutive queries; prepared-statement parameters; NULLs;
Unicode `_` and UTF-8 boundaries (four segmentations including token splits inside code points);
exact/prefix/suffix/contains/interior `%`; concatenation; out-of-vocabulary ids; odd/empty BLOBs;
invalid UTF-8 on decode; too-complex patterns; 1 MB rows; row-id equality via `EXCEPT` in both
directions; cache clear + repeat; filter then GROUP BY; joins/CTE/window functions; concurrency,
interrupts, vocabulary reload races and reopened databases.

## Measured compression (1,000,000 ClickBench rows; `results/storage.json`)

| | URL | Referer |
|---|---:|---:|
| text bytes | 88,562,192 | 79,583,339 |
| token bytes (2 × 36.1 / 33.0 tokens per row) | 72,272,106 | 65,951,752 |
| tokens ÷ text | 0.816 | 0.829 |
| (tokens + uint64 offsets) ÷ (text + uint64 offsets) | 0.831 | 0.844 |
| … + the whole 630,133 B vocabulary charged to this column | 0.838 | 0.852 |
| DuckDB default storage, text column alone | 38.0 MB | 33.5 MB |
| DuckDB default storage, token column alone | 35.5 MB | 28.25 MB |
| token ÷ text inside DuckDB | 0.934 | 0.843 |

DuckDB compresses **both** columns itself (Dictionary + FSST segments in both the VARCHAR and BLOB
columns). Once DuckDB's own compression is applied, token encoding saves 7% (URL) and 16% (Referer).
**Person B FSST results:** none in the repository, so DuckDB's built-in FSST is the only FSST
comparison made.

## Measured performance (`results/bench.json`, pinned to 4 P-cores)

* **Cold vs warm** (1M rows): the matcher is built once per pattern at bind time; warm queries
  reuse it (extension counters: 1 build cold, 0 builds warm, 7 cache hits in 7 warm runs, for every
  pattern). Cold minus warm is close to the build time: −0.5 to 10 ms for ordinary patterns (−0.5 is noise), 33 ms for
  `http://irr.ru/____%` (32 states), and 77 ms for the 65-state exact URL (91 ms cold against 14 ms
  warm). Every cold and warm count equals the raw-text count.
* **SQL scan floor** (1M rows, uncompressed storage, 1 thread, ns/row): DuckDB C-API scalar call that
  reads nothing 7.6; `octet_length` 7.9; + copying and validating the ids 14.9; `token_like('%.php')`
  20.0; raw `LIKE '%.php'` 8.7. So the matcher itself costs ~5 ns/row here, close to the standalone
  CLI's ~6–9 ns/row. The rest of the SQL time is DuckDB's scan plus the per-row id copy that the
  frozen `Column` API requires. With DuckDB's normal compressed storage, the scan floor is
  7.2–7.6 ms for 1M rows at 4 threads, and `token_like('%.php')` takes 10.6 ms.
* **Decode only the output**: filter with `token_like`, GROUP BY the token BLOB, `LIMIT 10`, then
  `detokenize`. That is 15.2 ms, with `detokenize()` run on **10 rows** (extension counter). Decoding
  every row first takes 25.1 ms (718,561 detokenized rows); the raw-text query takes 9.1 ms. The top-10
  counts are verified against the raw-text GROUP BY. In DuckDB's plan, the projection that outputs `url`
  (the `detokenize` alias) sits above `TOP_N` and receives 10 rows (`EXPLAIN ANALYZE` in RESULTS.md); that and the
  counter are the evidence, not an assumption.
* **token_like vs detokenize+LIKE** (same compressed column): 1.5–2.7× faster at 1M rows (normal
  storage), 1.6–4.6× on uncompressed storage, and 1.2–4.1× at 10k–100k rows. At 1k rows the
  difference is within statement overhead (0.64–2.1×).
* **token_like vs native LIKE on the VARCHAR column**: usually slower, 0.52–1.35× at 1M rows with
  normal storage and 0.28–1.15× uncompressed. It is faster only in 7 of the 40 1M-row cells (mostly
  `%/search%`, and `http://%` single-threaded).
* **SQL vs standalone CLI** (single thread, ns/row): 0.98–4.4× the CLI's scan-only time; contains
  patterns are least affected.
* **Cold start** (fresh process): `LOAD` 1.2–4.6 ms, vocabulary 7.6–11 ms; peak memory is the same in all modes (140–146 MB).

**Routing policy.** `demo_sql.py` contains an *application-level routing demonstration* that
chooses a predicate from measured class medians. With only the token column stored, it chose
`detokenize+LIKE` **0 of 5** times: that half is a demonstration, not a decision. With the original
text also stored, it chose raw LIKE 5 of 5 times. It is not a DuckDB optimizer.

## Caveats

1. Not planner-integrated: queries must call `token_like` explicitly. An optimizer extension would
   need DuckDB's C++ extension API and a DuckDB source build with the matching compiler (MSVC).
2. The extension is unsigned (`allow_unsigned_extensions` required); built for `windows_amd64`, C API
   v1.5.6; tested only with DuckDB 1.5.6.
3. The frozen matcher API takes `Column` (std::vectors), so ids are copied once per row per query
   (≈ 7 ns/row single-threaded, measured by `tokenlike_floor_copy`). A span-based entry point would
   remove it, but that is a change to the frozen API, so it is not done here.
4. Automaton limit 512 minimized states. Larger patterns fail with a clean SQL error, never a wrong
   result. The transient dense table can reach 64 MB at the limit.
5. MinGW-built DLL inside MSVC-built DuckDB: callbacks never let C++ exceptions escape. A DuckDB-internal
   exception (for example out-of-memory) thrown inside a C-API call made from a GCC frame was not
   tested. The MSVC build avoids mixed runtimes and passes the same suite.
6. Benchmarks on hybrid Intel CPUs must be pinned (the script pins itself). Unpinned runs varied 3–5×.

## Coordination needed

* **No shared files were modified.** Optional, for Person C to decide:
  * add a link to `integration/duckdb/README.md` in the top-level README;
  * optionally add `option(TOKENLIKE_BUILD_DUCKDB OFF)` + `add_subdirectory(integration/duckdb)` to
    the top-level CMake. It is not needed: the module builds standalone.
* **Person B:** the module reads `data/` (`vocab.bin`, `clickbench/*`, optional `edge`, `fuzz`,
  `tpch`) read-only, with the documented formats; `build_db.py` also reads
  `data/download/hits_0.parquet` for the `TraficSourceID` column, verified against the canonical row
  ids. `fuzz.py --random` must be given `--patterns build/duckdb/scratch/fuzz_s7.txt` so that it does not
  overwrite the tracked `patterns/generated_fuzz.txt`.
* **Person A (matcher):** no matcher bug was found and no matcher file was changed.
