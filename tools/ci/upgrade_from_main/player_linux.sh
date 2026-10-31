#!/usr/bin/env bash
# Build the fixture's Linux player and run it under a virtual X server with the game's autopilot
# until it quits after its last scene (generate.sh, check.sh). Two methods:
#   compile   (default) the editor's command-line exporter, then the exported CMake project built
#             as a user's game (no presets, no Werror); in the Linux build image
#   prebuilt  jasperengine-editor build --method prebuilt (S30): the project's content next to a
#             prebuilt player runtime, no compiler; in the runtime image (JASPER_PLAYERS_DIR)
#
#   player_linux.sh <editor> <project-dir> <work-dir> [compile|prebuilt]
#
# Writes <work-dir>/export.log, game-build.log, game.log and transcript.txt (the T| lines).
# Exit 1 when a step fails, the game logs an error or it does not reach its end.
set -euo pipefail
EDITOR="$1" PROJECT="$2" WORK="$3" METHOD="${4:-compile}"
rm -rf "$WORK"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
cp -r "$PROJECT" "$WORK/project"
cd "$WORK"

if [[ "$METHOD" == prebuilt ]]; then
    echo "== build with the prebuilt player ($EDITOR)"
    "$EDITOR" build --project project --platform linux --backend opengl --out game-out --method prebuilt > build.log 2>&1 \
        || { echo "FAIL: build"; tail -30 build.log; exit 1; }
    grep -q "^method: prebuilt" build.log || { echo "FAIL: the build did not use the prebuilt player"; tail -30 build.log; exit 1; }
    GAME="$(sed -n "s/^executable: //p" build.log | head -1)"
    [[ -x "$GAME" ]] || { echo "FAIL: no executable ($GAME)"; exit 1; }
else
    echo "== export ($EDITOR)"
    "$EDITOR" export --project project --out export --backend opengl > export.log 2>&1 \
        || { echo "FAIL: export"; tail -30 export.log; exit 1; }

    echo "== build the exported project"
    cmake -S export -B export-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRAPHIC_BACKEND=glcore \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > game-build.log 2>&1 \
        && cmake --build export-build >> game-build.log 2>&1 \
        || { echo "FAIL: build of the exported project"; tail -30 game-build.log; exit 1; }
    GAME="$(find export-build -maxdepth 1 -type f -executable | head -1)"
    [[ -n "$GAME" ]] || { echo "FAIL: no executable in export-build"; exit 1; }
fi
GAME="$(cd "$(dirname "$GAME")" && pwd)/$(basename "$GAME")"

echo "== run $(basename "$GAME") (autopilot)"
Xvfb :97 -screen 0 1280x720x24 > /dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null || true' EXIT
export DISPLAY=:97
sleep 1
status=0
(cd "$(dirname "$GAME")" && UPGRADE_AUTOPILOT=1 UPGRADE_EXIT_AT_END=1 JASPERENGINE_READY_SIGNAL=1 \
    timeout 600 stdbuf -oL -eL "./$(basename "$GAME")" > "$WORK/game.log" 2>&1) || status=$?
grep -ao 'T|[^[:cntrl:]]*' game.log | sed 's/[[:space:]]*$//' > transcript.txt || true
echo "exit status $status, $(wc -l < transcript.txt) transcript lines"
if grep -aE "\((ERROR|PANIC)\)|\[ERROR\]|Lua Error|[Mm]issing (precompiled )?shader|Segmentation fault|terminate called|JASPERENGINE CRASH:" game.log \
        | grep -avE "Failed to initialize audio output"; then
    echo "FAIL: the game logged errors"; exit 1
fi
grep -q '^T|Arena2D|[0-9]*|end|' transcript.txt || { echo "FAIL: the game did not reach its end"; tail -20 game.log; exit 1; }
[[ $status == 0 ]] || { echo "FAIL: the game exited with status $status"; exit 1; }
echo "PASS"
