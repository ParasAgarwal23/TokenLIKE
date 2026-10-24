# FSST-LIKE — final report (v2, integration)

SQL `LIKE` evaluated directly on FSST-compressed strings, by lifting a
byte-level `LIKE` automaton onto the FSST symbol alphabet — the same move
TokenLIKE makes for an LLM token table.

This is **v2**. It supersedes the standalone experiment's report, which is kept
verbatim at `bench/v1-experiment-standalone/FINAL_REPORT_v1.md` together with
its measurements and a `PROVENANCE.md` saying what in it is now known to be
misleading. Nothing in v1 was edited or deleted.

## What changed in v2, and why

| | v1 claim | v2 finding |
|---|---|---|
| **raw baseline** | one generic forward byte DFA for every shape; "1.09×–43.7× faster than raw" | a review found that overstates the case, and it does. A real system answers `%.php` with a four-byte compare, not an automaton walk. v2 adds `raw-opt` — anchored compares, `memmem`, a segment walk — and against that fair baseline the result is **0.36×–3.10×**, i.e. compressed-domain matching **loses on 3 of 15 patterns**. §J. |
| **reverse-scan sidecar** | 4 bytes/row of `.fsst.rev` metadata, +8.0% storage, "the paper's approach is better and we don't match it" | the review's hypothesis that the sidecar is removable is **correct, and now proved** (§H). Mode `fsst-rev2` reverse-scans with **no metadata at all**, is *faster* on the shapes reverse scanning exists for, and never falls back. The 8% is gone. |
| **state counts** | one table showed `%.php` as "5 → 7 states", which read as minimisation *adding* states | three counts are now reported separately — subset construction, Hopcroft minimal, canonical — and forward and reverse automata are labelled. §H. |
| **compression** | payload-only ratios | full physical accounting: codes, row offsets, symbol table, sidecar, plus the same rows in the token-table representation. §F. |
| **DFA reuse** | not measured | one byte DFA lifted onto 6 symbol tables costs **5.28× less** than rebuilding per table, verified bit-identical. §G. |
| **checks** | 2,528 DuckDB checks over 6 modes | **3,367** over 8 modes, 0 failures; unit checks 4,725 → **6,429**. §D. |

Everything below was measured on this machine, in this session, by the commands
in §L. Nothing is carried over from another machine or another paper.

**Environment, as detected rather than assumed**

| | |
|---|---|
| machine | Apple M2, 8 cores, 16 GiB |
| architecture | **arm64**, native (`uname -m` = `arm64`, `hw.optional.arm64` = 1, `sysctl.proc_translated` = 0) |
| OS | macOS 26.5.2 (Darwin 25.5.0) |
| compiler | **Apple clang 21.0.0** (`clang-2100.1.1.101`), target `arm64-apple-darwin25.5.0`, Xcode CLT 26.6.0.0.1781586589, SDK 26.5 |
| build | CMake 4.4.4 + Ninja 1.13.2; `-O3 -DNDEBUG` release, `-O0 -g -fsanitize=address,undefined` for the sanitizer build |
| other | Python 3.14.7, DuckDB 1.5.6 (module and pinned CLI), git 2.53.0 |
| binaries | `Mach-O 64-bit executable arm64` |

Single-threaded throughout. **No SIMD, no JIT, no threads** — which matters when
reading §K. Homebrew LLVM is not installed and not needed; nothing was installed
on this machine for this work.

---

## A. FSST version and licence

| | |
|---|---|
| upstream | <https://github.com/cwida/fsst> |
| pinned commit | `e638d4cf8c26129d73c242a4127b42b975de5b63` (2026-04-07) |
| format id | `FSST_VERSION` = `20190218` |
| licence | MIT (CWI, TU Munich, FSU Jena) |
| vendored? | **no.** `integration/fsst/CMakeLists.txt` holds the pin; the source comes from `-DTOKENLIKE_FSST_SOURCE_DIR`, `integration/fsst/third_party/fsst`, or `FetchContent` at that commit |

`THIRD_PARTY.md` has the full notice. Only `libfsst.cpp` and
`fsst_avx512.cpp` are compiled, unmodified; the latter has a runtime CPU check
and becomes a stub on Apple Silicon, which is why no porting was needed.

Format facts this module relies on, read out of that source rather than assumed:
codes are `0..254` and **255 is the escape marker** with the next byte a literal
(`fsst.h:74`); a symbol is 1–8 bytes, packed little-endian in
`fsst_decoder_t::symbol[255]` (`fsst.h:80-85`); `fsst_export`/`fsst_import`
serialise at most `FSST_MAXHEADER` = 2066 bytes (`fsst.h:102`,
`libfsst.cpp:517-596`); the hidden symbol count in the version word and the
length histogram are cross-checked, because everything downstream depends on
"codes ≥ `nsymbols` never occur"; `fsst_import` fills unused codes with the text
`"corrupt"`, which is why that check matters; and `fsst_compress` may compress
fewer strings than asked, so the encoder loops on its return value.

Two things deliberately **not** assumed: a compressed byte is not a unit
(`<255,b>` is two bytes, one unit), and symbol boundaries are not character
boundaries — FSST routinely cuts a multi-byte UTF-8 character in half, and the
unit tests assert that this actually happens in their corpus rather than hoping
it does.

---

## B. The module, and what is in it

`integration/fsst/`, **off by default**. With `-DTOKENLIKE_WITH_FSST=OFF` the
directory is never added to the build: the repository has no FSST dependency and
`tokenlike`, `test_baselines`, `test_matcher`, `test_io` and the CTest suite
build and pass exactly as before — verified, §D.

| component | file | note |
|---|---|---|
| `LIKE` parser, DuckDB semantics | `src/pattern.cpp` | |
| independent backtracking reference matcher | `src/pattern.cpp` | the test oracle |
| Thompson NFA → subset construction → **Hopcroft** | `src/dfa.cpp` | with a state cap |
| dead-state and accept-sink analysis | `src/dfa.cpp` | drives early exit |
| FSST column format, loader, validator, **both unit parsers** | `src/fsstcol.cpp` | §H |
| encoder with mandatory per-row round-trip | `src/encode.cpp` | |
| dense and compact lifted tables, **`LiftedSet` reuse** | `src/lift.cpp` | §G |
| **optimised raw baseline** | `src/rawopt.cpp` | new in v2, §J |
| seven scan paths incl. **sidecar-free reverse** | `src/scan.cpp` | §H |
| CLI, encoder, reuse benchmark | `src/cli.cpp`, `encode.cpp`, `liftbench.cpp` | |
| unit + fuzz + adversarial tests | `tests/test_unit.cpp` | 6,429 checks |
| DuckDB ground truth, checker, benchmarks, storage accounting | `scripts/` | |

### Character semantics inside a byte automaton

`LIKE` is defined on characters; FSST symbols are arbitrary byte sequences. The
automaton is therefore built over **bytes** with the character rules pushed into
its structure: `_` expands to a nine-branch sub-automaton accepting exactly one
well-formed UTF-8 character (1–4 bytes, no surrogates, no overlongs, nothing
above U+10FFFF), and `%` expands to a self-loop over all 256 byte values.

