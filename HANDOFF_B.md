# Person B → Person C handoff

Everything below is executable as written from the repository root. Branch
`b/data`, commit `a74b435`.

`main` currently lacks only four Person B files; merging `b/data` adds them and
CMake switches from fixture loaders to the official ones automatically:

```
include/tokenlike/vocab.h    include/tokenlike/column.h
src/vocab.cpp                src/column.cpp
tests/test_io.cpp            (loader tests, optional to wire into CTest)
```

Confirm the switch happened: the CLI's JSON report must say
`"loaders": "official"`. If it says `"fixture"`, the build did not pick up the
loaders and no correctness result from it is meaningful.

---

## 1. Python dependencies

One package. Python 3.12–3.14 all work.

```bash
python3 -m venv .venv
.venv/bin/python -m pip install 'duckdb==1.5.6'
```

If the PyPI CDN stalls (it did here, repeatedly, at ~10/15 MB), use the pinned
CLI instead — the tooling takes either backend:

```bash
mkdir -p tools
curl -L -o tools/duckdb.zip \
  https://github.com/duckdb/duckdb/releases/download/v1.5.6/duckdb_cli-osx-universal.zip
unzip -o tools/duckdb.zip -d tools/ && chmod +x tools/duckdb
```

Do **not** `brew install duckdb`: it installs a 2.0.0-dev build whose output
format differs. `cmake` is also needed (`brew install cmake`).

### Note on the ClickBench download

`datasets.clickhouse.com` answers **HTTP 403** to the default
`Python-urllib/3.x` User-Agent. `fetch_clickbench.py` now sends an explicit
one, so the download works unattended; if you see a 403 from a script of your
own, that is the cause.

---

## 2. Regenerating all data (~4 minutes, no Git involvement)

```bash
.venv/bin/python scripts/encode/build_vocab.py                                    # ~5 s
.venv/bin/python scripts/encode/fetch_clickbench.py --rows 1000000 --columns URL,Referer   # ~3 min (117 MB download)
.venv/bin/python scripts/encode/encode.py      data/clickbench/URL                # ~5 s
.venv/bin/python scripts/encode/encode.py      data/clickbench/Referer
.venv/bin/python scripts/encode/gen_tpch.py    --scale 1 --rows 1000000           # optional
.venv/bin/python scripts/encode/encode.py      data/tpch/l_comment
.venv/bin/python scripts/encode/encode.py      data/tpch/o_comment
.venv/bin/python scripts/verify/fuzz.py --edge                                    # edge fixtures
.venv/bin/python scripts/encode/encode.py      data/edge/values
```

Smaller first, to get moving quickly:

```bash
.venv/bin/python scripts/encode/fetch_clickbench.py --rows 1000 --columns URL,Referer
.venv/bin/python scripts/encode/encode.py data/clickbench/URL
```

The pipeline is **byte-deterministic**: regenerating produces artifacts with
identical SHA-256, so your machine and mine are comparable without copying
anything. Every `.meta.json` records the hashes if you want to confirm.

### Verify the regeneration

```bash
.venv/bin/python scripts/encode/roundtrip.py data/clickbench/URL   # must print 0 mismatches
bash scripts/check_all.sh                                          # whole gate
```

`check_all.sh` finds the CLI at `build/tokenlike` or `$TOKENLIKE_CLI`.

---

## 3. Datasets: paths, rows, sizes

| Column prefix | Rows | Raw bytes | Tokens | On-disk (all 4 files) |
|---|---|---|---|---|
| `data/clickbench/URL` | 1,000,000 | 88,562,192 | 36,136,053 | ~241 MB |
| `data/clickbench/Referer` | 1,000,000 | 79,583,339 | 32,975,876 | ~224 MB |
| `data/tpch/l_comment` | 1,000,000 | 26,511,095 | 6,583,689 | ~56 MB |
| `data/tpch/o_comment` | 1,000,000 | 48,529,673 | 11,137,450 | ~90 MB |
| `data/dbtext/urls` | 100,000 | 6,227,875 | 2,425,704 | ~12 MB |
| `data/sample/urls` | 1,000 | 62,455 | 24,349 | ~132 KB |
| `data/edge/values` | 51 | 669 | 217 | ~24 KB |

