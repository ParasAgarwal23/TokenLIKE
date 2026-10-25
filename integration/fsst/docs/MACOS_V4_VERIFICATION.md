# macOS verification of the v4 integration

Person B's re-verification of the **integrated** FSST module — not the
standalone experiment, and not `feature/fsst-like` — after Person C merged it
into `main` and patched it for Windows.

| | |
|---|---|
| commit verified | `0eab88d16cb7085c0a5543ac74546ce8d0cdf7dc` (`main`, `integration/sql-fsst-v4`, tag `v4-integrated` → `bd03c47`) |
| date | 2026-10-25 |
| verified by | Person B, on the machine below |
| outcome | every gate passes; two defects found and fixed in B-owned files (§5) |

Nothing in `v3-freeze` (`7fa32c5` → `c58d066`) or in the original working
checkout was touched; see §7.

## 1. Platform

| | |
|---|---|
| machine | Apple M2 (`arm64`, T8112), 8 cores, 16 GiB — native, not Rosetta |
| OS | macOS 26.5.2 (build 25F84), Darwin 25.5.0 |
| compiler | Apple clang 21.0.0 (`clang-2100.1.1.101`), target `arm64-apple-darwin25.5.0` |
| build | CMake 4.4.4, Ninja 1.13.2 |
| Python | 3.14.7 |
| DuckDB | 1.5.6 — Python module for the FSST scripts, pinned CLI `v1.5.6 069cc9f9b5` for the core scripts |
| git | 2.53.0 |

Person C measured the same commit on Windows 11, i5-1334U, mingw-w64 GCC 14.2.
Timings from the two machines are not comparable; the pass/fail counts are.

## 2. Builds

| configuration | result |
|---|---|
| `-DTOKENLIKE_WITH_FSST=OFF` (default) | configures and builds clean; `nm -gU` finds **0 FSST symbols** in `libtokenlike.a` and in the `tokenlike` CLI; **ctest 16/16** |
| `-DTOKENLIKE_WITH_FSST=ON`, Release | **0 warnings**; uses the local snapshot at `third_party/fsst` @ `e638d4c`; **ctest 17/17** (adds `fsst_unit`) |
| ASan + UBSan (`Debug` + `-fsanitize=address,undefined`) | builds clean; see §4 |

`ctest` counts depend on the datasets: `io_clickbench_url` only registers when
`data/vocab.bin` and `data/clickbench/URL.tok` exist. Without them the counts
are 15 and 16. With them they are 16 and 17, which matches Person C's 17/17 on
Windows.

Both `if(MINGW)` blocks Person C added to the root `CMakeLists.txt` are inert
here: the macOS configure output is unchanged and the `RC` language is never
enabled.

## 3. Correctness

| suite | result |
|---|---|
| `fsst_test_unit`, Release | **6,429 checks, 0 failures** |
| — reverse unit parsing, forward vs sidecar-free reverse | **108,598 rows, 26,099 of them malformed, both parsers agreed** (including on *which* rows are malformed) |
| — fixture cross-product | 35 values × 47 patterns × 7 modes, 82 symbols |
| — lift reuse | one byte DFA over symbol tables of 82, 54 and 56 symbols |
| structural validation (`--validate`) | **12 columns**: offsets, code range, escapes and `.fsst.rev` all consistent |
| DuckDB LIKE semantics probes | all as expected |
| **pattern gate, 8 modes, oracle re-derived** | **3,367 passed, 0 failed, 33 skipped** |
| cross-check vs this tree's `tokenlike` CLI | **15/15 patterns, identical row ids** |
| FSST round-trip, every row | **12 columns, 4,332,067 rows, 267,373,051 raw bytes, 0 mismatches** |
| core TokenLIKE gate (FSST off) | `check_all: ALL CHECKS PASSED` — 22 checks, **282 pattern checks, 0 failed, 0 skipped**, 8 token round-trips over 4,121,067 rows, `test_io` on every column |

Pattern gate by file: `edge` 360, `core` 240, `wildcard` 184, `concat` 55
(+33 skipped), `scales` 128, `fuzz` 2,400.

The 33 skips are `--col2` with `fsst-compact`, `fsst-rev` and `fsst-rev2`, which
exit 2 rather than fall back. No pattern is unsupported; concatenation is
verified in the compressed domain through `fsst`.

