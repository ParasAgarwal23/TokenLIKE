# Reproducing every number in FINAL_REPORT.md

These commands are for the **integrated** repository, where the module lives in
`integration/fsst/` and is built from the repository root with
`-DTOKENLIKE_WITH_FSST=ON`. The standalone experiment this module came from had
its own layout (`~/TokenLIKE-Experiments/fsst-like`, `build/rel`, `build/asan`,
a `test_unit` binary); the commands that produced the v1 numbers are recorded
with those numbers in `bench/v1-experiment-standalone/PROVENANCE.md`.

macOS, Apple Silicon (arm64), Apple Clang. Every command below is macOS-native:
no MSYS2, no Windows GCC, no `C:\` paths. Windows (mingw-w64) build notes are in
`../INTEGRATION_HANDOFF.md`.

```bash
cd <repo root>                                   # not integration/fsst
export ORIG=~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE   # read-only, never written
export PY=$ORIG/.venv/bin/python          # duckdb 1.5.6; any python3 with duckdb works
```

`$ORIG` supplies the raw columns only. If you do not have that checkout, set
`TOKENLIKE_DIR` to any tree holding the TokenLIKE `data/` layout, or regenerate
it with that project's own pipeline (`scripts/encode/build_vocab.py`,
`fetch_clickbench.py`, `gen_tpch.py`, `verify/fuzz.py --edge`) — this module
does not reimplement it.

## Prerequisites — all of these were already present; nothing was installed

```bash
uname -m                                   # arm64
sysctl -n hw.optional.arm64                # 1
sysctl -n sysctl.proc_translated           # 0  (native, not Rosetta)
clang++ --version                          # Apple clang 21.0.0, arm64-apple-darwin
xcode-select -p                             # /Library/Developer/CommandLineTools
brew --version && cmake --version && ninja --version && git --version && python3 -V
```

If any were missing on another Mac, the minimal set is:

```bash
xcode-select --install                     # Apple Clang + SDK; nothing else is required
brew install cmake ninja                   # Ninja is optional; CMake can emit Makefiles
$PY -m pip install 'duckdb==1.5.6'         # only needed for the DuckDB ground truth
```

Homebrew LLVM is **not** needed — Apple Clang compiles this module and the FSST
snapshot without changes.

## 0. Dependencies

```bash
bash integration/fsst/scripts/fetch_fsst.sh      # FSST, pinned; optional
```

Without it, configuring with `-DTOKENLIKE_WITH_FSST=ON` clones the same pinned
revision itself. Either way the snapshot lands in
`integration/fsst/third_party/fsst` at
`e638d4cf8c26129d73c242a4127b42b975de5b63`, and upstream's own CMakeLists is
never executed (see `../THIRD_PARTY.md` for why).

```bash
# The published prior-art implementation, for inspection only (see §K).
git clone --depth 1 https://github.com/calin2110/FSST-LIKE-Matching.git \
    integration/fsst/third_party/priorart-fsst-like
```

## 1. Build

Release, and a second tree with AddressSanitizer + UndefinedBehaviorSanitizer:

```bash
cmake -G Ninja -S . -B build/fsst -DCMAKE_BUILD_TYPE=Release -DTOKENLIKE_WITH_FSST=ON
cmake --build build/fsst                   # 0 warnings

cmake -G Ninja -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DTOKENLIKE_WITH_FSST=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/asan
```

There is no module-specific sanitizer option: the flags above sanitize the whole
build, module and core alike. Drop `-G Ninja` to use Makefiles instead; add
`-j8` in that case. Both trees produce `Mach-O 64-bit executable arm64` — check
with `file build/fsst/fsst_like`.

The default build must stay free of FSST entirely:

```bash
cmake -G Ninja -S . -B build/default -DCMAKE_BUILD_TYPE=Release && cmake --build build/default
nm -gU build/default/libtokenlike.a | grep -ci fsst      # 0
(cd build/default && ctest)                              # all pass, no fsst_unit
```

Upstream FSST also compiles with its own CMake, as a cross-check that the
snapshot is intact (use a build directory of its own):

```bash
cmake -G Ninja -S integration/fsst/third_party/fsst -B build/fsst-upstream \
  -DCMAKE_BUILD_TYPE=Release && cmake --build build/fsst-upstream --target fsst
