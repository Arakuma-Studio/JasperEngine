#!/usr/bin/env bash
# The scene merge driver end to end (docs/specs/S29-version-control.md R17-R22, AC14), in the
# linux build image (docker/linux.Dockerfile has git):
#
#   tools/vcs-tests/merge_driver_test.sh [<jasperengine-editor>]
#
# tools/fixtures/prefab-runtime is committed to a local temp repository (no remote) and two
# branches edit its Main Scene.scene:
#   1. different GameObjects (Spawner's and Enemy's positions): with the driver configured as the
#      editor sets it up (Preferences > Version Control > Set Up Smart Merge), `git merge` exits 0
#      and `scene-dump` of the result loads both edits;
#   2. the same property on both: `git merge` exits non-zero, the scene is ours (it loads) and
#      .jasperengine/merge/Main Scene.scene.conflicts.yaml names the property;
#   3. the same as 2 without the driver: git's text merge leaves conflict markers, which no
#      loader reads (what a project without Smart Merge gets).
# The editor defaults to the one tools/build.sh linux (or unit-tests) built.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EDITOR="${1:-}"
if [[ -z "$EDITOR" ]]; then
    for candidate in build/linux/editor/jasperengine-editor build/linux/editor-tests/jasperengine-editor \
                     build/linux/editor-install/bin/jasperengine-editor; do
        if [[ -x "$ROOT/$candidate" ]]; then EDITOR="$ROOT/$candidate"; break; fi
    done
fi
[[ -n "$EDITOR" && -x "$EDITOR" ]] || { echo "No editor binary: run tools/build.sh linux" >&2; exit 1; }
EDITOR="$(cd "$(dirname "$EDITOR")" && pwd)/$(basename "$EDITOR")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/je-merge-driver.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
export HOME="$WORK/home" GIT_CONFIG_NOSYSTEM=1 GIT_TERMINAL_PROMPT=0 LC_ALL=C
mkdir -p "$HOME"
git config --global user.name "Merge Test"
git config --global user.email "merge@example.com"
git config --global init.defaultBranch main
git config --global commit.gpgsign false

SCENE="Main Scene.scene"
FAILED=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILED=1; }

# a fresh repository of the fixture, the driver configured or not
make_repo() {
    local dir="$1" driver="$2"
    cp -r "$ROOT/tools/fixtures/prefab-runtime" "$dir"
    cd "$dir"
    git init -q
    printf '.jasperengine/\n*.orig\n' > .gitignore
    # what the editor generates for a project (Project::writeVersionControlMetadata)
    printf '*.scene  text eol=lf merge=jasperengine-scene\n*.prefab text eol=lf merge=jasperengine-scene\n' > .gitattributes
    if [[ "$driver" == yes ]]; then
        # GitProvider::setupMergeDriver's command
        git config merge.jasperengine-scene.name "JasperEngine scene merge"
        git config merge.jasperengine-scene.driver "'$EDITOR' merge-scene %O %A %B %P"
    fi
    git add -A
    git commit -q -m base
}

edit() {
    # edit <from> <to>: one line of the scene
    python3 - "$SCENE" "$1" "$2" <<'EOF'
import sys
path, old, new = sys.argv[1:]
text = open(path, encoding="utf-8").read()
assert text.count(old) == 1, f"{old!r} is not one line of {path}"
open(path, "w", encoding="utf-8").write(text.replace(old, new))
EOF
}

branches() {
    # branches <ours from> <ours to> <theirs from> <theirs to>
    git checkout -q -b theirs
    edit "$3" "$4"
    git commit -q -am theirs
    git checkout -q main
    edit "$1" "$2"
    git commit -q -am ours
}

dump() {
    "$EDITOR" scene-dump --project "$PWD" --out "$WORK/dump.yaml" > "$WORK/dump.log" 2>&1
}

SPAWNER='position: [10, 0, 0]'
ENEMY='position: [1, 2, 3]'

# 1. different GameObjects merge
make_repo "$WORK/clean" yes
branches "$SPAWNER" 'position: [10, 5, 0]' "$ENEMY" 'position: [1, 2, 9]'
if git merge -q --no-edit theirs > "$WORK/merge.log" 2>&1; then
    pass "different GameObjects: git merge exits 0"
else
    fail "different GameObjects: git merge failed: $(cat "$WORK/merge.log")"
fi
if dump; then
    python3 - "$WORK/dump.yaml" <<'EOF' && pass "different GameObjects: the merged scene loads with both edits" || fail "different GameObjects: the dump lacks an edit"
import re, sys
# the dump's GameObjects are keyed by path; the first position after each key is its transform's
text = open(sys.argv[1], encoding="utf-8").read()
def position(name):
    key = re.search(r"^\s+" + name + r":\s*$", text, re.M)
    assert key, name
    found = re.compile(r"position: \[([^\]]*)\]").search(text, key.end())
    return [float(v) for v in found.group(1).split(",")]
assert position("Spawner") == [10, 5, 0], position("Spawner")
assert position("Enemy") == [1, 2, 9], position("Enemy")
EOF
else
    fail "different GameObjects: scene-dump failed: $(tail -20 "$WORK/dump.log")"
fi
grep -q '<<<<<<<' "$SCENE" && fail "different GameObjects: conflict markers in the scene" || true

# 2. the same property on both sides: a conflict, ours kept, recorded
make_repo "$WORK/conflict" yes
branches "$SPAWNER" 'position: [10, 5, 0]' "$SPAWNER" 'position: [10, 7, 0]'
if git merge -q --no-edit theirs > "$WORK/merge.log" 2>&1; then
    fail "same property: git merge succeeded"
else
    pass "same property: git merge exits non-zero"
fi
grep -q '<<<<<<<' "$SCENE" && fail "same property: conflict markers in the scene" || pass "same property: no conflict markers"
if dump; then pass "same property: the scene loads"; else fail "same property: scene-dump failed: $(tail -20 "$WORK/dump.log")"; fi
CONFLICTS=".jasperengine/merge/$SCENE.conflicts.yaml"
if [[ -f "$CONFLICTS" ]] && grep -q 'transform' "$CONFLICTS" && grep -q 'Spawner' "$CONFLICTS"; then
    pass "same property: $CONFLICTS names Spawner's transform"
else
    fail "same property: no $CONFLICTS naming the property"
fi
git ls-files -u -- "$SCENE" | grep -q . && pass "same property: git lists the scene as unmerged" || fail "same property: not unmerged"

# 3. without the driver: git's text merge
make_repo "$WORK/text" no
branches "$SPAWNER" 'position: [10, 5, 0]' "$SPAWNER" 'position: [10, 7, 0]'
if git merge -q --no-edit theirs > "$WORK/merge.log" 2>&1; then
    fail "no driver: git merge succeeded"
else
    pass "no driver: git merge exits non-zero"
fi
grep -q '<<<<<<<' "$SCENE" && pass "no driver: git's text merge left conflict markers (no loader reads them)" \
    || fail "no driver: expected git's conflict markers"

exit $FAILED
