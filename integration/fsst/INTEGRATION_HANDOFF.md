# FSST module — handoff to Person C

**Branch** `feature/fsst-like`
**Code commit** `b013fb90ed44834ef3cef19c64856b33d5606aa0`
**Base** `main` at `c58d066ccaa2cf96a7f96efeab9e4d5773994b94` = `v3-freeze`
**Not merged.** Only you merge into `main`. Nothing was force-pushed, rebased
or rewritten, and the frozen tag is untouched (verified below).

The branch head is the commit that adds this file; `git log -2` shows both.

---

## 1. What this is, in one paragraph

An **optional** module, `integration/fsst/`, that evaluates SQL `LIKE`
directly on FSST-compressed strings by lifting a byte-level `LIKE` automaton
onto the FSST symbol alphabet — the same idea as the token-table matcher, applied
to a different compressor. It is a research experiment, it is **off by default**,
and the technique is **not novel** (published at DaMoN'26; see
`FINAL_REPORT.md` §K). The genuinely new part is `_` support in the
compressed domain.

## 2. The one change outside integration/fsst/

Two files, both minimal:

* `CMakeLists.txt` — an 11-line opt-in block appended before the CLI smoke
  tests:

  ```cmake
  option(TOKENLIKE_WITH_FSST "Build the optional FSST-compressed LIKE module" OFF)
  if(TOKENLIKE_WITH_FSST)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/fsst/CMakeLists.txt)
      add_subdirectory(integration/fsst)
    else()
      message(FATAL_ERROR "TOKENLIKE_WITH_FSST=ON but integration/fsst/ is not in this tree")
    endif()
  endif()
  ```

* `.gitignore` — two lines, `integration/fsst/third_party/` and
  `integration/fsst/data/`.

**Nothing else is touched.** No existing source file, header, script, pattern
file or test is modified or renamed. If you would rather not take the root
`CMakeLists.txt` change, the module still builds standalone — but then
`add_subdirectory` has to come from somewhere, so this is the smallest hook I
could find. Your call.

### Proof the default build is unchanged

```bash
cmake -G Ninja -S . -B build/default -DCMAKE_BUILD_TYPE=Release
cmake --build build/default
ctest --test-dir build/default --output-on-failure
```

Measured on this branch: configure prints only
`TokenLIKE: loaders (Person B) = ON, matcher (Person A) = ON` (no FSST line),
the same six targets are produced (`libtokenlike.a`,
`libtokenlike_fixtures.a`, `tokenlike`, `test_baselines`, `test_matcher`,
`test_io`), `nm -gU build/default/tokenlike | grep -ci fsst` is **0**, and
**ctest is 5/5**.

## 3. Upstream FSST

| | |
|---|---|
| upstream | <https://github.com/cwida/fsst> |
| **pinned commit** | `e638d4cf8c26129d73c242a4127b42b975de5b63` (2026-04-07) |
| licence | **MIT**, Copyright 2018-2020 CWI / TU Munich / FSU Jena |
| vendored? | **no** — nothing of FSST is committed to this repository |

The pin lives in exactly one place, `integration/fsst/CMakeLists.txt`
(`FSST_PINNED_COMMIT`). The module finds the source in this order:
`-DTOKENLIKE_FSST_SOURCE_DIR=<path>`; then
`integration/fsst/third_party/fsst` (what `scripts/fetch_fsst.sh` creates,
gitignored); then `FetchContent` from GitHub at that commit. Only
`libfsst.cpp` and `fsst_avx512.cpp` are compiled, **unmodified**.

Full notice and the one upstream UBSan finding: `integration/fsst/THIRD_PARTY.md`.
**Anyone shipping a binary built with the module must carry upstream's MIT
notice.** Nothing in the module's own code is licensed differently from the rest
of this repository.

## 4. File list

```
CMakeLists.txt                          (modified: 11-line opt-in block)
.gitignore                              (modified: 2 lines)

integration/fsst/
  CMakeLists.txt                        module build, FSST discovery, targets
  README.md                             what it is, how to build and run
  INTEGRATION_HANDOFF.md                this file
  THIRD_PARTY.md                        FSST pin, licence, prior art
  FINAL_REPORT.md                       results (v2)
  demo_fsst.py                          runnable end-to-end demonstration
  docs/REPRODUCE.md                     every command, macOS
  include/fsstlike/
    common.h  pattern.h  dfa.h  fsstcol.h  lift.h  rawopt.h  scan.h
  src/
    common.cpp  pattern.cpp  dfa.cpp  fsstcol.cpp  lift.cpp  rawopt.cpp  scan.cpp
    cli.cpp         -> fsst_like
    encode.cpp      -> fsst_encode
    liftbench.cpp   -> fsst_liftbench
  tests/
    test_unit.cpp   -> fsst_test_unit   (CTest name: fsst_unit)
    ubsan.supp
  scripts/
    fetch_fsst.sh  build_columns.sh  check_all.sh
    common.py  truth.py  check.py  bench.py  report.py  analyze.py
    storage.py  compare_tokenlike.py
  patterns/
    core.txt  wildcard.txt  concat.txt  bench.txt  scales.txt  edge.txt  fuzz.txt
  bench/v1-experiment-standalone/       prior results, PROVENANCE.md, FINAL_REPORT_v1.md
  bench/v2-integration/                 results measured in this module
  third_party/fsst/                     GITIGNORED (fetched)
  data/                                 GITIGNORED (compressed columns, ground truth)
```

