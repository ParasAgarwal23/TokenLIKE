# Reproducing every number in FINAL_REPORT.md

macOS, Apple Silicon (arm64), Apple Clang. Every command below is macOS-native:
no MSYS2, no Windows GCC, no `C:\` paths.

```bash
cd ~/TokenLIKE-Experiments/fsst-like
export ORIG=~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE   # read-only, never written
export PY=$ORIG/.venv/bin/python          # duckdb 1.5.6; any python3 with duckdb works
```

## Prerequisites — all of these were already present; nothing was installed

```bash
uname -m                                   # arm64
sysctl -n hw.optional.arm64                # 1
sysctl -n sysctl.proc_translated           # 0  (native, not Rosetta)
clang++ --version                          # Apple clang 21.0.0, arm64-apple-darwin
xcode-select -p                             # /Library/Developer/CommandLineTools
pkgutil --pkg-info=com.apple.pkg.CLTools_Executables
brew --version && cmake --version && ninja --version && git --version && python3 -V
```

If any were missing on another Mac, the minimal set is:

```bash
xcode-select --install                     # Apple Clang + SDK; nothing else is required
brew install cmake ninja                   # Ninja is optional; CMake can emit Makefiles
$PY -m pip install 'duckdb==1.5.6'         # only needed for the DuckDB ground truth
```

Homebrew LLVM is **not** needed — Apple Clang compiles this experiment and the
FSST snapshot without changes. Nothing here uses `-march=native` on arm64 except
upstream FSST's own CMake, which probes for it and is unaffected either way.

## 0. Dependencies

```bash
# FSST, pinned. Already present in third_party/fsst; this is how it got there.
git clone https://github.com/cwida/fsst.git third_party/fsst
git -C third_party/fsst checkout e638d4cf8c26129d73c242a4127b42b975de5b63

# The published prior-art implementation, for inspection only (see §K).
git clone --depth 1 https://github.com/calin2110/FSST-LIKE-Matching.git \
    third_party/priorart-fsst-like
```

## 1. Build

```bash
cmake -G Ninja -S . -B build/rel  -DCMAKE_BUILD_TYPE=Release                      && cmake --build build/rel
cmake -G Ninja -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DFSSTLIKE_SANITIZE=ON && cmake --build build/asan
```

Drop `-G Ninja` to use Makefiles instead; add `-j8` in that case. Both produce
`Mach-O 64-bit executable arm64` — check with `file build/rel/fsst_like`.

Upstream FSST compiled cleanly with its own CMake too, as a cross-check that the
snapshot is intact:

```bash
cmake -G Ninja -S third_party/fsst -B build/fsst -DCMAKE_BUILD_TYPE=Release && cmake --build build/fsst --target fsst
```

## 2. Compress the columns

```bash
TOKENLIKE_DIR=$ORIG bash scripts/build_columns.sh
```

Each column is round-trip verified row by row inside `fsst_encode`; it refuses
to write anything if a single row fails. If the TokenLIKE datasets are absent,
regenerate them with that project's own documented pipeline
(`scripts/encode/build_vocab.py`, `fetch_clickbench.py`, `gen_tpch.py`,
`verify/fuzz.py --edge`) — this experiment does not reimplement it.

## 3. Correctness

```bash
bash scripts/check_all.sh                     # everything, one command
$PY scripts/truth.py --semantics              # the DuckDB LIKE facts we rely on
$PY scripts/check.py patterns/core.txt        # one list, all six modes
build/rel/test_unit                           # 4,725 in-process checks
```

Under sanitizers:

```bash
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS="print_stacktrace=1:suppressions=$PWD/tests/ubsan.supp"
build/asan/test_unit
for m in raw ref decode fsst fsst-compact fsst-rev; do
  build/asan/fsst_like --mode $m --col data/edge/values --pattern '%_a_%' --validate >/dev/null
done
# A bounded sweep; the whole gate under sanitizers takes hours at -O0.
FSSTLIKE_CLI=build/asan/fsst_like $PY scripts/check.py patterns/edge.txt
for m in raw ref decode fsst fsst-compact fsst-rev; do
  build/asan/fsst_like --mode $m --col data/clickbench/URL_1m --pattern '%/v_/%' --validate --runs 1 --json
done
```

## 4. Benchmarks

```bash
$PY scripts/bench.py --patterns patterns/bench.txt  --runs 7 --warmup 2 --rss --out bench/bench.json
$PY scripts/bench.py --patterns patterns/scales.txt --runs 7 --warmup 2       --out bench/scales.json
$PY scripts/report.py  bench/bench.json  --title "Per-pattern performance, 1,000,000 rows" > bench/bench_tables.md
$PY scripts/analyze.py                                                 # -> bench/tables.md
```

## 5. Same-machine comparison with the original TokenLIKE

`git archive` reads the object database and writes nothing to the repository —
no worktree, no branch, no tag, no fetch:

```bash
mkdir -p third_party/tokenlike-snapshot
git -C $ORIG archive origin/main | tar -x -C third_party/tokenlike-snapshot
cmake -G Ninja -S third_party/tokenlike-snapshot -B build/tokenlike -DCMAKE_BUILD_TYPE=Release
cmake --build build/tokenlike
$PY scripts/compare_tokenlike.py --out bench/vs_tokenlike.json
```

The configure step must print `loaders (Person B) = ON, matcher (Person A) = ON`.
Anything else means the comparison ran against fixtures, not the real matcher.

## 6. Demo

```bash
$PY demo_fsst.py --rows 100000
```

## 7. Confirm the original repository is untouched

```bash
git -C $ORIG rev-parse --abbrev-ref HEAD                 # b/data
git -C $ORIG rev-parse HEAD                              # 777b608779da48c1ca73ee73cc21be92fc92c82f
git -C $ORIG status --porcelain=v1 | wc -l               # 0
git -C $ORIG ls-files -s | awk '{print $2, $4}' | sort -k2 | shasum -a 256
#   73e01ffd605b11fd0b13186971e722f3e5d0242b8f3a0ffcb1cd0057b0f183d2
git -C $ORIG for-each-ref                                # 8 refs, none new
# Use an explicit timestamp: on macOS `find` may be `bfs`, which rejects
# relative ones and then prints nothing, which reads like a pass.
find $ORIG -newermt 2026-10-25T00:00:00                  # only ./.git (dir mtime)
find $ORIG/.git -newermt 2026-10-25T00:00:00             # only ./.git itself
find $ORIG/.git/objects -type f -newermt 2026-10-25T00:00:00 | wc -l   # 0
```
