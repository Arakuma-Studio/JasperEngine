#!/usr/bin/env bash
# The Windows prebuilt player runtimes of one backend (docs/specs/S30-shipping.md R8, S34 R4; the CI
# design §12.2), with MSVC and Ninja, under Git Bash inside an MSVC developer environment
# (ilammy/msvc-dev-cmd, arch x64):
#
#   tools/ci/players-windows.sh <d3d11|glcore|vulkan> ["<flavours>"]
#
# flavours: "release" (default), "development" or "release development". The release flavour builds
# every variant of engine/player/variants.json, the development flavour the full player. Each runtime
# is a static-CRT Release build (engine/CMakeLists.txt picks MultiThreaded for a non-shared engine)
# whose PDB the link puts in symbols/ (JASPERENGINE_SPLIT_SYMBOLS), packaged by
# tools/players/package_player.py into build/players/<id>/ and build/players/archives/
# (jasperengine-player-<id>-<version>.zip and -symbols.zip). Trees: build/players-build/windows-x64-
# <backend>-<flavour>[-<variant>]. Log: build/test-results/players-windows-<backend>.log.
# JASPERENGINE_VERSION is the version the runtimes are locked to (default git describe, as the
# editor's); JOBS and ccache on PATH as in tools/ci/platform-common.sh. Exit 2 on bad arguments.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=platform-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/platform-common.sh"

USAGE="$(ci_header "${BASH_SOURCE[0]}")"
[[ $# -ge 1 && $# -le 2 ]] || ci_usage "$USAGE"
BACKEND="$1"
FLAVOURS="${2:-release}"
[[ "$BACKEND" =~ ^(d3d11|glcore|vulkan)$ ]] || ci_usage "backend: d3d11, glcore or vulkan (got $BACKEND)"
ci_check_flavours "$FLAVOURS" || ci_usage "flavours: release, development or \"release development\" (got '$FLAVOURS')"

build_players() {
    ci_msvc
    local flavour pair variant suffix tree dev
    for flavour in $FLAVOURS; do
        dev=OFF
        [[ "$flavour" == development ]] && dev=ON
        for pair in $(ci_player_variants "$flavour"); do
            variant="${pair%%:*}" suffix="${pair#*:}"
            tree="build/players-build/windows-x64-$BACKEND-$flavour${suffix:+-$suffix}"
            echo "== player windows-x64-$BACKEND-$flavour${suffix:+-$suffix} ($variant)"
            cmake -S engine -B "$tree" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "${MSVC_LINKER_ARGS[@]}" \
                -DJASPERENGINE_PLAYER=ON -DGRAPHIC_BACKEND="$BACKEND" -DJASPERENGINE_DEVELOPMENT_BUILD="$dev" \
                -DJASPERENGINE_PLAYER_VARIANT="$variant" ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
            cmake --build "$tree" --target jasperengine-player ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
            "$PY" tools/players/package_player.py --platform windows --arch x64 --backend "$BACKEND" --flavour "$flavour" \
                --variant "$variant" --build "$tree" --symbols "$tree/symbols" --out build/players --zip build/players/archives \
                ${PLAYER_VERSION_ARGS[@]+"${PLAYER_VERSION_ARGS[@]}"}
        done
    done
    ls -la build/players/archives
}

ci_logged "players-windows-$BACKEND" build_players