Targets added: `fsst_like`, `fsst_encode`, `fsst_liftbench`,
`fsst_test_unit`. One CTest case added: `fsst_unit`. **No existing target or
test name is reused or shadowed.**

## 5. Supported platforms

| platform | state |
|---|---|
| **macOS arm64 (Apple Silicon)** | **fully tested** — Apple clang 21.0.0, CMake 4.4.4 + Ninja, Release and ASan/UBSan |
| macOS x86_64 | expected to work; `fsst_avx512.cpp` would take its AVX-512 path only on hardware that has it, and FSST's own runtime check handles that. Not tested here. |
| Linux x86_64 / arm64 | expected to work. `memmem` is used under `_GNU_SOURCE` (libstdc++ defines it); a portable fallback is compiled otherwise. Not tested here. |
| **Windows / MSVC** | **not tested.** See §9. |

C++17, no dependencies beyond the standard library and the FSST snapshot.
Python tooling needs only `duckdb==1.5.6` and is used for ground truth and
benchmarks, never for the build.

## 6. Build and run on macOS

```bash
cd <repo>
export TOKENLIKE_DIR=~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE   # read-only, for datasets
export PY=$TOKENLIKE_DIR/.venv/bin/python

bash integration/fsst/scripts/fetch_fsst.sh                 # optional; CMake can FetchContent
cmake -G Ninja -S . -B build/fsst -DCMAKE_BUILD_TYPE=Release -DTOKENLIKE_WITH_FSST=ON
cmake --build build/fsst                                    # 0 warnings

cd integration/fsst
bash scripts/build_columns.sh                               # compress the columns (~1 min)
B=../../build/fsst

$B/fsst_like --mode fsst      --col data/clickbench/URL_1m --pattern 'http://%' --json
$B/fsst_like --mode fsst-rev2 --col data/clickbench/URL_1m --pattern '%.php'    --json
$B/fsst_like --mode raw-opt   --col data/clickbench/URL_1m --pattern '%.php'    --json
$PY demo_fsst.py --rows 100000
```

Drop `-G Ninja` for Makefiles and add `-j8`. Verify everything:

```bash
cd integration/fsst && bash scripts/check_all.sh            # ~110 s
```

### Modes

| mode | what it does |
|---|---|
| `raw` | byte DFA over the uncompressed strings |
| `raw-opt` | optimised raw matcher — the fair baseline |
| `ref` | backtracking reference matcher (test oracle, slow) |
| `decode` | `fsst_decompress()` every row, then the byte DFA |
| `fsst` | lifted DFA over compressed codes, dense table, forward |
| `fsst-compact` | same with a default+exception table (measured slower; kept for the comparison) |
| `fsst-rev` | lifted reverse DFA using the `.fsst.rev` sidecar |
| **`fsst-rev2`** | lifted reverse DFA, **no sidecar** — the recommended reverse mode |

Exit codes: 0 ok, 1 error, **2 = this build does not support the request** (never
a silent fallback to decoding).

## 7. Tests and results

| | |
|---|---|
| repository **without** the module | same six targets, 0 FSST symbols, **ctest 5/5** |
| in-process unit / fuzz / adversarial | **6,429 checks, 0 failures** |
| reverse-parse equivalence | **108,598 rows, 26,099 malformed, full agreement** |
| DuckDB, 8 modes, up to 1M rows | **3,367 passed, 0 failed, 33 skipped** (425 patterns) |
| FSST round-trip | **4,332,067 rows, 267,373,051 bytes, 0 mismatches** |
| ASan + UBSan | unit suite + 24 whole-column runs, **0 reports** |
| vs this branch's `tokenlike` CLI | **15/15 patterns, identical row ids** across 9 mode combinations |

The 33 skips are `--col2` with `fsst-compact`/`fsst-rev`/`fsst-rev2`,
which exit 2. **No pattern is unsupported**; concatenation is verified in the
compressed domain through `fsst`.

## 8. Compression and performance, measured here

Apple M2, macOS 26.5.2 arm64, Apple clang 21.0.0 `-O3`, single-threaded, warm
page cache, best of 7 runs after 2 warmups. **Do not mix these with numbers
from any other machine.**

Storage, every byte a reader keeps, `u32` row offsets, no sidecar:

