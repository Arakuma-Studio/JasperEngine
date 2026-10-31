#!/usr/bin/env bash
# The upgrade test: open the fixture an origin/main editor made (tools/test-projects/
# upgrade-from-main, generate.sh) in THIS editor and check that the game survives the upgrade.
#
#   tools/build.sh linux                       # this editor
#   tools/build.sh players                     # the prebuilt player runtimes (build/players)
#   tools/ci/upgrade_from_main/check.sh [--skip-web] [--tolerance <t>]
#
#   1. check.py (in the Linux build image, through MCP): first open (no error or warning, empty
#      load reports, everything migrated to the current formats and saved, .meta files and
#      GUIDs, every reference resolving), second open (stable: nothing rewritten), and the
#      game's input and autopilot transcripts against the old editor's; screenshots compared;
#   2. the migrated project exported and compiled for Linux (player_linux.sh), and built with the
#      prebuilt Linux player (S30: jasperengine-editor build --method prebuilt, in the no-compiler
#      runtime image; needs the player runtimes: tools/build.sh players): each transcript against
#      the old Linux player's;
#   3. the same export for the web (player_web.sh, the web image): its transcript against the
#      old web player's.
# Results in build/upgrade-from-main/check/ (report.md, report.json, the transcripts, screenshots
# and their diffs). Exit 1 when a check fails. --tolerance: the largest difference allowed
# between transcript decimals (default 0: identical; docs/upgrading-from-main.md).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT/tools/ci/upgrade_from_main/lib.sh"
FIXTURE=tools/test-projects/upgrade-from-main
OUT=build/upgrade-from-main/check
SKIP_WEB=0 TOL=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-web) SKIP_WEB=1; shift ;;
        --tolerance) TOL="$2"; shift 2 ;;
        -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
EDITOR=/src/build/linux/editor-install/bin/jasperengine-editor
[[ -x "$ROOT/build/linux/editor-install/bin/jasperengine-editor" ]] || { echo "no editor: run tools/build.sh linux" >&2; exit 1; }
[[ -f "$ROOT/$FIXTURE/SOURCE.yaml" ]] || { echo "no fixture at $FIXTURE: run generate.sh" >&2; exit 1; }
[[ -d "$ROOT/build/players" ]] || { echo "no player runtimes in build/players: run tools/build.sh players" >&2; exit 1; }

ufm_image
failed=0
echo "== 1. the editor (check.py)"
ufm_run "$UFM_PY tools/ci/upgrade_from_main/check.py --editor $EDITOR --out $OUT --tolerance $TOL" || failed=1
[[ -d "$ROOT/$OUT/project" ]] || { echo "FAIL: check.py left no migrated project"; exit 1; }

echo "== 2. Linux, compiled"
if ufm_run "tools/ci/upgrade_from_main/player_linux.sh $EDITOR $OUT/project $OUT/player-linux"; then
    python3 "$ROOT/tools/ci/upgrade_from_main/compare.py" --tolerance "$TOL" \
        "$ROOT/$FIXTURE/transcripts/player-linux.txt" "$ROOT/$OUT/player-linux/transcript.txt" || failed=1
else
    failed=1
fi

echo "== 2b. Linux, prebuilt player"
ufm_runtime_image
if UFM_RUN_IMAGE="$UFM_RUNTIME_IMAGE" ufm_run "tools/ci/upgrade_from_main/player_linux.sh $EDITOR $OUT/project $OUT/player-prebuilt prebuilt"; then
    python3 "$ROOT/tools/ci/upgrade_from_main/compare.py" --tolerance "$TOL" \
        "$ROOT/$FIXTURE/transcripts/player-linux.txt" "$ROOT/$OUT/player-prebuilt/transcript.txt" || failed=1
else
    failed=1
fi

if [[ $SKIP_WEB == 0 ]]; then
    echo "== 3. web"
    # a copy: an export saves the project's scenes again
    ufm_run "rm -rf $OUT/web-export $OUT/web-project && cp -r $OUT/project $OUT/web-project && $EDITOR export --project $OUT/web-project --out $OUT/web-export --backend opengles > $OUT/web-export.log 2>&1" \
        || { echo "FAIL: web export"; tail -20 "$ROOT/$OUT/web-export.log"; failed=1; }
    if [[ -d "$ROOT/$OUT/web-export" ]] && "$ROOT/tools/build.sh" exec web tools/ci/upgrade_from_main/player_web.sh "$OUT/web-export" "$OUT/web"; then
        python3 "$ROOT/tools/ci/upgrade_from_main/compare.py" --tolerance "$TOL" \
            "$ROOT/$FIXTURE/transcripts/player-web.txt" "$ROOT/$OUT/web/transcript.txt" || failed=1
    else
        failed=1
    fi
fi

[[ $failed == 0 ]] && echo "UPGRADE CHECK PASS" || echo "UPGRADE CHECK FAIL (build/upgrade-from-main/check/report.md)"
exit $failed
