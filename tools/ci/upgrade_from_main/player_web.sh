#!/usr/bin/env bash
# Build an exported fixture (player_linux.sh's export, made with --backend opengles by
# check.sh / generate.sh) for the web with Emscripten and play it in headless Chrome
# (tools/web/browser_gate.py) with the game's autopilot (the web build turns it on). Runs inside
# the web build image (tools/build.sh exec web).
#
#   player_web.sh <export-dir> <work-dir> [seconds]
#
# The page runs until the game prints its end line, for at most <seconds> (1200) of wall clock: the
# game's autopilot fixes its clock at 1/60 s a frame, so a slow software WebGL takes longer, never
# other frames.
#
# Writes <work-dir>/web-build.log, console.txt (the browser console) and transcript.txt.
set -euo pipefail
EXPORT="$1" WORK="$2" SECONDS_TO_RUN="${3:-1200}"
rm -rf "$WORK"
mkdir -p "$WORK"
echo "== build the web player"
emcmake cmake -S "$EXPORT" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DGRAPHIC_BACKEND=gles3 > "$WORK/web-build.log" 2>&1 \
    && cmake --build "$WORK/build" --parallel "${JOBS:-$(nproc)}" >> "$WORK/web-build.log" 2>&1 \
    || { echo "FAIL: web build"; tail -30 "$WORK/web-build.log"; exit 1; }
# the game's page: the .html with its .js next to it
PAGE=""
for html in "$WORK"/build/*.html; do
    [[ -f "${html%.html}.js" ]] && PAGE="$(basename "$html")" && break
done
[[ -n "$PAGE" ]] || { echo "FAIL: no .html in $WORK/build"; exit 1; }
echo "== play $PAGE in headless Chrome until its end (at most ${SECONDS_TO_RUN}s)"
status=0
python3 tools/web/browser_gate.py --root "$WORK/build" --page "$PAGE" --until-line "|end|" \
    --seconds "$SECONDS_TO_RUN" --transcript "$WORK/console.txt" --screenshot "$WORK/web.png" > "$WORK/gate.log" 2>&1 || status=$?
grep -ao 'T|[^[:cntrl:]]*' "$WORK/console.txt" | sed 's/[[:space:]]*$//' > "$WORK/transcript.txt" || true
echo "gate exit $status, $(wc -l < "$WORK/transcript.txt") transcript lines"
tail -5 "$WORK/gate.log"
grep -q '^T|Arena2D|[0-9]*|end|' "$WORK/transcript.txt" || { echo "FAIL: the web game did not reach its end"; exit 1; }
[[ $status == 0 ]] || { echo "FAIL: the browser gate failed (status $status)"; exit 1; }
echo "PASS"
