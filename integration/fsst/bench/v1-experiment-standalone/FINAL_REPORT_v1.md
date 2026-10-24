# FSST-LIKE — final report

Local research experiment: evaluating SQL `LIKE` directly on FSST-compressed
strings by lifting a byte-level `LIKE` automaton onto the FSST symbol alphabet,
the same move TokenLIKE makes for LLM token tables.

All numbers below were produced on this machine, in this session, by the
commands in `docs/REPRODUCE.md`. Nothing is carried over from another machine
or another paper's tables.

**Environment, as detected rather than assumed**

| | |
|---|---|
| machine | Apple M2, 8 cores, 16 GiB |
| architecture | **arm64** (`uname -m` = `arm64`, `hw.optional.arm64` = 1, `sysctl.proc_translated` = 0, so native and not under Rosetta) |
| OS | macOS 26.5.2 (Darwin 25.5.0) |
| compiler | **Apple clang 21.0.0** (`clang-2100.1.1.101`), target `arm64-apple-darwin25.5.0`, from `/Library/Developer/CommandLineTools/usr/bin/clang++` |
| Xcode CLT | `/Library/Developer/CommandLineTools`, package 26.6.0.0.1781586589, SDK MacOSX 26.5 |
| Homebrew LLVM | not installed, and not needed — Apple Clang builds everything |
| build system | CMake 4.4.4 + **Ninja 1.13.2**, both from Homebrew 7.0.8 |
| Python | 3.14.7, with DuckDB 1.5.6 (module) and a pinned DuckDB 1.5.6 CLI as a second backend |
| git | 2.53.0 |
| binaries produced | `Mach-O 64-bit executable arm64` |
| flags | `-O3 -DNDEBUG` release; `-O0 -g -fsanitize=address,undefined` for the sanitizer build |

**Nothing was installed on this machine for this experiment.** Every tool above
was already present; the only downloads were the two source snapshots in
`third_party/`. No Homebrew formula was added, changed or removed.

Single-threaded throughout. No SIMD, no JIT, no threads anywhere in this
experiment — which matters when reading §K.

---

## A. Exact FSST version and source revision

| | |
|---|---|
| upstream | https://github.com/cwida/fsst |
| commit | `e638d4cf8c26129d73c242a4127b42b975de5b63` ("Update libfsst12.cpp", 2026-04-07) |
| internal format id | `FSST_VERSION` = `20190218` (`libfsst.hpp:54`) |
| snapshot | `third_party/fsst`, **unmodified**; compiled from source by this project's CMake, and separately by FSST's own CMake as a cross-check |
| license | MIT (CWI, TU Munich, FSU Jena) |

The format facts this experiment relies on were read out of that source, not
assumed:

* Codes are single bytes `0..254`; **255 is the escape marker** and the byte
  after it is a literal (`fsst.h:74`, `#define FSST_ESC 255`).
* A symbol is 1–8 bytes. `fsst_decoder_t` holds `len[255]` and `symbol[255]`,
  each symbol packed little-endian into a `uint64` (`fsst.h:80-85`).
* `fsst_export`/`fsst_import` serialise the table into at most
  `FSST_MAXHEADER` = 2066 bytes (`fsst.h:102`): an 8-byte version word, a `zeroTerminated`
  flag, an 8-entry length histogram, then the used symbol bytes
  (`libfsst.cpp:517-596`).
* `fsst_export` hides the symbol count in byte 1 of the version word, and the
  length histogram says the same thing independently. This experiment
  cross-checks the two and refuses a table where they disagree
  (`src/fsstcol.cpp`, `import_table`), because every later step depends on
  "codes ≥ `nsymbols` never occur in valid data".
* `fsst_import` fills unused codes with the text `"corrupt"` — so an undefined
  code decodes to *something*, which is exactly why the check above matters.
* `fsst_compress` may compress fewer strings than asked when the output buffer
  fills, so the encoder loops on its return value rather than trusting it to
  finish (`src/encode.cpp`).

Two things the experiment deliberately does **not** assume, both named in the
brief and both confirmed to matter:

1. **A compressed byte is not a symbol.** `<255, b>` is two bytes but one
   logical unit, and `b` is data.
2. **Compressed bytes are not UTF-8 and symbol boundaries are not character
   boundaries.** FSST routinely cuts a multi-byte UTF-8 character in half. The
   unit test asserts this actually happens in its corpus rather than hoping it
   does, and the ClickBench URL column has 137,578 non-ASCII rows to exercise it.

---

## B. What was implemented, and what it does

| component | file | state |
|---|---|---|
| `LIKE` parser, DuckDB semantics | `src/pattern.cpp` | done |
| independent backtracking reference matcher | `src/pattern.cpp` | done (test oracle) |
| Thompson NFA over byte ranges | `src/dfa.cpp` | done |
| subset construction | `src/dfa.cpp` | done, with a state cap |
| **Hopcroft minimisation** | `src/dfa.cpp` | done |
| dead-state and accept-sink analysis | `src/dfa.cpp` | done (drives early exit) |
| FSST column format, loader, validator | `src/fsstcol.cpp` | done |
| encoder with mandatory per-row round-trip | `src/encode.cpp` | done |
| dense lifted table | `src/lift.cpp` | done |
| compact lifted table (default + exceptions) | `src/lift.cpp` | done |
| reverse lifted table + `.rev` sidecar | `src/lift.cpp`, `src/scan.cpp` | done |
| five scan modes + reference mode | `src/scan.cpp` | done |
| cross-column concatenation | `src/scan.cpp` | done in `raw`, `ref`, `decode`, `fsst` |
| CLI with machine-readable JSON | `src/cli.cpp` | done |
| unit + fuzz tests | `tests/test_unit.cpp` | 4,725 checks |
| DuckDB ground truth and checker | `scripts/truth.py`, `check.py` | done |
| benchmark runner and report generator | `scripts/bench.py`, `analyze.py`, `report.py` | done |
| demo | `demo_fsst.py` | done |

### How character semantics survive byte-level matching

`LIKE` is defined on characters; FSST symbols are arbitrary byte sequences. The
automaton is therefore built over **bytes**, with the character rules pushed
into its structure:

* `_` expands to a nine-branch sub-automaton accepting exactly one well-formed
  UTF-8 character — 1–4 bytes, no surrogates, no overlongs, nothing above
  U+10FFFF.
* `%` expands to a self-loop over all 256 byte values.

Modelling `%` as "any bytes" is exact for well-formed UTF-8 input, not an
approximation. UTF-8 is self-synchronising: lead bytes (`00`–`7F`, `C2`–`F4`)
and continuation bytes (`80`–`BF`) are disjoint, so a valid UTF-8 literal can
only ever align on a character boundary, and the `_` sub-automaton cannot start
on a continuation byte. The 2,528 DuckDB checks in §D include Cyrillic,
Japanese, emoji, and `_` adjacent to multi-byte characters, so the argument is
also tested, not just argued.

### Lifting

For every state `s` and defined code `c`, precompute
`sym[s][c] = delta*(s, symbol_bytes(c))`. Escapes are *not* lifted: the scan
loop sees code 255, reads the next byte as data and applies the plain byte
transition. Undefined codes and code 255 itself map to the reject sink, so
corrupt input can never be read as a match.

States are renumbered so that **0 is the reject sink and 1 the accept sink**,
whether or not the pattern reaches them. That makes `s < 2` the single
early-exit test in every scan loop while `accept[s]` still gives the verdict,
which is where the anchored-pattern speedups come from.

### Making the backward scan exact

