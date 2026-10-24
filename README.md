# TokenLIKE

Exact SQL `LIKE` matching directly on compressed 16-bit token IDs, for URL / CDN-log
filtering (`LIKE '/static/%'`, `LIKE '%.m3u8'`, `LIKE '%/v_/%'`).
Hashcode 14.0, Track 04: Data Compression.

The repository has one core and two optional modules:

| Component | What it is | Owner | Build | Verified on |
|---|---|---|---|---|
| **Core TokenLIKE** | Token-level LIKE automaton over a global token table, CLI, baselines, benchmarks, Streamlit demo | A (matcher), B (data), C (CLI/bench/demo) | default | Windows (mingw-w64 GCC 14.2) |
| **`integration/duckdb/`** | Loadable DuckDB extension: `token_like()` runs the same matcher inside SQL queries | A | standalone CMake project | Windows (GCC 14.2 by C; GCC 16.2 and MSVC by A) |
| **`integration/fsst/`** | FSST-LIKE: LIKE evaluated directly on FSST-compressed strings | B | `-DTOKENLIKE_WITH_FSST=ON` | macOS arm64 (B); Windows GCC 14.2 (C) |

Measured results and the platform/test matrix: [FINAL_INTEGRATION_REPORT.md](FINAL_INTEGRATION_REPORT.md).
TokenLIKE is a research prototype for scan-time `LIKE`, not a drop-in storage format for
arbitrary databases.

The core compares TokenLIKE with two baselines that share one independent byte-level LIKE
implementation (`src/like_bytes.cpp`, no DFA):

| Mode     | What it does |
|----------|--------------|
| `token`  | TokenLIKE: token-level automaton over token IDs (Person A) |
| `decode` | Fused decode-then-match: decode each row's tokens into a reused buffer, then LIKE |
| `raw`    | LIKE over the original uncompressed UTF-8: the speed with no compression at all |

## LIKE semantics

Case-sensitive; `%` = any sequence; `_` = exactly **one Unicode code point**; no `ESCAPE`
character (backslash is literal). These match DuckDB (`'é' LIKE '_'` is true,
`'a_b' LIKE 'a\_b'` is false). Patterns and data must be valid UTF-8; the CLI rejects an
invalid-UTF-8 pattern in every mode. NULL is stored as `''`.

## Requirements

- CMake ≥ 3.16, Ninja, a C++17 compiler (GCC ≥ 8 or Clang ≥ 7).
  **Windows:** mingw-w64 GCC 14 (`C:\mingw-w64\mingw64\bin\g++.exe`, selected by the
  `mingw-release` preset). The old MinGW.org GCC 6.3 in `C:\MinGW\bin` has no
  `<string_view>`; CMake refuses it.
