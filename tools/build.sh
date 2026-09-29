#!/usr/bin/env bash
# Builds the engine and editor inside the Docker images under docker/, so the
# host only needs Docker. Output goes to build/<target>/ (git-ignored).
#
#   tools/build.sh linux [--clean]     editor + engine sample project (Release)
#   tools/build.sh web [--clean]       engine sample project for Emscripten
#   tools/build.sh android [--clean]   engine sample project APK (debug)
#   tools/build.sh shell <target>      interactive shell in that target's image
#
# BUILD_TYPE overrides the CMake build type (default Release, Debug for web).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR_TARGET=doriax-editor
ENGINE_TARGET=doriax-project

usage() {
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

[[ $# -ge 1 ]] || usage
TARGET="$1"; shift
CLEAN=0
SHELL_MODE=0
if [[ "$TARGET" == shell ]]; then
    [[ $# -ge 1 ]] || usage
    SHELL_MODE=1
    TARGET="$1"; shift
fi
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        *) usage ;;
    esac
done

case "$TARGET" in
    linux|web|android) ;;
    *) usage ;;
esac

IMAGE="jasperengine/${TARGET}-build"
OUT="build/$TARGET"
CACHE="build/.cache"

# Rebuilding an unchanged image is a no-op thanks to the layer cache.
docker build -q -t "$IMAGE" -f "$ROOT/docker/$TARGET.Dockerfile" "$ROOT/docker" >/dev/null

if [[ $CLEAN -eq 1 ]]; then
    # Files inside may belong to the container user; remove them from inside.
    docker run --rm -v "$ROOT:/src" -w /src "$IMAGE" rm -rf "$OUT"
fi
mkdir -p "$ROOT/$OUT" "$ROOT/$CACHE/home" "$ROOT/$CACHE/ccache"

TTY_FLAGS=()
[[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)

run() {
    docker run --rm "${TTY_FLAGS[@]}" \
        --user "$(id -u):$(id -g)" \
        -v "$ROOT:/src" -w /src \
        -e HOME="/src/$CACHE/home" \
        -e CCACHE_DIR="/src/$CACHE/ccache" \
        -e GRADLE_USER_HOME="/src/$CACHE/gradle" \
        "$IMAGE" "$@"
}

if [[ $SHELL_MODE -eq 1 ]]; then
    run bash
    exit 0
fi

CCACHE_FLAGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"

case "$TARGET" in
linux)
    BT="${BUILD_TYPE:-Release}"
    run bash -euo pipefail -c "
        cmake -S . -B $OUT/editor -G Ninja -DCMAKE_BUILD_TYPE=$BT \
            -DCMAKE_INSTALL_PREFIX=$OUT/editor-install $CCACHE_FLAGS
        cmake --build $OUT/editor --target $EDITOR_TARGET
        cmake --install $OUT/editor --strip
        cmake -S engine -B $OUT/engine -G Ninja -DCMAKE_BUILD_TYPE=$BT $CCACHE_FLAGS
        cmake --build $OUT/engine --target $ENGINE_TARGET
        ccache --show-stats --verbose | grep -E 'Hits|Misses' || true
    "
    ;;
web)
    BT="${BUILD_TYPE:-Debug}"
    run bash -euo pipefail -c "
        emcmake cmake -S engine -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=$BT $CCACHE_FLAGS
        cmake --build $OUT --target $ENGINE_TARGET
    "
    ;;
android)
    # Gradle writes its intermediates next to the project, so the output
    # directory is only used for the copied APK.
    run bash -euo pipefail -c "
        cd engine/workspaces/androidstudio
        ./gradlew --no-daemon assembleDebug
        cp app/build/outputs/apk/debug/*.apk /src/$OUT/
    "
    ls -l "$ROOT/$OUT"/*.apk
    ;;
esac
