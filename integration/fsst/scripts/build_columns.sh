#!/usr/bin/env bash
# Builds every FSST column this experiment benchmarks, from the original
# TokenLIKE raw columns (read-only). Idempotent; re-run to rebuild.
set -u -o pipefail
cd "$(dirname "$0")/.." || exit 1
ORIG="${TOKENLIKE_DIR:-/Users/prahasbodanapati/Documents/Desk/semV/HASHCODE/hash/TokenLIKE}"
BUILD="${FSSTLIKE_BUILD:-../../build/fsst}"
ENC="$BUILD/fsst_encode"
[ -x "$ENC" ] || { echo "no fsst_encode at $ENC; configure with -DTOKENLIKE_WITH_FSST=ON" >&2; exit 1; }
[ -d "$ORIG/data" ] || { echo "no TokenLIKE data at $ORIG/data; set TOKENLIKE_DIR" >&2; exit 1; }

mk() { # mk <src prefix> <dst prefix> [rows]
   local src="$ORIG/data/$1" dst="data/$2" rows="${3:-0}"
   mkdir -p "$(dirname "$dst")"
   if [ ! -f "$src.raw" ]; then echo "SKIP $2 ($src.raw not present)"; return 0; fi
   printf '==> %s' "$2"; [ "$rows" != 0 ] && printf ' (%s rows)' "$rows"; printf '\n'
   if [ "$rows" = 0 ]; then "$ENC" --raw "$src" --out "$dst" > "$dst.encode.log" 2>&1
   else "$ENC" --raw "$src" --out "$dst" --rows "$rows" > "$dst.encode.log" 2>&1; fi
   local rc=$?
   if [ $rc -ne 0 ]; then echo "FAILED $2"; cat "$dst.encode.log"; return 1; fi
   grep -E '"(rows|raw_bytes|cmp_bytes|ratio|nsymbols|escape_units|rows_with_escape)"' "$dst.encode.log" | tr -d ' \n'
   printf '\n'
}

mk clickbench/URL      clickbench/URL_1k      1000     || exit 1
mk clickbench/URL      clickbench/URL_10k     10000    || exit 1
mk clickbench/URL      clickbench/URL_100k    100000   || exit 1
mk clickbench/URL      clickbench/URL_1m      1000000  || exit 1
mk clickbench/Referer  clickbench/Referer_1m  1000000  || exit 1
mk tpch/l_comment      tpch/l_comment_1m      1000000  || exit 1
mk tpch/o_comment      tpch/o_comment_1m      1000000  || exit 1
mk dbtext/urls         dbtext/urls_100k       0        || exit 1
mk fuzz/values         fuzz/values            0        || exit 1
mk edge/values         edge/values            0        || exit 1
mk sample/urls         sample/urls_1k         0        || exit 1
echo "build_columns: done"