A right-to-left walk over FSST bytes is *not* automatically valid: codes are
`0..254`, so a `0xFF` in the stream is either an escape marker or an escaped
literal `0xFF`, and only a forward parse can tell. The experiment stores a
4-byte-per-row sidecar `.fsst.rev`: the offset just past the row's **last**
escape sequence. Every byte at or after it is a plain one-byte code, so the
backward walk over that region is unambiguous; a scan that reaches the boundary
undecided falls back to a forward scan of that row, and the fallback count is
reported. §H measures both this and a cheaper 1-bit-per-row alternative.

---

## C. Which parts come from the original TokenLIKE design

Reused as **ideas**, re-implemented from scratch here:

* the central move — lift a byte-level `LIKE` DFA onto compression symbols and
  scan the compressed units;
* early exit via a reject sink and an accept sink, with the sink states
  renumbered to 0 and 1 so one comparison covers both;
* a reverse automaton for suffix-anchored patterns;
* the dense-versus-compact transition-table question;
* the correctness method: DuckDB as the only authority, exact row-id set
  comparison, a bit-packed LSB-first match bitmap, sorted `uint32` row-id files,
  tab-separated pattern lists, and an "exit 2 means unsupported, never a silent
  fallback" CLI contract.

Reused as **artifacts**, by copying, with the column names remapped:

* `patterns/edge.txt` — the 36 DuckDB-verified adversarial patterns against the
  51-row edge fixture, plus 9 new FSST-specific ones;
* `patterns/fuzz.txt` — all 300 seeded patterns, so both experiments face the
  same adversary;
* the raw datasets, read from the original `data/` directory and copied into
  this experiment by `fsst_encode` (`.raw`/`.rawoff` only).

**Not** reused: no TokenLIKE source file is compiled, linked, or included here.
`pattern.cpp`, `dfa.cpp`, `lift.cpp`, `scan.cpp`, `fsstcol.cpp` and the test
suite are new code. The one place a TokenLIKE binary appears is §J, where it is
built from a read-only snapshot purely to be measured against.

---

## D. Test pass/fail counts

Everything below is from the single final run of `bash scripts/check_all.sh`.

### In-process unit and fuzz tests — `build/rel/test_unit`

**4,725 checks, 0 failures.** The oracle is the backtracking reference matcher,
written independently of the DFA, so this is not the implementation checking
itself. Coverage:

| group | what it covers |
|---|---|
| pattern parsing | empty, `%%%%` collapsing, `_`, backslash-as-literal, 4-byte characters, invalid and truncated UTF-8 rejected |
| DFA limits | over-`--max-states` rejected; `%` collapses to the accept sink; empty pattern accepts only the empty string |
| reverse-safe boundary | 7 hand-built code streams including `FF FF`, `FF FF FF FF`; dangling marker rejected |
| robustness | 12 distinct malformed inputs (see §K/M) |
| deterministic matrix | 35-value fixture × 47 patterns × 5 modes, each cross-checked against the reference, plus exhaustive lifted-table verification per state × code |
| compressibility extremes | 4× compressible, barely-compressible random ASCII, and a table deliberately starved of symbol slots so rare bytes must escape |
| seeded random | 2 seeds × 12 corpora × 30 patterns, mixed ASCII/2-/3-/4-byte UTF-8, patterns cut from real substrings on character boundaries |

The matrix test asserts that at least one UTF-8 character *is* split by a symbol
boundary and that the fixture *does* produce escapes, so a corpus that stopped
exercising those paths would fail rather than quietly pass.

### Against DuckDB, on real data, all six modes

DuckDB reads the original canonical parquet files and never sees anything this
experiment produced, so it cannot agree by construction. Row-id sets are
compared exactly.

| pattern list | patterns | rows per column | checks | passed | failed | skipped |
|---|---|---|---|---|---|---|
| `patterns/edge.txt` | 45 | 51 | 270 | **270** | 0 | 0 |
| `patterns/core.txt` | 30 | 1,000,000 | 180 | **180** | 0 | 0 |
| `patterns/wildcard.txt` | 23 | 1,000,000 | 138 | **138** | 0 | 0 |
| `patterns/concat.txt` | 11 | 1,000,000 | 66 | **44** | 0 | 22 |
| `patterns/scales.txt` | 16 | 1k / 10k / 100k / 1M | 96 | **96** | 0 | 0 |
| `patterns/fuzz.txt` | 300 | 20,016 | 1,800 | **1,800** | 0 | 0 |
| **total** | **425** | | **2,550** | **2,528** | **0** | **22** |

The 22 skips are the CLI correctly exiting 2 for `--col2` in `fsst-compact` and
`fsst-rev`, which are not implemented for concatenation (§E). Concatenation
itself *is* verified in the compressed domain: the 11 concat patterns pass in
`fsst` mode. `check.py` exits non-zero if nothing was verified, so an
all-skipped run cannot read as a pass.

### Structural validation

All 12 compressed columns pass `--validate` — the eleven in §F plus the one
`demo_fsst.py` builds: monotonic offsets, `off[0] == 0`,
terminal offset equal to the payload, every code below `nsymbols`, no dangling
escape marker, and `.fsst.rev` re-derived and compared row by row.

### Round-trip

`fsst_encode` decodes every row with upstream `fsst_decompress` and compares it
with the source bytes before writing anything. Across all 12 columns —
**4,332,067 rows, 267,373,051 source bytes — 0 mismatches.** A single mismatch
aborts the encode and leaves no files behind.

### Sanitizers

AddressSanitizer + UndefinedBehaviorSanitizer, `build/asan`
(`build/asan_sweep.out`). **0 sanitizer reports in total** across:

* `test_unit` — 4,725 checks, 0 failures, 71 s;
* the whole `patterns/edge.txt` list through the sanitizer CLI — 45 patterns ×
  6 modes = **270 checks, all matching DuckDB**;
* 18 scans of the 1,000,000-row URL column — three patterns (`http://%`,
  `%.php`, `%/v_/%`) × all six modes, each with `--validate`, all six modes
  returning identical match counts (517,519 / 12,714 / 165) and `malformed=0`;
* structural validation of `edge/values`, `fuzz/values`, `clickbench/URL_1m`
  and `tpch/o_comment_1m`.

The full 2,528-check suite was **not** run under sanitizers: at `-O0` with both
sanitizers a 1M-row scan is roughly 30× slower, which puts the whole suite in
the hours (it was started, left running, and abandoned on that basis). The
release build runs it in 60 s and the sweep above covers every code path in it. Said plainly so the sanitizer claim is not read as wider than
it is.

One caveat, stated plainly: UBSan reports 10 misaligned 8-byte stores inside
**upstream** `fsst_decompress`, from
`#define FSST_UNALIGNED_STORE(dst,src) memcpy((unsigned long long*)(dst), &(src), 8)`
(`third_party/fsst/fsst.h:156`, expanded at lines 163-201). The `memcpy` is byte-wise and safe; the
cast of an unaligned destination to `unsigned long long*` is what UBSan objects
to. FSST ships `-DFSST_MUST_ALIGN` for platforms that cannot take unaligned
access, so this is deliberate upstream behaviour. Rather than patch the
snapshot, `tests/ubsan.supp` suppresses the `alignment` check for
`third_party/fsst` only — and with that suppression the run is *completely*
clean, so any remaining report would belong to this experiment's code.

### Cross-check against a different implementation

`scripts/compare_tokenlike.py` runs this experiment and the original TokenLIKE
binary over columns derived from the same raw bytes. **15 of 15 patterns: all
seven mode combinations of the two independent implementations returned
identical row-id sets.**

