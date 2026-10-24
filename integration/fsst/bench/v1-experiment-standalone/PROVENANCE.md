# Provenance: standalone-experiment results (v1)

These are the results of the standalone FSST-LIKE experiment, preserved
unchanged. **Do not edit them.** New measurements go in `../v2-integration/`.

| | |
|---|---|
| produced by | `~/TokenLIKE-Experiments/fsst-like`, the standalone experiment |
| machine | Apple M2, 8 cores, 16 GiB, macOS 26.5.2 arm64 |
| compiler | Apple clang 21.0.0, `-O3 -DNDEBUG`, single-threaded |
| FSST | upstream `cwida/fsst` @ `e638d4cf8c26129d73c242a4127b42b975de5b63` |
| statistic | best-of-7 wall time of the scan only, after 2 warmups |
| raw baseline | **byte DFA over uncompressed strings** — see the caveat below |

**Known caveat, fixed in v2.** The `raw` mode in these results is a generic
forward byte-level DFA for *every* pattern shape, including suffixes and
substrings. A real system would use an anchored comparison or a substring
search for those, so the "vs raw" speedups here are **overstated** for suffix,
exact and contains patterns. `v2-integration/` adds a `raw-opt` baseline that
does what a real system does, and reports both side by side. The `vs decode`
numbers are unaffected.