- `clang++` on `PATH` for `scripts/check_all.sh` (it compiles Person B's `test_io` itself).
  mingw-w64 ships it next to `g++`.
- Python ≥ 3.10. Versions are pinned:

```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements.txt      # Linux/macOS: .venv/bin/python
```

- Windows without long-path support: keep the checkout in a short path (pip fails to
  install Streamlit into deeply nested directories).

## Build

```bash
# Windows (pinned mingw-w64 GCC 14):
cmake --preset mingw-release
# Linux/macOS, or any default compiler:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3 -march=native"

cmake --build build -j
ctest --test-dir build --output-on-failure      # 16 tests with data present (15 without)
```

Outputs: `build/libtokenlike.a`, `build/tokenlike(.exe)` (CLI), `build/test_baselines`,
`build/test_matcher`, `build/test_io`. `test_io` (Person B's loader test on the real
column) is registered only if `data/` exists at configure time: re-run the configure
step after generating the data.

The configure line `TokenLIKE: loaders (Person B) = ON/OFF, matcher (Person A) = ON/OFF`
says what is built. Without the matcher, `--mode token` exits with code 2; there is no
stub and no fallback to decoding.

### Optional: DuckDB extension (Person A, `integration/duckdb/`)

A standalone CMake project; the core build does not need DuckDB.

```bash
cmake -S integration/duckdb -B build/duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DCMAKE_CXX_FLAGS=-O3 -march=native" -DCMAKE_CXX_COMPILER=C:/mingw-w64/mingw64/bin/g++.exe \
      -DPython3_EXECUTABLE=$PWD/.venv/Scripts/python.exe
cmake --build build/duckdb                    # -> build/duckdb/tokenlike.duckdb_extension
ctest --test-dir build/duckdb                 # data-free smoke test
```

Details, the full SQL suite and caveats: [integration/duckdb/README.md](integration/duckdb/README.md).
Queries must call `token_like(...)` explicitly: there is **no** optimizer rule, so a plain
`LIKE` is never rewritten.

### Optional: FSST-LIKE (Person B, `integration/fsst/`)

```bash
cmake -S . -B build-fsst -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOKENLIKE_WITH_FSST=ON \
      -DCMAKE_CXX_COMPILER=C:/mingw-w64/mingw64/bin/g++.exe   # Windows; omit on macOS
cmake --build build-fsst -j
ctest --test-dir build-fsst                   # core tests + fsst_unit
```

The first configure clones upstream FSST at a pinned commit (network needed once); see
[integration/fsst/README.md](integration/fsst/README.md) and
[integration/fsst/THIRD_PARTY.md](integration/fsst/THIRD_PARTY.md). Binaries built with
the module must carry upstream FSST's MIT notice.

## Data

Produced by Person B's pipeline (`scripts/encode/`), never committed. It is
byte-deterministic, so each machine regenerates it instead of copying ~800 MB:

```bash
P=.venv/Scripts/python                    # Linux/macOS: .venv/bin/python
$P scripts/encode/build_vocab.py          # data/vocab.bin: 65,536 tokens, 630,133 bytes
$P scripts/encode/fetch_clickbench.py --rows 1000000 --columns URL,Referer   # downloads 122 MB once
$P scripts/encode/encode.py data/clickbench/URL
$P scripts/encode/encode.py data/clickbench/Referer
```

Layout: `data/vocab.bin` and `data/<dataset>/<column>.{tok,off,raw,rawoff,meta.json,parquet}`
(`.tok` uint16, `.off`/`.rawoff` uint64 with `rows + 1` entries starting at 0, little-endian;
see [include/tokenlike/common.h](include/tokenlike/common.h)). DuckDB ground truth goes to
`data/truth/<dataset>/<name>.ids`. The optional modules keep their own generated data in
git-ignored places (`data/duckdb/`, `integration/fsst/data/`).

## CLI

```bash
build/tokenlike --mode token|decode|raw --pattern '%.m3u8' --col data/clickbench/URL \
    [--col2 PREFIX] [--vocab data/vocab.bin] [--runs 5] [--warmup 1] \
    [--rowids FILE] [--stats FILE] [--pin-cpu N] [--pattern-file FILE]
build/tokenlike --help
```

- stdout gets **exactly one JSON line**; diagnostics go to stderr. Exit codes: 0 ok,
  1 error (including every rejected input), 2 mode unavailable in this build.
- Rejected with exit 1 in every mode: a pattern that is not valid UTF-8, a token id outside
  the vocabulary (checked after loading, before any timing), an option given twice.
- `--column`, `--column2`, `--out` are aliases of `--col`, `--col2`, `--rowids`.
  `--col2` matches `col[r] || col2[r]` in all three modes (equal row counts required).
- Timing fields: `ns_per_row` / `gbps` are the **median warm scan** over `--runs` timed
  scans after `--warmup` scans; `build_us` is matcher construction; `first_scan_ns` is the
  process's first scan; `load_ms`, `validate_ms`; `cold_total_ms` = load + id validation +
  build + first scan, the cost of a single one-off query.
- `--rowids` writes matching row ids as uint32 little-endian, strictly ascending.

**Shell quoting:** in Git Bash, MSYS rewrites arguments that look like paths:
`'/static/%'` arrives as `C:/Program Files/Git/static/%`. The CLI warns when this
happens; use `MSYS_NO_PATHCONV=1`, PowerShell, or `--pattern-file`. In `cmd.exe`,
`%...%` expands variables.

### Token-mode limits

Token mode compiles the pattern into a byte DFA and lifts it onto the vocabulary. It
refuses (exit 1, clear message, never a wrong answer) patterns whose minimized DFA needs
more than **512 states**:

- a literal longer than about **510 bytes** (exact, prefix, suffix or contains);
- patterns with many `_` between literals, e.g. `%a________b%` (8 `_`) or 150 `_`.

`decode` and `raw` modes have no such limit. Building a pattern near the limit takes about
1 s and up to 64 MB of transient memory; a 4,000-byte literal spends about 20 s before it
is refused. The limit is Person A's (`kMaxStates` in `include/tokenlike/dfa.h`) and is not
changed without a separate correctness and performance assessment.

## Correctness

```bash
ctest --test-dir build --output-on-failure        # unit tests + CLI regression tests
TOKENLIKE_PYTHON=.venv/Scripts/python.exe TOKENLIKE_CLI=build/tokenlike bash scripts/check_all.sh
.venv/Scripts/python scripts/verify/check.py patterns/generated_fuzz.txt --cli build/tokenlike
```

Release gate: 1,182 (pattern, mode) checks against DuckDB (core 66, tier 1 39, edge 108,
tier 2 69, fuzz 900), 0 failures, 0 skips. `check.py` fails the run if a requested mode did
not execute, if row ids are not strictly ascending, or if the CLI used test-fixture loaders;
a skip that is genuinely expected must be named with `--allow-skip MODE` and is reported
separately.

## Benchmarks

```bash
python scripts/bench/run_bench.py patterns/core.txt --runs 7 --repeats 3 --pin-cpu 2
```

- Writes `results/runs/run-<UTC time>.json` by default and **never overwrites** an existing
  file unless `--overwrite` is given. The recorded results in `results/*.json` are kept as
  they were measured.
- Ground truth is refreshed first, then the machine idles for `--cooldown` seconds (default
  10) before timing. Each (pattern, mode) runs in `--repeats` separate processes;
  `ns_per_row` is the median of the per-process warm medians, and the per-process values,
  first-scan (cold) times and build times are stored.
- A pattern is `pass` only if **every** requested mode ran and its row ids are
  byte-identical to the DuckDB truth; a mode that did not run makes it `incomplete` (exit 1).
- The hostname is not recorded unless `--record-host` is given.

Rules: Release build, one machine and one commit per results file, at least 5 runs, pin to
a performance core on hybrid CPUs, plug laptops in.

## Demonstrations

```bash
python demo/menu.py          # lists every demo, what it needs and where it was verified
python demo/menu.py 1        # runs one
```

| # | Demo | Command (repository root) | Verified on |
|---|---|---|---|
| 1 | TokenLIKE replay (saved, verified results) | `streamlit run demo/app.py -- --replay results/final.json` | Windows |
| 2 | TokenLIKE live (runs and verifies now) | `streamlit run demo/app.py` | Windows |
| 3 | TokenLIKE CLI | `build/tokenlike --mode token --pattern '%.php' --col data/clickbench/URL` | Windows |
| 4 | DuckDB `token_like()` in SQL | `python -X utf8 integration/duckdb/python/demo_sql.py` | Windows |
| 5 | DuckDB plain-SQL script | `python -X utf8 integration/duckdb/python/run_sql.py integration/duckdb/examples/judge_demo.sql` | Windows |
| 6 | FSST-LIKE end to end | `python integration/fsst/demo_fsst.py --rows 100000 --tokenlike .` | macOS (B), Windows (C) |

Streamlit binds to **127.0.0.1** only (`.streamlit/config.toml`; the menu also passes
`--server.address 127.0.0.1`), so live mode is not reachable from a venue network.
"Verified" is shown only when **every** mode ran and matched the DuckDB truth; replay mode
always shows a banner with the commit, CPU and timestamp of the saved run.

## Repository ownership

| Owner | Files |
|-------|-------|
| A: matcher, DuckDB | `include/tokenlike/{pattern,dfa,matcher}.h`, `src/{pattern,nfa,dfa,lift,compress,scan,matcher}.cpp`, `tests/test_matcher.cpp`, `integration/duckdb/` |
| B: data, FSST | `include/tokenlike/{vocab,column}.h`, `src/{vocab,column}.cpp`, `scripts/encode/*`, `scripts/verify/*`, `scripts/check_all.sh`, `patterns/*`, `tests/test_io.cpp`, `integration/fsst/` |
| C: bench, integration | `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `requirements.txt`, `.gitignore`, `.gitattributes`, `.streamlit/`, `include/tokenlike/{common,baselines}.h`, `src/{like_bytes,baselines,cli}.cpp`, `tests/{test_baselines.cpp,fixture_io.*,expect_exit.cmake}`, `scripts/bench/*`, `demo/*`, `results/` |

Only C merges into `main`, after the integration branch passes its release gates.
