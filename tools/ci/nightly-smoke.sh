#!/usr/bin/env bash
# The smoke test of every fixture project: tools/smoke-test.sh on the projects
# under tools/fixtures/, after tools/build.sh linux. One backend per call:
#
#   tools/ci/nightly-smoke.sh opengl    every fixture project
#   tools/ci/nightly-smoke.sh gles3     the GLES3 set of docs/html-ui/phase-6-acceptance.md §4.4 and S14's textures fixture
#   tools/ci/nightly-smoke.sh vulkan    the Vulkan set (the same projects and cases)
#
# Each run writes build/smoke-<backend>-<case>/ (game*.png, editor.png, the logs). The GLES3 and
# Vulkan sets split the shader library into groups, a few at a time: lavapipe recompiles its
# pipelines every frame past its variant caches (tools/smoke-test.sh). Every run is attempted;
# the exit status is 1 when any failed, and the failures are listed at the end.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BACKEND="${1:-}"
FIXTURES="$ROOT/tools/fixtures"
failed=()

# smoke <case> <project> [VAR=value...]: one tools/smoke-test.sh run
smoke() {
    local name="$1" project="$2"; shift 2
    echo "== nightly smoke: $BACKEND $name"
    if ! env "$@" SMOKE_BACKEND="$BACKEND" SMOKE_DIR="build/smoke-$BACKEND-$name" \
            "$ROOT/tools/smoke-test.sh" "$FIXTURES/$project"; then
        failed+=("$BACKEND $name")
    fi
}

# the post-processing cases of the GLES3 and Vulkan sets, as runs of one export
PP_RUNS="bloom PP_CASE=bloom; msaa4 PP_CASE=msaa4; mobile_full PP_CASE=mobile_full; lib_crt PP_CASE=lib_crt"

case "$BACKEND" in
opengl)
    for project in "$FIXTURES"/*/; do
        project="$(basename "$project")"
        [[ -f "$FIXTURES/$project/project.yaml" ]] && smoke "$project" "$project"
    done
    ;;
gles3|vulkan)
    smoke post-processing post-processing SMOKE_GAME_RUNS="$PP_RUNS"
    smoke scene-inputs scene-inputs
    for group in core phase2 phase3a phase3b; do
        smoke "shader-library-$group" shader-library SMOKE_GAME_ENV="SL_GROUPS=$group"
    done
    smoke stencil stencil
    smoke extra-pass extra-pass
    smoke textures textures
    # S07: an exported game that destroys bodies and toggles a component in contact callbacks
    smoke ecs-collision ecs-collision
    ;;
*)
    echo "usage: tools/ci/nightly-smoke.sh opengl|gles3|vulkan" >&2
    exit 2
    ;;
esac

if [[ ${#failed[@]} != 0 ]]; then
    printf 'nightly smoke: FAIL: %s\n' "${failed[@]}" >&2
    exit 1
fi
echo "nightly smoke: $BACKEND: every run passed"
