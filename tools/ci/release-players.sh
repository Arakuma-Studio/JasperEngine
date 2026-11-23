#!/usr/bin/env bash
# A release editor against its release's player archives (.github/workflows/build-release.yml,
# docs/superpowers/specs/2026-11-12-ci-design.md AC10): the editor is that version, and installs
# every archive from disk with `players install` (each hash-checked against the table it embeds,
# tools/players/write_hashes.py); a copy with one byte more is refused. The archives whose names
# start with one of the kept ids (e.g. linux- web-) are installed into <players dir> for the
# prebuilt end to end; the others into a scratch directory, removed after each.
#
#   tools/build.sh exec runtime tools/ci/release-players.sh <version> <archive dir> <players dir> [<kept id prefix>...]
#
# The editor is build/linux/editor-install/bin/jasperengine-editor.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

[[ $# -ge 3 ]] || { echo "usage: tools/ci/release-players.sh <version> <archive dir> <players dir> [<kept id prefix>...]" >&2; exit 2; }
VERSION="$1" ARCHIVES="$2" PLAYERS="$3"
shift 3
KEEP=("$@")
EDITOR=build/linux/editor-install/bin/jasperengine-editor
[[ -x "$EDITOR" ]] || { echo "release-players: no editor at $EDITOR" >&2; exit 2; }
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

reported="$("$EDITOR" players list --json --players-dir "$SCRATCH/none" | python3 -c 'import json, sys; print(json.load(sys.stdin)["editor_version"])')"
[[ "$reported" == "$VERSION" ]] || { echo "release-players: the editor is $reported, not $VERSION" >&2; exit 1; }

installed=0
first=""
while IFS= read -r -d '' archive; do
    name="$(basename "$archive")"
    target="$SCRATCH/players"
    for prefix in ${KEEP[@]+"${KEEP[@]}"}; do
        [[ "$name" == "jasperengine-player-$prefix"* ]] && target="$PLAYERS"
    done
    if ! out="$("$EDITOR" players install "$archive" --players-dir "$target" 2>&1)"; then
        echo "$out"
        echo "release-players: $name was refused" >&2
        exit 1
    fi
    echo "$out"
    if grep -q "no release hashes" <<< "$out"; then
        echo "release-players: a development editor (no release hashes) installed $name" >&2
        exit 1
    fi
    rm -rf "$SCRATCH/players"
    installed=$((installed + 1))
    [[ -n "$first" ]] || first="$archive"
done < <(find "$ARCHIVES" -name "jasperengine-player-*-$VERSION.zip" -print0 | sort -z)
[[ $installed -gt 0 ]] || { echo "release-players: no jasperengine-player-*-$VERSION.zip in $ARCHIVES" >&2; exit 1; }

# the release's hash, not the archive's own player-info: one byte more is another archive
cp "$first" "$SCRATCH/tampered.zip"
printf '\0' >> "$SCRATCH/tampered.zip"
if out="$("$EDITOR" players install "$SCRATCH/tampered.zip" --players-dir "$SCRATCH/players" 2>&1)"; then
    echo "$out"
    echo "release-players: a tampered copy of $(basename "$first") was installed" >&2
    exit 1
fi
grep -q "is not the release's archive" <<< "$out" \
    || { echo "$out"; echo "release-players: the tampered copy was refused for another reason" >&2; exit 1; }
echo "release-players: $installed archives installed against the table of $VERSION; a tampered copy refused"
