#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# S68 AC2, the web half: builds the source export test_linked_folders_export.py::test_web_export_for_the_web_game
# left in <export dir> and runs it in the web image's headless Chrome. The game must draw the texture of
# a linked folder and run the Lua module of another (tools/web/gates/linked-folder-game.json).
#   tools/web/linked_folder_game.sh <export dir> [results dir]     in the web image (docs/testing.md)
set -euo pipefail
EXPORT="$(cd "$1" && pwd)"
RESULTS="${2:-$EXPORT/results}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$EXPORT/build-web"
mkdir -p "$RESULTS"
emcmake cmake -S "$EXPORT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DGRAPHIC_BACKEND=gles3
cmake --build "$BUILD" -j "${JOBS:-8}"
# (not <app>.export.html, the template the page is made from)
PAGE="$(cd "$BUILD" && ls -- *.html | grep -v "\.export\.html$" | head -1)"
python3 "$ROOT/tools/web/browser_gate.py" --root "$BUILD" --page "$PAGE" --until-ready \
    --baseline "$ROOT/tools/web/gates/linked-folder-game.json" --expect LINKED-LUA-OK \
    --screenshot "$RESULTS/linked-folder-game.png" --transcript "$RESULTS/linked-folder-game.log"
