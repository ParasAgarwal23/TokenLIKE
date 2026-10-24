#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Person B's correctness gate. Reproduces every claim we make about the data,
# from a clean checkout, in one command.
#
#   bash scripts/check_all.sh              # verify whatever datasets exist
#   bash scripts/check_all.sh --quick      # skip the full-scale round-trip
#   TOKENLIKE_CLI=build/tokenlike bash scripts/check_all.sh
#
# Exits non-zero on the first failure. Patterns whose CLI is missing are
# reported as skipped, never as passed.
# ---------------------------------------------------------------------------
set -u -o pipefail

cd "$(dirname "$0")/.." || exit 1

PY="${TOKENLIKE_PYTHON:-.venv/bin/python}"
[ -x "$PY" ] || PY="python3"
CLI="${TOKENLIKE_CLI:-build/tokenlike}"
VOCAB="data/vocab.bin"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

failures=0
step() { printf '\n=== %s\n' "$1"; }
ok()   { printf '  OK: %s\n' "$1"; }
bad()  { printf '  FAIL: %s\n' "$1" >&2; failures=$((failures + 1)); }
run()  { if "$@"; then ok "$*"; else bad "$*"; fi; }

step "environment"
printf '  python : %s (%s)\n' "$PY" "$("$PY" -V 2>&1)"
printf '  cli    : %s%s\n' "$CLI" "$([ -x "$CLI" ] || printf ' (not built -- pattern checks will be skipped)')"
if "$PY" -c 'import duckdb' 2>/dev/null; then
   printf '  duckdb : python module %s\n' "$("$PY" -c 'import duckdb;print(duckdb.__version__)')"
elif [ -x tools/duckdb ]; then
   printf '  duckdb : pinned CLI %s\n' "$(tools/duckdb --version 2>/dev/null | head -1)"
else
   bad "no DuckDB backend available; see scripts/verify/common.py for setup"
fi

step "vocabulary"
if [ ! -f "$VOCAB" ]; then
   printf '  building %s\n' "$VOCAB"
   run "$PY" scripts/encode/build_vocab.py
else
   ok "$VOCAB present ($(wc -c <"$VOCAB" | tr -d ' ') bytes)"
fi

step "C++ loaders"
mkdir -p build
# Person C's CMake is the real build; this keeps the loaders testable on their
# own. The stub include path is only used when C's common.h is not there yet.
STUB_FLAG=""
if [ ! -f include/tokenlike/common.h ]; then
   if [ -n "${TOKENLIKE_COMMON_STUB:-}" ] && [ -f "$TOKENLIKE_COMMON_STUB/tokenlike/common.h" ]; then
      STUB_FLAG="-I$TOKENLIKE_COMMON_STUB"
      printf '  note: using common.h stub from %s\n' "$TOKENLIKE_COMMON_STUB"
   else
      bad "include/tokenlike/common.h is missing (Person C owns it); set TOKENLIKE_COMMON_STUB to build anyway"
   fi
fi
if [ -f include/tokenlike/common.h ] || [ -n "$STUB_FLAG" ]; then
   if clang++ -std=c++20 -O2 -Wall -Wextra -Iinclude $STUB_FLAG \
        src/vocab.cpp src/column.cpp tests/test_io.cpp -o build/test_io 2>build/test_io.buildlog; then
      ok "test_io compiled"
      for col in data/clickbench/URL data/sample/urls data/edge/values; do
         [ -f "$col.tok" ] || continue
         run ./build/test_io "$VOCAB" "$col"
      done
   else
      bad "test_io failed to compile (see build/test_io.buildlog)"
      sed 's/^/    /' build/test_io.buildlog >&2
   fi
fi

step "round-trip (decode every row, compare with original bytes)"
for col in data/edge/values data/sample/urls data/clickbench/URL data/clickbench/Referer; do
   [ -f "$col.tok" ] || continue
   if [ "$QUICK" = "1" ] && [ "${col#data/clickbench/}" != "$col" ]; then
      printf '  skipped %s (--quick)\n' "$col"
      continue
    fi
   run "$PY" scripts/encode/roundtrip.py "$col" --vocab "$VOCAB"
done

step "pattern correctness vs DuckDB"
for pf in patterns/core.txt patterns/tier1.txt patterns/generated_edge.txt patterns/tier2.txt; do
   [ -f "$pf" ] || continue
   printf '  -- %s\n' "$pf"
   if "$PY" scripts/verify/check.py "$pf" --modes token,decode,raw --cli "$CLI" --vocab "$VOCAB"; then
      ok "$pf"
   else
      bad "$pf"
   fi
done

printf '\n'
if [ "$failures" -eq 0 ]; then
   printf 'check_all: ALL CHECKS PASSED\n'
   exit 0
fi
printf 'check_all: %d CHECK(S) FAILED\n' "$failures" >&2
exit 1
