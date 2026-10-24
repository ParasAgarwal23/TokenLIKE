# TokenLIKE — STATUS

> Each person owns their own section. Person B edits only the Person B section.

---

## Person B — Data and Correctness (branch `b/data`)

**State: data pipeline and correctness tooling complete. Verified against the
real integrated TokenLIKE matcher — 1,163 of 1,164 integrated checks pass, with
one Person A capacity limit documented below.**

See `HANDOFF_B.md` for the executable handoff (formats, commands, CLI contract).

### Integrated verification vs the real matcher

Binary: `origin/main` @ `ec189f8` + the four Person B loader files from
`b/data` @ `a74b435`. CMake reported `loaders (Person B) = ON, matcher
(Person A) = ON`, and every CLI invocation reported `"loaders": "official"`,
so these results exercise the real loaders and the real datasets, not fixtures.

| Stage | Dataset | Patterns | Modes | Checks | Pass | Fail | Skip |
|---|---|---|---|---|---|---|---|
| 1 edge | `data/edge/values` (51 rows) | 36 | token, decode, raw | 108 | 108 | 0 | 0 |
| 2 core | ClickBench 1,000,000 rows | 22 | token, decode, raw | 66 | 66 | 0 | 0 |
| 3 tier1 | ClickBench 1,000,000 rows | 13 | token, decode, raw | 39 | 39 | 0 | 0 |
| 4 tier2 | ClickBench + TPC-H, 1,000,000 rows | 23 | token, decode, raw | 69 | 51 | 0 | 18 |
| 5 fuzz | `data/fuzz/values` (20,016 rows) | 300 | token, decode, raw | 900 | 899 | 1 | 0 |
| **Total** | | **394** | | **1,182** | **1,163** | **1** | **18** |

The 18 skips are legitimate and self-reported by the CLI with exit code 2:
`--col2` is only implemented for `token` mode, so the 9 concatenation patterns
cannot run in `decode` or `raw`. Concatenation **is** verified in `token` mode,
which is the mode that matters for TokenLIKE.

Anchored-prefix early exit is working: `http://%` over 1,000,000 rows reads
**1.52 tokens per row** at 3.0 ns/row.

### Open item for Person A: 512-state DFA cap (1 fuzz pattern)

`fuzz_0082` fails in `token` mode only, with
`error: pattern needs more than 512 DFA states` (exit code 1). `decode` and
`raw` return the correct answer for the same pattern on the same data, and
round-trip on that column is 0 mismatches, so **this is not a data, encoding,
loader or alignment problem** — it is a capacity limit in token-mode DFA
construction.

Measured cost model, from controlled probes against the integrated binary:

| Pattern shape | Result |
|---|---|
| plain literal + `%`, 500 bytes | ok |
| plain literal + `%`, 600 bytes | over the cap |
| Cyrillic literal, 250 chars / 500 bytes | ok |
| `_` repeated 120× | ok |
| `_` repeated 150× | over the cap |
| 300 literal bytes + 40 `_` | ok |
| 300 literal bytes + 60 `_` | over the cap |

So roughly **1 state per literal byte, ~4 states per `_`** (one per UTF-8 length
class), against a hard cap of 512. But cost grows faster once several interior
`%` segments and `_` are combined: `fuzz_0082` has 6 `%` and 9 `_` and hits the
cap at only ~300 literal bytes. Bisecting its prefix, 302 characters is accepted
and 303 is rejected — the character that tips it over is a single 2-byte
Cyrillic `с`.

Suggested resolution, Person A's and C's call:
1. raise the cap, or
2. fall back to the decode path when construction would exceed it, or
3. return exit code 2 (unavailable) instead of 1, so it is classified as an
   unsupported pattern rather than a wrong answer.

Option 3 alone would make this a documented limitation rather than a failure in
the final numbers. **No pattern in mandatory scope reaches this**: it is only
reachable because the fuzzer synthesizes a 353-character pattern out of a real
1,991-byte URL, and real analytics patterns are short.

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
those row-id files.

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
| TPC-H l_comment | 1,000,000 | 26,511,095 | 6,584,993 | 4.0268 | 2.0134× | 0 mismatches |
| TPC-H o_comment | 1,000,000 | 48,529,673 | 11,135,756 | 4.3573 | 2.1787× | 0 mismatches |

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

**1. `Bitmap` layout — needs A and C to agree.** `std::vector<uint8_t>` is
ambiguous. Proposed and assumed by my tooling: **one byte per row, `size() ==
rows`, values 0 or 1.** Bit-packing changes A's `match()` and C's
bitmap→row-id conversion, so decide once, now.

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
