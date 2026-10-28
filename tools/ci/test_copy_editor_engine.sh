#!/usr/bin/env bash
# CopyEditorEngine.cmake mirrors engine/ (BF5): a file deleted from engine/ leaves the editor
# build's copy (manifest, and a copy made before the manifest) and the installed copy
# (PRUNE_ONLY), while the build products in the build tree's copy stay.
#   tools/ci/test_copy_editor_engine.sh        (needs cmake; a few seconds)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT/CopyEditorEngine.cmake"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0
check() { if eval "$1"; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }

SRC="$TMP/engine"
mkdir -p "$SRC/core/action" "$SRC/core/object" "$SRC/tests" "$SRC/libs/rmlui/Tests" "$SRC/libs/foo/build"
echo a > "$SRC/core/action/ParticleSystem.h"
echo b > "$SRC/core/object/ParticleSystem.h"
echo c > "$SRC/tests/SomeTest.cpp"
echo d > "$SRC/libs/rmlui/Tests/T.cpp"
echo e > "$SRC/libs/foo/build/out.h"
echo f > "$SRC/CMakePresets.json"
copy() { cmake -DENGINE_SOURCE="$SRC" -DENGINE_DEST="$1" "${@:2}" -P "$SCRIPT" > "$TMP/last.log"; }

# the build tree's copy, with a manifest; its build products are not the engine's
DEST="$TMP/editor/engine"; MANIFEST="$TMP/editor/engine-copy-manifest.txt"
copy "$DEST" -DMANIFEST="$MANIFEST"
check '[[ -f $DEST/core/action/ParticleSystem.h && -f $DEST/core/object/ParticleSystem.h ]]' "copied"
check '[[ ! -e $DEST/tests && ! -e $DEST/libs/foo/build && ! -e $DEST/CMakePresets.json ]]' "exclusions not copied"
mkdir -p "$DEST/CMakeFiles/x.dir" "$DEST/generated" "$DEST/libs/foo"
echo o > "$DEST/CMakeFiles/x.dir/a.cpp.o"; echo g > "$DEST/generated/shaderlib.h"
echo l > "$DEST/libs/foo/libfoo.a"; echo i > "$DEST/cmake_install.cmake"
rm "$SRC/core/action/ParticleSystem.h"; rmdir "$SRC/core/action"
copy "$DEST" -DMANIFEST="$MANIFEST"
check '[[ ! -e $DEST/core/action/ParticleSystem.h && ! -e $DEST/core/action ]]' "a deleted header and its emptied folder leave the copy"
check '[[ -f $DEST/core/object/ParticleSystem.h ]]' "the other header stays"
check '[[ -f $DEST/CMakeFiles/x.dir/a.cpp.o && -f $DEST/generated/shaderlib.h && -f $DEST/libs/foo/libfoo.a && -f $DEST/cmake_install.cmake ]]' "build products stay"
check 'grep -q "Removed from the engine copy (gone from engine/): core/action/ParticleSystem.h" $TMP/last.log' "the removal is logged"

# a copy made before the manifest existed: its stale source files go, build products stay
OLD="$TMP/old/engine"
mkdir -p "$OLD/core/action" "$OLD/CMakeFiles/y.dir" "$OLD/generated"
echo a > "$OLD/core/action/ParticleSystem.h"; echo a > "$OLD/core/action/ParticleSystem.cpp"
echo o > "$OLD/CMakeFiles/y.dir/gone.cpp"; echo g > "$OLD/generated/gone.h"
copy "$OLD" -DMANIFEST="$TMP/old/engine-copy-manifest.txt"
check '[[ ! -e $OLD/core/action/ParticleSystem.h && ! -e $OLD/core/action/ParticleSystem.cpp ]]' "an old copy loses the deleted sources"
check '[[ -f $OLD/CMakeFiles/y.dir/gone.cpp && -f $OLD/generated/gone.h ]]' "an old copy keeps CMakeFiles/ and generated/"
check '[[ -s $TMP/old/engine-copy-manifest.txt ]]' "the manifest is written"

# the installed copy: a pure mirror after the install exclusions
INST="$TMP/install/bin/engine"
mkdir -p "$INST/core/action" "$INST/core/object" "$INST/libs/rmlui/Tests"
echo a > "$INST/core/action/ParticleSystem.h"; echo b > "$INST/core/object/ParticleSystem.h"
echo d > "$INST/libs/rmlui/Tests/T.cpp"
copy "$INST" -DPRUNE_ONLY=ON -DINSTALL_EXCLUDES=ON
check '[[ ! -e $INST/core/action/ParticleSystem.h && ! -e $INST/libs/rmlui/Tests/T.cpp ]]' "the install loses the deleted header and the excluded files"
check '[[ -f $INST/core/object/ParticleSystem.h ]]' "the install keeps the engine's files"
check '[[ ! -e $INST/tests && ! -e $INST/CMakePresets.json ]]' "PRUNE_ONLY copies nothing"

if (( fails )); then echo "$fails check(s) failed"; exit 1; fi
echo "all checks passed"
