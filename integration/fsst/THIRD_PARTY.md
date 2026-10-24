# Third-party code used by the FSST module

## FSST — Fast Static Symbol Table

| | |
|---|---|
| upstream | <https://github.com/cwida/fsst> |
| **pinned commit** | `e638d4cf8c26129d73c242a4127b42b975de5b63` |
| commit date | 2026-04-07 |
| internal format id | `FSST_VERSION` = `20190218` (`libfsst.hpp`) |
| licence | **MIT** — Copyright 2018-2020, CWI, TU Munich, FSU Jena |
| authors | Peter Boncz, Viktor Leis, Thomas Neumann |
| paper | *FSST: Fast Random Access String Compression*, PVLDB 13(12), <https://www.vldb.org/pvldb/vol13/p2649-boncz.pdf> |

**Not vendored.** No FSST file is committed to this repository. The pin lives in
one place, `integration/fsst/CMakeLists.txt` (`FSST_PINNED_COMMIT`), and the
module finds the source in this order:

1. `-DTOKENLIKE_FSST_SOURCE_DIR=<path>` — an existing checkout.
2. `integration/fsst/third_party/fsst` — what `scripts/fetch_fsst.sh` creates.
   Gitignored.
3. a direct `git clone` of the pinned commit into (2), done by the module's
   CMake at configure time.

**Why not `FetchContent_MakeAvailable`.** It runs upstream's own
`CMakeLists.txt`, which declares an `fsst12` target — and upstream's
`libfsst12.cpp` / `fsst12.h` **do not compile with Apple clang on arm64**:
`write_two_codes_aligned()` and `write_one_code_aligned()` are declared taking
`unsigned long*` and called with `unsigned long long*` (`fsst12.h:125`, `:138`
against `:157`). A bare `clang++ -std=c++17 -c libfsst12.cpp` fails the same
way, so it is an upstream bug and not an artifact of this build; `libfsst.cpp`,
which is what this module uses, compiles clean. `fsst12` is the 12-bit-code
variant and nothing here touches it, but `MakeAvailable` would add it to the
default build target and break `cmake --build`. So the module clones the pinned
revision itself and compiles only the two translation units it needs. This was
caught by building a fresh clone of the pushed branch, not by the in-place
build, which already had a snapshot on disk.

Only two upstream translation units are compiled, **unmodified**:
`libfsst.cpp` and `fsst_avx512.cpp`. The latter carries its own runtime CPU
check and compiles to a stub on non-x86 hosts, which is why the module builds on
Apple Silicon without any porting.

Upstream's MIT licence requires its copyright notice and permission notice to be
kept with copies of the software. Since nothing is copied into this repository,
that notice travels with the fetched checkout
(`integration/fsst/third_party/fsst/LICENSE`). Anyone redistributing a *binary*
built with the module must include it.

### One upstream finding, reported and not patched

UndefinedBehaviorSanitizer flags ten misaligned 8-byte stores inside
`fsst_decompress`, from

```c
#define FSST_UNALIGNED_STORE(dst,src) memcpy((unsigned long long*) (dst), &(src), sizeof(unsigned long long))
```

(`fsst.h:156`, expanded at lines 163-201). The `memcpy` is byte-wise and safe;
the cast of an unaligned destination to `unsigned long long*` is what UBSan
objects to. It is deliberate upstream behaviour — FSST ships
`-DFSST_MUST_ALIGN` for platforms that cannot take unaligned access — so the
snapshot is left alone and `tests/ubsan.supp` suppresses the `alignment` check
for `third_party` only. With that suppression the sanitizer run is completely
clean, so nothing of this module's own is hidden by it.

## Prior art this module reproduces

Compressed-domain FSST `LIKE` is **published work**, not an invention of this
module. See `FINAL_REPORT.md` §K. In short:

* Pop, Riedl, Neumann, *Compression-Aware LIKE: Matching Patterns in the FSST
  Domain*, **DaMoN'26**, <https://doi.org/10.1145/3789237.3809128>. Reference
  implementation `calin2110/FSST-LIKE-Matching` (MIT), inspected at `b1eb3ab`.
  Not vendored, not linked, not built — read only, to establish what is prior
  art and what is not.
* Vortex (`vortex-data/vortex`), crate `vortex-fsst`: FSST `LIKE` pushdown for
  `prefix%` and `%needle%`, plus `%suffix` in PR #9728.
