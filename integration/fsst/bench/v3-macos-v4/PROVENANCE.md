# bench/v3-macos-v4 — smoke measurements on the integrated main

These two files come from a **smoke run**, not a measurement campaign. They
exist to show that the benchmark path works on the integrated tree and that the
numbers are in the same place as before; they are not the figures quoted in
`FINAL_REPORT.md`.

| | |
|---|---|
| commit | `0eab88d` (`main`, tag `v4-integrated`) |
| machine | Apple M2, macOS 26.5.2 arm64, Apple clang 21.0.0 `-O3`, single-threaded |
| command | `scripts/bench.py --patterns patterns/bench.txt --runs 5 --warmup 2` |
| rows | 1,000,000 (ClickBench URL / Referer, TPC-H comments as the pattern file specifies) |

**5 runs with 2 warmups, no `--rss`.** The reported figures in
`bench/v2-integration/` used `--runs 7 --warmup 2 --rss`, so these are noisier
and not a drop-in replacement. Nothing here is averaged with, or substituted
for, the v2 numbers.

Both earlier sets are kept as they were:

- `bench/v1-experiment-standalone/` — the standalone experiment, with its own
  `PROVENANCE.md` and the withdrawn raw-baseline claim it contains.
- `bench/v2-integration/` — the measurements `FINAL_REPORT.md` (v2) quotes.

Context for these numbers: `docs/MACOS_V4_VERIFICATION.md`.