`URL` and `Referer` are row-aligned (same `row_id`), as are `l_comment` and
`o_comment`. `data/vocab.bin` is 630,133 bytes. Total ~800 MB including the
download and ground truth; **none of it is in Git, and none of it should be.**

---

## 4. File formats (little-endian throughout)

```
data/vocab.bin      uint32 n_tokens
                    uint32 off[n_tokens + 1]
                    uint8  bytes[off[n_tokens]]
                    token t = bytes[off[t] .. off[t+1])

<prefix>.tok        uint16[total_tokens]
<prefix>.off        uint64[rows + 1]      row i tokens = tok[off[i] .. off[i+1])
<prefix>.raw        uint8[total_bytes]
<prefix>.rawoff     uint64[rows + 1]      row i text   = raw[rawoff[i] .. rawoff[i+1])
<prefix>.parquet    row_id BIGINT, value VARCHAR    (DuckDB ground-truth input)
<prefix>.meta.json  counts, ratios, sha256 of every artifact, provenance
```

Offset arrays are prefix sums with a leading 0; `off[0] == 0`,
`off[rows] == total_tokens`, `rawoff[rows] == total_bytes`. This matches the
conventions block in `include/tokenlike/common.h`.

`row_id` is the 0-based position in the canonical parquet, which is built once
and is the single source of truth for row identity. NULL → empty string, applied
once at that point.

Vocabulary: 65,536 tokens (ids fill `uint16` exactly), all 256 single-byte
tokens present, max token length 12.

---

## 4a. Bitmap interface (settled: bit-packed, lowest bit first)

`include/tokenlike/common.h` is authoritative; restated here because the row-id
files my tooling consumes are derived from it.

```cpp
using Bitmap = std::vector<uint8_t>;          // BIT-PACKED, LSB-first

// row r matches  <=>  (b[r >> 3] >> (r & 7)) & 1
inline size_t bitmap_bytes(uint64_t rows) { return (rows + 7) / 8; }
inline bool   bitmap_bit(const Bitmap& b, uint64_t r) { return (b[r >> 3] >> (r & 7)) & 1; }
```

* `size() == ceil(rows / 8)`.
* **Lowest bit first**: in byte `i`, bit 0 is row `8i` and bit 7 is row `8i+7`.
* Padding bits above `rows` must be zero; the CLI rejects a set padding bit.
* The callee of a match function resizes and overwrites the bitmap.

Person B components never read a bitmap: the CLI converts it to row ids
(`--rowids`, sorted `uint32` little-endian) and that file is what `check.py`
compares against DuckDB. Equivalent decode in Python:

```python
ids = [8 * i + b for i, byte in enumerate(bitmap) for b in range(8) if byte >> b & 1]
```

---

## 5. C++ loader interfaces

```cpp
#include "tokenlike/vocab.h"
#include "tokenlike/column.h"

Vocab  load_vocab(const std::string& path);     // "data/vocab.bin"
Column load_column(const std::string& prefix);  // "data/clickbench/URL"

// Optional, stronger check; separate because the frozen loader signature has
// no Vocab parameter. O(total_tokens).
void validate_column(const Column& c, const Vocab& v);
```

Both throw `std::runtime_error` naming the file and the offending index on any
malformed input (missing file, size not a whole multiple of the element width,
non-monotonic or non-zero-based offsets, terminal offset disagreeing with the
payload, `.off`/`.rawoff` disagreeing on the row count, vocabulary larger than
the `uint16` id space). They allocate once and are read-only; keep them on the
setup path, not inside a timed region. C++17, no dependencies beyond the
standard library.

