#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Regenerates assets/je-cooked of the texture-formats test project: writes source/
# (make_sources.py) and cooks it for desktop, mobile and web into assets/ as an export would.
#   tools/test-projects/texture-formats/make_assets.sh [editor binary]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EDITOR="${1:-build/linux/editor-install/bin/jasperengine-editor}"
python3 "$HERE/make_sources.py"
rm -rf "$HERE/assets/je-cooked"
"$EDITOR" cook-textures --project "$HERE/source" --platform desktop --platform mobile --platform web --out "$HERE/assets"
rm -rf "$HERE/source/.jasperengine"
