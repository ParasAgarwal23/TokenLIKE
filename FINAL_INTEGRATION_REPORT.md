# TokenLIKE v4 integration report

Integration of the core TokenLIKE project (`v3-freeze`, `c58d066`) with Person A's DuckDB
extension and Person B's FSST-LIKE module, plus fixes for the v3-freeze master audit.
Branch `integration/sql-fsst-v4`. Every number below was measured on the machine named next
to it; numbers from different machines are never mixed in one table.

## 1. Outcome

| | Result |
|---|---|
| Original release gate (Windows) | CTest **16/16**; DuckDB correctness **1,182/1,182**, 0 failed, **0 skipped**, official loaders |
| Person A's SQL gate (Windows, GCC 14.2) | **1,116/1,116** checks, 694 real-data patterns, 0 skipped |
| Person B's FSST gate (Windows, GCC 14.2) | unit **6,429/0**; patterns **3,367 passed, 0 failed**, 33 documented skips; **15/15** patterns identical to the TokenLIKE CLI |
| FSST columns, macOS (B) vs Windows (C) | **9/9 byte-identical** (FNV-64 of codes, offsets, symbol table) |
| Audit defects | every item listed for this integration addressed or documented (section 4); regression tests added |
| `v3-freeze` | untouched (`c58d066`); all earlier tags unchanged |

Platform support, as actually built and executed:

| Component | Windows 11 x86-64 (mingw-w64 GCC 14.2) | macOS arm64 | Linux |
|---|---|---|---|
| Core TokenLIKE | built, all gates pass (C) | not tested by C | not tested |
| DuckDB extension | built, smoke + full SQL suite pass (C); also GCC 16.2 and MSVC (A) | not supported (extension built for `windows_amd64`) | not tested |
| FSST-LIKE | built, unit + full gate pass (C), **after 4 Windows fixes** (section 5) | verified by B at `99accef`; **C's portability patches not yet re-run on macOS** (B to confirm) | not tested |

## 2. What was merged

| Branch | Head | Owner | Content | Shared files touched |
|---|---|---|---|---|
| `feature/duckdb-sql` | `3fbef34` | A | `integration/duckdb/`: native DuckDB C-API extension (`token_like`, `token_like_concat`, `tokenize`, `detokenize`), tests, demo, results | none |
| `feature/fsst-like` | `99accef` | B | `integration/fsst/`: FSST-LIKE library, tools, tests, scripts, results | root `CMakeLists.txt` (11-line opt-in hook), `.gitignore` (2 lines) |

Both branches were based on `c58d066` and merged with `--no-ff` without conflicts. No
algorithm in either module was changed by C. Changes C made inside A/B-owned paths are
separate commits titled `[B-patch by C]`, listed in section 5, for the owner's review.

Build modules:

- **Core** (default): `cmake --preset mingw-release` (or `cmake -B build ...`). Unchanged
  targets; FSST and DuckDB are not compiled (`nm` shows 0 FSST symbols in the CLI).
- **DuckDB extension**: standalone project, `cmake -S integration/duckdb -B build/duckdb`.
  Not added to the root build (it needs Python for the extension footer and is
  Windows-specific); the core build never needs DuckDB.
- **FSST-LIKE**: `-DTOKENLIKE_WITH_FSST=ON` (default OFF). Clones upstream FSST at the pinned
  commit `e638d4c` on first configure; compiles only `libfsst.cpp` and `fsst_avx512.cpp`,
  unmodified. Binaries must carry upstream FSST's MIT notice.

No module adds a fallback to decoding: the TokenLIKE CLI exits 2 when token mode is not
built, the FSST tool exits 2 for unsupported mode/flag combinations, and the DuckDB extension
raises a SQL error for patterns over the state limit.

## 3. Test matrix (Windows 11, Intel Core i5-1334U, mingw-w64 GCC 14.2.0, Python 3.13.0, DuckDB 1.5.6)