**The oracle was re-derived, not reused.** `check.py` caches DuckDB ground truth
keyed by a fingerprint of the column and pattern, so a plain re-run compares
against a cached oracle. Both gates were re-run with `--force`: all **425** FSST
truth files and **97** core truth files were rewritten from DuckDB on this
machine during this verification.

## 4. Memory safety

`-fsanitize=address,undefined`, `ASAN_OPTIONS=detect_leaks=0`,
`UBSAN_OPTIONS=halt_on_error=1` plus `tests/ubsan.supp`:

| run | result |
|---|---|
| `fsst_test_unit` | 6,429 checks, 0 failures, **0 sanitizer reports** (73 s at `-O0`) |
| whole-column sweep: 3 columns × 8 modes × 3 patterns, `--validate` | **72 runs, 0 reports** |
| pattern gate on `patterns/edge.txt` through the sanitized CLI | **360 passed, 0 failed** |

The full gate under sanitizers would take hours at `-O0`; the sweep above is
bounded on purpose, and that is stated rather than implied.

## 5. Defects found and fixed here

Both are in Person B-owned files and are fixed on the branch this document
arrives on.

**`scripts/verify/check.py` — the out-of-range guard was incomplete.** Person
C's audit fix (B-5, `a11649d`) labels a row id past the end of the raw values
instead of raising `IndexError`, but the evidence printer also indexes the token
rows and, for a concatenation pattern, the second column. A row id inside the
first array and past either of the others still raised, and the raised exception
replaced the mismatch report that was being produced — the failure would be
masked by a traceback. The bound now covers every array indexed. Demonstrated
directly: with mismatched arrays, `main`'s version raises
`IndexError: list index out of range` where the patched version prints
`row 5: OUT OF RANGE (column has 3 describable rows)` and continues.

**`scripts/check_all.sh` — the gate reported `FAIL` for a working DuckDB.** The
environment probe accepted only the Python module or a CLI at `tools/duckdb`,
while `scripts/verify/common.py` also honours `TOKENLIKE_DUCKDB` and `PATH`. On
a checkout with neither the module nor `tools/duckdb` — which is exactly a fresh
integration clone — the gate printed `FAIL: no DuckDB backend available` and
exited 1 while every one of its 282 pattern checks passed against that very
backend. The probe now asks `common.find_duckdb_cli()`, the same resolver the
scripts use, and prints which CLI and version it found. Verified in both
directions: with the pinned CLI the gate now exits 0, and with no CLI reachable
the resolver returns empty and the probe still fails loudly.

**`integration/fsst/docs/REPRODUCE.md` was the standalone experiment's file.**
It told the reader to `cd ~/TokenLIKE-Experiments/fsst-like`, build `build/rel`
and `build/asan` with `-DFSSTLIKE_SANITIZE=ON` — an option that does not exist
in the integrated module — run a binary called `test_unit` with "4,725 checks",
and loop over six modes. Nobody could reproduce the sanitizer run or the gate
from the integrated tree. Rewritten for this layout, including the sanitizer
flags that actually work here; the standalone commands stay with the v1 numbers
in `bench/v1-experiment-standalone/PROVENANCE.md`.

## 6. Person C's patches, reviewed

All four `[B-patch by C]` commits and the three Windows build commits were read
and re-run here. None changes an algorithm, a file format or a test expectation,
and none has any effect on macOS behaviour beyond what is noted.

