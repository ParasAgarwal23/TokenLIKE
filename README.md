# TokenLIKE

Exact SQL `LIKE` matching directly on compressed 16-bit token IDs, for URL / CDN-log
filtering (`LIKE '/static/%'`, `LIKE '%.m3u8'`, `LIKE '%/v_/%'`).
Hashcode 14.0, Track 04: Data Compression.

TokenLIKE is compared against two baselines that share one independent byte-level
LIKE implementation (`src/like_bytes.cpp`, no DFA):

| Mode     | What it does |
|----------|--------------|
| `token`  | TokenLIKE: token-level automaton over token IDs (Person A) |
| `decode` | Fused decode-then-match: decode each row's tokens into a reused buffer, then LIKE. **The main competitor.** |
| `raw`    | LIKE over the original uncompressed UTF-8. Shows the speed with no compression; it reads a different, larger representation. |

## LIKE semantics

Case-sensitive; `%` = any sequence; `_` = exactly **one Unicode code point**; no `ESCAPE`
character (backslash is literal). These match DuckDB (checked: `'é' LIKE '_'` is true,
`'a_b' LIKE 'a\_b'` is false). Data must be valid UTF-8; NULL is stored as `''`.

## Requirements

- CMake ≥ 3.16, Ninja, a C++17 compiler (GCC ≥ 8 or Clang ≥ 7).
  **Windows:** use mingw-w64 GCC 14 (`C:\mingw-w64\mingw64\bin\g++.exe`). The old
  MinGW.org GCC 6.3 in `C:\MinGW\bin` has no `<string_view>`; CMake refuses it.
- Python ≥ 3.10 with `streamlit duckdb pyarrow pandas` (demo and correctness checks).

```bash
python -m venv .venv
.venv/Scripts/python -m pip install streamlit duckdb pyarrow pandas   # Linux/macOS: .venv/bin/python
```

## Build

```bash
# Windows (pinned mingw-w64 GCC 14):
cmake --preset mingw-release
# Linux/macOS, or any default compiler:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3 -march=native"

cmake --build build -j
ctest --test-dir build --output-on-failure
```

Outputs: `build/libtokenlike.a`, `build/tokenlike(.exe)` (CLI), `build/test_baselines`,
and `build/test_matcher` once Person A's test exists.

The build adapts to what is merged. The configure line
`TokenLIKE: loaders (Person B) = ON/OFF, matcher (Person A) = ON/OFF` says what is in:

- **No matcher:** `--mode token` exits with code 2 (`token mode not built`). There is no stub.
- **No loaders:** the CLI links *test fixture* loaders (`tests/fixture_io.cpp`), prints a
  warning on stderr and reports `"loaders": "fixture"` in its JSON. Use it only with
  synthetic fixtures (`build/test_baselines --write-fixture DIR`).

## Data

Produced by Person B (`scripts/encode/`), never committed:

```
data/vocab.bin
data/<dataset>/<column>.{tok,off,raw,rawoff,meta.json,parquet}
```

`.tok` uint16, `.off`/`.rawoff` uint64 with `rows + 1` entries starting at 0, `.raw` bytes;
little-endian. The layout is documented in [include/tokenlike/common.h](include/tokenlike/common.h).

## CLI

```bash
build/tokenlike --mode token|decode|raw --pattern '%.m3u8' --col data/clickbench/url \
    [--col2 PREFIX] [--vocab data/vocab.bin] [--runs 5] [--warmup 1] \
    [--rowids results/rowids/example.bin] [--pin-cpu 0] [--pattern-file FILE]
```

- Loading is not timed. Matcher construction is timed separately (`build_us`). Each of
  `--runs` scans is timed on its own after `--warmup` untimed scans; `ns_per_row` and
  `gbps` come from the **median**.
- `gbps` = original text bytes of the scanned rows / median scan time (decimal GB/s),
  the same for every mode.
- stdout gets **exactly one JSON line**; diagnostics go to stderr. Exit codes:
  0 ok, 1 error, 2 mode unavailable in this build.
- `--rowids` writes matching row ids as uint32 little-endian, ascending.
- `table_kb` / `dense_table_kb` are `null` for the baselines (they have no transition table).

**Shell quoting:** in Git Bash, set `MSYS_NO_PATHCONV=1` or MSYS rewrites patterns like
`'%/v_/%'` into Windows paths. In `cmd.exe`, `%...%` expands variables; use PowerShell or
`--pattern-file`. Non-ASCII patterns work (the CLI reads the UTF-16 command line on Windows).

## Correctness

```bash
ctest --test-dir build --output-on-failure    # baselines: hand cases + 1.3M fuzz vs. a reference DP
bash scripts/check_all.sh                     # Person B: DuckDB ground truth on the real data
```

`test_baselines` checks the byte matcher against a slow code-point DP reference
(including targeted fuzzing of every fast path), and raw == decode == reference on a
synthetic URL column.

## Benchmarks

```bash
python scripts/bench/run_bench.py patterns/core.txt --runs 5 --out results/final.json
```

For each pattern it runs token, decode and raw in separate processes, pinned to CPU 0
(`--pin-cpu -1` disables pinning; pick a performance core on hybrid CPUs). Mode order
rotates per pattern. It checks that all modes return identical row ids and runs Person B's
DuckDB checker (`scripts/verify/check.py`, or `--checker 'cmd {col} {pattern} {rowids}'`).
It prints a comparison table and writes `results/final.json` with the git commit, dirty
flag, compiler and flags, CPU, data fingerprint and per-pattern verification. It refuses
a dirty tree unless `--allow-dirty` is passed.

Rules: Release build, same machine and commit for every number in one file, verify
correctness before comparing speed, at least 5 runs. On laptops, plug in and use the
best-performance power mode.

## Demo

```bash
streamlit run demo/app.py                                  # live: runs the CLI
streamlit run demo/app.py -- --replay results/final.json   # saved results
```

Options after `--`: `--patterns`, `--col`, `--vocab`, `--build`, `--runs`, `--checker`.
"Verified" is shown only when the DuckDB checker passed for those exact row ids. Replay
mode always shows a banner with the commit, CPU and timestamp of the saved run.

## Repository ownership

| Owner | Files |
|-------|-------|
| A: matcher | `include/tokenlike/{pattern,dfa,matcher}.h`, `src/{pattern,nfa,dfa,lift,compress,scan,matcher}.cpp`, `tests/test_matcher.cpp` |
| B: data | `include/tokenlike/{vocab,column}.h`, `src/{vocab,column}.cpp`, `scripts/encode/*`, `scripts/verify/*`, `scripts/check_all.sh`, `patterns/*` |
| C: bench / integration | `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `.gitignore`, `.gitattributes`, `include/tokenlike/{common,baselines}.h`, `src/{like_bytes,baselines,cli}.cpp`, `tests/{test_baselines.cpp,fixture_io.*}`, `scripts/bench/*`, `demo/*` |

Only C merges into `main`. Merges are tested on an `int/*` branch first, and `main` only
fast-forwards to a state that builds and passes tests and correctness checks.
