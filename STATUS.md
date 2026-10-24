# TokenLIKE — STATUS

> Each person owns their own section. Person B edits only the Person B section.

---

## Person B — Data and Correctness (branch `b/data`)

**State: data pipeline and correctness tooling complete and stable. Verified
against the real integrated TokenLIKE matcher at `main` `183d947`
(`v1-core-demo`) — 1,164 of 1,164 executed checks pass, zero mismatches. The
DFA state-limit blocker reported earlier is resolved by Person A's `b2bfecc`.**

See `HANDOFF_B.md` for the executable handoff (formats, commands, CLI contract).

### Integrated verification vs the real matcher

Binary: `main` @ `183d947`, tag `v1-core-demo`, built as-is (my loaders are
merged into main as of `39de2b1`). CMake reported
`loaders (Person B) = ON, matcher (Person A) = ON`, and every CLI invocation
reported `"loaders": "official"`, so these results exercise the real loaders,
the real matcher and the real datasets — not fixtures.

| Stage | Dataset | Rows | Patterns | Modes | Checks | Pass | Fail | Skip |
|---|---|---|---|---|---|---|---|---|
| 1 edge | `data/edge/values` | 51 | 36 | token, decode, raw | 108 | 108 | 0 | 0 |
| 2 core | ClickBench URL/Referer | 1,000,000 | 22 | token, decode, raw | 66 | 66 | 0 | 0 |
| 3 tier1 | ClickBench URL | 1,000,000 | 13 | token, decode, raw | 39 | 39 | 0 | 0 |
| 4 tier2 | ClickBench + TPC-H | 1,000,000 | 23 | token, decode, raw | 69 | 51 | 0 | 18 |
| 5 fuzz | `data/fuzz/values` | 20,016 | 300 | token, decode, raw | 900 | 900 | 0 | 0 |
| **Total** | | | **394** | | **1,182** | **1,164** | **0** | **18** |

The 18 skips are legitimate and self-reported by the CLI with exit code 2:
`--col2` is implemented only for `token` mode, so the 9 concatenation patterns
cannot run in `decode` or `raw`. Concatenation **is** verified in `token` mode,
which is the mode that matters for TokenLIKE.

Round-trip is 0 mismatches on all eight columns (4,123,118 rows), and
`test_io` passes 32/32 against each of them.

Anchored-prefix early exit is working: `http://%` over 1,000,000 rows reads
**1.52 tokens per row** at 3.0 ns/row.

### Resolved: the 512-state DFA cap

Previously `fuzz_0082` failed in `token` mode with
`pattern needs more than 512 DFA states` (a 353-character pattern with 6 `%`
and 9 `_`, synthesized by the fuzzer from a real 1,991-byte URL). Person A's
`b2bfecc` applies the limit to the **minimized** DFA rather than to subset
construction, and the pattern now passes in all three modes. Fuzz is 900/900.

### Person B change required by integration

`check.py` originally used the `--column` / `--column2` / `--out` flags I had
proposed. The merged `src/cli.cpp` is the authority and uses
`--col` / `--col2` / `--rowids`, so `check.py` now matches it, passes
`--runs 1 --warmup 0` for correctness runs, distinguishes CLI exit code 2
(unsupported → SKIP) from 1 (error → FAIL), and records the CLI's
`"loaders"` field so a fixture-loader build can never be mistaken for an
integrated run. My loaders also compile clean under the project's C++17 setting
against the real frozen `common.h`.

The bit-packed `Bitmap` decision (commit `092fc58`) does not affect my
components: the CLI converts the bitmap to row ids, and my tooling only reads
those row-id files. The settled interface is documented above and in
`HANDOFF_B.md` §4a.

### Stabilization fixes (this round)

