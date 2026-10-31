#!/usr/bin/env bash
# The prebuilt player against the Compile path (docs/specs/S30-shipping.md AC5): every Lua-only
# fixture project (tools/fixtures/*, no C++ scripts and no ProjectBuild.cmake) runs through
# tools/smoke-test.sh with SMOKE_METHOD=compile and SMOKE_METHOD=prebuilt, and the two frames
# must match (tools/mcp/pngdiff.py --tolerance 24 --max-fraction 0.01). Needs the editor
# (tools/build.sh linux) and the player runtimes (tools/build.sh players).
#
#   tools/ci/parity.sh [fixture...]        # default: every Lua-only fixture
#
# Frames and diffs go to build/parity/<fixture>/. Exit 1 when any fixture differs or fails.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
fixtures=("$@")
if [[ ${#fixtures[@]} -eq 0 ]]; then
    for dir in tools/fixtures/*/; do
        dir="${dir%/}"
        [[ -f "$dir/project.yaml" ]] || continue
        [[ -e "$dir/ProjectBuild.cmake" ]] && continue
        if find "$dir" -name '*.cpp' -not -path '*/assets/*' | grep -q .; then continue; fi
        fixtures+=("$dir")
    done
fi
status=0
for dir in "${fixtures[@]}"; do
    name="$(basename "$dir")"
    out="build/parity/$name"
    rm -rf "$out"; mkdir -p "$out"
    ok=1
    for method in compile prebuilt; do
        # both run the player's own frame (--after-ready --quit-after 30 --fixed-fps 60 --screenshot)
        if ! SMOKE_PLAY=1 SMOKE_METHOD=$method SMOKE_DIR="build/parity-$method" tools/smoke-test.sh "$dir" > "$out/$method.log" 2>&1; then
            echo "FAIL: $name ($method): $(tail -3 "$out/$method.log" | tr '\n' ' ')"
            ok=0
            break
        fi
        cp "build/parity-$method/game.png" "$out/$method.png"
    done
    if [[ $ok == 1 ]] && python3 tools/mcp/pngdiff.py "$out/compile.png" "$out/prebuilt.png" --tolerance 24 --max-fraction 0.01 \
            --diff "$out/diff.png" > "$out/pngdiff.txt" 2>&1; then
        echo "ok: $name"
    else
        [[ $ok == 1 ]] && echo "FAIL: $name: the prebuilt frame differs ($(cat "$out/pngdiff.txt"))"
        status=1
    fi
done
exit $status