```

## 2. Compress the columns

```bash
cd integration/fsst
TOKENLIKE_DIR=$ORIG bash scripts/build_columns.sh
```

Each column is round-trip verified row by row inside `fsst_encode`; it refuses
to write anything if a single row fails. Re-encoding an existing column
reproduces it byte for byte, which is worth checking after any change to the
encoder:

```bash
../../build/fsst/fsst_encode --raw $ORIG/data/edge/values --out /tmp/values
cmp data/edge/values.fsst.cmp /tmp/values.fsst.cmp && echo identical
```

## 3. Correctness

All paths below are relative to `integration/fsst`.

```bash
bash scripts/check_all.sh                     # everything, one command (~110 s)
$PY scripts/truth.py --semantics              # the DuckDB LIKE facts we rely on
$PY scripts/check.py patterns/core.txt        # one list, all eight modes
$PY scripts/check.py patterns/core.txt --force   # re-derive the oracle from DuckDB
../../build/fsst/fsst_test_unit               # 6,429 in-process checks
```

`check.py` caches the DuckDB ground truth, keyed by a fingerprint of the column
and the pattern, so a re-run compares against a cached oracle. `--force`
re-queries DuckDB; use it when the point is to verify the oracle too.

Under sanitizers:

```bash
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:suppressions=$PWD/tests/ubsan.supp"
../../build/asan/fsst_test_unit
for col in data/edge/values data/clickbench/URL_100k data/tpch/o_comment_1m; do
  for m in raw raw-opt ref decode fsst fsst-compact fsst-rev fsst-rev2; do
    for p in '%/v_/%' 'http://%' '%.php'; do
      ../../build/asan/fsst_like --mode $m --col $col --pattern "$p" --validate --runs 1 >/dev/null || echo "FAILED $m $col $p"
    done
  done
done
# A bounded sweep; the whole gate under sanitizers takes hours at -O0.
FSSTLIKE_CLI=../../build/asan/fsst_like $PY scripts/check.py patterns/edge.txt
```

`tests/ubsan.supp` suppresses one upstream check only — see §11 of
`../FINAL_REPORT.md`. Drop the `suppressions=` setting to see the upstream
reports themselves; `halt_on_error=1` makes any report in this module's own code
a hard failure.

## 4. Benchmarks

New measurements go in a new directory; `bench/v1-experiment-standalone/` and
`bench/v2-integration/` are evidence, not scratch space.

```bash
$PY scripts/bench.py --patterns patterns/bench.txt  --runs 7 --warmup 2 --rss --out bench/<new>/bench.json
$PY scripts/bench.py --patterns patterns/scales.txt --runs 7 --warmup 2       --out bench/<new>/scales.json
$PY scripts/report.py  bench/<new>/bench.json  --title "Per-pattern performance, 1,000,000 rows" > bench/<new>/bench_tables.md
$PY scripts/analyze.py                                                 # -> bench/v2-integration/tables.md
$PY scripts/storage.py                                                 # -> bench/v2-integration/storage.md
```

## 5. Same-machine comparison with the original TokenLIKE

The integrated repository builds the TokenLIKE CLI itself, so no snapshot is
needed — point the comparison at the same build directory:

```bash
$PY scripts/compare_tokenlike.py --tl-cli ../../build/fsst/tokenlike \
    --tokenlike $ORIG --out bench/<new>/vs_tokenlike.json
```

The configure step must have printed `loaders (Person B) = ON, matcher
(Person A) = ON`. Anything else means the comparison ran against fixtures, not
the real matcher.

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
find $ORIG/.git/objects -type f -newermt 2026-10-25T00:00:00 | wc -l   # 0
```

If the datasets are read straight out of `$ORIG/data`, use an APFS clone
(`cp -Rc $ORIG/data ./data`) rather than writing ground truth back into that
checkout: the clone costs no space and is copy-on-write, so the original files
stay untouched.