| column | rows | raw | FSST total | ratio | token-table total | FSST / token |
|---|---|---|---|---|---|---|
| `clickbench/URL` | 1,000,000 | 88,562,192 | 53,965,609 | 1.641x | 76,272,110 | **1.413x smaller** |
| `clickbench/Referer` | 1,000,000 | 79,583,339 | 50,113,308 | 1.588x | 69,951,756 | **1.396x** |
| `tpch/l_comment` | 1,000,000 | 26,511,095 | 13,321,798 | 1.990x | 17,167,382 | **1.289x** |
| `tpch/o_comment` | 1,000,000 | 48,529,673 | 19,162,103 | 2.533x | 26,274,904 | **1.371x** |

Performance headlines (full table in `FINAL_REPORT.md` §I):

* **versus full decode: 2.64x – 81.3x**, never slower. This is the result that
  matters: decode-then-match is slower than *not compressing at all* on every
  pattern measured.
* **versus a fair raw baseline: 0.36x – 3.10x**, losing on 3 of 15 patterns.
  v1's "up to 43.7x over raw" was an artifact of an unfair baseline and is
  withdrawn.
* automaton build 68 µs – 1.15 ms; lifted table 4.0 – 66.1 KiB; peak RSS ~37 MiB
  *below* the raw baselines at 1M rows.
* one automaton reused across 6 symbol tables: **5.28x less build work**,
  bit-identical results.

## 9. Known limitations

1. **Windows / MSVC is untested.** Likely needed: `memmem` has no MSVC
   equivalent — `src/rawopt.cpp` already compiles a portable fallback when
   neither Apple/BSD nor `_GNU_SOURCE` is detected, so this should just work,
   but it has not been run. The module uses `/W4` under MSVC. FSST's own
   `fsst.h` has an `_MSC_VER` block, so upstream expects MSVC. Treat the
   module as macOS/Linux-only until someone builds it on Windows.
2. **`--col2` is not implemented for `fsst-compact`, `fsst-rev`,
   `fsst-rev2`** — exit 2.
3. **No threads, no SIMD, no JIT.** The published prior art has all three and is
   faster; §K is explicit about it.
4. **No SQL / DuckDB integration.** Out of scope here.
5. **Patterns over 20,000 DFA states are refused** (hard ceiling 65,535, because
   state ids are `uint16`).
6. **`ESCAPE` clauses are not parsed.** DuckDB's default — backslash is an
   ordinary byte — is what is implemented and verified.
7. **`_` assumes well-formed UTF-8 input**, which the datasets are by
   construction. The sidecar-free reverse parse assumes nothing of the kind.
8. **`fsst-rev2` is ~20% slower than `fsst-rev` on full reverse traversals**
   of unanchored patterns, where reverse scanning is the wrong plan anyway. It is
   faster on suffixes and costs no storage.
9. **The benchmark data is not in the repository** (gitignored, ~450 MB).
   `scripts/build_columns.sh` regenerates it byte-deterministically from the
   original project's raw columns.

## 10. Requests for Person C

1. **Please review the root `CMakeLists.txt` hook** (§2). It is 11 lines and
   opt-in, but it is your file and I did not want to add it silently.
2. **Decide whether the module belongs on `main` at all.** It is a research
   experiment, the technique is published prior art, and it adds a third-party
   dependency behind a flag. A reasonable call is to keep `feature/fsst-like`
   unmerged as an experiment branch and reference it from `STATUS.md`. I have
   no stake in which.
3. **If you do merge:** please keep it `OFF` by default and do not add it to
   any CI or demo path that must work without network access, since an
   unprimed build uses `FetchContent`.
4. **I did not touch `STATUS.md` or `README.md`.** If the module should be
   mentioned there, tell me the wording you want and I will add it on this
   branch, or add it yourself at merge time.
5. **The encoder still writes `.fsst.rev`** so the sidecar comparison stays
   reproducible. If you want the module leaner, `fsst_encode --no-rev` skips
   it and `fsst-rev2` does not need it; say the word and I will flip the
   default.
6. **Nothing here blocks you.** The module is additive, the default build is
   proven unchanged, and `v3-freeze` is untouched.

## 11. Original repository and frozen tag, verified unchanged

The separate working checkout at `~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE`
was used **read-only** for datasets and was never fetched into, committed to, or
modified. All integration work happened in a **fresh clone** at
`~/TokenLIKE-Integration/TokenLIKE`.

```
main        c58d066ccaa2cf96a7f96efeab9e4d5773994b94   unchanged
v3-freeze   7fa32c5c6ca42254b462e14b1469f4447b7745ee -> c58d066...   unchanged
main tree   577e54c4dd7bf9913f541dc8dded6c25e385006e   unchanged
```

Only `feature/fsst-like` was created and pushed. No branch was deleted, no tag
created or moved, no history rewritten, no force-push.