Modelling `%` as "any bytes" is exact for well-formed UTF-8 input, not an
approximation: UTF-8 is self-synchronising, so a valid UTF-8 literal can only
align on a character boundary and the `_` sub-automaton cannot start on a
continuation byte. The DuckDB suite exercises it on 137,578 non-ASCII rows plus
Cyrillic, Japanese and emoji fixtures, so the argument is tested and not merely
argued. The argument **does** assume the data is valid UTF-8; the datasets are,
by construction (the original pipeline builds them through DuckDB's `decode()`).
Note that the sidecar-free reverse proof in §H assumes nothing of the kind.

---

## C. What comes from the original TokenLIKE design

Reused as **ideas**, re-implemented from scratch here: lifting a byte-level
`LIKE` DFA onto compression symbols; early exit through a reject sink and an
accept sink renumbered to states 0 and 1 so one comparison covers both; a
reverse automaton for suffix-anchored patterns; the dense-versus-compact table
question; and the correctness method — DuckDB as the only authority, exact
row-id set comparison, a bit-packed LSB-first bitmap, sorted `uint32` row-id
files, tab-separated pattern lists, and an "exit 2 means unsupported, never a
silent fallback" CLI contract.

Reused as **artifacts**, copied with the column names remapped: the 36
DuckDB-verified adversarial edge patterns (plus 9 FSST-specific ones), all 300
seeded fuzz patterns, and the raw datasets, read from the original `data/`
directory and copied into the module by `fsst_encode`.

**No TokenLIKE source file is compiled, linked or included by the module.**
`pattern.cpp`, `dfa.cpp`, `lift.cpp`, `rawopt.cpp`, `scan.cpp`, `fsstcol.cpp`
and the test suite are new code. The one place the project's own matcher appears
is §J, where this branch's `tokenlike` binary is run as an independent
implementation to be measured and cross-checked against.

---

## D. Test results

From the final run of `bash integration/fsst/scripts/check_all.sh`.

### The repository without the module

`-DTOKENLIKE_WITH_FSST=OFF`: the same six targets as before
(`libtokenlike.a`, `libtokenlike_fixtures.a`, `tokenlike`, `test_baselines`,
`test_matcher`, `test_io`), **zero FSST symbols** in `tokenlike`
(`nm -gU | grep -ci fsst` → 0), and **CTest 5/5 passing** (`baselines`,
`matcher`, `fixture_write`, `cli_raw`, `cli_decode`).

### In-process unit, fuzz and adversarial tests

**6,429 checks, 0 failures.** The oracle is the backtracking reference matcher,
written independently of the DFA. New in v2:

| group | what it establishes |
|---|---|
| reverse unit parsing | every string over `{0x00, 0x05, 0xFF}` up to length 10, all-`0xFF` runs up to 24 bytes, and 20,000 seeded random rows — **108,598 rows, of which 26,099 are malformed; the forward and reverse parsers agreed on every one**, including on *which* rows are malformed |
| adversarial escape columns | columns built from arbitrary valid unit sequences the real encoder would never emit — escape-heavy, full of escaped `0xFF` literals — where `fsst`, `fsst-rev`, `fsst-rev2`, `decode` and `raw` must all agree, and `fsst-rev2` must need no fallback |
| optimised raw baseline | shape classification checked against intent for 13 patterns, then `raw_match` checked against the reference for every edge pattern × value |
| lift reuse | three corpora with different symbol tables (82 / 54 / 56 symbols); a reused automaton must give both the same bitmaps **and byte-identical lifted tables** as a fresh build |

Carried over from v1: pattern parsing, DFA limits, the 35-value × 47-pattern × 7-mode
matrix with exhaustive per-(state, code) lifted-table verification, twelve
distinct malformed inputs, compressibility extremes, and two seeded random
suites. The matrix asserts that a UTF-8 character *is* split by a symbol
boundary and that the fixture *does* produce escapes, so a corpus that stopped
exercising those paths would fail rather than quietly pass.

### Against DuckDB, on real data, all eight modes

DuckDB reads the original canonical parquet and never sees anything the module
produced, so it cannot agree by construction. Row-id sets are compared exactly.

| pattern list | patterns | rows per column | checks | passed | failed | skipped |
|---|---|---|---|---|---|---|
| `patterns/edge.txt` | 45 | 51 | 360 | **360** | 0 | 0 |
| `patterns/core.txt` | 30 | 1,000,000 | 240 | **240** | 0 | 0 |
| `patterns/wildcard.txt` | 23 | 1,000,000 | 184 | **184** | 0 | 0 |
| `patterns/concat.txt` | 11 | 1,000,000 | 88 | **55** | 0 | 33 |
| `patterns/scales.txt` | 16 | 1k / 10k / 100k / 1M | 128 | **128** | 0 | 0 |
| `patterns/fuzz.txt` | 300 | 20,016 | 2,400 | **2,400** | 0 | 0 |
| **total** | **425** | | **3,400** | **3,367** | **0** | **33** |

The 33 skips are the CLI exiting 2 for `--col2` in `fsst-compact`, `fsst-rev`
and `fsst-rev2`, which do not implement concatenation (§E). Concatenation itself
*is* verified in the compressed domain: the 11 concat patterns pass in `fsst`
mode. `check.py` exits non-zero if nothing was verified, so an all-skipped run
cannot read as a pass.

**On the original pattern lists.** The brief asked how many of the project's own
core/tier1/tier2/fuzz patterns run over the FSST path. All of them do, as the
module's `core.txt` (TokenLIKE core + tier-1 boundary cases), `wildcard.txt`
(tier-2 `_` and interior `%`), `concat.txt` (tier-2 concatenation) and the
verbatim 300-pattern `fuzz.txt`. Executed: **425 patterns, 3,367 of 3,400
mode-checks**. Unsupported: **0 patterns**; 33 mode-checks skipped, all of them
the `--col2`-plus-reverse/compact combination, never a pattern the module cannot
answer.

### Structural validation and round-trip

All compressed columns pass `--validate`: monotonic offsets, `off[0] == 0`,
terminal offset equal to the payload, every code below `nsymbols`, no dangling
escape marker, and `.fsst.rev` re-derived and compared row by row.
`fsst_encode` decodes every row with upstream `fsst_decompress` and compares it
with the source before writing anything — **4,332,067 rows, 267,373,051 source
bytes, 0 mismatches** (the 11 benchmark columns plus the one `demo_fsst.py` builds); one mismatch aborts the encode and leaves no files.

### Sanitizers

AddressSanitizer + UndefinedBehaviorSanitizer, `build/fsst-asan`:
`fsst_test_unit` **6,429 checks, 0 failures, 0 reports**; then **24 runs** of
the 1,000,000-row URL column — all eight modes × three patterns
(`http://%`, `%.php`, `%/v_/%`) with `--validate` — **0 sanitizer reports in
total**.

The full 3,367-check suite was **not** run under sanitizers: at `-O0` with both
sanitizers a 1M-row scan is roughly 30× slower, putting the whole suite in the
hours. It was started, left running and abandoned on that basis. The release
build runs it in about 75 s, and the sweep above covers every code path in it.

One caveat, stated plainly: the 10 UBSan misaligned-store reports are in
**upstream** `fsst_decompress` and are suppressed for `third_party` only; see
`THIRD_PARTY.md`. With that suppression the run is completely clean, so nothing
of the module's own is hidden.

### Cross-check against a different implementation

`scripts/compare_tokenlike.py` runs the module and **this branch's own
`tokenlike` CLI** over columns derived from the same raw bytes — one source
tree, one compiler invocation, two independent matchers over two different
compression schemes. **15 of 15 patterns: all nine mode combinations returned
identical row-id sets.**

---

## E. Features that remain unsupported

Each produces an explicit error, never a silent fall back to full decoding.

| not supported | behaviour | why |
|---|---|---|
| `--col2` in `fsst-compact`, `fsst-rev`, `fsst-rev2` | exit **2**, naming the modes that do support it | not implemented. Concatenation *is* supported in the compressed domain through `fsst`, and in `raw`, `raw-opt`, `ref` and `decode`. The reverse modes would additionally need a defined scan order across the column boundary. |
| `--rev-mode bit` outside `fsst-rev` | exit **2** | the 1-bit sidecar emulation only means something for the sidecar mode; `fsst-rev2` stores nothing. |
| patterns over `--max-states` (default 20,000) | exit 1, naming the limit | deliberate. The hard ceiling is 65,535 because state ids are `uint16`; above that the build refuses rather than truncating. |
| `ESCAPE` clauses | not parsed | DuckDB without an explicit `ESCAPE` treats backslash as an ordinary byte, which is the semantics this module matches and verifies. The published prior art assumes the other convention (§K). |
| patterns that are not well-formed UTF-8 | exit 1 at parse time | DuckDB `VARCHAR` patterns always are. |
| input strings that are not well-formed UTF-8 | out of scope for `_` | the datasets are valid UTF-8 by construction. `_` is defined on characters, so there is nothing exact to match against otherwise. **The sidecar-free reverse scan does not depend on this** (§H). |
| `--affinity N` | accepted, reported as `requested-but-unsupported-on-darwin` | macOS has no `sched_setaffinity`. Reported rather than silently ignored. |
| threads, SIMD, JIT | not implemented | out of scope; §K says what it costs against the prior art. |
| DuckDB / SQL engine integration | not implemented | separate scope. `build_dfa`, `lift_for`, `scan_fsst`, `scan_fsst_rev2` and the `--json` CLI are the seam. |

---

## F. Compression and physical storage

FSST calibrates one symbol table per column. v1 reported payload-only ratios;
this is every byte a reader has to keep.
| column | rows | original B | codes B | symbol table B | .rev B | ratio (codes) | ratio (codes+table+.rev) | symbols | escape units | % escape units | rows with escape |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `clickbench/URL_1k` | 1,000 | 79,362 | 38,219 | 554 | 4,000 | 2.077x | 1.855x | 236 | 72 | 0.19% | 41 |
| `clickbench/URL_10k` | 10,000 | 760,012 | 396,374 | 541 | 40,000 | 1.917x | 1.739x | 255 | 256 | 0.06% | 210 |
| `clickbench/URL_100k` | 100,000 | 7,646,758 | 3,982,606 | 561 | 400,000 | 1.920x | 1.745x | 252 | 6,868 | 0.17% | 5,315 |
| `clickbench/URL_1m` | 1,000,000 | 88,562,192 | 49,965,040 | 565 | 4,000,000 | 1.772x | 1.641x | 255 | 74,457 | 0.15% | 43,769 |
| `clickbench/Referer_1m` | 1,000,000 | 79,583,339 | 46,112,704 | 600 | 4,000,000 | 1.726x | 1.588x | 255 | 153,570 | 0.33% | 68,971 |
| `tpch/l_comment_1m` | 1,000,000 | 26,511,095 | 9,320,929 | 865 | 4,000,000 | 2.844x | 1.990x | 212 | 0 | 0.00% | 0 |
| `tpch/o_comment_1m` | 1,000,000 | 48,529,673 | 15,161,196 | 903 | 4,000,000 | 3.201x | 2.533x | 216 | 0 | 0.00% | 0 |
| `dbtext/urls_100k` | 100,000 | 6,227,875 | 2,668,368 | 596 | 400,000 | 2.334x | 2.029x | 253 | 16,606 | 0.63% | 2,231 |
| `fuzz/values` | 20,016 | 1,762,863 | 988,670 | 559 | 80,064 | 1.783x | 1.649x | 255 | 1,546 | 0.16% | 1,081 |
| `sample/urls_1k` | 1,000 | 62,455 | 28,265 | 570 | 4,000 | 2.210x | 1.902x | 210 | 316 | 1.13% | 35 |
| `edge/values` | 51 | 669 | 500 | 123 | 204 | 1.338x | 0.809x | 92 | 8 | 1.63% | 4 |

The honest reading: on ClickBench URL the codes alone give **1.772×**, but the
`uint64` row offsets the loader actually reads cost 8 MB on a 50 MB payload. With
`uint32` offsets — sound for any payload under 4 GiB, and what a real engine
would store — the end-to-end ratio is **1.641×**. The symbol table is 565 bytes
and irrelevant. **The sidecar is gone in v2** (§H), which is where the remaining
8% went.

TPC-H comments reach **2.533×** end to end and contain **no escapes at all** —
plain ASCII natural language is covered completely by 216 symbols. On URL only
**0.15%** of logical units are escapes, but they are spread over **4.4%** of
rows; that distinction is what the v1 sidecar discussion turned on, since
per-row metadata is priced by the fraction of *rows* touched.
Every byte a reader has to keep, not just the payload. `.fsst.off` is stored as `uint64` per row boundary, which is what the loader reads; the `u32 offsets` column is what the same data would cost with 4-byte offsets, which is sound for any payload under 4 GiB and is what a real engine would do. The sidecar is **no longer required** — mode `fsst-rev2` reverse-scans without it (see FINAL_REPORT.md §H) — so it is listed but excluded from the totals, with the sidecar total shown for comparison.

| column | rows | raw bytes | codes | offsets (u64) | offsets (u32) | symbol table | sidecar (optional) | total, u32 off, no sidecar | ratio, payload only | ratio, total | ratio, total + sidecar |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `clickbench/URL_1m` | 1,000,000 | 88,562,192 | 49,965,040 | 8,000,008 | 4,000,004 | 565 | 4,000,000 | 53,965,609 | 1.772x | 1.641x | 1.528x |
| `clickbench/Referer_1m` | 1,000,000 | 79,583,339 | 46,112,704 | 8,000,008 | 4,000,004 | 600 | 4,000,000 | 50,113,308 | 1.726x | 1.588x | 1.471x |
| `tpch/l_comment_1m` | 1,000,000 | 26,511,095 | 9,320,929 | 8,000,008 | 4,000,004 | 865 | 4,000,000 | 13,321,798 | 2.844x | 1.990x | 1.531x |
| `tpch/o_comment_1m` | 1,000,000 | 48,529,673 | 15,161,196 | 8,000,008 | 4,000,004 | 903 | 4,000,000 | 19,162,103 | 3.201x | 2.533x | 2.095x |
| `dbtext/urls_100k` | 100,000 | 6,227,875 | 2,668,368 | 800,008 | 400,004 | 596 | 400,000 | 3,068,968 | 2.334x | 2.029x | 1.795x |
| `clickbench/URL_100k` | 100,000 | 7,646,758 | 3,982,606 | 800,008 | 400,004 | 561 | 400,000 | 4,383,171 | 1.920x | 1.745x | 1.599x |
| `clickbench/URL_10k` | 10,000 | 760,012 | 396,374 | 80,008 | 40,004 | 541 | 40,000 | 436,919 | 1.917x | 1.739x | 1.594x |
| `clickbench/URL_1k` | 1,000 | 79,362 | 38,219 | 8,008 | 4,004 | 554 | 4,000 | 42,777 | 2.077x | 1.855x | 1.697x |

The token-table columns come from the original TokenLIKE pipeline over the identical canonical parquet rows. Its vocabulary (`data/vocab.bin`, 630,133 B) is **shared by every column**, so it is shown separately rather than charged to each one; FSST's symbol table is per column and is charged. Both use `u32` offsets here, so the comparison is of payload plus per-column metadata.

| column | rows | raw bytes | FSST codes + table | token ids (`uint16`) | FSST total (u32 off) | token total (u32 off) | FSST ratio | token ratio | FSST / token |
|---|---|---|---|---|---|---|---|---|---|
| `clickbench/URL_1m` | 1,000,000 | 88,562,192 | 49,965,605 | 72,272,106 | 53,965,609 | 76,272,110 | 1.641x | 1.161x | 1.413x |
| `clickbench/Referer_1m` | 1,000,000 | 79,583,339 | 46,113,304 | 65,951,752 | 50,113,308 | 69,951,756 | 1.588x | 1.138x | 1.396x |
| `tpch/l_comment_1m` | 1,000,000 | 26,511,095 | 9,321,794 | 13,167,378 | 13,321,798 | 17,167,382 | 1.990x | 1.544x | 1.289x |
| `tpch/o_comment_1m` | 1,000,000 | 48,529,673 | 15,162,099 | 22,274,900 | 19,162,103 | 26,274,904 | 2.533x | 1.847x | 1.371x |
| `dbtext/urls_100k` | 100,000 | 6,227,875 | 2,668,964 | 4,851,408 | 3,068,968 | 5,251,412 | 2.029x | 1.186x | 1.711x |

The token table's 630,133-byte vocabulary is amortised across however many columns a deployment has. Charged in full to a single 1M-row column it would add about 1.3% to that column's FSST-equivalent footprint; across the four 1M-row columns measured here it is under 0.4% each. FSST's per-column tables total 5,185 B for all 8 columns.

FSST wins the storage comparison on every column, **1.29×–1.71×** smaller than
the token-table representation of the identical rows, because a table calibrated
to the data beats a fixed table calibrated to English text. §J shows the
matcher story runs the other way for unanchored scans, and why.

---

## G. Separating DFA construction from lifting

The two halves depend on different things, and the split is not incidental:
`build_dfa(pattern)` never looks at a symbol table, and `lift_dense(dfa, table)`
is a pure function that only reads `dfa.delta`. FSST calibrates a table per block
or per column, so one query predicate normally has to be matched against many
tables. `LiftedSet` / `lift_for()` is the safe form of that reuse — it owns the
automaton, so a caller cannot lift against a `Dfa` that has since been
reassigned, and it records which table each entry came from.

`fsst_liftbench` measures it, and **verifies it before timing anything**: for
every table it scans once with a table lifted from the shared automaton and once
with a table lifted from a freshly built one, and compares the bitmaps.

```
pattern %.php   7 DFA states   6 symbol tables   median of 9 runs
  build the byte DFA once             92.5 us
  lift onto clickbench/URL_1m          2.4 us  (255 symbols)
  lift onto clickbench/Referer_1m      2.5 us  (255 symbols)
  lift onto tpch/l_comment_1m          3.2 us  (212 symbols)
  lift onto tpch/o_comment_1m          3.3 us  (216 symbols)
  lift onto dbtext/urls_100k           2.4 us  (253 symbols)
  lift onto clickbench/URL_100k        2.3 us  (252 symbols)
  reuse one automaton                108.1 us
  rebuild for every table            570.6 us   (5.28x more)
  reuse verified: 6 of 6 tables gave bitmaps identical to a fresh build
```

So the expensive half is the automaton (92.5 µs) and the cheap half is the
lifting (2.3–3.3 µs, **about 3% of it**), because the alphabet is 255 codes and
not 65,536 tokens. Reuse across 6 tables costs **5.28×** less than rebuilding;
the saving grows linearly with the number of blocks, and in the limit the
per-block cost is the lift alone. The unit tests additionally require the lifted
tables to be **byte-identical**, not merely equivalent, so this is a safe
interface and not just a fast one.

Caveat on size: this is a microbenchmark of construction, not of query latency.
Construction is 0.1–1.1 ms against scans of 2–60 ms per million rows, so reuse
matters for many small blocks, not for one large scan.

---

## H. The reverse-scan sidecar is removable — proved, implemented, measured

v1 stored `.fsst.rev`: 4 bytes per row giving the offset just past each row's
last escape sequence, because a right-to-left walk over FSST bytes looked
ambiguous. Byte 255 is never a code, so a `0xFF` is either a marker or the
literal of an escape pair, and reading backwards you meet the literal first. The
review's hypothesis was that this is removable. It is, and here is why.

> **Lemma.** The first byte of a maximal run of `0xFF` always starts a unit.
>
> A `0xFF` is either a marker, which starts a unit, or the literal of an escape
> pair, whose marker is the immediately preceding byte and is also `0xFF`. The
> first byte of a *maximal* run has no `0xFF` before it, so it cannot be a
> literal. Hence it is a marker, and markers start units. ∎
>
> **Consequence.** Let `b` be the row's last byte.
> * `b == 0xFF`: let `m` be the length of the maximal `0xFF` run ending at the
>   row's end. By the lemma it starts a unit, so its parse is forced: `m/2`
>   escape pairs `<255,255>`. `m` odd means the row ends in a dangling marker and
>   is malformed.
> * `b < 0xFF`: let `r` be the length of the maximal `0xFF` run ending just
>   before `b`. By the lemma it starts a unit, so the parse of those `r` bytes
>   plus `b` is forced by the **parity of `r`**. Even ⇒ `r/2` pairs and `b` is a
>   code. Odd ⇒ `(r-1)/2` pairs and then `<255, b>`.
>
> The same argument then applies to each unit in turn.

What the argument does **not** use: that `0xFF` bytes can be interpreted
independently (they cannot — that is exactly why single-byte lookahead is
insufficient, and why `255 255 b` would be misread by it); any UTF-8 property;
or any assumption that the data is valid UTF-8. It uses only that the run is
maximal. Walking a run is handled in one step, so the scan stays linear in the
bytes it touches even for a row that is entirely `0xFF`.

**Tested, not just argued.** `parse_units_forward` and `parse_units_reverse`
exist so the claim is checkable: every string over `{0x00, 0x05, 0xFF}` up to
length 10, all-`0xFF` runs up to 24 bytes, and 20,000 random rows —
**108,598 rows, 26,099 of them malformed, and the two parsers agreed on every
one, including on which rows are malformed.** Separately, columns built from
arbitrary escape-heavy unit sequences (full of escaped `0xFF` literals, which
the real encoder never emits) must give `fsst-rev2` the same answers as
`decode`, `fsst`, `fsst-rev` and scanning the expanded bytes — with **zero**
fallbacks.

Mode `fsst-rev2` is that scanner. On real data it needs **no sidecar, no
fallback and no `malformed` reports**.

The trade-off, measured rather than argued:

* **On the shapes reverse scanning exists for** — suffixes — the sidecar-free
  scan is **faster**: 1.92–2.09 ns/row against 2.39–2.53 with the sidecar, and
  it reads fewer units (≈1.0 against ≈1.27) because it never abandons a row.
* **On a full reverse traversal** — an unanchored `%yandex%`, where reverse
  scanning is the wrong plan anyway — it is about **20% slower** per row,
  because it spends two extra compares per unit instead of consulting a stored
  boundary. A planner picks `fsst` there, at 58 ns/row.
* **Storage: 4,000,000 B saved per million rows, 8.0% of the payload**, with no
  correctness compromise. A 1-bit-per-row has-escape flag (0.25%) is also
  measured and is strictly worse than both: it forces all 43,769 escape rows
  down the forward path for 8.5 ns/row.

**Conclusion: retire the sidecar, keep `fsst-rev2`.** `fsst-rev` stays in the
module so the comparison stays reproducible, and `.fsst.rev` is still written by
the encoder for it; a deployment that only uses `fsst-rev2` should pass
`--no-rev` and save the 4 bytes a row. This also closes the gap §K noted against
the published prior art, which resolves the same ambiguity inside the automaton
and likewise stores nothing.

### Automaton construction and table size

Three state counts are now reported separately, because conflating them was a
real reporting bug in v1: a table showing `%.php` as "5 → 7" read as if
minimisation had *added* states, when 5 was the subset-construction count and 7
was the canonical count including the two always-allocated sink slots. The
columns below are **subset** (after subset construction), **minimal** (Hopcroft
blocks — the true minimal automaton), and **canonical forward / canonical
reverse** (minimal with the dead and accept-sink classes mapped onto the
reserved slots 0 and 1). `n = minimal + (2 − classes merged)`.
| pattern | subset states | minimal states | canonical states (fwd) | canonical states (rev) | DFA build us | lift build us | table bytes | units/row (fsst) | early-exit rows | fallback rows (fsst-rev) | fallback rows (fsst-rev2) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `http://%` | 9 | 9 | 9 | 10 | 99.5 | 4.7 | 9,225 | 2.35 | 999,725 | 43,769 | 0 |
| `/static/%` | 10 | 10 | 10 | 11 | 106.8 | 4.9 | 10,250 | 1.00 | 999,735 | 43,769 | 0 |
| `http://irr.ru%` | 15 | 15 | 15 | 16 | 127.0 | 5.2 | 15,375 | 2.87 | 999,723 | 43,769 | 0 |
| `%.php` | 5 | 5 | 7 | 6 | 122.7 | 4.2 | 7,175 | 49.89 | 0 | 3,596 | 0 |
| `%.m3u8` | 6 | 6 | 8 | 7 | 139.9 | 3.4 | 8,200 | 49.89 | 0 | 3,596 | 0 |
| `%.html` | 6 | 6 | 8 | 7 | 137.3 | 4.2 | 8,200 | 49.89 | 0 | 3,596 | 0 |
| `http://irr.ru/index.php?showalbum/login-leniya7777294,938303130` | 65 | 65 | 66 | 66 | 438.5 | 18.8 | 67,650 | 6.24 | 926,828 | 3,596 | 0 |
| `%yandex%` | 12 | 7 | 8 | 8 | 279.0 | 4.5 | 8,200 | 48.94 | 23,785 | 43,285 | 0 |
| `%/catalog/%` | 18 | 10 | 11 | 11 | 414.9 | 5.3 | 11,275 | 49.23 | 14,285 | 43,453 | 0 |
| `%/v_/%` | 46 | 13 | 14 | 13 | 1,138.3 | 6.3 | 14,350 | 49.89 | 126 | 43,761 | 0 |
| `%сезон%` | 20 | 11 | 12 | 12 | 460.6 | 5.3 | 12,300 | 49.81 | 1,916 | 43,326 | 0 |
| `http%irr%php%` | 18 | 12 | 12 | 13 | 394.0 | 5.3 | 12,300 | 33.36 | 328,652 | 43,769 | 0 |
| `http://_rr.ru/%php%` | 39 | 26 | 26 | 26 | 449.0 | 8.8 | 26,650 | 3.76 | 985,981 | 43,769 | 0 |
| `%regular%` | 14 | 8 | 9 | 9 | 347.8 | 5.5 | 9,225 | 13.35 | 216,740 | 0 | 0 |
| `%s` | 2 | 2 | 4 | 3 | 65.0 | 2.7 | 4,100 | 9.32 | 0 | 0 | 0 |

Minimisation earns its place where wildcards interact — `%/v_/%` goes from 46
subset states to 14 — and is a no-op where subset construction is already
minimal, which the three columns now make visible instead of confusing.

### Dense versus compact lifted tables

The DFA's own 256-wide byte table is in both figures, since the escape path needs it.

| pattern | states | dense ns/row | compact ns/row | dense total table B | compact total table B | compact/dense size | compact/dense time |
|---|---|---|---|---|---|---|---|
| `http://%` (prefix) | 9 | 3.09 | 6.61 | 9,225 | 5,065 | 0.55x | 2.14x |
| `%.php` (suffix) | 7 | 59.03 | 119.26 | 7,175 | 3,957 | 0.55x | 2.02x |
| `%yandex%` (contains) | 8 | 58.57 | 117.12 | 8,200 | 4,552 | 0.56x | 2.00x |
| `%/v_/%` (underscore) | 14 | 59.05 | 135.07 | 14,350 | 8,504 | 0.59x | 2.29x |
| `http%irr%php%` (complex) | 12 | 38.76 | 93.84 | 12,300 | 6,872 | 0.56x | 2.42x |
| `http://irr.ru/index.php?showalbum/log...` (long literal) | 66 | 8.59 | 30.57 | 67,650 | 37,086 | 0.55x | 3.56x |

**The dense table wins.** Compact is consistently 0.55–0.59× the size and
2.0–3.6× slower. At 255 codes a dense row is 512 bytes, so a 14-state automaton
is 7 KiB and sits in L1; the saving buys nothing while the
default-plus-exception lookup costs a branch, a mask load, a popcount and a
second indirection per unit. `fsst-compact` is kept as a mode so the claim stays
reproducible rather than anecdotal, and it is excluded from "best compressed" in
§I and §J because a planner would never choose it.

### Peak process memory

| mode | min MiB | median MiB | max MiB | what it has to hold |
|---|---|---|---|---|
| raw | 43.1 | 101.6 | 105.5 | 88 MB raw column + offsets |
| raw-opt | 43.0 | 101.5 | 105.4 |  |
| decode | 26.7 | 64.8 | 68.7 | 50 MB codes + offsets + one decode buffer |
| fsst | 26.6 | 64.8 | 68.7 | 50 MB codes + offsets + lifted table |
| fsst-compact | 26.7 | 64.8 | 68.7 | 50 MB codes + offsets + compact table |
| fsst-rev | 30.5 | 68.9 | 72.6 | 50 MB codes + offsets + .rev + two lifted tables |
| fsst-rev2 | 26.7 | 65.0 | 68.8 |  |

The compressed modes hold ~37 MiB less than the raw baselines at 1M rows, which
is the compression ratio showing up as resident memory. `fsst-rev` carries ~4 MiB
more than `fsst` for the sidecar; **`fsst-rev2` carries ~0.2 MiB more**, which is
the second lifted table and nothing else.

---

## I. Per-pattern performance

Machine: Apple M2, macOS-26.5.2-arm64-arm-64bit-Mach-O  
Best of 7 runs after 2 warmups; best-of-N wall time of the scan only; column load and automaton build are outside it.

| pattern | matches | raw ns/row | raw-opt ns/row | decode ns/row | fsst ns/row | fsst-compact ns/row | fsst-rev ns/row | fsst-rev2 ns/row | best compressed | fair raw = min(raw, raw-opt) | compressed vs fair raw | compressed vs decode |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `http://%` | 517,519 | 3.38 | 3.48 | 31.19 | 3.09 | 6.61 | 58.66 | 76.26 | 3.09 (fsst) | 3.38 | 1.09x | 10.09x |
| `/static/%` | 0 | 1.53 | 2.13 | 28.07 | 1.26 | 1.70 | 57.34 | 75.99 | 1.26 (fsst) | 1.53 | 1.21x | 22.23x |
| `http://irr.ru%` | 341,755 | 5.86 | 2.79 | 33.39 | 3.38 | 8.01 | 59.01 | 76.90 | 3.38 (fsst) | 2.79 | 0.83x | 9.88x |
| `%.php` | 12,714 | 104.04 | 2.50 | 155.32 | 58.97 | 118.53 | 2.57 | 2.09 | 2.09 (fsst-rev2) | 2.50 | 1.20x | 74.46x |
| `%.m3u8` | 0 | 103.62 | 2.40 | 155.97 | 58.61 | 115.42 | 2.48 | 1.92 | 1.92 (fsst-rev2) | 2.40 | 1.25x | 81.34x |
| `%.html` | 7,683 | 104.23 | 2.41 | 155.78 | 58.96 | 145.15 | 2.51 | 2.03 | 2.03 (fsst-rev2) | 2.41 | 1.19x | 76.92x |
| `http://irr.ru/index.php?showalbum/login-leniya7777294,938303130` | 58,976 | 19.92 | 1.25 | 50.60 | 8.57 | 30.39 | 3.50 | 3.99 | 3.50 (fsst-rev) | 1.25 | 0.36x | 14.47x |
| `%yandex%` | 27,864 | 102.98 | 62.38 | 153.56 | 58.25 | 116.80 | 61.65 | 74.32 | 58.25 (fsst) | 62.38 | 1.07x | 2.64x |
| `%/catalog/%` | 14,286 | 103.38 | 70.69 | 154.22 | 58.43 | 132.61 | 62.39 | 74.74 | 58.43 (fsst) | 70.69 | 1.21x | 2.64x |
| `%/v_/%` | 165 | 103.95 | 87.26 | 155.01 | 58.50 | 133.78 | 62.78 | 76.14 | 58.50 (fsst) | 87.26 | 1.49x | 2.65x |
| `%сезон%` | 2,116 | 103.72 | 57.67 | 155.31 | 58.66 | 107.79 | 62.94 | 75.64 | 58.66 (fsst) | 57.67 | 0.98x | 2.65x |
| `http%irr%php%` | 336,463 | 67.57 | 64.75 | 113.23 | 38.71 | 91.50 | 61.26 | 77.18 | 38.71 (fsst) | 64.75 | 1.67x | 2.93x |
| `http://_rr.ru/%php%` | 335,775 | 9.06 | 16.48 | 38.50 | 4.46 | 11.33 | 58.45 | 76.54 | 4.46 (fsst) | 9.06 | 2.03x | 8.62x |
| `%regular%` | 224,710 | 44.14 | 39.37 | 57.36 | 12.69 | 30.79 | 13.54 | 18.89 | 12.69 (fsst) | 39.37 | 3.10x | 4.52x |
| `%s` | 75,650 | 23.48 | 3.43 | 33.70 | 12.12 | 38.81 | 2.70 | 2.49 | 2.49 (fsst-rev2) | 3.43 | 1.38x | 13.54x |

Reading it:

* **Anchored prefixes.** `http://%` reads 1.0–2.9 symbol units per row and runs
  at 1.26–3.38 ns/row — **9.9–22.2× faster than decode-then-match**, and roughly
  level with a fair raw baseline.
* **Anchored suffixes.** `fsst-rev2` finishes `%.php` in **2.09 ns/row** against
  155.32 for decode (**74.5×**) and 2.50 for the optimised raw matcher
  (**1.20×**). Scanned forward the same pattern costs 58.97, because a suffix
  cannot be decided until the last unit.
* **Unanchored `contains`.** The honest weak spot. `%yandex%` must read every
  unit of every non-matching row, so the gain is bounded by how many fewer units
  there are: 58.18 against 103.22 for the byte DFA, i.e. 1.77×, almost exactly
  the 1.772× compression ratio — but only **1.07×** against `memmem` on the raw
  strings (62.38 ns/row).
* **`_` costs nothing extra at scan time.** `%/v_/%` runs at 58.50 ns/row,
  within noise of `%yandex%` at 58.18. The UTF-8 character sub-automaton is paid
  for once, in 46 → 14 states at construction, not per row. Against raw it is
  the module's **best** unanchored case (1.49×), because `memmem` has no good
  way to seed a search for a segment containing a wildcard.
* **`fsst-rev2` is the wrong tool for prefixes** — 76–77 ns/row, ~0.4× decode —
  because it walks the whole row from the wrong end. A planner picks the
  direction from the pattern shape; the module exposes both rather than hiding
  the choice.
* **Decode is slower than both raw baselines on every pattern measured.** That
  is the result that makes the exercise worthwhile: a system that compresses with
  FSST and then decompresses to evaluate `LIKE` is strictly worse off than if it
  had never compressed, and compressed-domain matching is what recovers the loss.

### Scaling

Same four shapes at 1k / 10k / 100k / 1M rows:
| shape | 1k | 10k | 100k | 1M |
|---|---|---|---|---|
| `http://%` (fsst) | 1.58 | 1.51 | 1.44 | 3.10 |
| `%.php` (fsst-rev2) | 1.75 | 1.87 | 1.86 | 2.09 |
| `%yandex%` (fsst) | 40.92 | 41.85 | 42.13 | 59.56 |
| `%/v_/%` (fsst) | 40.71 | 42.92 | 42.23 | 59.17 |

Per-row cost is flat to 100k and rises at 1M. That is memory, not algorithm:
50 MB of codes no longer fits the cache hierarchy the way 4 MB does. Anchored
patterns move least, because they touch only the first unit or two per row.

---

## J. Against raw strings, and against the original TokenLIKE

### Versus raw — the v1 correction

v1 used one generic forward byte DFA as the raw baseline for every shape. That
is a correct matcher but not a fair one, and the review was right that it
overstated the result. v2 adds `raw-opt`:

| shape | how `raw-opt` answers it |
|---|---|
| `abc` | length check and `memcmp` |
| `abc%` | `memcmp` at the front |
| `%abc` | `memcmp` at the back |
| `%abc%` | the platform's `memmem` |
| anything else, including every pattern with `_` | cut the pattern at `%` into segments of fixed character length; anchor the first at the start and the last at the end where the pattern is anchored; match the middle ones greedily left to right, locating each with `memmem` on its leading literal. Greedy-leftmost is correct because a `%` between two segments absorbs anything, so matching a segment earlier never costs a later one a match. |

It is an optimisation of the *baseline*, not a handicap. Shape dispatch is
hoisted out of the row loop — leaving the switch inside cost ~2 ns/row and would
have quietly handicapped the very thing it exists to make fair. It is checked
against DuckDB on the same 3,367-case suite as every other mode and against the
independent reference matcher in the unit tests, so "optimised" never gets to
mean "wrong".

**The fair comparison, taking `min(raw, raw-opt)` per pattern** — the best raw
implementation available — is the `compressed vs fair raw` column above:

| | v1 claim | v2, fair baseline |
|---|---|---|
| range | 1.09×–43.7× faster than raw | **0.36×–3.10×** |
| patterns where compressed-domain loses | none reported | **3 of 15**: exact literal **0.36×**, `http://irr.ru%` 0.83×, `%сезон%` 0.98× |
| best case | `%.m3u8` 53.9× | `%regular%` (TPC-H) **3.10×** |
| versus decode | 2.6×–65× | **2.64×–81.3×**, unchanged in character |

The exact-literal case is worth stating plainly: `raw-opt` answers it with a
length check and one `memcmp` at **1.25 ns/row**, and the best compressed mode
needs 3.50. Compressed-domain matching is simply the wrong tool there, and v1's
"7.65× faster than raw" was an artifact of the baseline.

What survives the correction: the **2.64×–81.3× advantage over full decode**,
which is the comparison that actually decides whether a system should push `LIKE`
into FSST, and a modest but real win over raw on prefixes, suffixes, substrings
and `_` patterns — at 1.6× less storage.

### Versus the original TokenLIKE, same machine, same rows, same build

Both systems were built by one CMake invocation on this branch and run here.
TokenLIKE is `main` at `c58d066` (= `v3-freeze`), configured
`loaders (Person B) = ON, matcher (Person A) = ON`. Both CLIs report their
*minimum* per-row time over 7 runs after 2 warmups, the only statistic both
expose.
| pattern | shape | best FSST-LIKE ns/row (mode) | TokenLIKE token ns/row | ratio | FSST units/row | TokenLIKE tokens/row |
|---|---|---|---|---|---|---|
| `http://%` | prefix | 3.09 (fsst) | 3.01 | 0.97x | 2.35 | 1.52 |
| `/static/%` | prefix, 0 matches | 1.26 (fsst) | 2.14 | 1.70x | 1.00 | 1.00 |
| `http://irr.ru%` | prefix | 3.39 (fsst) | 5.27 | 1.55x | 2.87 | 3.06 |
| `%.php` | suffix | 2.15 (fsst-rev2) | 2.24 | 1.05x | 1.04 | 1.01 |
| `%.m3u8` | suffix, 0 matches | 1.91 (fsst-rev2) | 2.23 | 1.17x | 1.01 | 1.00 |
| `%.html` | suffix | 2.02 (fsst-rev2) | 2.29 | 1.13x | 1.02 | 1.01 |
| `http://irr.ru/index.php?showalbum/log...` | exact literal | 3.65 (fsst-rev) | 12.26 | 3.35x | 2.17 | 7.46 |
| `%yandex%` | contains | 58.44 (fsst) | 45.04 | 0.77x | 48.94 | 35.54 |
| `%/catalog/%` | contains | 58.10 (fsst) | 51.08 | 0.88x | 49.23 | 35.76 |
| `%/v_/%` | contains with '_' | 59.06 (fsst) | 73.43 | 1.24x | 49.89 | 36.13 |
| `%сезон%` | Cyrillic contains | 58.69 (fsst) | 31.06 | 0.53x | 49.81 | 36.09 |
| `http%irr%php%` | multi-% | 38.66 (fsst) | 36.45 | 0.94x | 33.36 | 25.14 |
| `http://_rr.ru/%php%` | prefix with '_' | 4.47 (fsst) | 8.01 | 1.79x | 3.76 | 4.97 |
| `%regular%` | contains (TPC-H) | 12.66 (fsst) | 13.13 | 1.04x | 13.35 | 9.86 |
| `%s` | suffix (TPC-H) | 2.49 (fsst-rev2) | 2.41 | 0.97x | 1.00 | 1.00 |

**Row ids: identical on 15 of 15 patterns, across all nine mode combinations of
the two implementations.** Two independently written matchers over two different
compression schemes agreeing exactly is the strongest correctness evidence here.

On speed it is a split, and the reason is instructive. **Scan cost tracks units
per row, not bytes per row.** A 65,536-entry token table parses a URL into ~36.1
tokens; FSST's 255-symbol table parses the same URL into ~49.9 codes. FSST's
bytes are fewer (1 byte per unit against 2), so FSST wins storage (§F, 1.29×–1.71×)
and anything that stops early, while TokenLIKE wins when the whole row must be
read because it has fewer things to read.

Two secondary results favour the small alphabet decisively: automaton
construction is **~30× cheaper** (92.5 µs median against ~3,100 µs, §G), and the
lifted table is smaller (9.0 KiB against 12.6 KiB) *without* needing a compact
encoding — a dense table over 65,536 tokens would be 1,152 KiB for the same
automaton, which is why TokenLIKE has to compress its table and FSST does not.

Caveats so these are not over-read: one machine, one thread, warm page cache,
best-of-7; the two systems use different compressors on the same source text, so
this compares *schemes plus implementations*, not algorithms in isolation.

---

## K. Prior art

**The core idea is not new.** It is published, and part of it ships. v1 said so
and v2 says so again, because it is the most important sentence here.

**Pop, Riedl, Neumann, "Compression-Aware LIKE: Matching Patterns in the FSST
Domain", DaMoN'26** — ACM DOI [10.1145/3789237.3809128](https://doi.org/10.1145/3789237.3809128),
PDF <https://db.in.tum.de/~pop/fsstlike.pdf>, reference implementation
<https://github.com/calin2110/FSST-LIKE-Matching> inspected at `b1eb3ab`. They
state the thesis: *"instead of decompressing data to match patterns, we compress
the pattern by constructing finite automata that parse FSST-encoded symbol
sequences directly."*

| | prior art | this module |
|---|---|---|
| automaton | specialised per sub-pattern (start / middle / end), trie-first in the style of Aho–Corasick, directly over code sequences, then stitched | one minimised byte-level DFA lifted onto the code alphabet; uniform over any mix of `%` and `_` |
| escape ambiguity backwards | pseudo-end states plus a one-byte lookahead, **no metadata** | v1 used a 4-byte-per-row sidecar; **v2 stores nothing either** (§H), by run parity rather than lookahead |
| codegen | interpreted **and** LLVM-JIT | interpreted only |
| SIMD / threads | SIMD self-loop traversal; morsel-driven, near-linear to 10 cores | neither |
| reported gains | **2.5–17×** over decompression-based matching (HSS and Vectorscan on decompressed data) on TPC-H, StackOverflow, IMDB, PublicBI | 2.64×–81.3× over full decode on ClickBench and TPC-H |

**Those ranges are not comparable** and are not treated as such: different
machine, datasets, baselines (tuned SIMD substring search against a byte DFA)
and statistics. Where shapes line up, their single-threaded JIT implementation
does more per cycle than this one — they have JIT and SIMD and this has neither.

### The one thing this module adds: `_` in the compressed domain

The paper is explicit about scope: *"Considering only the '%' wildcard character
that matches any number of characters…"*. Their implementation parses `_` —
there is an `UnderscorePattern` in `include/pattern.hpp:55` — but all three of
its automaton builders are stubs:

```
third_party/priorart-fsst-like/src/pattern.cpp:229   throw std::runtime_error("Not implemented");
third_party/priorart-fsst-like/src/pattern.cpp:233   throw std::runtime_error("Not implemented");
third_party/priorart-fsst-like/src/pattern.cpp:237   throw std::runtime_error("Not implemented");
```

(`createStartAutomaton`, `createMiddleAutomaton`, `createEndAutomaton`.) So a
pattern containing `_` cannot be matched in the compressed domain there at all.

This module supports `_` as **one Unicode character**, in the compressed domain,
at no measurable per-row cost (§I), verified against DuckDB on 1M real rows
including 137,578 non-ASCII ones. It works because the automaton is byte-level
with the character rule inside it, so a symbol boundary landing mid-character is
just another transition. An approach built on code sequences has to handle "`_`
consumed 2 of the 3 bytes this symbol expands to" explicitly, which is
presumably why it was left unimplemented. It is also the module's best
unanchored case against raw (1.49×), because `memmem` cannot seed a search for a
segment containing a wildcard.

A smaller difference: the prior-art parser treats backslash as an escape
character (Postgres/Umbra default). DuckDB does not without an explicit `ESCAPE`
clause — measured, `scripts/truth.py --semantics`. Different dialects, not a
defect; this module matches the one its oracle uses.

**Vortex** (`vortex-data/vortex`, crate `vortex-fsst`) has an FSST `LIKE` fast
path for `prefix%` and `%needle%` with documented byte limits (253 / 254) above
which it decompresses, plus `%suffix` pushdown in PR
[#9728](https://github.com/vortex-data/vortex/pull/9728) using a backward scan —
explicitly because a forward DFA cannot exit early on a suffix, the same
reasoning behind `fsst-rev2` — reporting roughly 4–5× speedups, with no gain on
an email dataset whose 9-byte suffix exceeds FSST's 8-byte symbol cap. No `_`
fast path is documented.

### Attribution summary

| | |
|---|---|
| **prior art** | compressed-domain FSST `LIKE`; prefix, suffix and substring pushdown; backward scanning for suffixes; the escape-marker ambiguity and a metadata-free resolution of it; early rejection; "compress the pattern, not the data" |
| **this module reproduces** | all of the above, independently, by a different construction (lift a minimised byte DFA, rather than build per-sub-pattern automata over codes) |
| **this module adds** | `_` as one Unicode character in the compressed domain, which the published implementation leaves unimplemented; DuckDB-exact UTF-8 character semantics across symbol boundaries; `(a ‖ b) LIKE p` over two columns with two symbol tables and one shared automaton; a parity-based sidecar-free reverse parse with an exhaustive equivalence test; measured answers for dense-vs-compact at 255 codes, for sidecar cost, for DFA/lift separation, and a fair-raw baseline |
| **still unresolved here** | JIT and SIMD; multi-threading; integration into a real query engine |

**Why no head-to-head run.** The prior-art repository was cloned and read, but
its executables cannot be built here: its `CMakeLists.txt` gates **every** binary
behind `find_package(LLVM 16 CONFIG)` with a hardcoded Linux path
(`/usr/lib/llvm-16/lib/cmake/llvm`) — *"LLVM 16 not found. All executables are
unavailable."* — and gates benchmarks and tests additionally behind Vectorscan,
an x86-SIMD library not packaged for arm64 macOS. It also fetches its own fork
of FSST. The comparison above is from the paper, the source and the docs, not
from a run, and is labelled as such throughout.

---

## L. Reproducing this

macOS arm64, Apple Clang. All commands macOS-native: no MSYS2, no Windows GCC.

```bash
cd ~/TokenLIKE-Integration/TokenLIKE            # the integration clone
export TOKENLIKE_DIR=~/Documents/Desk/semV/HASHCODE/hash/TokenLIKE   # read-only
export PY=$TOKENLIKE_DIR/.venv/bin/python

# 1. the repository WITHOUT the module must be untouched

cmake -G Ninja -S . -B build/default -DCMAKE_BUILD_TYPE=Release
cmake --build build/default && ctest --test-dir build/default --output-on-failure

# 2. the module

bash integration/fsst/scripts/fetch_fsst.sh      # optional; CMake can FetchContent
cmake -G Ninja -S . -B build/fsst -DCMAKE_BUILD_TYPE=Release -DTOKENLIKE_WITH_FSST=ON
cmake --build build/fsst

# 3. compress the columns (per-row round-trip verified inside the encoder)

cd integration/fsst && bash scripts/build_columns.sh

# 4. correctness: 6,429 unit + 3,367 DuckDB checks + the cross-check

bash scripts/check_all.sh

# 5. sanitizers

cmake -G Ninja -S ../.. -B ../../build/fsst-asan -DCMAKE_BUILD_TYPE=Debug \
      -DTOKENLIKE_WITH_FSST=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build ../../build/fsst-asan
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS="print_stacktrace=1:suppressions=$PWD/tests/ubsan.supp"
../../build/fsst-asan/fsst_test_unit

# 6. measurements

$PY scripts/bench.py   --patterns patterns/bench.txt  --runs 7 --warmup 2 --rss \
                       --out bench/v2-integration/bench.json
$PY scripts/bench.py   --patterns patterns/scales.txt --runs 7 --warmup 2 \
                       --out bench/v2-integration/scales.json
$PY scripts/analyze.py                                  # -> bench/v2-integration/tables.md
$PY scripts/report.py  bench/v2-integration/bench.json  > bench/v2-integration/bench_tables.md
$PY scripts/storage.py                                  # -> bench/v2-integration/storage.md
$PY scripts/compare_tokenlike.py --out bench/v2-integration/vs_tokenlike.json
../../build/fsst/fsst_liftbench --pattern '%.php' --repeat 9 \
   --col data/clickbench/URL_1m --col data/clickbench/Referer_1m \
   --col data/tpch/l_comment_1m --col data/tpch/o_comment_1m \
   --col data/dbtext/urls_100k --col data/clickbench/URL_100k

# 7. demo

$PY demo_fsst.py --rows 100000
```

Artifacts: `bench/v2-integration/` for everything measured here,
`bench/v1-experiment-standalone/` for the superseded standalone results with
their `PROVENANCE.md`, and `data/*/*.fsst.meta.json` for per-column counts plus
FNV-1a-64 digests of `.fsst.cmp`, `.fsst.off` and `.fsst.sym` so a rebuild can be
checked for bit-identity. The compression pipeline is byte-deterministic: the
rebuild in the integration tree reproduced every ratio, symbol count and escape
count from the standalone run exactly.

---

## M. Defects found and corrected

All fixes are inside the module. No TokenLIKE file and no FSST file was modified.

**v2-1. The raw baseline overstated the result. (Reporting defect; fixed.)**
A generic forward byte DFA was used as the raw baseline for every pattern shape,
including suffixes and substrings. Against the optimised baseline the headline
"vs raw" range goes from 1.09×–43.7× to **0.36×–3.10×**, and three of fifteen
patterns now lose. Found by review, not by me. §J.

**v2-2. The reverse-scan sidecar was unnecessary. (Design defect; fixed.)**
8% of the compressed payload was spent on metadata that a parity argument makes
redundant. §H. Mode `fsst-rev2` stores nothing, is faster on suffixes, and needs
no fallback.

**v2-3. State counts were reported in a way that read as wrong.** A table
showing `%.php` as "5 → 7 states" looked like minimisation adding states. Three
counts (subset / minimal / canonical) are now separate fields in the JSON and
separate columns in the report, and forward and reverse automata are labelled.

**v2-4. The demo truncated patterns mid-string and mislabelled fallbacks.**
`shown[:34]` cut `one Unicode character wildcard: '%/v_/%'` down to `…: '%`,
losing the pattern entirely; and the reverse-scan fallback count was printed
under the forward scan's state counts, implying the forward scan had fallen
back. Both fixed; the fallback line now names the mode and says explicitly when
no fallback and no sidecar were needed.

**v2-5. `fsst-rev2` was wired into the CLI but never given its lifted tables**
— the mode-dispatch condition listed the three older FSST modes by name and so
skipped the new one, producing `lifted table was built for a different
automaton` on first use. Caught by the first smoke test after wiring.

**v2-6. `raw-opt` had ~2 ns/row of avoidable per-row dispatch** (shape switch
plus a call inside the row loop), which would have handicapped the baseline it
exists to make fair. Hoisted out of the loop; prefix went from 5.28 to
3.39 ns/row, which is what changed `http://%` from 1.71× to 1.10×.

### Carried over from v1 (unchanged, still true)

**v1-1. The backward scan could silently misread an escaped literal as a code.**
The reverse scan guarded against the escape region by checking for byte 255, but
walking right to left the *literal* of an escape pair is read first, and it can
be any value including one that looks like an ordinary code. Fixed by testing
`code >= nsymbols` instead. Found by my own test, which had been passing for the
wrong reason.

**v1-2. Two of my own test expectations were wrong, not the code** — a zeroed
`.fsst.rev` need not produce a `malformed` report, and an incompressible
random-ASCII column produces no escapes at all (FSST can give each of 127
distinct bytes its own code). Both replaced with sharper tests.

**v1-3. A bounds check in `utf8_valid` was written wrong**, which would have
read past the buffer for a truncated multi-byte character at the very end of an
input. Caught on review before the first build.

**v1-4. Upstream finding, not fixed deliberately:** 10 UBSan misaligned-store
reports in `fsst_decompress`. See `THIRD_PARTY.md`.

**v1-5. Robustness hardening** that came out of writing the tests, each with a
test proving the rejection: twelve malformed inputs — a symbol table whose
hidden count disagrees with its own histogram, an undefined code, a row ending
in a dangling escape marker, non-monotonic offsets, `off[0] != 0`, a terminal
offset disagreeing with the payload, an offset file whose size is not a multiple
of 8, a `meta.json` row count disagreeing with the offsets, a missing column, a
`.fsst.rev` entry past the end of its row, an over-budget pattern, an
invalid-UTF-8 pattern — twelve clean `std::runtime_error`s naming the row or the
file, zero crashes, zero sanitizer reports.

**Also corrected in v1, worth repeating:** a verification check of mine was
wrong twice. `find -newermt '-12 hours'` resolves to `bfs` on this machine, which
rejects relative timestamps, errors to stderr and prints nothing — which reads
exactly like "no files modified". Always use an explicit ISO timestamp.

---

## N. Is this worth doing?

**Against full decode: yes, clearly. 2.64×–81.3×, never slower.** If a system
compresses strings with FSST and must evaluate `LIKE`, pushing the predicate into
the compressed domain is the difference between keeping and losing the
compression benefit. Decode-then-match is slower than *both* raw baselines on
every pattern measured; a system that decompresses to match has paid CPU to give
up its own compression.

**Against raw strings: much more narrowly than v1 said.** 0.36×–3.10× against a
fair baseline, losing on 3 of 15 patterns. The wins that survive are prefixes
(1.10–1.21×), suffixes (1.19–1.25×), multi-segment patterns (1.67×), `_`
patterns (1.49×, the best unanchored case) and TPC-H `contains` (3.10×) — while
storing 1.6× less. The losses are exact literals (0.36×, where one `memcmp`
beats any automaton) and short Cyrillic substrings (0.98×). v1's claim of up to
43.7× over raw was an artifact of its baseline and is withdrawn.

**The seven questions the original brief asked, answered with v2 data:**

1. *Exactly over FSST codes?* **Yes.** 3,367 DuckDB checks over 8 modes, 0
   mismatches; 6,429 in-process checks against an independent reference; 15/15
   identical row-id sets against a different implementation.
2. *Can the token-lifting algorithm be reused?* **Yes, essentially unchanged.**
   The alphabet shrinks from 65,536 to 255 and one new concern appears, the
   escape marker — which costs a branch in the scan loop and, as of v2,
   **nothing in storage**.
3. *Prefix, suffix, exact, contains, Unicode `_`, concatenation?* **All six.**
4. *Faster than full decode?* **2.64×–81.3×.**
5. *Versus raw strings?* **0.36×–3.10×** against a fair baseline. See above.
6. *Memory and construction cost?* Lifted table 4.0–66.1 KiB; construction
   68 µs – 1.15 ms, of which lifting is ~3% and reusable across tables (5.28×
   saving over 6 tables); peak RSS ~37 MiB *below* raw at 1M rows; **no sidecar**.
7. *Anything beyond existing FSST-LIKE work?* **`_` in the compressed domain**,
   plus exact UTF-8 character semantics, cross-column concatenation, and the
   measured answers in §G/§H/§J. Everything else is reproduction.

**As a research contribution: no, and that goes first.** The idea was published
at DaMoN'26 before this work started, with a faster implementation (JIT, SIMD,
threads), a metadata-free escape resolution, and a formal complexity result. v2
now matches the metadata-free property but not the speed. The honest framing is
"reproduce and extend".

**Where it would pay off in a real engine:**

* **Worth it:** any workload that would otherwise decompress to match. That is
  the whole case, and it is strong.
* **Worth it, modestly:** prefixes, suffixes, substrings and `_` patterns, for
  1.1×–3.1× over a good raw matcher at 1.6× less storage. Worth having; not
  worth restructuring an engine for on its own.
* **Not worth it:** exact-literal predicates. Compare bytes.
* **Needs a planner.** Forward and reverse differ by more than 25× in both
  directions depending on shape (`fsst-rev2` is 28× better than `fsst` on
  `%.php` and 25× worse on `http://%`). Choosing the direction from the pattern
  is a trivial rule but not optional.

**What I would do next, in order:** a direction chooser so one mode suffices;
drop `.fsst.rev` from the encoder's default output now that `fsst-rev2` exists;
then SIMD for the self-loop of unanchored patterns, the only case still reading
every unit. Narrower row offsets (`u32`, or delta-encoded) are worth more to the
storage figure than anything left in the payload.

**A footnote on the two schemes.** FSST beats the patched Claude token table on
footprint for every column measured, 1.29×–1.71× on total physical storage, and
builds its matcher ~30× faster. The token table wins unanchored scans because a
coarser parse means fewer units to read. Compression ratio and scan cost are not
the same axis, and a system choosing between them is trading footprint and build
cost against full-scan throughput.
