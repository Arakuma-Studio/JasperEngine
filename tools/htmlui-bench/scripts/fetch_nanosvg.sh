#!/usr/bin/env bash
# Clones nanosvg at the commit the phase-0 benchmark measured (the engine's former copy,
# engine/libs/nanosvg, removed in phase 1 when TextureData moved to LunaSVG) into
# /tmp/htmlui-src/nanosvg, the default NANOSVG_DIR of CMakeLists.txt (headers in src/).
#
#   tools/htmlui-bench/scripts/fetch_nanosvg.sh [DIR]
set -euo pipefail

DIR="${1:-/tmp/htmlui-src/nanosvg}"
COMMIT=c59ff2349dc40967bbc0d840f8be17be21e82c68   # nanosvg.h md5 468808c5..., nanosvgrast.h ef21471c...

if [ -d "$DIR/.git" ] && [ "$(git -C "$DIR" rev-parse HEAD)" = "$COMMIT" ]; then
    echo "$DIR already at $COMMIT"
    exit 0
fi
rm -rf "$DIR"
git clone -q https://github.com/memononen/nanosvg "$DIR"
git -C "$DIR" checkout -q "$COMMIT"
echo "nanosvg $COMMIT in $DIR"