| Gate | Configuration | Result |
|---|---|---|
| Core CTest | Release `-O3 -march=native` | 16/16 |
| Core CTest | Debug + `_GLIBCXX_ASSERTIONS` | 16/16 |
| Core CTest | UBSan (trap mode) + `_GLIBCXX_ASSERTIONS` | 16/16 |
| Core CTest | `TOKENLIKE_CHECKED` + `_GLIBCXX_ASSERTIONS` | 16/16 |
| A's matcher suite (inside CTest) | all four builds | 3,354,002 checks, 0 failures |
| `scripts/check_all.sh` (B's gate, patched checker) | Release | loaders and round trip on 8 columns; core 66, tier 1 39, edge 108, tier 2 69 |
| `check.py patterns/generated_fuzz.txt` | Release | 900/900 |
| **Original total** | | **1,182/1,182, 0 failed, 0 skipped, 0 expected skips** |
| DuckDB module build | GCC 14.2, Release | 0 warnings; DLL depends only on Windows system DLLs |
| DuckDB smoke (CTest, data-free) | | 1/1 |
| DuckDB full SQL suite (`test_sql.py`) | real data, 1M rows | 1,116 checks, 0 failed, 694 patterns, 0 skipped |
| Combined build core + FSST | Release | CTest 17/17 |
| FSST unit suite | Release | 6,429 checks, 0 failures |
| FSST unit suite | UBSan trap + assertions, upstream FSST not sanitized | 6,429/0 (CTest 17/17) |
| FSST unit suite | UBSan trap on everything incl. upstream | **traps inside upstream `libfsst.cpp`** (see section 6) |
| FSST gate (`integration/fsst/scripts/check_all.sh`) | Release | ALL CHECKS PASSED: structural validation of 9 columns, DuckDB semantics probes, 3,367 pattern checks passed, 0 failed, 33 documented skips (`--col2` with three reverse/compact modes, exit 2), 15/15 patterns identical to TokenLIKE |
| CLI regression tests (new) | | invalid UTF-8 pattern (3 modes), out-of-range token id (token, decode), `--help`, repeated option: exact exit codes; all fail on the old `v3-freeze` binary |
| Checker regression (test double corrupting the real CLI's output) | | dropped / added / unsorted / duplicated / truncated / byte-swapped ids and a missing mode all fail; an uncorrupted run passes 22/22 |
| Streamlit, real browser (Edge 155 via Playwright) | replay, live, concat, error paths | 36/36 checks; localhost-only binding confirmed; no temp files left behind |
| Demo menu | demos 1-6 | all run (demos 1-2 via the browser suite, 3-6 directly) |

NOT TESTED: macOS and Linux builds by C; AddressSanitizer (no ASan runtime in either MinGW
toolchain); MSVC build of the core and FSST; the DuckDB extension inside an MSVC-built DuckDB
with a GCC-built DLL under out-of-memory conditions (A's caveat).

## 4. Audit fixes

| ID | Sev. | Fix | Evidence |
|---|---|---|---|
| C-2 | High | CLI validates token ids against the vocabulary after loading and before timing (Person B's `validate_column`), for token and decode modes. Costs 17.5 ms for 36.1M ids; the timed scan is unchanged | `cli_rejects_bad_ids_*` tests; the old binary crashed (decode) or returned exit 0 (token) |
| B-3 | High | `check.py`: a requested mode that did not run fails the run (INCOMPLETE). Expected skips must be named with `--allow-skip MODE` and are reported separately; fixture loaders fail the run | test double: token mode missing now exits 1; with `--allow-skip token`, 22 expected skips reported |
| C-3 | Medium | `run_bench`: `pass` only if every requested mode ran and matched; otherwise `incomplete` (exit 1). Demo badge recomputed from per-mode statuses | browser test: a result whose token mode failed shows "Incomplete … NOT verified" even though the file says pass |
| C-7 | Medium | `.streamlit/config.toml` binds to 127.0.0.1; demo menu passes `--server.address 127.0.0.1` | Streamlit now advertises only `http://127.0.0.1:<port>` |
| C-1 | Medium | invalid-UTF-8 pattern rejected with exit 1 and a byte offset in every mode | `cli_rejects_bad_utf8_*` (3 tests) |
| B-2 | Medium | `check.py` requires strictly ascending row ids | test double: unsorted and duplicated output now fail |
| B-1 | Medium | `check.py` writes UTF-8 (errors=replace) | test double without any encoding override: mismatch evidence printed, no crash |
| A-1 | Medium | token-mode limits documented (README, CLI hint on the error); limit unchanged | README "Token-mode limits" |
| C-11 | Medium | `run_bench`: cooldown after ground truth, `--repeats` processes (median of process medians), first-scan and cold totals recorded; CLI JSON gains `load_ms`, `validate_ms`, `first_scan_ns`, `cold_total_ms` | section 7 |
| B-4, B-5, B-6 | Low | evidence columns cached; out-of-range ids labelled; truncated file reported as failure | test double |
| C-4, C-6, C-8, C-9 | Low | malformed replay entries skipped with a warning; `--runs` clamped; live temp dirs removed; no delta arrows, readable chart labels | browser suite |
| C-12 | Low | `--help` exits 0; repeated options rejected | CTest |
| C-13 | Low | CLI warns when Git Bash has rewritten a pattern into a Windows path | manual run |
| C-14 | Low | CTest exercises token mode and compares row-id files of all modes byte for byte | CTest 16 |
| C-15 | Low | hostname recorded only with `--record-host` (earlier committed results unchanged) | new results contain no hostname |
| deps, docs | Low | `requirements.txt` pins exact versions; README lists clang++ for `check_all.sh`, removes the obsolete curl step, documents long-path issue; benchmark scripts never overwrite results without `--overwrite` | |

Correction to the audit: it reported a pandas version drift (2.3.2 vs 3.1.0). The 2.3.2
reading came from the system Python, not the project venv; both venvs use pandas 3.1.0.

Not changed (by design or out of scope): the 512-state token-mode limit (A-1, needs its own
assessment); the minimisation time for very long patterns (A-2, ~20 s before rejecting a
4,000-byte literal); compact-table size for pathological patterns (A-3); behaviour on
invalid UTF-8 *data* (A/C-3, the pipeline guarantees valid UTF-8).

## 5. Windows portability fixes in the modules

Found by building and running the modules on Windows for the first time. Each is a minimal
change; none alters an algorithm, a data format or a test expectation.

| Where | Problem | Fix | Owner of the file |
|---|---|---|---|
| upstream `fsst_avx512.cpp` | `__cpuidex` used without `<intrin.h>` (MSVC-only assumption); build fails with MinGW | root hook force-includes `intrin.h` for `fsst_upstream` under MinGW; upstream unmodified | C (`CMakeLists.txt`) |
| FSST tools `main(int, char**)` | argv in the ANSI code page: `'https://с%'` arrived as `'https://?%'`; every Cyrillic pattern failed in all 8 modes (32 + 40 + 192 checks) | UTF-8 `activeCodePage` application manifest embedded via the root hook (`cmake/utf8.manifest`) | C |
| FSST tools linking | dynamic MinGW runtime; started from a shell with `C:\MinGW\bin` (GCC 6.3) first on PATH they failed with `STATUS_ENTRYPOINT_NOT_FOUND` | linked `-static` like the core tools | C |
| `tests/test_unit.cpp` | `std::system("mkdir -p ...")` | `std::filesystem::create_directories` | B (patched by C) |
| `src/common.cpp` | `std::rename` does not replace an existing file on Windows | `std::filesystem::rename` | B (patched by C) |
| `src/encode.cpp` | paths written unescaped into JSON; `C:\...` made `.fsst.meta.json` invalid | JSON-escape the paths | B (patched by C) |
| `scripts/*.py`, `demo_fsst.py` | subprocess output decoded as cp1252; `.exe` not found; Cyrillic printing crashed | decode as UTF-8; `resolve_cli`; UTF-8 console streams | B (patched by C) |

**Person B: please re-run `bash scripts/check_all.sh` on macOS** on this branch to confirm the
patched files behave as before there.

## 6. Robustness and sanitizers

- Core + DuckDB-matcher sources: UBSan (trap) + libstdc++ assertions + `TOKENLIKE_CHECKED`
  builds pass all tests.
- FSST-LIKE (B's code): passes the unit suite under UBSan (trap) + assertions when the
  upstream FSST files are compiled without the sanitizer.
- Upstream FSST: with the sanitizer also applied to it, GCC's UBSan traps inside
  `libfsst.cpp:220` (`buildSymbolTable`), and still does with `-fno-sanitize=alignment`.
  B's macOS run suppresses one upstream class (`alignment:*third_party/fsst/*`) with Clang's
  runtime; GCC flags at least one more check there. The upstream code is used unmodified, as
  B intends; this is recorded, not fixed.

## 7. Performance and compression

### 7.1 Same machine, same rows: TokenLIKE vs FSST-LIKE vs baselines

Intel Core i5-1334U (hybrid; every process pinned to logical CPU 2, a performance core),
Windows 11, GCC 14.2 `-O3 -march=native`, 1,000,000 ClickBench URL rows, commit `460400a`.
**Best** ns/row = minimum over 7 warm runs x 3 separate processes; every cell's row ids are
byte-identical to the DuckDB truth (`results/integration-v4/i5-1334U/compare.json`).
"FSST best plan" is the better of FSST-LIKE's forward and sidecar-free reverse scan; the user
picks it, whereas TokenLIKE chooses its direction automatically.

| Pattern | Class | TokenLIKE | Token-table full decode | FSST-LIKE best plan | FSST full decode | Raw, C's SSE2 matcher | Raw, B's raw-opt |
|---|---|---|---|---|---|---|---|
| `http://%` | prefix | 5.76 | 39.32 | 4.87 (fwd) | 34.90 | 4.95 | 5.44 |
| `http://irr.ru%` | prefix | 8.10 | 41.46 | 5.01 (fwd) | 38.07 | 5.09 | 10.81 |
| `%.php` | suffix | 4.79 | 40.67 | 3.73 (rev2) | 162.56 | 4.95 | 5.08 |
| `%.html` | suffix | 4.70 | 41.71 | 3.81 (rev2) | 162.99 | 4.93 | 4.62 |
| exact 64-byte URL | exact | 15.14 | 41.56 | 6.18 (rev2) | 57.73 | 1.99 | 2.43 |
| `%yandex%` | contains | 47.43 | 58.05 | 60.98 (fwd) | 160.17 | 21.82 | 43.13 |
| `%/v_/%` | contains + `_` | 86.28 | 66.87 | 61.37 (fwd) | 160.84 | 29.30 | 78.75 |
| `%сезон%` | contains, Cyrillic | 43.07 | 57.45 | 62.82 (fwd) | 163.03 | 22.09 | 37.14 |

Matcher construction (median, ms): TokenLIKE 2.4-7.8 for ordinary patterns, 65.7 for the
exact URL (65 states lifted over 65,536 tokens); FSST-LIKE 0.12-1.99 (alphabet of 256 symbols).
TokenLIKE's first (cold) scan is within 1-10% of its warm median.

Reading this table honestly:

- Neither compressed matcher beats a good uncompressed matcher in general. On anchored
  prefix/suffix patterns FSST-LIKE's best plan is 0.75-1.0x the time of C's raw matcher
  (slightly faster) and TokenLIKE is 0.95-1.6x (equal or slower); on the exact URL and on
  contains patterns raw is 2-8x faster than both.
- Against full decode-then-match, both compressed matchers win on anchored patterns
  (TokenLIKE 5.1-8.9x on prefix/suffix and 2.7x on the exact URL; FSST-LIKE 7-44x, its
  decoder being slower) because they stop after 1-3 units. On contains patterns the
  advantage shrinks or reverses (TokenLIKE on `%/v_/%` is slower than decoding).
- The two raw baselines differ by up to 2.7x (`%/v_/%`); "optimized raw" depends on the
  implementation, so both are shown.

### 7.2 Storage, same 1,000,000 URL rows

| Representation | Bytes | vs raw + offsets |
|---|---|---|
| Raw UTF-8 + u64 offsets | 96,562,200 | 1.00 |
| Token ids (u16) + u64 offsets | 80,272,114 | 0.83 |
| ... + the whole shared vocabulary (630,133 B) | 80,902,247 | 0.84 |
| FSST codes + u64 offsets + symbol table | 57,965,613 | 0.60 |
| ... + optional `.fsst.rev` sidecar (needed only by `fsst-rev`) | 61,965,613 | 0.64 |
| DuckDB 1.5.6, VARCHAR column, default storage (Dictionary + DuckDB's own FSST) | 39,321,600 | 0.41 |
| DuckDB 1.5.6, token BLOB column, default storage | 35,127,296 | 0.36 |
| DuckDB 1.5.6, VARCHAR column, uncompressed | 93,061,120 | 0.96 |

DuckDB's default storage is smaller than either standalone encoding because it also
deduplicates repeated URLs (dictionary segments). TokenLIKE is a research prototype for
scan-time `LIKE`, not a replacement for a database's storage layer.

### 7.3 DuckDB SQL, end to end (same i5-1334U, A's `bench_sql.py`, pinned to 4 performance threads)

Statement time from Python, median of 7 runs after a warm-up, 1,000,000 rows, DuckDB default
storage, 4 threads; every cell verified (`results/integration-v4/i5-1334U/duckdb/bench_i5-1334U.json`).

| Query | native `LIKE` on VARCHAR | `detokenize()` + `LIKE` | `token_like()` |
|---|---|---|---|
| `http://%` | 18.42 ms | 40.90 ms | 20.67 ms |
| `%.php` | 15.36 ms | 43.19 ms | 20.43 ms |
| exact URL | 14.53 ms | 43.32 ms | 26.51 ms |
| `%/search%` | 33.65 ms | 59.75 ms | 34.09 ms |
| `http://irr.ru/____%` | 21.86 ms | 44.32 ms | 21.35 ms |
| `url \|\| referer LIKE '%yandex%'` | 55.01 ms | 106.34 ms | 71.88 ms |

`token_like` is 1.5-2.3x faster than decoding inside SQL, and slower than or equal to native
`LIKE` on the stored text in 9 of 10 queries. The filter runs inside DuckDB's sequential scan
as a scalar predicate; **plain `LIKE` is never rewritten to `token_like`** (no optimizer rule).
Cold vs warm: the first query with a new pattern pays the matcher build (2.5-9 ms for ordinary
patterns, 37 ms for `http://irr.ru/____%`, 70 ms for the exact URL); later queries reuse it.
Filtering in token space and decoding only the 10 output rows took 23.7 ms vs 48.9 ms for
decoding every row first (raw text: 16.9 ms).

### 7.4 Numbers from other machines (not comparable with the tables above)

- Person A, Intel Core Ultra 9 288V: `integration/duckdb/RESULTS.md`, `results/bench.json`.
- Person B, Apple M2 (macOS arm64, Apple clang 21): `integration/fsst/FINAL_REPORT.md`,
  `bench/v2-integration/`.
- C's earlier core results (i5-1334U, commit `f26d302`): `results/{core,tier1,tier2,final}.json`,
  unchanged.

Uncertainty: one laptop; run-to-run differences between sessions of up to ~45% were measured
in the audit for raw numbers, and less than 16% across processes within one session.

## 8. Generated data, binaries, secrets

Not tracked: `data/` (incl. `data/duckdb/`), `integration/fsst/data/`,
`integration/fsst/third_party/`, every `build*/` directory, `.venv/`. New tracked results are
JSON/Markdown only (`results/integration-v4/`, about 330 KB) and contain no hostname or user
path. A's and B's committed results were restored after every run that rewrote them (their
scripts write into their tracked results directories; noted for A and B).

## 9. Reproduce

```bash
python -m venv .venv && .venv/Scripts/python -m pip install -r requirements.txt
cmake --preset mingw-release && cmake --build build -j && ctest --test-dir build
TOKENLIKE_PYTHON=.venv/Scripts/python.exe TOKENLIKE_CLI=build/tokenlike bash scripts/check_all.sh
.venv/Scripts/python scripts/verify/check.py patterns/generated_fuzz.txt --cli build/tokenlike

cmake -S integration/duckdb -B build/duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DCMAKE_CXX_FLAGS=-O3 -march=native" -DCMAKE_CXX_COMPILER=C:/mingw-w64/mingw64/bin/g++.exe \
      -DPython3_EXECUTABLE=$PWD/.venv/Scripts/python.exe
cmake --build build/duckdb && ctest --test-dir build/duckdb
.venv/Scripts/python -X utf8 integration/duckdb/python/build_db.py
.venv/Scripts/python -X utf8 integration/duckdb/python/test_sql.py

cmake -S . -B build-fsst -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOKENLIKE_WITH_FSST=ON \
      -DCMAKE_CXX_COMPILER=C:/mingw-w64/mingw64/bin/g++.exe && cmake --build build-fsst -j
(cd integration/fsst && TOKENLIKE_DIR=$PWD/../.. FSSTLIKE_BUILD=../../build-fsst \
   FSSTLIKE_PYTHON=$PWD/../../.venv/Scripts/python.exe bash scripts/build_columns.sh && \
 TOKENLIKE_DIR=$PWD/../.. FSSTLIKE_BUILD=../../build-fsst \
   FSSTLIKE_PYTHON=$PWD/../../.venv/Scripts/python.exe bash scripts/check_all.sh)

python scripts/bench/compare_modules.py --out results/runs/compare.json
python demo/menu.py
```

## 10. Open items for the owners

- **Person B:** review the four `[B-patch by C]` commits; re-run the FSST gate on macOS.
  Note that `integration/fsst/scripts/check_all.sh` and `compare_tokenlike.py` rewrite the
  tracked `bench/v2-integration/vs_tokenlike.json` on every run.
- **Person A:** `build_db.py`, `storage_report.py` and `bench_sql.py` (untagged) rewrite the
  tracked files in `integration/duckdb/results/`; the README's
  `fuzz.py --random --seed 7 ...` step without `--out` overwrites `data/fuzz/values`, the
  dataset the original 900-check gate uses. C reproduced the 1,116-check suite by generating
  the seed-7 patterns into `build/duckdb/scratch` instead.
- **Person A (matcher):** token-mode limits and the long-literal build time (A-1, A-2).
