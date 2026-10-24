# TokenLIKE — STATUS

> Each person owns their own section. Person B edits only the Person B section.

---

## Person B — Data and Correctness (branch `b/data`)

**State: data pipeline and correctness tooling complete and verified at full scale. Ready for Person A's matcher.**

### What A and C can use right now

```bash
data/vocab.bin                   # 65,536 tokens, frozen, 630,133 bytes
data/clickbench/URL.{tok,off,raw,rawoff,parquet,meta.json}       # 1,000,000 rows
data/clickbench/Referer.{tok,off,raw,rawoff,parquet,meta.json}   # 1,000,000 rows, row-aligned with URL
data/sample/urls.*               # 1,000-row sample for fast iteration
data/edge/values.*               # 51 hand-built edge cases
patterns/core.txt                # 22 mandatory prefix/suffix patterns
patterns/tier1.txt               # 13 token-boundary and UTF-8 patterns
patterns/tier2.txt               # 18 optional: '_', interior '%', concatenation
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

Encoding runs at ~215k rows/s, so the full column encodes in under 5 seconds.

**Read the ratio carefully when writing the report.** At the token-ID level the
URL column compresses only **1.23×**. That is the honest figure for this
vocabulary on URL data: the patched Claude table has no `www.`, `.com` or
`.html` token, so URLs segment into short pieces (`http | :// | www | . | bbc`).
The paper's strong ratios come from stacking a second-level compressor on top of
SGTT, which is out of our scope. **Our story is scan reduction, not ratio:**
TokenLIKE inspects 36.1M tokens instead of 88.6M bytes, i.e. **2.45× fewer units
per scan**, and for anchored patterns it stops after the first few.

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

- **Round-trip: 0 mismatches** on all four datasets, checked independently in
  Python (`roundtrip.py`) and in C++ (`tests/test_io.cpp`), so a format bug
  cannot hide behind agreement between one writer and one reader. Confirmed the
  check has teeth: flipping one bit in `.tok` is detected and reported.
- **Loaders: 32/32 checks**, including one corruption fixture per validation
  rule, each proven to be rejected rather than silently accepted.
- **Patterns: all 53 verified at 1M-row scale** against an independent
  reference matcher, plus 36 edge patterns × 3 modes = 108/108. Three
  implementations agree: DuckDB, a naive backtracking matcher, and a regex
  translation (270 cross-checked cases).
- **Negative control:** `check.py` detects a single wrong row id even when the
  match count is unchanged, because it compares sets, not counts.

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

### Not done / out of scope so far

- **TPC-H concatenation data (`gen_tpch.py`) is not built yet.** Optional scope;
  deliberately deferred until after the full-scale ClickBench gate. Concatenated
  matching is already covered on ClickBench via `patterns/tier2.txt`
  (`URL || Referer`), so A can develop `match_concat` today.
- Randomized fuzzing exists (`fuzz.py --random`) but has not been run at volume.
- No pattern has been verified against A's real matcher yet; A's matcher does
  not exist. Everything above is matcher-independent by design.

---

## Person A — Matcher Core (branch `a/matcher`)

_Owner: Person A._

## Person C — Baselines, Benchmarking, Demo (branch `c/bench`)

_Owner: Person C._
