#!/usr/bin/env bash
# Generate the upgrade-from-main fixture (tools/test-projects/upgrade-from-main) with an OLD
# editor: the editor of an origin/main commit, before the integration branch reaches main.
#
#   tools/ci/upgrade_from_main/generate.sh --ref origin/main            # build that commit, then generate
#   tools/ci/upgrade_from_main/generate.sh --old-src <checkout> [--commit <sha>]
#   tools/ci/upgrade_from_main/generate.sh --old-editor <editor-install> --commit <sha>
#
# --ref <ref>        a commit of this repository: checked out (git worktree) in
#                    build/upgrade-from-main/old-src and built there with its own tools/build.sh linux
# --old-src <dir>    a checkout of the old commit, built there (tools/build.sh linux); --commit
#                    names its commit when <dir> is not a git checkout of it (a build server copy)
# --old-editor <dir> an old editor-install directory already built (no build)
# --skip-player      do not export, build and run the old Linux and web players
# --skip-cpp         do not make the C++ script fixture
# --no-install       leave the results in build/upgrade-from-main/gen (no copy into tools/test-projects)
#
# The old editor makes the game through its own MCP tools (gen_fixture.py), plays it twice
# (input: keys sent by send_input; autopilot: the game plays its own schedule) and records the
# transcripts and screenshots; its exported Linux player plays the autopilot run once more
# (player_linux.sh), and so does its web export in headless Chrome (player_web.sh). Then the
# results replace tools/test-projects/upgrade-from-main/:
#     project/ cpp-project/ transcripts/ screenshots/ recipe/ SOURCE.yaml
# Needs Docker and git; runs the old editor and its player in this checkout's Linux build image.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=lib.sh
source "$ROOT/tools/ci/upgrade_from_main/lib.sh"
WORK="$ROOT/build/upgrade-from-main"
DEST="$ROOT/tools/test-projects/upgrade-from-main"

usage() { sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
REF="" OLD_SRC="" OLD_EDITOR="" COMMIT="" SKIP_PLAYER=0 SKIP_CPP=0 INSTALL=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ref) REF="$2"; shift 2 ;;
        --old-src) OLD_SRC="$(cd "$2" && pwd)"; shift 2 ;;
        --old-editor) OLD_EDITOR="$(cd "$2" && pwd)"; shift 2 ;;
        --commit) COMMIT="$2"; shift 2 ;;
        --skip-player) SKIP_PLAYER=1; shift ;;
        --skip-cpp) SKIP_CPP=1; shift ;;
        --no-install) INSTALL=0; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done
[[ -n "$REF$OLD_SRC$OLD_EDITOR" ]] || usage
mkdir -p "$WORK"

if [[ -n "$REF" ]]; then
    COMMIT="$(git -C "$ROOT" rev-parse --verify "$REF^{commit}")"
    OLD_SRC="$WORK/old-src"
    if [[ -e "$OLD_SRC/.git" ]]; then
        git -C "$OLD_SRC" checkout --quiet --detach "$COMMIT"
    else
        rm -rf "$OLD_SRC"
        git -C "$ROOT" worktree add --quiet --detach "$OLD_SRC" "$COMMIT"
    fi
fi
if [[ -n "$OLD_SRC" ]]; then
    if [[ -z "$COMMIT" ]]; then
        COMMIT="$(git -C "$OLD_SRC" rev-parse HEAD)"
    fi
    echo "== build the old editor ($COMMIT) in $OLD_SRC"
    (cd "$OLD_SRC" && tools/build.sh linux)
    OLD_EDITOR="$OLD_SRC/build/linux/editor-install"
fi
[[ -n "$COMMIT" ]] || { echo "--old-editor needs --commit <sha>" >&2; exit 2; }
[[ -x "$OLD_EDITOR/bin/jasperengine-editor" ]] || { echo "no editor at $OLD_EDITOR/bin/jasperengine-editor" >&2; exit 1; }

# the image mounts this checkout only: the old editor goes under build/
if [[ "$OLD_EDITOR" != "$WORK/old-editor" ]]; then
    rm -rf "$WORK/old-editor"
    cp -a "$OLD_EDITOR" "$WORK/old-editor"
fi

ufm_image
rm -rf "$WORK/gen"
mkdir -p "$WORK/gen"
CPP_FLAG=""
[[ $SKIP_CPP == 1 ]] && CPP_FLAG="--skip-cpp"
echo "== generate the game with the old editor"
ufm_run "$UFM_PY tools/ci/upgrade_from_main/gen_fixture.py --editor build/upgrade-from-main/old-editor/bin/jasperengine-editor --out build/upgrade-from-main/gen $CPP_FLAG"

if [[ $SKIP_PLAYER == 0 ]]; then
    echo "== the old Linux player"
    ufm_run "tools/ci/upgrade_from_main/player_linux.sh /src/build/upgrade-from-main/old-editor/bin/jasperengine-editor build/upgrade-from-main/gen/project build/upgrade-from-main/gen/player-linux"
    echo "== the old web player"
    ufm_run "rm -rf build/upgrade-from-main/gen/web-project build/upgrade-from-main/gen/web-export && cp -r build/upgrade-from-main/gen/project build/upgrade-from-main/gen/web-project && build/upgrade-from-main/old-editor/bin/jasperengine-editor export --project build/upgrade-from-main/gen/web-project --out build/upgrade-from-main/gen/web-export --backend opengles > build/upgrade-from-main/gen/web-export.log 2>&1"
    "$ROOT/tools/build.sh" exec web tools/ci/upgrade_from_main/player_web.sh build/upgrade-from-main/gen/web-export build/upgrade-from-main/gen/web
fi

[[ $INSTALL == 1 ]] || { echo "results in $WORK/gen"; exit 0; }
GEN="$WORK/gen"
rm -rf "$DEST"
mkdir -p "$DEST/transcripts" "$DEST/screenshots" "$DEST/recipe"
cp -a "$GEN/project" "$DEST/project"
[[ -d "$GEN/cpp-project" ]] && cp -a "$GEN/cpp-project" "$DEST/cpp-project"
cp "$GEN/editor/input.txt" "$DEST/transcripts/editor-input.txt"
cp "$GEN/editor/autopilot.txt" "$DEST/transcripts/editor-autopilot.txt"
[[ -f "$GEN/editor/cpp.txt" ]] && cp "$GEN/editor/cpp.txt" "$DEST/transcripts/cpp-editor.txt"
[[ -f "$GEN/player-linux/transcript.txt" ]] && cp "$GEN/player-linux/transcript.txt" "$DEST/transcripts/player-linux.txt"
[[ -f "$GEN/web/transcript.txt" ]] && cp "$GEN/web/transcript.txt" "$DEST/transcripts/player-web.txt"
cp "$GEN"/editor/*.png "$DEST/screenshots/"
cp "$GEN"/calls.json "$DEST/recipe/calls.json"
[[ -f "$GEN/cpp-calls.json" ]] && cp "$GEN/cpp-calls.json" "$DEST/recipe/cpp-calls.json"
cat > "$DEST/SOURCE.yaml" <<EOF
# Made by tools/ci/upgrade_from_main/generate.sh: do not edit by hand, generate again.
origin_main_commit: $COMMIT
generated_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
echo "installed in $DEST"