| commit | change | verdict on macOS |
|---|---|---|
| `6a8ee36` | `std::filesystem::rename` instead of `std::rename`; `create_directories` instead of `std::system("mkdir -p")` | correct. POSIX `rename` already replaced the destination, so behaviour here is unchanged; both paths are exercised by the corrupt-file fixtures in the 6,429-check suite, which passes |
| `a91e7e6` | JSON-escape paths in `fsst_encode`'s output | correct, and the one escape site covers both the stdout JSON and `.fsst.meta.json`, which share the buffer. Tested on macOS with an output directory whose name contains `\` and `"`: both documents parse as JSON |
| `460400a` | decode subprocess output as UTF-8; UTF-8 console streams; `resolve_cli` in `compare_tokenlike.py` | correct. macOS was already UTF-8 so nothing changes; the Cyrillic patterns in the gate pass as before. `resolve_cli` returns an absolute path, which is harmless here |
| `a11649d` | `scripts/verify/check.py`: skips fail the run, strictly ascending row ids, UTF-8 evidence, truncated row-id file, out-of-range row ids, columns loaded once | sound, and the stricter skip policy is right. One incomplete guard, fixed in §5. The policy does **not** break `scripts/check_all.sh`: `tier2` produces 0 skips on this tree, because the integrated CLI supports `--col2` in `token`, `decode` and `raw` |
| `4ed6bb1`, `98a1366` | MinGW: force-include `<intrin.h>`; UTF-8 `activeCodePage` manifest; `-static` | entirely inside `if(MINGW)`; verified inert here |
| `99a9528` | `demo/menu.py` absolute paths | Person C's file; the FSST demo runs here unchanged |

The encoder was re-run after `a91e7e6`: re-encoding `clickbench/URL_100k` and
`edge/values` reproduces all twelve artifacts (`.fsst.cmp`, `.fsst.sym`,
`.fsst.off`, `.fsst.rev`, `.raw`, `.rawoff`) **byte for byte**.

`FINAL_INTEGRATION_REPORT.md` line 25 still reads "C's portability patches not
yet re-run on macOS (B to confirm)". This document is that confirmation; the
line is Person C's to update.

## 7. Upstream FSST under UBSan on macOS

**They reproduce, and they are upstream's, not this module's.**

Ten distinct report sites, all in `third_party/fsst/fsst.h` — lines 163, 164,
165, 166, 170, 172, 174, 184, 186 and 195 — every one of them:

```
runtime error: store to misaligned address 0x… for type 'unsigned long long *',
which requires 8 byte alignment
    #0 fsst_decompress fsst.h:186
    #1 fsstlike::scan_decode(...) scan.cpp:143
```

All ten are expansions of one macro, `fsst.h:156`:

```c
#define FSST_UNALIGNED_STORE(dst,src) memcpy((unsigned long long*) (dst), &(src), sizeof(unsigned long long))
```

`memcpy` itself is byte-wise and safe; what UBSan objects to is the cast of an
unaligned destination to `unsigned long long*`. The macro is deliberate — it
writes an 8-byte symbol and advances by the symbol's real length, which is how
`fsst_decompress` is fast — and upstream guards it with `#ifndef FSST_MUST_ALIGN`
precisely for platforms that cannot take an unaligned store. arm64 can, so the
behaviour is correct in practice; it is formally undefined in C++, and this
module does not patch the snapshot. `tests/ubsan.supp` suppresses exactly one
class for exactly one directory:

```
alignment:*third_party/fsst/*
```

With that suppression every run above is completely clean, so any remaining
report belongs to this module's own code.

**Person C's GCC finding does not reproduce here.** Under mingw-w64 GCC 14.2
with UBSan trapping on upstream too, C hit a trap inside
`libfsst.cpp:220`/`:232` (`buildSymbolTable` → `Counters::restore1`, which
`memcpy`s into a `u8[1024]` stack buffer used as `u16` storage) that survived
`-fno-sanitize=alignment`. Under Apple clang 21 the entire encode path —
`fsst_create` and `fsst_compress` included — produces **no UBSan report at all**,
with or without suppressions. The symbol-table build is clean here; only
`fsst_decompress` reports, and only for alignment.

## 8. Original repository and frozen tag, unchanged

```
working checkout      b/data @ 777b608779da48c1ca73ee73cc21be92fc92c82f, 0 changed paths
tracked blob list     git ls-files -s | awk '{print $2, $4}' | sort -k2 | shasum -a 256
                      73e01ffd605b11fd0b13186971e722f3e5d0242b8f3a0ffcb1cd0057b0f183d2
                      -- identical to the value recorded before this module existed
refs                  8, none new; git reflog shows 0 rewrite entries
v3-freeze             7fa32c5c6ca42254b462e14b1469f4447b7745ee -> c58d066ccaa2cf96a7f96efeab9e4d5773994b94
```

All verification ran in a separate clone. The datasets were read from the
original checkout through an APFS clone (`cp -Rc`), which is copy-on-write, so
the ground truth this verification regenerated was written to the copy and not
back into that checkout.