---

## E. Features that remain unsupported

Each produces an explicit error and never a silent fall back to full decoding.

| not supported | behaviour | why |
|---|---|---|
| `--col2` in `fsst-compact` and `fsst-rev` | exit **2**, message naming the supported modes | Not implemented. Concatenation *is* supported in the compressed domain through `fsst`; the compact and reverse variants would each need a second lifted table per column and, for the reverse case, a defined scan order across the boundary. |
| patterns needing more than `--max-states` (default 20,000) DFA states | exit 1, `"pattern needs more than N DFA states; raise --max-states or simplify the pattern"` | A deliberate bound. The hard ceiling is 65,535, because state ids are `uint16`; above that the build refuses rather than truncating. |
| `ESCAPE` clauses, e.g. `LIKE 'a\%b' ESCAPE '\'` | not parsed | DuckDB without an explicit `ESCAPE` treats backslash as an ordinary byte, which is the semantics this experiment matches and verifies. Note the published prior art assumes the *other* convention (§K). |
| patterns that are not well-formed UTF-8 | exit 1 at parse time | DuckDB `VARCHAR` patterns always are. Guessing byte semantics would make the comparison meaningless. |
| input strings that are not well-formed UTF-8 | out of scope | The datasets are byte-exact UTF-8 by construction (the original pipeline builds them with DuckDB's `decode()`, which rejects invalid UTF-8). `_` is defined on characters, so there is nothing exact to match against for invalid input. The reference matcher and the DFA agree that `_` matches no invalid sequence, but this is not claimed as verified behaviour. |
| `--affinity N` | accepted, reported in JSON as `requested-but-unsupported-on-darwin` | macOS has no `sched_setaffinity`. Reported rather than silently ignored. |
| multi-threading, SIMD, JIT | not implemented | Out of scope; see §K for what this costs against the prior art. |
| DuckDB / SQL engine integration | not implemented | Separate scope. The C++ surface is a clean seam for it: `build_dfa`, `lift_dense`, `scan_fsst` and the `--json` CLI. |

---

## F. Compression on real data

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

FSST calibrates one symbol table per column. The sidecar `.fsst.rev` is listed
separately because it is this experiment's addition, not part of FSST.

Reading the table: on ClickBench URL, codes alone give **1.772×**; the symbol
table costs 565 bytes (negligible); the 4-byte-per-row reverse sidecar is the
expensive part, pulling the end-to-end figure down to 1.641×. TPC-H comments
reach **3.201×** and contain **no escapes at all** — plain ASCII natural
language is covered completely by 216 symbols. `edge/values` compresses to
*worse* than its original size once the sidecar is counted (0.809×); 51 rows is
far too little to amortise anything, and it is in the table precisely because an
adversarial fixture should be visible rather than hidden.

A note on where the escapes are: on URL only **0.15%** of logical units are
escapes, but they are spread over **4.4%** of rows. That distinction is what
§H's sidecar discussion turns on — per-row metadata is decided by the fraction
of *rows* touched, not the fraction of units.

---

## G. Per-pattern performance

Machine: Apple M2, macOS-26.5.2-arm64-arm-64bit-Mach-O  
Best of 7 runs after 2 warmups; best-of-N wall time of the scan only; column load and automaton build are outside it.

| pattern | matches | raw ns/row | decode ns/row | fsst ns/row | fsst-compact ns/row | fsst-rev ns/row | fsst vs decode | fsst-rev vs decode | best vs raw |
|---|---|---|---|---|---|---|---|---|---|
| `http://%` | 517,519 | 3.28 | 30.59 | 3.02 | 6.42 | 57.41 | 10.14x | 0.53x | 1.09x |
| `/static/%` | 0 | 1.54 | 28.16 | 1.22 | 1.68 | 56.50 | 22.99x | 0.50x | 1.26x |
| `http://irr.ru%` | 341,755 | 5.68 | 32.95 | 3.29 | 7.77 | 57.74 | 10.01x | 0.57x | 1.72x |
| `%.php` | 12,714 | 103.85 | 154.46 | 58.72 | 117.58 | 2.50 | 2.63x | 61.79x | 41.54x |
| `%.m3u8` | 0 | 103.74 | 154.47 | 58.79 | 141.80 | 2.37 | 2.63x | 65.29x | 43.85x |
| `%.html` | 7,683 | 104.20 | 154.60 | 58.88 | 144.98 | 2.53 | 2.63x | 61.03x | 41.13x |
| `http://irr.ru/index.php?showalbum/login-leniya7777294,938303130` | 58,976 | 19.93 | 50.69 | 8.33 | 30.32 | 3.49 | 6.08x | 14.53x | 5.71x |
| `%yandex%` | 27,864 | 102.68 | 153.18 | 58.17 | 116.05 | 61.46 | 2.63x | 2.49x | 1.77x |
| `%/catalog/%` | 14,286 | 103.16 | 153.28 | 58.20 | 132.97 | 62.08 | 2.63x | 2.47x | 1.77x |
| `%/v_/%` | 165 | 103.92 | 154.17 | 58.50 | 134.00 | 62.76 | 2.64x | 2.46x | 1.78x |
| `%сезон%` | 2,116 | 103.46 | 154.34 | 58.70 | 107.37 | 62.66 | 2.63x | 2.46x | 1.76x |
| `http%irr%php%` | 336,463 | 67.45 | 112.10 | 38.44 | 90.76 | 61.60 | 2.92x | 1.82x | 1.75x |
| `http://_rr.ru/%php%` | 335,775 | 8.83 | 38.58 | 4.47 | 11.15 | 58.04 | 8.63x | 0.66x | 1.98x |
| `%regular%` | 224,710 | 43.58 | 56.49 | 12.31 | 31.34 | 13.54 | 4.59x | 4.17x | 3.54x |
| `%s` | 75,650 | 23.24 | 33.24 | 11.85 | 37.85 | 2.70 | 2.80x | 12.29x | 8.60x |

### Automaton construction and table size

| pattern | states | pre-min states | DFA build us | lift build us | table bytes | units/row (fsst) | early-exit rows | fallback rows (rev) |
|---|---|---|---|---|---|---|---|---|
| `http://%` | 9 | 9 | 95.8 | 4.5 | 9,225 | 2.35 | 999,725 | 43,769 |
| `/static/%` | 10 | 10 | 104.0 | 4.9 | 10,250 | 1.00 | 999,735 | 43,769 |
| `http://irr.ru%` | 15 | 15 | 128.8 | 5.1 | 15,375 | 2.87 | 999,723 | 43,769 |
| `%.php` | 7 | 5 | 123.2 | 4.1 | 7,175 | 49.89 | 0 | 3,596 |
| `%.m3u8` | 8 | 6 | 138.2 | 3.4 | 8,200 | 49.89 | 0 | 3,596 |
| `%.html` | 8 | 6 | 137.8 | 3.4 | 8,200 | 49.89 | 0 | 3,596 |
| `http://irr.ru/index.php?showalbum/login-leniya7777294,938303130` | 66 | 65 | 417.0 | 19.0 | 67,650 | 6.24 | 926,828 | 3,596 |
| `%yandex%` | 8 | 12 | 286.2 | 3.4 | 8,200 | 48.94 | 23,785 | 43,285 |
| `%/catalog/%` | 11 | 18 | 395.0 | 5.2 | 11,275 | 49.23 | 14,285 | 43,453 |
| `%/v_/%` | 14 | 46 | 1,052.8 | 6.0 | 14,350 | 49.89 | 126 | 43,761 |
| `%сезон%` | 12 | 20 | 435.4 | 4.5 | 12,300 | 49.81 | 1,916 | 43,326 |
| `http%irr%php%` | 12 | 18 | 385.0 | 4.4 | 12,300 | 33.36 | 328,652 | 43,769 |
| `http://_rr.ru/%php%` | 26 | 39 | 440.2 | 9.2 | 26,650 | 3.76 | 985,981 | 43,769 |
| `%regular%` | 9 | 14 | 308.8 | 24.4 | 9,225 | 13.35 | 216,740 | 0 |
| `%s` | 4 | 2 | 60.2 | 2.7 | 4,100 | 9.32 | 0 | 0 |

A plain reading of that table:

* **Anchored prefixes** are where compressed-domain matching is unambiguously
  worth it. `http://%` reads **1.0–2.9 symbol units per row** and finishes in
  1.22–3.29 ns/row — **10–23× faster than decode-then-match**, and faster
  than scanning the uncompressed strings. `/static/%` matches nothing and costs
  1.22 ns/row, because the reject sink is reached on the first unit of
  essentially every row.
* **Anchored suffixes** need the reverse scan. `%.php` costs 2.50 ns/row in
  `fsst-rev` against 154.46 for decode (**61.8×**) and 103.85 for raw
  (**41.5×**). The same pattern scanned forward costs 58.72, because a suffix
  cannot be decided until the last unit.
* **Exact literals** benefit from both ends: 3.49 ns/row reverse, 8.33 forward,
  50.69 decode.
* **Unanchored `contains`** is the honest weak spot. `%yandex%` must read every
  unit of every non-matching row, so the gain over raw is bounded by how many
  fewer units there are: 58.17 against 102.68, i.e. **1.77×**, which is almost
  exactly the 1.772× compression ratio. Against decode it is 2.63×.
* **`_` costs nothing extra at scan time.** `%/v_/%` runs at 58.50 ns/row,
  within noise of `%yandex%` at 58.17 — the UTF-8 character sub-automaton is
  paid for once, in 46 → 14 states during construction, not per row.
* **`fsst-rev` is the wrong tool for prefixes** — 57–58 ns/row, roughly 0.5×
  decode, because it walks the whole row from the wrong end. A planner would
  pick the direction from the pattern shape; this experiment exposes both modes
  separately rather than hiding the choice.
* **Decode is slower than raw in every single row of the table.** That is the
  result that makes the whole exercise worthwhile: if a system has to
  decompress before matching, it is strictly worse off than if it had never
  compressed, and the compressed-domain scan is what recovers the loss.

### Scaling

`bench/scales.json`, same four shapes at 1k / 10k / 100k / 1M rows
(`fsst` ns/row, then `fsst-rev` for the suffix):

| shape | 1k | 10k | 100k | 1M |
|---|---|---|---|---|
| `http://%` (fsst) | 1.46 | 1.45 | 1.44 | 3.04 |
| `%.php` (fsst-rev) | 1.88 | 2.05 | 2.06 | 2.50 |
| `%yandex%` (fsst) | 39.92 | 41.92 | 42.22 | 58.44 |
| `%/v_/%` (fsst) | 39.62 | 41.77 | 42.15 | 58.59 |

Per-row cost is flat from 1k to 100k and rises roughly 1.4× at 1M. That is
memory, not algorithm: 50 MB of codes no longer fits the cache hierarchy the way
4 MB does. The anchored patterns move least, because they touch only the first
unit or two of each row regardless of column size.

---

## H. Table construction cost and memory

Construction is per query, not per row, and it is small: **60–1,053 µs** of
DFA build (parse → NFA → subset construction → Hopcroft) plus **2.7–24.4 µs** of
lifting, so **63 µs to 1.06 ms** end to end, against scans of 2.5–60 ms
for a million rows. Even the most expensive pattern measured, `%/v_/%` at
1,053 µs of DFA build, is 1.8% of its own scan.

Minimisation earns its place where wildcards interact: `%/v_/%` goes from 46
states to 14 and `%/catalog/%` from 18 to 11. (Where the "before" number is
*smaller* — `%.php` shows 5 → 7 — the difference is the two reserved sink slots,
which the canonical numbering always allocates even when the pattern never
reaches them.)

Lifting is the cheap half, because the alphabet is only 255 codes.
This is a structural advantage of FSST over a large LLM token table; §J measures
it against TokenLIKE's 65,536-token alphabet.

### Dense versus compact lifted tables

The DFA's own 256-wide byte table is in both figures, since the escape path needs it.

| pattern | states | dense ns/row | compact ns/row | dense total table B | compact total table B | compact/dense size | compact/dense time |
|---|---|---|---|---|---|---|---|
| `http://%` (prefix) | 9 | 3.04 | 6.62 | 9,225 | 5,065 | 0.55x | 2.18x |
| `%.php` (suffix) | 7 | 58.73 | 117.57 | 7,175 | 3,957 | 0.55x | 2.00x |
| `%yandex%` (contains) | 8 | 58.15 | 116.22 | 8,200 | 4,552 | 0.56x | 2.00x |
| `%/v_/%` (underscore) | 14 | 58.72 | 134.03 | 14,350 | 8,504 | 0.59x | 2.28x |
| `http%irr%php%` (complex) | 12 | 38.32 | 90.41 | 12,300 | 6,872 | 0.56x | 2.36x |
| `http://irr.ru/index.php?showalbum/log...` (long literal) | 66 | 8.39 | 30.48 | 67,650 | 37,086 | 0.55x | 3.63x |

**The dense table wins, and the brief was right to say not to assume otherwise —
it just turns out the assumption that needed checking was the opposite one.**
The compact representation is consistently 0.55–0.59× the size, and
consistently **2.0–3.6× slower**. At 255 codes a dense row is 512 bytes, so a
whole 14-state automaton is 7 KiB and sits in L1; the saving buys nothing, while
the default-plus-exception lookup costs a branch, a mask load, a popcount and a
second indirection on every unit. All later measurements use the dense table,
and `fsst-compact` is kept as a mode so the claim stays reproducible rather than
anecdotal.

### Cost of making the backward scan exact

The 4-byte-per-row `.fsst.rev` sidecar is 4,000,000 B, 8.0% on top of the 49,965,040 B of codes. A 1-bit-per-row has-escape flag would be 125,000 B, 0.25%, but then every one of the 43,769 rows with an escape (4.4%) has to be scanned forward instead.

| pattern | shape | offset sidecar ns/row | 1-bit flag ns/row | offset fallback rows | 1-bit fallback rows | offset units/row | 1-bit units/row |
|---|---|---|---|---|---|---|---|
| `%.php` | suffix | 2.56 | 8.61 | 3,596 | 43,769 | 1.28 | 5.46 |
| `%.html` | suffix | 2.51 | 8.59 | 3,596 | 43,769 | 1.27 | 5.45 |
| `%.m3u8` | suffix | 2.34 | 8.50 | 3,596 | 43,769 | 1.25 | 5.43 |
| `http://irr.ru/index.php?showalbum/log...` | exact | 3.44 | 3.96 | 3,596 | 43,769 | 2.17 | 2.35 |
| `%yandex%` | contains | 61.43 | 58.76 | 43,285 | 43,769 | 50.01 | 48.57 |

This is the trade-off the brief asked to be measured rather than waved at, and
it is a real one:

* The **4-byte offset sidecar** costs **8.0%** on top of the compressed column
  (4,000,000 B against 49,965,040 B) and lets `%.php` finish in 2.56 ns/row
  reading 1.28 units, falling back on only **3,596 rows (0.36%)** — far fewer
  than the 43,769 rows that contain an escape, because a suffix pattern usually
  decides long before the walk reaches the escape region.
* The **1-bit has-escape flag** costs **0.25%** (125,000 B) but forces all
  43,769 escape rows down the forward path: 8.61 ns/row, 5.46 units.
* So the sidecar buys **3.4×** on suffix patterns for 8% more storage. For an
  exact literal the gap nearly closes (3.44 vs 3.96 ns/row), and for a
  `contains` pattern it reverses within noise — there is nothing to save when
  the scan has to read everything anyway.

An implementation that cared about storage would carry the 1 bit and accept the
slower tail; one that cared about suffix latency would carry the 4 bytes.
Neither is free, and **the published prior art needs neither** — it resolves the
same ambiguity inside the automaton with "pseudo-end states" plus a one-byte
lookahead (§K). That is the better engineering answer and this experiment does
not match it.

### Peak process memory

| mode | min MiB | median MiB | max MiB | what it has to hold |
|---|---|---|---|---|
| raw | 43.0 | 101.6 | 105.5 | 88 MB raw column + offsets |
| decode | 26.6 | 64.8 | 68.7 | 50 MB codes + offsets + one decode buffer |
| fsst | 26.6 | 64.8 | 68.7 | 50 MB codes + offsets + lifted table |
| fsst-compact | 26.6 | 64.8 | 68.7 | 50 MB codes + offsets + compact table |
| fsst-rev | 30.5 | 68.8 | 72.6 | 50 MB codes + offsets + .rev + two lifted tables |

The compressed modes hold ~37 MiB less than the raw baseline at 1M rows, which
is the compression ratio showing up as resident memory. `fsst-rev` carries about
4 MiB more than `fsst` — the sidecar and the second lifted table.

### Compression cost

| column | rows | fsst_create ms | fsst_compress ms | MB/s compressed |
|---|---|---|---|---|
| `clickbench/URL_100k` | 100,000 | 1 | 14 | 531 |
| `clickbench/URL_1m` | 1,000,000 | 1 | 160 | 554 |
| `clickbench/Referer_1m` | 1,000,000 | 1 | 144 | 551 |
| `tpch/l_comment_1m` | 1,000,000 | 1 | 88 | 300 |
| `tpch/o_comment_1m` | 1,000,000 | 1 | 92 | 525 |
| `dbtext/urls_100k` | 100,000 | 1 | 12 | 518 |

`fsst_create` samples the input and needs about **1 ms regardless of column
size**; `fsst_compress` runs at **300–554 MB/s** single-threaded on this
machine. Building the four 1M-row columns, including the mandatory per-row
round-trip verification, takes well under a minute in total.

---

## I. FSST-LIKE versus full FSST decoding

The decode baseline is the honest one the brief asked for: `fsst_decompress()`
the whole row with the upstream inline decoder, then run the *same* minimised
byte DFA with the *same* early exit over the decompressed bytes. The only
difference between the two columns is where the bytes come from.

| pattern shape | decode ns/row | best compressed-domain ns/row | speedup |
|---|---|---|---|
| prefix, 0 matches (`/static/%`) | 28.16 | 1.22 (`fsst`) | **22.99×** |
| prefix (`http://%`) | 30.59 | 3.02 (`fsst`) | **10.14×** |
| prefix (`http://irr.ru%`) | 32.95 | 3.29 (`fsst`) | **10.01×** |
| prefix with `_` | 38.58 | 4.47 (`fsst`) | **8.63×** |
| suffix (`%.m3u8`) | 154.47 | 2.37 (`fsst-rev`) | **65.29×** |
| suffix (`%.html`) | 154.60 | 2.53 (`fsst-rev`) | **61.03×** |
| suffix (`%.php`) | 154.46 | 2.50 (`fsst-rev`) | **61.79×** |
| exact literal | 50.69 | 3.49 (`fsst-rev`) | **14.53×** |
| suffix, TPC-H (`%s`) | 33.24 | 2.70 (`fsst-rev`) | **12.29×** |
| contains, TPC-H (`%regular%`) | 56.49 | 12.31 (`fsst`) | **4.59×** |
| multi-`%` (`http%irr%php%`) | 112.10 | 38.44 (`fsst`) | **2.92×** |
| contains (`%yandex%`) | 153.18 | 58.17 (`fsst`) | **2.63×** |
| contains (`%/catalog/%`) | 153.28 | 58.20 (`fsst`) | **2.63×** |
| contains with `_` (`%/v_/%`) | 154.17 | 58.50 (`fsst`) | **2.64×** |
| Cyrillic contains (`%сезон%`) | 154.34 | 58.70 (`fsst`) | **2.63×** |

**Range: 2.63× to 65.29×, never below 1×.** The spread is explained entirely by how
early the automaton can stop: anchored patterns stop after one or two units and
win by an order of magnitude or more; unanchored patterns must read everything
and win only by the ratio of units to bytes.

One caveat on the decode baseline: a smarter implementation could decode
*incrementally* and stop early, which would close much of the prefix gap,
although it would still pay for materialising bytes it then has to scan. This
experiment implements decode-then-match as specified, so the prefix numbers
should be read as "versus full decode", not "versus the best conceivable
decoding scanner".

---

## J. FSST-LIKE versus raw strings, and versus the original TokenLIKE

### Versus raw

The raw baseline is strong on purpose: the same minimised byte DFA with the same
early exit, over the uncompressed bytes. It is not a naive matcher.

Compressed-domain matching still beats it on every shape measured —
**1.09× to 43.85×** (`best vs raw` in §G) — for the simple reason that the
compressed representation has fewer units to look at and the automaton can stop
on a unit boundary. For unanchored patterns the margin is the compression ratio
and nothing more (1.77× on URL, 3.54× on TPC-H comments, where compression is
better). For suffixes the margin is enormous because raw has no equivalent of
the reverse-scan early exit on byte data without the same kind of metadata.

### Versus the original TokenLIKE, same machine, same rows

Both systems were built and run here, on this laptop, over columns derived from
the same raw bytes. TokenLIKE comes from a read-only `git archive` snapshot of
`origin/main` at `183d9474e733f5435342075d6cd3a673f55f5a08`, configured with
`loaders (Person B) = ON, matcher (Person A) = ON`. Both CLIs report their
*minimum* per-row time over 7 runs after 2 warmups, the only statistic both
expose.

(The brief names `v3-freeze` / `c58d066` as the frozen release. That commit is
not present in this local clone — it was pushed after this clone's last fetch —
and fetching would have modified the original repository, which was out of
bounds. The comparison therefore uses the newest locally available integrated
commit, and says so.)
| pattern | shape | FSST-LIKE best ns/row (mode) | TokenLIKE token ns/row | ratio | FSST units/row | TokenLIKE tokens/row |
|---|---|---|---|---|---|---|
| `http://%` | prefix | 3.09 (fsst) | 2.99 | 0.97x | 2.35 | 1.52 |
| `/static/%` | prefix, 0 matches | 1.25 (fsst) | 2.05 | 1.64x | 1.00 | 1.00 |
| `http://irr.ru%` | prefix | 3.38 (fsst) | 5.24 | 1.55x | 2.87 | 3.06 |
| `%.php` | suffix | 2.56 (fsst-rev) | 2.19 | 0.86x | 1.28 | 1.01 |
| `%.m3u8` | suffix, 0 matches | 2.39 (fsst-rev) | 2.22 | 0.93x | 1.25 | 1.00 |
| `%.html` | suffix | 2.51 (fsst-rev) | 2.26 | 0.90x | 1.27 | 1.01 |
| `http://irr.ru/index.php?showalbum/log...` | exact literal | 3.51 (fsst-rev) | 12.21 | 3.48x | 2.17 | 7.46 |
| `%yandex%` | contains | 57.92 (fsst) | 45.20 | 0.78x | 48.94 | 35.54 |
| `%/catalog/%` | contains | 57.80 (fsst) | 50.82 | 0.88x | 49.23 | 35.76 |
| `%/v_/%` | contains with '_' | 58.26 (fsst) | 71.52 | 1.23x | 49.89 | 36.13 |
| `%сезон%` | Cyrillic contains | 58.28 (fsst) | 29.46 | 0.51x | 49.81 | 36.09 |
| `http%irr%php%` | multi-% | 38.12 (fsst) | 36.18 | 0.95x | 33.36 | 25.14 |
| `http://_rr.ru/%php%` | prefix with '_' | 4.46 (fsst) | 7.85 | 1.76x | 3.76 | 4.97 |
| `%regular%` | contains (TPC-H) | 12.65 (fsst) | 12.92 | 1.02x | 13.35 | 9.86 |
| `%s` | suffix (TPC-H) | 2.71 (fsst-rev) | 2.38 | 0.88x | 1.00 | 1.00 |

**Row ids: identical on 15 of 15 patterns, across all seven mode combinations of
the two implementations.** Two independently written matchers over two different
compression schemes agreeing exactly is the strongest correctness evidence in
this report.

On speed the result is a genuine split, and the reason is instructive:

* **FSST-LIKE wins** on exact literals (**3.51×**), prefixes with `_`
  (**1.75×**), long and zero-match prefixes (**1.55–1.64×**) and `contains`
  with `_` (**1.22×**).
* **TokenLIKE wins** on unanchored scans: `%сезон%` (0.50×), `%yandex%`
  (0.76×), `%/catalog/%` (0.87×), and slightly on pure suffixes (0.85–0.93×).
* **`http://%` and `%regular%` are ties** (0.99× and 1.03×), within the
  run-to-run spread of either system.

The explanation is in the last two columns. **Scan cost tracks units per row,
not bytes per row.** A 65,536-entry LLM token table parses a URL into ~36.1
tokens; FSST's 255-symbol table parses the same URL into ~49.9 codes. FSST's
*bytes* are fewer (49.97 MB of codes against 72.27 MB of `uint16` tokens — FSST
is 1 byte per unit, TokenLIKE 2), so FSST wins on memory footprint and on
anything that stops early, while TokenLIKE wins when the whole row must be read
because it has fewer things to read.

Two secondary results favour the small alphabet decisively:

* **Automaton construction is ~31× cheaper**: for `http://%`, median of 9 runs,
  **100.7 µs** here (range 98.5–105.0) against **3,099.9 µs** for TokenLIKE
  (range 3,078.4–3,185.0), because lifting onto 255 symbols is two orders of
  magnitude less work than lifting onto 65,536 tokens.
* **The lifted table is smaller**: 9.0 KiB against 12.6 KiB for the same
  pattern, and it gets there with a plain dense table (§H), whereas TokenLIKE
  has to use its compact encoding — a dense table over 65,536 tokens would be
  1,152 KiB for that automaton.

Caveats, so these numbers are not over-read: one machine, one thread, warm page
cache, best-of-7; the two systems use different compressors on the same source
text, so this compares *schemes plus implementations*, not algorithms in
isolation.

---

## K. Comparison with existing FSST-LIKE prior art

**The core idea of this experiment is not new.** It is published work, and part
of it ships in a production columnar format. Stating that plainly is the most
important sentence in this report.

### K.1 Pop, Riedl, Neumann — "Compression-Aware LIKE: Matching Patterns in the FSST Domain", DaMoN'26

ACM DOI [10.1145/3789237.3809128](https://doi.org/10.1145/3789237.3809128),
22nd International Workshop on Data Management on New Hardware, May 31 – June 5
2026, Bengaluru. PDF: <https://db.in.tum.de/~pop/fsstlike.pdf>. Reference
implementation: <https://github.com/calin2110/FSST-LIKE-Matching>, inspected at
commit `b1eb3ab9c63ea0199a381b92371a3154190b4406` (2026-06-25), snapshot in
`third_party/priorart-fsst-like`.

They state the thesis this experiment set out to test, and had already answered
it: *"instead of decompressing data to match patterns, we compress the pattern
by constructing finite automata that parse FSST-encoded symbol sequences
directly."*

What they do that this experiment does not:

| | prior art | this experiment |
|---|---|---|
| automaton | specialised per sub-pattern — start, middle, end — built trie-first in the style of Aho–Corasick, directly over code sequences, then stitched together | one minimised byte-level DFA, lifted onto the code alphabet; uniform over any mix of `%` and `_` |
| escape ambiguity on backward scans | solved *inside* the automaton: "pseudo-end states" plus a one-byte lookahead to check the preceding byte is not an escape marker — **no per-row metadata at all** | solved with a 4-byte-per-row `.fsst.rev` sidecar, +8.0% storage (§H) |
| code generation | interpreted **and** LLVM-JIT-compiled kernels | interpreted only |
| SIMD | SIMD-accelerated self-loop traversal | none |
| threads | morsel-driven multithreaded, near-linear to 10 cores | single-threaded |
| complexity result | prefix/suffix best case O(m) → O(1), worst case O(m) → O(min(ω−1, m)) | same early-exit structure, not analysed formally |
| reported gains | **2.5–17×** over decompression-based matching (HybridStringSearch and Vectorscan on decompressed data), on TPC-H, StackOverflow, IMDB and PublicBI | 2.6–65× over full decode, on ClickBench and TPC-H |

**Those two speedup ranges are not comparable** and this report does not treat
them as such: different machine (their 10-core x86 Linux box versus an Apple
M2), different datasets, different baselines (tuned SIMD substring search versus
a byte DFA), different statistics. Where the shapes do line up, their
single-threaded JIT implementation is doing more work per cycle than this one —
they have JIT and SIMD and this experiment has neither.

### K.2 The one thing this experiment adds: `_` in the compressed domain

The paper is explicit about its scope: *"Considering only the '%' wildcard
character that matches any number of characters, we categorise sub-patterns into
three distinct classes…"* Their reference implementation does parse `_` — there
is an `UnderscorePattern` class in `include/pattern.hpp:55` — but all three of
its automaton builders are stubs:

```
third_party/priorart-fsst-like/src/pattern.cpp:229   throw std::runtime_error("Not implemented");
third_party/priorart-fsst-like/src/pattern.cpp:233   throw std::runtime_error("Not implemented");
third_party/priorart-fsst-like/src/pattern.cpp:237   throw std::runtime_error("Not implemented");
```

(`UnderscorePattern::createStartAutomaton`, `createMiddleAutomaton`,
`createEndAutomaton`.) So in the published implementation a pattern containing
`_` cannot be matched in the compressed domain at all.

This experiment supports `_` as **one Unicode character**, in the compressed
domain, at no measurable per-row cost (§G: `%/v_/%` 58.37 ns/row against
`%yandex%` 57.95), verified against DuckDB on 1M real rows including 137,578
non-ASCII ones. It works because the automaton is byte-level and the character
rule is inside it, so a symbol boundary falling in the middle of a 3-byte
character is just another state transition. An approach built on code sequences
has to handle "`_` consumed 2 of the 3 bytes this symbol expands to" explicitly,
which is presumably why it was left unimplemented.

A second, smaller difference: the prior-art parser treats backslash as an escape
character (`pattern[idx-1] == '\\'` in `splitIntoSubpatterns`), i.e.
Postgres/Umbra default semantics. DuckDB does not, without an explicit `ESCAPE`
clause — measured, see `scripts/truth.py --semantics`. Neither is wrong; they
are different dialects, and this experiment matches the one its oracle uses.

### K.3 Vortex

The `vortex-fsst` crate in the Vortex columnar format (`vortex-data/vortex`)
has an FSST fast path for `LIKE`, covering `prefix%` and `%needle%`, with
documented pattern byte limits (253 bytes for prefixes, 254 for needles) above
which it falls back to decompressing and comparing. PR
[#9728](https://github.com/vortex-data/vortex/pull/9728) adds `%suffix`
pushdown using a **backward scan**, explicitly because a forward DFA cannot exit
early on a suffix — the same reasoning behind `fsst-rev` here — and reports
roughly 4–5× speedups, with no gain on an email dataset whose 9-byte suffix
exceeds FSST's 8-byte symbol cap. PR
[#10507](https://github.com/vortex-data/vortex/pull/10507) mentions ordered
multi-literal `LIKE` over FSST. No `_` fast path is documented.

### K.4 Summary of attribution

| | |
|---|---|
| **prior art** | compressed-domain FSST `LIKE`; prefix, suffix and substring pushdown; backward scanning for suffixes; escape-marker ambiguity and its resolution; early rejection; the "compress the pattern, not the data" framing |
| **this experiment reproduces** | all of the above, independently, with a different construction (lift a minimised byte DFA rather than build per-sub-pattern automata over codes) |
| **this experiment adds** | `_` as one Unicode character in the compressed domain, which the published implementation leaves unimplemented; DuckDB-exact UTF-8 character semantics across symbol boundaries; `(a ‖ b) LIKE p` across two columns with two symbol tables and one shared automaton; a measured dense-vs-compact answer for a 255-code alphabet; a measured cost for the backward-scan metadata; a same-machine comparison against an LLM-token-table equivalent |
| **unresolved here** | JIT and SIMD (prior art has both); multi-threading; eliminating the `.fsst.rev` sidecar the way the paper's pseudo-end states do; integration into a real query engine |

### K.5 Why a direct head-to-head run was not possible

The prior-art repository was cloned and inspected, but its executables cannot
be built on this machine. Its `CMakeLists.txt` gates **every** binary behind
`find_package(LLVM 16 CONFIG)` with a hardcoded Linux path
(`/usr/lib/llvm-16/lib/cmake/llvm`) — *"LLVM 16 not found. All executables are
unavailable."* — and gates the benchmarks and tests additionally behind
Vectorscan (`pkg_check_modules(VECTORSCAN IMPORTED_TARGET libhs)`), an x86-SIMD
library not packaged for arm64 macOS. It also fetches its own fork of FSST
rather than upstream. Standing up LLVM 16 and a Vectorscan port on Apple
silicon, then adapting its column format to these datasets, was out of scope for
this session. The comparison above is therefore from the paper, the source, and
the documentation — not from a run — and is labelled as such throughout.

---

## L. Reproducibility

Full command list: `docs/REPRODUCE.md`. The short version:

```bash
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release && cmake --build build/rel -j8
TOKENLIKE_DIR=$ORIG bash scripts/build_columns.sh
bash scripts/check_all.sh                 # 4,725 unit checks + 2,528 DuckDB checks + cross-check
$PY scripts/bench.py --patterns patterns/bench.txt --runs 7 --warmup 2 --rss --out bench/bench.json
$PY scripts/report.py bench/bench.json > bench/bench_tables.md
$PY scripts/analyze.py                    # -> bench/tables.md, bench/analysis.json
$PY demo_fsst.py --rows 100000
```

Raw artifacts behind every table: `bench/bench.json`, `bench/scales.json`,
`bench/vs_tokenlike.json`, `bench/analysis.json`, and `data/*/*.fsst.meta.json`
(per-column counts plus FNV-1a-64 digests of `.fsst.cmp`, `.fsst.off` and
`.fsst.sym`, so a rebuild can be checked for bit-identity).

### Isolation, verified

The experiment lives in **`~/TokenLIKE-Experiments/fsst-like`**
(`/Users/prahasbodanapati/TokenLIKE-Experiments/fsst-like`), outside the
TokenLIKE repository and outside its parent tree entirely. The directory did not
exist beforehand — checked — and nothing was overwritten. There is exactly one
copy: an earlier run of this experiment was built under
`~/Documents/Desk/semV/HASHCODE/TokenLIKE-Experiments/`, and that path was moved
here and removed, so no stale duplicate remains.

After the move, everything was rebuilt and re-verified from scratch at the new
path rather than assumed to still hold: all four build trees reconfigured with
CMake + Ninja (0 warnings, `Mach-O 64-bit executable arm64`), all 11 benchmark
columns re-compressed, and the whole gate re-run. The compression pipeline is
byte-deterministic, so every ratio, symbol count and escape count came back
**identical** to the pre-move run, and the aggregate round-trip is the same
4,332,067 rows / 267,373,051 bytes. The benchmarks were re-measured too: median
change across all 164 differing table cells was **0.70%**, with the outliers
confined to one-shot microsecond construction timings. Every table and figure in
this report is from the post-move run.

State of the original repository, recorded before any work and re-checked after:

| | before | after |
|---|---|---|
| branch | `b/data` | `b/data` |
| HEAD | `777b608779da48c1ca73ee73cc21be92fc92c82f` | identical |
| `git status --porcelain` | 0 paths | 0 paths |
| sha256 of the sorted tracked blob list | `73e01ffd605b11fd0b13186971e722f3e5d0242b8f3a0ffcb1cd0057b0f183d2` | identical |
| refs | 4 heads/remotes + 2 tags, 8 total | identical, none added |
| remotes | `origin` → `ParasAgarwal23/TokenLIKE` | unchanged |
| stash entries | 0 | 0 |
| unpushed commits | 0 | 0 |
| files modified during this session | — | none (see below) |
| `.git` contents — index, `HEAD`, refs, all 213 objects | — | all unchanged, Oct 24 timestamps |

One honest footnote on that last row. A first pass used
`find $ORIG -newermt '-12 hours'`, which on this machine resolves to `bfs`
rather than GNU find and rejects relative timestamps — it errored to stderr and
printed nothing, which looks exactly like "no files modified". Redone with an
explicit cutoff (`-newermt 2026-10-25T00:00:00`, this session's start), the real
answer is **one** hit: the `.git` *directory*'s own mtime. Nothing inside it
moved — `.git/index` and `.git/HEAD` still carry Oct 24 timestamps, all 213
loose objects are untouched, and no object was written this session. A directory
mtime changes when an entry is created and removed inside it, which is what a
read-only `git status` does with its momentary lock file. No content changed.

Nothing was committed, branched, tagged, fetched, rebased, reset, merged or
pushed. The TokenLIKE snapshot used in §J was produced with
`git -C $ORIG archive origin/main | tar -x`, which reads the object database and
writes nothing — no worktree, no ref. The original datasets were read in place;
`fsst_encode` copies `.raw`/`.rawoff` into this experiment so every mode here
reads from this directory only. Every generated file — builds, compressed
columns, ground truth, benchmark output — is under
`~/TokenLIKE-Experiments/fsst-like` (1.0 GiB, of which 516 MiB is compressed and
copied column data and 547 MiB is the two third-party snapshots).

---

## M. Defects found and corrected

All fixes are inside this experiment; no TokenLIKE file and no FSST file was
modified.

**1. The backward scan could silently misread an escaped literal byte as a code.
(Real bug, found by my own test, fixed.)**

The reverse scan guarded against stepping into the escape region by checking for
byte 255. That check is insufficient: walking right to left, the *literal* byte
of an escape pair is read **before** its `0xFF` marker, and that literal can be
any value — including one that looks like a perfectly ordinary code. The test
that was supposed to catch a lying `.fsst.rev` passed with `malformed == 0`,
and investigation showed why: the escaped literal `0x9E` was read as code 158,
which happened to be undefined and mapped to the reject sink, so the row was
silently answered "no match" rather than flagged. On data where that literal
maps to a *defined* code, the answer would have been silently **wrong**.

Fixed by testing `code >= nsymbols` instead of `code == 255` in the reverse
loop (`src/scan.cpp`). Any byte the backward walk cannot legitimately interpret
now forces the forward fallback for that row. One comparison, no measurable
cost, and it makes the reverse path self-checking against both corrupt data and
a corrupt sidecar. The test now asserts the fallback actually fires.

**2. Two of my own test expectations were wrong, not the code.**

* A test asserted that zeroing `.fsst.rev` must produce a `malformed` report.
  It did not, for the reason above; the test was also exercising a scenario that
  cannot arise with correctly generated data. Replaced with three sharper tests:
  a correct sidecar on a pattern that forces whole-row reverse scans (asserts
  the fallback fires and `malformed == 0`), a zeroed sidecar (asserts it is now
  detected), and a sidecar pointing past the end of a row (asserts rejection).
* A test asserted that an incompressible random-ASCII column must produce
  escapes. It produces none, and that is correct: FSST can give each of the 127
  distinct byte values its own single-byte code. Replaced with a poor-ratio
  assertion plus a separate corpus built to **starve** the table — 400 long
  repeated URLs to consume the 255 slots, then rare bytes with nothing left —
  which does produce escapes, as asserted.

**3. A bounds check in `utf8_valid` was written wrong** —
`i + need >= n + 0 && i + need > n - 1` — which would have read past the buffer
for a truncated multi-byte character at the very end of an input. Caught on
review before the first build and replaced with `i + need >= n`.

**4. Upstream finding (not fixed, deliberately): 10 UBSan misaligned-store
reports in `fsst_decompress`.** `FSST_UNALIGNED_STORE` casts an unaligned
destination to `unsigned long long*` before a byte-wise `memcpy`
(`third_party/fsst/fsst.h:156`, expanded at lines 163-201). Benign in practice and deliberate upstream
— FSST ships `-DFSST_MUST_ALIGN` for platforms that cannot take it — so the
snapshot was left alone and the `alignment` check is suppressed for
`third_party/fsst` in `tests/ubsan.supp`. With that suppression the sanitizer
run is completely clean, so the suppression hides nothing of this experiment's
own. Worth reporting upstream; not this experiment's to patch.

**5. Robustness hardening that came out of writing the tests**, each with a test
that proves the rejection: a symbol table whose hidden count disagrees with its
own length histogram; an undefined code in the stream; a row ending in a
dangling escape marker (the worst possible truncation point); non-monotonic
offsets; `off[0] != 0`; a terminal offset disagreeing with the payload; an
offset file whose size is not a multiple of 8; a `meta.json` row count
disagreeing with the offsets; a missing column; a `.fsst.rev` entry past the end
of its row; an over-budget pattern; an invalid-UTF-8 pattern. Twelve malformed
inputs, twelve clean `std::runtime_error`s naming the row or the file, zero
crashes, zero sanitizer reports.

---

## N. Is adapting TokenLIKE to FSST practically worthwhile?

**Technically: yes, and the experiment answers all seven questions the brief
posed.**

1. *Can `LIKE` be evaluated exactly over FSST codes?* **Yes.** 2,528 checks
   against DuckDB on up to 1M real rows per column, 0 mismatches, plus 4,725
   in-process checks against an independent reference matcher, plus 15/15
   identical row-id sets against a completely different implementation.
2. *Can the token-lifting algorithm be reused?* **Yes, essentially unchanged.**
   The alphabet shrinks from 65,536 tokens to 255 codes and one new concern
   appears — the escape marker — which costs a branch in the scan loop and a
   sidecar for backward scans.
3. *Does it work for prefixes, suffixes, exact, contains, Unicode `_`, and
   concatenation?* **Yes, all six**, verified individually.
4. *Is it faster than full decode?* **Yes, 2.63× to 65.29×**, never slower.
5. *How does it compare with raw strings?* **1.09× to 43.85× faster**, on a
   strong raw baseline using the same automaton.
6. *What are the memory and construction costs?* Lifted table 4.0–66.1 KiB;
   construction 63 µs – 1.06 ms per query; peak RSS ~37 MiB *below* the raw
   baseline at 1M rows; +8.0% storage if backward scans are wanted.
7. *Does it offer anything beyond existing FSST-LIKE implementations?* **One
   thing: `_` in the compressed domain** (§K.2), plus exact UTF-8 character
   semantics and cross-column concatenation. Everything else is reproduction.

**As a research contribution: no, and that should be said first.** The idea was
published at DaMoN'26 four months before this experiment ran, with a faster
implementation (JIT + SIMD + multithreading), a cleverer solution to the escape
ambiguity that needs no per-row metadata, and a formal complexity result. Had
that paper been found before writing code rather than during, the honest framing
from the start would have been "reproduce and extend", which is what this is.

**Where it would actually pay off in a real workload:**

* **Worth it, clearly:** anchored prefixes and suffixes, and exact-literal
  predicates — 10–65× over decode, and faster than having the data
  uncompressed. These are also the common shapes in real `LIKE` predicates
  (`'http%'`, `'%.pdf'`, `'ABC-%'`).
* **Worth it, modestly:** unanchored `contains`. The gain is capped by the
  compression ratio — 1.8× on URLs, 3.5× on natural-language text — so it tracks
  how compressible the column is. Worth having, not worth restructuring an
  engine for on its own.
* **Needs a planner:** the forward and reverse scans differ by more than **19×**
  in both directions depending on pattern shape (`fsst-rev` is 23× better on
  `%.php` and 19× worse on `http://%`). Shipping this means choosing the direction from
  the pattern, which is a trivial rule but not optional.
* **The decisive practical argument** is not the speedup over decode but what
  §G shows about decode itself: **decode-then-match is slower than never
  compressing at all, on every pattern measured.** A system that compresses
  strings with FSST and then decompresses them to evaluate `LIKE` has given up
  the compression benefit and paid CPU for the privilege. Compressed-domain
  matching is what makes FSST and string predicates coexist.

**What I would do next, in order:** eliminate the `.fsst.rev` sidecar using the
paper's pseudo-end-state construction (removes the 8% storage cost and the
fallback path entirely); add a direction chooser so one mode suffices; then look
at SIMD for the self-loop of unanchored patterns, which is the only case still
reading every unit.

**A footnote on the two compression schemes.** FSST beats the patched Claude
token table on every column measured — 1.77× against 1.23× on ClickBench URL,
3.20× against 2.18× on TPC-H `o_comment` — because its table is calibrated to
the data instead of to English text. But §J shows the matcher story is the
opposite: the large token table yields *fewer units per row*, so it wins the
unanchored scans. Compression ratio and scan cost are not the same axis, and a
system choosing between the two schemes would be trading footprint against
full-scan throughput.
