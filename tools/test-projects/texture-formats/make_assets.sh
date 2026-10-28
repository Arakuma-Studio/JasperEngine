#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Regenerates assets/je-cooked of the texture-formats test project: writes source/
# (make_sources.py) and cooks it for desktop, mobile, android_etc2 and web into assets/ as an export
# with the ETC2 fallback would; then the web transcode bench's file, assets/bench/uastc-photo-512.ktx2
# (bench-source/, its web cook).
#   tools/test-projects/texture-formats/make_assets.sh [editor binary]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EDITOR="${1:-build/linux/editor-install/bin/jasperengine-editor}"
python3 "$HERE/make_sources.py"
rm -rf "$HERE/assets/je-cooked"
"$EDITOR" cook-textures --project "$HERE/source" --platform desktop --platform mobile --platform android_etc2 --platform web --out "$HERE/assets"
rm -rf "$HERE/source/.jasperengine"
BENCH_OUT="$(mktemp -d)"
"$EDITOR" cook-textures --project "$HERE/bench-source" --platform web --out "$BENCH_OUT"
mkdir -p "$HERE/assets/bench"
python3 - "$BENCH_OUT/je-cooked/web" "$HERE/assets/bench/uastc-photo-512.ktx2" <<'PY'
import json, os, shutil, sys
entry = json.load(open(os.path.join(sys.argv[1], "index.json")))["textures"]["textures/photo.png"]
assert entry["format"] == "UASTC" and entry["width"] == 512, entry
shutil.copyfile(os.path.join(sys.argv[1], entry["file"]), sys.argv[2])
PY
rm -rf "$BENCH_OUT" "$HERE/bench-source/.jasperengine"