1. **ClickBench download returned HTTP 403.** `datasets.clickhouse.com` rejects
   the default `Python-urllib/3.x` User-Agent. `fetch_clickbench.py` now sends
   an explicit one and reports a 403 with the cause named. This was invisible
   for a long time because a manually `curl`-ed copy was already cached in
   `data/download`, so the cache hit skipped the request entirely. Verified by
   deleting the cache: the script downloaded all 122,446,530 bytes unattended.
   The same User-Agent was added to `build_vocab.py` defensively;
   `raw.githubusercontent.com` currently accepts the default agent, so that one
   was not actually failing.
2. **Windows relative executable paths in `check.py`.** `build/tokenlike` never
   resolved on Windows: the file is `tokenlike.exe`, and `CreateProcess` does
   not apply `PATHEXT` to a path containing a separator. `check.py` now
   resolves the CLI (trying `PATHEXT` extensions, then `PATH`) to an absolute
   path and prints the resolution it used. `check_all.sh` accepts either
   spelling.
3. **TPC-H token counts in this file were wrong** — 6,584,993 and 11,135,756
   had been back-computed from a rounded bytes/token figure. Corrected to the
   encoder's actual output, 6,583,689 and 11,137,450, read from `*.meta.json`.
   Ratios and byte counts were already right.
4. **`test_io` read out of bounds on an unexpected vocabulary.** It probed
   token id 23638 unconditionally; against a smaller vocabulary that is a read
   past the end of `Vocab::off`, which AddressSanitizer reports as a
   heap-buffer-overflow. It now range-checks the probe, reports a clean failure
   naming the problem, skips the checks that only make sense against the frozen
   vocabulary, still runs every corruption fixture, and exits non-zero. A
   loader rejection of a command-line input is now reported as a clean error
   rather than an "unexpected exception". Confirmed clean under
   `-fsanitize=address,undefined`.

   This is the root cause of the workaround in Person C's `bfdc5ee` ("run
   Person B's test_io only on the real data"). That CMake guard is still
   sensible, but it is no longer protecting against a crash.

### What A and C can use right now

```bash
data/vocab.bin                   # 65,536 tokens, frozen, 630,133 bytes
data/clickbench/URL.{tok,off,raw,rawoff,parquet,meta.json}       # 1,000,000 rows
data/clickbench/Referer.{tok,off,raw,rawoff,parquet,meta.json}   # 1,000,000 rows, row-aligned with URL
data/tpch/l_comment.*            # 1,000,000 rows, row-aligned with o_comment
data/tpch/o_comment.*            # for (l_comment || o_comment) LIKE patterns
data/sample/urls.*               # 1,000-row sample for fast iteration
data/edge/values.*               # 51 hand-built edge cases
patterns/core.txt                # 22 mandatory prefix/suffix patterns
patterns/tier1.txt               # 13 token-boundary and UTF-8 patterns
patterns/tier2.txt               # 23 optional: '_', interior '%', concatenation, TPC-H
data/truth/<dataset>/*.ids       # DuckDB ground truth, sorted uint32 row ids
```

Reproduce everything from a clean checkout:

```bash
python3 -m venv .venv && .venv/bin/python -m pip install 'duckdb==1.5.6'
.venv/bin/python scripts/encode/build_vocab.py
.venv/bin/python scripts/encode/fetch_clickbench.py --rows 1000000 --columns URL,Referer
.venv/bin/python scripts/encode/encode.py data/clickbench/URL
.venv/bin/python scripts/encode/roundtrip.py data/clickbench/URL
bash scripts/check_all.sh
```

### Measured numbers (not estimates)

| Column | Rows | Raw bytes | Tokens | Bytes/token | Ratio vs 2B ids | Round-trip |
|---|---|---|---|---|---|---|
| ClickBench URL | 1,000,000 | 88,562,192 | 36,136,053 | 2.4508 | 1.2254× | 0 mismatches |
| ClickBench Referer | 1,000,000 | 79,583,339 | 32,975,876 | 2.4134 | 1.2067× | 0 mismatches |
| TPC-H l_comment | 1,000,000 | 26,511,095 | 6,583,689 | 4.0268 | 2.0134× | 0 mismatches |
| TPC-H o_comment | 1,000,000 | 48,529,673 | 11,137,450 | 4.3573 | 2.1787× | 0 mismatches |

