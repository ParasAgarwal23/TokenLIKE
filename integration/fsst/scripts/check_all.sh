#!/usr/bin/env bash
# One command that reproduces every correctness claim in FINAL_REPORT.md.
#
#   bash scripts/check_all.sh            # release build
#   FSSTLIKE_BUILD=build/asan bash scripts/check_all.sh
#
# Exits non-zero if anything fails. Skipped checks are reported as skipped and
# never counted as passes.
set -u -o pipefail
cd "$(dirname "$0")/.." || exit 1

ORIG="${TOKENLIKE_DIR:-/Users/prahasbodanapati/Documents/Desk/semV/HASHCODE/hash/TokenLIKE}"
PY="${FSSTLIKE_PYTHON:-$ORIG/.venv/bin/python}"
[ -x "$PY" ] || PY="python3"
BUILD="${FSSTLIKE_BUILD:-../../build/fsst}"
export FSSTLIKE_CLI="$BUILD/fsst_like"

failures=0
step() { printf '\n=== %s\n' "$1"; }
ok()   { printf '  OK: %s\n' "$1"; }
bad()  { printf '  FAIL: %s\n' "$1" >&2; failures=$((failures + 1)); }

step "environment"
printf '  python     : %s (%s)\n' "$PY" "$("$PY" -V 2>&1)"
printf '  cli        : %s\n' "$FSSTLIKE_CLI"
printf '  fsst       : third_party/fsst @ %s\n' "$(git -C third_party/fsst rev-parse --short HEAD 2>/dev/null || echo unknown)"
if "$PY" -c 'import duckdb' 2>/dev/null; then
   printf '  duckdb     : python module %s\n' "$("$PY" -c 'import duckdb;print(duckdb.__version__)')"
elif [ -x "$ORIG/tools/duckdb" ]; then
   printf '  duckdb     : pinned CLI %s\n' "$("$ORIG"/tools/duckdb --version 2>/dev/null | head -1)"
else
   bad "no DuckDB backend; the ground truth cannot be produced"
fi
[ -x "$FSSTLIKE_CLI" ] || { bad "$FSSTLIKE_CLI is not built (cmake --build $BUILD -j8)"; }

step "unit tests (in-process FSST, reference matcher as oracle)"
if [ -x "$BUILD/fsst_test_unit" ]; then
   if "$BUILD/fsst_test_unit" | tail -4; then ok "test_unit"; else bad "test_unit"; fi
else
   bad "$BUILD/fsst_test_unit is not built"
fi

step "structural validation of every compressed column"
for meta in data/*/*.fsst.meta.json; do
   [ -f "$meta" ] || continue
   col="${meta%.fsst.meta.json}"
   if "$FSSTLIKE_CLI" --mode fsst-rev --col "$col" --pattern '%' --validate --runs 1 >/dev/null 2>&1; then
      ok "$(basename "$col"): offsets, code range, escapes and .fsst.rev all consistent"
   else
      bad "$(basename "$col") failed --validate"
   fi
done

step "DuckDB LIKE semantics this experiment is built on"
if "$PY" scripts/truth.py --semantics > /dev/null 2>&1; then ok "all semantics probes as expected"
else bad "a DuckDB semantics probe disagrees"; fi

step "pattern correctness against DuckDB, all eight modes"
total_pass=0; total_skip=0
for pf in patterns/edge.txt patterns/core.txt patterns/wildcard.txt patterns/concat.txt \
          patterns/scales.txt patterns/fuzz.txt; do
   [ -f "$pf" ] || continue
   out=$("$PY" scripts/check.py "$pf" 2>&1)
   line=$(printf '%s\n' "$out" | grep -E '^ +[0-9]+ passed' | tail -1)
   if printf '%s\n' "$out" | grep -q 'match DuckDB'; then
      ok "$pf -- $(echo "$line" | tr -s ' ')"
      p=$(echo "$line" | sed -E 's/^ *([0-9]+) passed.*/\1/'); s=$(echo "$line" | sed -E 's/.* ([0-9]+) skipped.*/\1/')
      total_pass=$((total_pass + p)); total_skip=$((total_skip + s))
   else
      bad "$pf"
      printf '%s\n' "$out" | sed 's/^/    /' | tail -20 >&2
   fi
done
printf '  total: %d checks passed, %d skipped\n' "$total_pass" "$total_skip"

step "cross-check against the original TokenLIKE (different implementation, same rows)"
if [ -x "$BUILD/tokenlike" ] && [ -f "$ORIG/data/vocab.bin" ]; then
   # Writes to a scratch path: bench/v1-* and bench/v2-* are recorded evidence,
   # and a gate that overwrites them on every run destroys what it documents.
   OUT="${FSSTLIKE_COMPARE_OUT:-bench/latest/vs_tokenlike.json}"
   mkdir -p "$(dirname "$OUT")"
   if "$PY" scripts/compare_tokenlike.py --out "$OUT" 2>&1 | tail -3; then
      ok "both systems return identical row ids"
   else
      bad "the two systems disagree"
   fi
else
   printf '  SKIPPED: TokenLIKE snapshot not built, or its data/ is absent.\n'
   printf '           See docs/REPRODUCE.md for the read-only git archive step.\n'
fi

printf '\n'
if [ "$failures" -eq 0 ]; then printf 'check_all: ALL CHECKS PASSED\n'; exit 0; fi
printf 'check_all: %d CHECK(S) FAILED\n' "$failures" >&2
exit 1
