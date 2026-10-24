# integration/fsst — SQL LIKE over FSST-compressed strings

Optional TokenLIKE module. It applies the project's core idea — lift a
byte-level `LIKE` automaton onto compression symbols so rows never have to be
decompressed — to [FSST](https://github.com/cwida/fsst) instead of an LLM token
table.

**It is off by default and the repository has no FSST dependency without it.**
With `-DTOKENLIKE_WITH_FSST=OFF` (the default) this directory is never added to
the build, and `tokenlike`, `test_baselines`, `test_matcher`, `test_io` and the
CTest suite build and pass exactly as they did before.

Read `FINAL_REPORT.md` for results and `INTEGRATION_HANDOFF.md` for what
Person C needs. `THIRD_PARTY.md` has the FSST pin and licence. The technique is
not novel — see §K of the report — the `_` wildcard support is the part that is.

## Build

```bash
# from the repository root
bash integration/fsst/scripts/fetch_fsst.sh          # optional; CMake can fetch instead
cmake -G Ninja -S . -B build/fsst -DCMAKE_BUILD_TYPE=Release -DTOKENLIKE_WITH_FSST=ON
cmake --build build/fsst
```

Targets added: `fsst_like` (CLI), `fsst_encode` (compressor), `fsst_liftbench`
(automaton-reuse measurement), `fsst_test_unit` (CTest name `fsst_unit`).
Nothing existing is renamed or changed.

## Use

```bash
cd integration/fsst
TOKENLIKE_DIR=/path/to/TokenLIKE bash scripts/build_columns.sh   # compress the columns
B=../../build/fsst

$B/fsst_like --mode fsst      --col data/clickbench/URL_1m --pattern 'http://%' --json
$B/fsst_like --mode fsst-rev2 --col data/clickbench/URL_1m --pattern '%.php'    --json
$B/fsst_like --mode raw-opt   --col data/clickbench/URL_1m --pattern '%.php'    --json
```

### Modes

| mode | what it does |
|---|---|
| `raw` | byte DFA over the uncompressed strings |
| `raw-opt` | **optimised** raw matcher: anchored compares, `memmem`, segment walk |
| `ref` | backtracking reference matcher (the test oracle; slow) |
| `decode` | `fsst_decompress()` every row in full, then the byte DFA |
| `fsst` | lifted DFA over the compressed codes, dense table, forward |
| `fsst-compact` | same, default+exception table (measured: slower; kept for the comparison) |
| `fsst-rev` | lifted reverse DFA using the 4-byte-per-row `.fsst.rev` sidecar |
| `fsst-rev2` | lifted reverse DFA with **no sidecar at all** — the recommended one |

`--col2 P` evaluates `(col || col2) LIKE pattern` in `raw`, `raw-opt`, `ref`,
`decode` and `fsst`. Unsupported combinations exit **2** with a message and
never silently fall back to decoding. Errors exit 1.

## Verify

```bash
cd integration/fsst
TOKENLIKE_DIR=/path/to/TokenLIKE bash scripts/check_all.sh
```

Unit tests, structural validation of every column, the DuckDB `LIKE` semantics
probes, the pattern suite against DuckDB across all eight modes, and a row-id
cross-check against this branch's own `tokenlike` CLI.

## Layout

```
include/fsstlike/   pattern, byte DFA, FSST column, lifted tables, raw-opt, scans
src/                implementations, CLI, encoder, lift-reuse benchmark
tests/              test_unit.cpp, ubsan.supp
scripts/            column builder, DuckDB ground truth, checker, benchmarks,
                    storage accounting, FSST fetch
patterns/           tab-separated pattern lists
bench/v1-experiment-standalone/   earlier results, preserved, see its PROVENANCE.md
bench/v2-integration/             results measured in this module
third_party/fsst/   gitignored; the pinned upstream snapshot
data/               gitignored; compressed columns and ground truth
```