All figures are read from the generated `*.meta.json`. The two TPC-H token
counts previously shown here (6,584,993 and 11,135,756) were wrong: they had
been back-computed from the rounded bytes/token figure instead of taken from
the encoder's output. Everything else in the table was already correct.

Encoding runs at ~215k rows/s, so the full column encodes in under 5 seconds.

The pipeline is **byte-deterministic**: rebuilding from the download produces
artifacts with identical SHA-256, which is how the ground-truth cache detects
staleness.

**Read the ratio carefully when writing the report.** At the token-ID level the
URL column compresses only **1.23×**. That is the honest figure for this
vocabulary on URL data: the patched Claude table has no `www.`, `.com` or
`.html` token, so URLs segment into short pieces (`http | :// | www | . | bbc`).
The paper's strong ratios come from stacking a second-level compressor on top of
SGTT, which is out of our scope. **Our story is scan reduction, not ratio:**
TokenLIKE inspects 36.1M tokens instead of 88.6M bytes, i.e. **2.45× fewer units
per scan**, and for anchored patterns it stops after the first few.

The TPC-H rows above make the same point from the other side and are worth a
line in the report: the same vocabulary gets **4.03–4.36 bytes/token (≈2.1×)**
on natural-language comments versus **2.45 on URLs**, because an LLM
word-piece vocabulary is built for prose, not for URL punctuation. If we want a
headline compression number, the TPC-H comment columns are the honest place to
get it; if we want a headline *speed* number, URLs are the right dataset.

### Interface decisions — PLEASE CONFIRM

**1. `Bitmap` layout — SETTLED, bit-packed.** I had proposed one byte per row;
the team chose bit-packing (commit `092fc58`) and `include/tokenlike/common.h`
is now authoritative:

```cpp
using Bitmap = std::vector<uint8_t>;          // BIT-PACKED, LSB-first

// row r matches  <=>  (b[r >> 3] >> (r & 7)) & 1
inline size_t bitmap_bytes(uint64_t rows) { return (rows + 7) / 8; }
inline bool   bitmap_bit(const Bitmap& b, uint64_t r) { return (b[r >> 3] >> (r & 7)) & 1; }
```

- `size() == bitmap_bytes(rows) == ceil(rows / 8)`.
- **Lowest bit first**: within byte `i`, bit 0 is row `8i`, bit 7 is row `8i+7`.
- Padding bits above `rows` are zero; the CLI treats a set padding bit as an
  error.
- The callee of a match function resizes and overwrites the bitmap.

This does not affect any Person B component: the CLI converts the bitmap to row
ids and my tooling only ever reads those row-id files (sorted `uint32` LE).
Decoding one in Python, for reference:

```python
ids = [8 * i + b for i, byte in enumerate(bitmap) for b in range(8) if byte >> b & 1]
```

**2. CLI contract — `check.py` is built against this.** Replace the reference
stub with the real binary and the correctness gate runs unchanged:

```
tokenlike --mode token|decode|raw \
          --column data/clickbench/URL \
          --vocab  data/vocab.bin \
          --pattern '/static/%' \
          --out    out.ids          # sorted uint32 LE row ids, one per match
          [--column2 data/clickbench/Referer]   # concatenation patterns
          [--stats stats.json]
exit 0 on success, non-zero on error
```

The CLI converts bitmap → row ids; I do not. Point `check.py` at it with
`--cli build/tokenlike` or `TOKENLIKE_CLI=...`.

**3. `Vocab::token()` must be defined inline in `common.h`** (Person C). It is
declared in the frozen struct but needs a body; I must not define C's member
function in my `.cpp`.

**4. Paths:** `data/vocab.bin`, `data/<dataset>/<column>.*`,
`data/truth/<dataset>/<name>.ids`.

