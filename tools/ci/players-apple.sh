#!/usr/bin/env bash
# The macOS or iOS prebuilt player runtimes (docs/specs/S30-shipping.md R8, R11, R14, S34 R4; the CI
# design §12.2), on a Mac with Xcode (and ninja on PATH for macOS):
#
#   tools/ci/players-apple.sh <macos-metal|macos-glcore|ios> ["<flavours>"]
#
# flavours: "release" (default), "development" or "release development". The release flavour builds
# every variant of engine/player/variants.json, the development flavour the full player.
#   macos-<backend>  a universal (arm64 + x86_64) Release jasperengine-player.app, Ninja, its symbols split
#                    into symbols/<exe>.dSYM by the link (JASPERENGINE_SPLIT_SYMBOLS), then ad-hoc signed
#                    (the split strips the binary, which voids the linker's signature; arm64 runs nothing
#                    unsigned); trees build/players-build/macos-universal-<backend>-<flavour>[-<variant>]
#   ios              tools/players/ios_player.sh: the xcframework (iphoneos arm64, iphonesimulator) and
#                    the Xcode template; trees build/players-build/ios-<flavour>[-<variant>]
# Each is packaged by tools/players/package_player.py into build/players/<id>/ and
# build/players/archives/. Log: build/test-results/players-apple-<target>.log. JASPERENGINE_VERSION is
# the version the runtimes are locked to (default git describe); JOBS and ccache on PATH (macOS
# runtimes) as in tools/ci/platform-common.sh. Exit 2 on bad arguments.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=platform-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/platform-common.sh"

USAGE="$(ci_header "${BASH_SOURCE[0]}")"
[[ $# -ge 1 && $# -le 2 ]] || ci_usage "$USAGE"
TARGET="$1"
FLAVOURS="${2:-release}"
[[ "$TARGET" =~ ^(macos-metal|macos-glcore|ios)$ ]] || ci_usage "target: macos-metal, macos-glcore or ios (got $TARGET)"
ci_check_flavours "$FLAVOURS" || ci_usage "flavours: release, development or \"release development\" (got '$FLAVOURS')"

build_macos() {
    local backend="${TARGET#macos-}" flavour pair variant suffix tree dev
    command -v ninja >/dev/null 2>&1 || { echo "ninja is not on PATH (brew install ninja)" >&2; exit 1; }
    for flavour in $FLAVOURS; do
        dev=OFF
        [[ "$flavour" == development ]] && dev=ON
        for pair in $(ci_player_variants "$flavour"); do
            variant="${pair%%:*}" suffix="${pair#*:}"
            tree="build/players-build/macos-universal-$backend-$flavour${suffix:+-$suffix}"
            echo "== player macos-universal-$backend-$flavour${suffix:+-$suffix} ($variant)"
            cmake -S engine -B "$tree" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
                -DJASPERENGINE_PLAYER=ON -DGRAPHIC_BACKEND="$backend" -DJASPERENGINE_DEVELOPMENT_BUILD="$dev" \
                -DJASPERENGINE_PLAYER_VARIANT="$variant" ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
            cmake --build "$tree" --target jasperengine-player ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
            codesign --force --deep --sign - --timestamp=none "$tree/jasperengine-player.app"
            codesign --verify --strict --verbose=2 "$tree/jasperengine-player.app"
            "$PY" tools/players/package_player.py --platform macos --arch universal --backend "$backend" --flavour "$flavour" \
                --variant "$variant" --build "$tree" --symbols "$tree/symbols" --out build/players --zip build/players/archives \
                ${PLAYER_VERSION_ARGS[@]+"${PLAYER_VERSION_ARGS[@]}"}
        done
    done
    ls -la build/players/archives
}

build_ios() {
    local flavour pair variant suffix dir
    for flavour in $FLAVOURS; do
        for pair in $(ci_player_variants "$flavour"); do
            variant="${pair%%:*}" suffix="${pair#*:}"
            dir="build/players-build/ios-$flavour${suffix:+-$suffix}"
            echo "== player ios-arm64-metal-$flavour${suffix:+-$suffix} ($variant)"
            tools/players/ios_player.sh "$flavour" "$dir" "$variant"
            "$PY" tools/players/package_player.py --platform ios --arch arm64 --backend metal --flavour "$flavour" \
                --variant "$variant" --ios-dir "$dir" --out build/players --zip build/players/archives \
                ${PLAYER_VERSION_ARGS[@]+"${PLAYER_VERSION_ARGS[@]}"}
        done
    done
    ls -la build/players/archives
}

if [[ "$TARGET" == ios ]]; then
    ci_logged players-apple-ios build_ios
else
    ci_logged "players-apple-$TARGET" build_macos
fi
