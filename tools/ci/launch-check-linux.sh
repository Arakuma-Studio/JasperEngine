#!/usr/bin/env bash
# The launch check of a release's Linux editor (.github/workflows/build-release.yml): the packaged
# AppImage (or an editor binary) prints `call --help`, then opens a scratch copy of a fixture
# project as a one-shot headless editor (`call --project <copy> editor_status`) under Xvfb. With
# --download <runtime id> it also downloads that player runtime from its own release (a published
# release's editor, hash-checked against its table). Runs in the Linux image:
#
#   tools/build.sh exec linux tools/ci/launch-check-linux.sh dist/JasperEngine-<v>-linux-x86_64.AppImage \
#       [--download linux-x64-glcore-release] [--fixture tools/fixtures/<name>]
#
# Scratch and logs go to build/launch-check/ (editor.log of the one-shot editor included).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

usage() { echo "usage: tools/ci/launch-check-linux.sh <AppImage | editor> [--download <runtime id>] [--fixture <project dir>]" >&2; exit 2; }
[[ $# -ge 1 ]] || usage
EDITOR="$(realpath "$1")"; shift
DOWNLOAD=""
FIXTURE=tools/fixtures/post-processing
while [[ $# -gt 0 ]]; do
    case "$1" in
        --download) [[ $# -ge 2 ]] || usage; DOWNLOAD="$2"; shift 2 ;;
        --fixture) [[ $# -ge 2 ]] || usage; FIXTURE="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[[ -f "$EDITOR" ]] || { echo "launch-check: no editor at $EDITOR" >&2; exit 2; }
[[ -f "$FIXTURE/project.yaml" ]] || { echo "launch-check: $FIXTURE is not a project" >&2; exit 2; }
chmod +x "$EDITOR"
# no FUSE in a container: the AppImage runtime extracts itself instead of mounting
export APPIMAGE_EXTRACT_AND_RUN=1

OUT="$ROOT/build/launch-check"
rm -rf "$OUT"
mkdir -p "$OUT/cwd"
cp -r "$FIXTURE" "$OUT/project"

echo "== launch-check: call --help"
(cd "$OUT/cwd" && "$EDITOR" call --help) > "$OUT/call-help.log" 2>&1 \
    || { cat "$OUT/call-help.log"; echo "launch-check: call --help failed" >&2; exit 1; }
grep -q -- "--project" "$OUT/call-help.log" \
    || { cat "$OUT/call-help.log"; echo "launch-check: call --help printed no usage" >&2; exit 1; }

echo "== launch-check: call --project <copy of $FIXTURE> editor_status"
if ! (cd "$OUT/cwd" && "$ROOT/tools/with-xvfb.sh" "$EDITOR" call --project "$OUT/project" --json editor_status) \
        > "$OUT/editor-status.json" 2> "$OUT/editor-status.log"; then
    cat "$OUT/editor-status.log" "$OUT/editor-status.json"
    [[ -f "$OUT/cwd/editor.log" ]] && tail -n 80 "$OUT/cwd/editor.log"
    echo "launch-check: the one-shot editor did not answer editor_status" >&2
    exit 1
fi
python3 - "$OUT/editor-status.json" "$OUT/project" <<'PY'
import json, os, sys
text = open(sys.argv[1]).read()
json.loads(text)
if os.path.basename(sys.argv[2]) not in text:
    sys.exit(f"launch-check: editor_status names no project {sys.argv[2]}: {text[:400]}")
print("launch-check: editor_status ok")
PY

if [[ -n "$DOWNLOAD" ]]; then
    echo "== launch-check: players download $DOWNLOAD"
    if ! (cd "$OUT/cwd" && "$EDITOR" players download "$DOWNLOAD" --players-dir "$OUT/players") > "$OUT/download.log" 2>&1; then
        cat "$OUT/download.log"
        echo "launch-check: players download $DOWNLOAD failed" >&2
        exit 1
    fi
    cat "$OUT/download.log"
    if grep -q "no release hashes" "$OUT/download.log"; then
        echo "launch-check: a development editor (no release hashes) downloaded $DOWNLOAD" >&2
        exit 1
    fi
fi
echo "launch-check: ok"