`load_column` sets `rows = off.size() - 1`, so `cli.cpp`'s `validate()` layout
assertions hold by construction.

---

## 6. Correctness checker

```bash
.venv/bin/python scripts/verify/check.py patterns/core.txt \
    --modes token,decode,raw \
    --cli build/tokenlike
```

Options: `--vocab` (default `data/vocab.bin`), `--runs` (default 1 — correctness
needs one scan), `--timeout` (default 600 s), `--force` (regenerate ground
truth). `$TOKENLIKE_CLI` overrides `--cli`.

It invokes your CLI with **your** flag spelling:

```
--mode M --col PREFIX [--col2 PREFIX] --vocab F --pattern P --rowids OUT --runs 1 --warmup 0
```

Exit-code handling follows `src/cli.cpp`: `2` is treated as SKIP (a feature this
build does not support, e.g. `--col2` outside token mode), `1` as a hard
failure. `check.py` exits non-zero if any pattern is wrong, and also if nothing
was actually verified, so a skipped run can never be mistaken for a pass.

Ground truth alone, without a CLI:

```bash
.venv/bin/python scripts/verify/ground_truth.py patterns/core.txt
.venv/bin/python scripts/verify/ground_truth.py --semantics   # DuckDB LIKE truth table
```

Truth lands in `data/truth/<dataset>/<name>.ids` (sorted uint32 LE row ids) with
a `truth.json` manifest holding each pattern's match count and input
fingerprint, so stale truth is detected after a data regeneration.

---

## 7. Pattern files

Tab-separated, `#` comments: `name<TAB>column_prefix<TAB>pattern[<TAB>column2_prefix]`

| File | Patterns | Scope |
|---|---|---|
| `patterns/core.txt` | 22 | mandatory anchored prefix/suffix, 0.02%–81% selectivity |
| `patterns/tier1.txt` | 13 | boundaries inside a token, multi-byte UTF-8, Cyrillic case |
| `patterns/tier2.txt` | 23 | `_`, interior `%`, `URL \|\| Referer`, TPC-H concat |
| `patterns/generated_edge.txt` | 36 | 51-row adversarial fixture |
| `patterns/generated_fuzz.txt` | 300 | seeded, from real substrings |

For the demo use `core.txt` and `tier1.txt`; everything there matches a non-zero
number of rows. `tier2.txt` contains three deliberate zero-match controls
(`z_zzz_prefix`, `z_zzz_suffix`, `z_ftp`) — do not put those on a slide.

---

## 8. Two LIKE semantics facts, measured not assumed

Already reflected in your `common.h`, repeated here because they are the easy
things to get wrong:

- **`_` matches one character, not one byte.** `'日本語' LIKE '___'` is true.
  137,578 of the 1M URLs are non-ASCII, so a byte-wise `_` fails loudly.
- **Backslash is not an escape character.** `'a%b' LIKE 'a\%b'` is **false**
  without an explicit `ESCAPE '\'`, which we never write.

Reproduce: `.venv/bin/python scripts/verify/ground_truth.py --semantics`

---

## 9. Reporting the compression number honestly

At the token-ID level ClickBench URL compresses **1.23×** (2.45 bytes/token).
The patched Claude vocabulary has no `www.`, `.com` or `.html` token, so URLs
shred into short pieces. The paper's strong ratios come from stacking a second
compressor on SGTT, which is out of our scope.

**The URL story is scan reduction, not ratio:** 36.1M tokens inspected instead
of 88.6M bytes (2.45× fewer units), and anchored patterns stop after the first
few — measured 1.52 tokens read per row for `http://%`.

For a headline *compression* figure use the TPC-H comments: **2.01× and 2.18×**
(4.03 / 4.36 bytes/token) on natural-language text with the same vocabulary.

Our encoder is corroborated against the paper's own published numbers: on their
own `urls` file our ratio of 1.2837× sits inside their published SGTT range of
1.158–1.448, and our `.raw` size is exactly their reported `original_size` minus
the 100,000 newlines.
