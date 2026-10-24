#!/usr/bin/env bash
# Fetches the pinned upstream FSST snapshot into integration/fsst/third_party/fsst.
# Optional: the module's CMake will FetchContent the same commit if this is not
# run. Useful for offline or repeated builds.
set -eu
cd "$(dirname "$0")/.."
PIN=e638d4cf8c26129d73c242a4127b42b975de5b63
URL=https://github.com/cwida/fsst.git
DEST=third_party/fsst
if [ -d "$DEST/.git" ]; then
   echo "already present: $DEST @ $(git -C "$DEST" rev-parse --short HEAD)"
else
   mkdir -p third_party
   git clone --quiet "$URL" "$DEST"
fi
git -C "$DEST" -c advice.detachedHead=false checkout --quiet "$PIN"
echo "FSST pinned at $(git -C "$DEST" rev-parse HEAD)"
test -f "$DEST/fsst.h" && echo "fsst.h present: ready to build"
