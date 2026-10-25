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
# Windows/MSYS builds produce tokenlike.exe; accept either spelling.
[ -x "$CLI" ] || [ ! -x "$CLI.exe" ] || CLI="$CLI.exe"
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
elif CLI_DUCKDB=$("$PY" -c 'import sys; sys.path.insert(0, "scripts/verify"); import common; print(common.find_duckdb_cli() or "")' 2>/dev/null) \
     && [ -n "$CLI_DUCKDB" ]; then
   # Ask the resolver the verification scripts themselves use, so the probe
   # cannot report "no backend" for a CLI that check.py then goes on to use
   # (TOKENLIKE_DUCKDB, tools/duckdb, or one on PATH).
   printf '  duckdb : CLI %s (%s)\n' "$CLI_DUCKDB" "$("$CLI_DUCKDB" --version 2>/dev/null | head -1)"
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
HAVE_COMMON=1
if [ ! -f include/tokenlike/common.h ]; then
   if [ -n "${TOKENLIKE_COMMON_STUB:-}" ] && [ -f "$TOKENLIKE_COMMON_STUB/tokenlike/common.h" ]; then
      STUB_FLAG="-I$TOKENLIKE_COMMON_STUB"
      printf '  note: using common.h from %s\n' "$TOKENLIKE_COMMON_STUB"
   else
      # common.h is Person C's file and is not on the b/data branch. Its
      # absence means "this tree is not the integrated one", not "something is
      # broken", so skip the C++ section loudly instead of failing: the
      # loaders are compiled and tested by CMake on main.
      HAVE_COMMON=0
      printf '  SKIPPED: include/tokenlike/common.h is not in this tree (Person C owns it).\n'
      printf '           Run this on the integrated tree, or point TOKENLIKE_COMMON_STUB\n'
      printf '           at a directory containing tokenlike/common.h, to build test_io here.\n'
   fi
fi
if [ "$HAVE_COMMON" = "1" ] || [ -n "$STUB_FLAG" ]; then
   if clang++ -std=c++20 -O2 -Wall -Wextra -Iinclude $STUB_FLAG \
        src/vocab.cpp src/column.cpp tests/test_io.cpp -o build/test_io 2>build/test_io.buildlog; then
      ok "test_io compiled"
      # Every encoded column present, so a new dataset is covered automatically.
      for tok in data/*/*.tok; do
         [ -f "$tok" ] || continue
         run ./build/test_io "$VOCAB" "${tok%.tok}"
      done
   else
      bad "test_io failed to compile (see build/test_io.buildlog)"
      sed 's/^/    /' build/test_io.buildlog >&2
   fi
fi

step "round-trip (decode every row, compare with original bytes)"
for tok in data/*/*.tok; do
   [ -f "$tok" ] || continue
   col="${tok%.tok}"
   rows=$(( $(wc -c <"${col}.off" | tr -d ' ') / 8 - 1 ))
   if [ "$QUICK" = "1" ] && [ "$rows" -gt 100000 ]; then
      printf '  skipped %s (%s rows, --quick)\n' "$col" "$rows"
      continue
   fi
   run "$PY" scripts/encode/roundtrip.py "$col" --vocab "$VOCAB"
done

step "pattern correctness vs DuckDB"
if [ ! -x "$CLI" ]; then
   # Person C's CLI is what turns patterns into row ids. Its absence is not a
   # failure of the data pipeline, so it is reported loudly and skipped -- but
   # if a CLI IS present and gets any pattern wrong, that is a hard failure.
   printf '  SKIPPED: no CLI at %s\n' "$CLI"
   printf '           build Person C'"'"'s CLI, or set TOKENLIKE_CLI, to run this section.\n'
   printf '           ground truth is still generated and verified below.\n'
   for pf in patterns/core.txt patterns/tier1.txt patterns/generated_edge.txt patterns/tier2.txt; do
      [ -f "$pf" ] || continue
      if "$PY" scripts/verify/ground_truth.py "$pf" >/dev/null 2>&1; then
         ok "ground truth for $pf"
      else
         bad "ground truth generation failed for $pf"
      fi
   done
else
   for pf in patterns/core.txt patterns/tier1.txt patterns/generated_edge.txt patterns/tier2.txt; do
      [ -f "$pf" ] || continue
      printf '  -- %s\n' "$pf"
      if "$PY" scripts/verify/check.py "$pf" --modes token,decode,raw --cli "$CLI" --vocab "$VOCAB"; then
         ok "$pf"
      else
         bad "$pf"
      fi
   done
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
   printf 'check_all: ALL CHECKS PASSED\n'
   exit 0
fi
printf 'check_all: %d CHECK(S) FAILED\n' "$failures" >&2
exit 1