### Measured DuckDB LIKE semantics — Person A must match these

Run `.venv/bin/python scripts/verify/ground_truth.py --semantics` to reproduce.
Two findings are easy to get wrong:

- **`_` matches one CHARACTER, not one byte.** `'日本語' LIKE '___'` is **true**;
  `'é' LIKE '_'` is true and `'é' LIKE '__'` is false. A byte-wise `_` fails on
  the 137,578 non-ASCII URLs in the dataset.
- **Backslash is NOT an escape character.** `'a%b' LIKE 'a\%b'` is **false**;
  it only becomes true with an explicit `ESCAPE '\'`, which we never write. So
  `\` is an ordinary literal byte.
- `'' LIKE ''` is true, `'' LIKE '%'` is true, `'' LIKE '_'` is false.
- `LIKE` is case-sensitive, in ASCII and in Cyrillic.

### Verification evidence

- **Round-trip: 0 mismatches across 4,002,051 rows** over six columns, checked
  independently in Python (`roundtrip.py`) and in C++ (`tests/test_io.cpp`), so
  a format bug cannot hide behind agreement between one writer and one reader.
  Confirmed the check has teeth: flipping one bit in `.tok` is detected and
  reported with the row, both byte strings and the token expansion.
- **Loaders: 32/32 checks on each of the six columns**, including one corruption
  fixture per validation rule, each proven to be rejected rather than silently
  accepted.
- **Patterns: 1,182 (pattern, mode) checks, 0 mismatches**, every one against an
  independent reference matcher, all three modes (`token`, `decode`, `raw`):

  | Pattern set | Patterns | Checks | Scale | Result |
  |---|---|---|---|---|
  | `core.txt` | 22 | 66/66 | 1M rows | pass |
  | `tier1.txt` | 13 | 39/39 | 1M rows | pass |
  | `tier2.txt` (incl. concat + TPC-H) | 23 | 69/69 | 1M rows | pass |
  | `generated_edge.txt` | 36 | 108/108 | 51 rows | pass |
  | `generated_fuzz.txt` (seed 1) | 300 | 900/900 | 20,016 rows | pass |

  Three implementations agree: DuckDB, a naive backtracking matcher, and a
  regex translation (cross-checked against each other on 270 cases).
- **Negative control:** `check.py` detects a single wrong row id even when the
  match count is unchanged, because it compares sets, not counts.
- **Randomized fuzzing:** 300 seeded patterns derived from real ClickBench
  substrings; reproduce with
  `fuzz.py --random --from data/clickbench/URL --seed 1 --rows 20000 --count 300`.

### Blocked: I cannot push

```
remote: Permission to ParasAgarwal23/TokenLIKE.git denied to Prahas-B.
fatal: ... error: 403
```

**7 commits are ready on local `b/data`.** Whoever owns the repo needs to add
`Prahas-B` as a collaborator, then `git push -u origin b/data` goes through. I
have deliberately not worked around this (no fork, no push elsewhere), since C
owns the merge path and a fork would complicate it.

### Bugs found and fixed during preparation

Recorded because each would have produced false matcher failures later:

1. **`CAST(BLOB AS VARCHAR)` corrupts the data.** ClickBench stores URL and
   Referer as bare `BYTE_ARRAY` with no UTF8 logical type, so DuckDB types them
   `BLOB`, and `LIKE` is not defined on `BLOB`. The obvious cast renders the
   blob's *escaped* form — `сезон` became `\xD1\x81\xD0\xB5...` — inflating the
   column from 88,562,192 to 100,529,987 bytes and corrupting every non-ASCII
   URL. `TRY_CAST` does not reject it, because escaping never fails. The fix is
   `decode(blob)`, plus an assertion comparing total byte length against the
   source. Note `strlen()` counts bytes and `length()` counts characters; using
   `length()` here would have hidden the bug.
2. **`read_csv` silently drops an empty line.** A 51-value fixture became 50
   parquet rows, shifting every row id after the empty value. The canonical
   parquet is now built from explicit inserts. Caught by the parquet-vs-raw
   cross-check, which is why that check exists.

### Environment notes for A and C

- **`brew install cmake`** is needed; it is not installed on this machine.
- **PyPI was effectively unusable on this network** (duckdb wheel stalled
  repeatedly at ~10/15 MB). If `pip install duckdb` fails, fetch the pinned CLI
  from GitHub instead, which the tooling supports as a second backend:
  ```bash
  mkdir -p tools && curl -L -o tools/duckdb.zip \
    https://github.com/duckdb/duckdb/releases/download/v1.5.6/duckdb_cli-osx-universal.zip
  unzip -o tools/duckdb.zip -d tools/ && chmod +x tools/duckdb
  ```
  Avoid `brew install duckdb`: it installs a 2.0.0-dev build whose output format
  differs.
- Until `include/tokenlike/common.h` exists, my loaders compile against a stub
  kept **outside** the repo (I must not create C's file). Build with:
  ```bash
  clang++ -std=c++20 -O2 -Iinclude -I<stub_dir> \
      src/vocab.cpp src/column.cpp tests/test_io.cpp -o build/test_io
  ```
  `scripts/check_all.sh` picks this up from `TOKENLIKE_COMMON_STUB`, and uses
  C's real header automatically once it lands.
- `data/`, `tools/`, `.venv/` and `build/` are kept out of git via
  `.git/info/exclude` until C's `.gitignore` lands.

### Vocabulary provenance

Official patched Claude token table from the paper's own repository,
`umbra-db/token-vldb2026`, pinned at commit `e202e36e`, file
`tokens/claude_patched.csv` (sha256 `f31c7154…b899b`, verified against the
pinned commit). Exactly 65,536 tokens so ids fit `uint16_t`; all 256
single-byte tokens present so greedy encoding never dead-ends; max token
length 12.

The encoder is **our reimplementation of SGTT's greedy longest-match**, read
from `src/tokenizer/encoder/StandardEncoder.hpp::encode()` in that repo: it
probes token lengths longest-first and falls back to the single-byte token,
with **no BPE merge loop and no pre-tokenization regex**. So this is fallback
tier 2 from the plan and is semantically identical to the official encoder,
not an approximation. Token bytes come from the authoritative hex column, never
the human-readable text column.

**The encoder is corroborated against the paper's own published numbers.** We
encoded the paper's own `data/dbtext/urls` file (`data/dbtext/urls.*` here):

| | |
|---|---|
| Paper's `original_size` for `urls` | 6,327,875 bytes |
| Our `.raw` bytes | 6,227,875 = 6,327,875 − 100,000 newlines (exact match) |
| Our ratio, `claude_patched` | **1.2837×** |
| Paper's published SGTT ratios, same file | 1.158 (openai50k) … 1.448 (openai100kpatched) |

Our independent implementation lands inside their published range on their own
dataset, and reproduces their qualitative result that URLs compress far worse
than prose (their `l_comment` ratios are 1.515–1.928; ours is 2.013). They did
not publish a Claude row for `urls`, so this is a corroboration of the
algorithm and vocabulary handling, not a bit-exact replication.

### Not done / out of scope so far

- **No pattern has been verified against A's real matcher**, because A's matcher
  does not exist yet. Everything above is matcher-independent by design; the
  moment `build/tokenlike` appears, `bash scripts/check_all.sh` becomes the
  real H10 gate with no changes needed.
- The `--stats` output of the CLI is accepted by the contract but not consumed
  by my tooling; Person C owns the benchmark numbers.
- `_` and interior `%` are in tier 2 and have verified ground truth, but they
  are optional scope: do not put them in the demo until they pass against A's
  matcher.

---

## Person A — Matcher Core (branch `a/matcher`)

_Owner: Person A._

## Person C — Baselines, Benchmarking, Demo (branch `c/bench`)

_Owner: Person C._
