#!/usr/bin/env bash
# Builds the engine and editor inside the Docker images under docker/, so the
# host only needs Docker. Output goes to build/<target>/ (git-ignored).
#
#   tools/build.sh linux [--clean]     editor + engine sample project (Release)
#   tools/build.sh web [--clean]       engine sample project for Emscripten
#   tools/build.sh android [--clean]   engine sample project APK (debug)
#   tools/build.sh htmlui-tests [--clean]  builds and runs jasperengine-htmlui-tests headless
#   tools/build.sh htmlui-editor-tests      builds the linux editor with jasperengine-htmlui-editor-tests, runs them
#   tools/build.sh shell <target>      interactive shell in that target's image
#
# BUILD_TYPE overrides the CMake build type (default Release, Debug for web).
# SKIP_ENGINE=1 builds only the editor for linux. JOBS caps build parallelism and, with
# CPUS (default: DOCKER_CPUS, else JOBS), the CPUs the container may use (docker --cpus),
# which also caps what ignores JOBS: the Android gradle/ninja build and the test binaries.
# DOCKER_CPUS is the same cap under the name tools/smoke-test.sh passes (e.g. DOCKER_CPUS=4
# on a shared host).
# GRAPHIC_BACKEND=vulkan builds the linux editor for Vulkan (build/linux/editor-vulkan).
# htmlui-tests: HTMLUI_BACKEND=glcore|vulkan (default glcore), RUN=0 builds only,
# HTMLUI_TEST_ARGS goes to the test binary (e.g. --gtest_filter=Golden*),
# JASPER_HTMLUI_UPDATE_GOLDEN=1 rewrites the golden images (glcore only).
# JASPER_HTMLUI_WRITE_ACTUAL=1 keeps every compared image in the test output directory (both backends).
# htmlui-editor-tests: HTMLUI_TEST_ARGS goes to the test binary, RUN=0 builds only,
# HTMLUI_LIVE_NETWORK=1 adds the live Google Fonts test.
# From a git worktree, the main checkout's ccache and .git are shared.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR_TARGET=jasperengine-editor
ENGINE_TARGET=jasperengine-project

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
    linux|web|android|htmlui-tests|htmlui-editor-tests) ;;
    *) usage ;;
esac

# The tests run in the linux image, next to the linux build output
IMAGE_TARGET="$TARGET"
[[ "$TARGET" == htmlui-tests || "$TARGET" == htmlui-editor-tests ]] && IMAGE_TARGET=linux
IMAGE="jasperengine/${IMAGE_TARGET}-build"
OUT="build/$IMAGE_TARGET"
CACHE="build/.cache"
# The main checkout, even when running from a git worktree.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
JOBS_FLAG=""
[[ -n "${JOBS:-}" ]] && JOBS_FLAG="-j $JOBS"

# Rebuilding an unchanged image is a no-op thanks to the layer cache. The containers run the
# image by id: other worktrees (with another Dockerfile) may retag the shared name meanwhile.
IMAGE_ID="$(docker build -q -t "$IMAGE" -f "$ROOT/docker/$IMAGE_TARGET.Dockerfile" "$ROOT/docker")"

HTMLUI_BACKEND="${HTMLUI_BACKEND:-glcore}"
LINUX_BACKEND="${GRAPHIC_BACKEND:-glcore}"
if [[ "$TARGET" == htmlui-tests && "$HTMLUI_BACKEND" != glcore && "$HTMLUI_BACKEND" != vulkan ]]; then
    echo "HTMLUI_BACKEND must be glcore or vulkan" >&2; exit 1
fi
if [[ "$TARGET" == linux && "$LINUX_BACKEND" != glcore && "$LINUX_BACKEND" != vulkan ]]; then
    echo "GRAPHIC_BACKEND must be glcore or vulkan for linux" >&2; exit 1
fi

# What --clean removes: the whole output of the target, or only this test build
CLEAN_DIR="$OUT"
[[ "$TARGET" == htmlui-tests ]] && CLEAN_DIR="$OUT/htmlui-tests-$HTMLUI_BACKEND"

if [[ $CLEAN -eq 1 ]]; then
    # Files inside may belong to the container user; remove them from inside.
    docker run --rm -v "$ROOT:/src" -w /src "$IMAGE_ID" rm -rf "$CLEAN_DIR"
fi
mkdir -p "$ROOT/$OUT" "$ROOT/$CACHE/home" "$MAIN/$CACHE/ccache"

TTY_FLAGS=()
[[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
CPU_FLAGS=()
CPUS="${CPUS:-${DOCKER_CPUS:-${JOBS:-}}}"
[[ -n "$CPUS" ]] && CPU_FLAGS=(--cpus "$CPUS")
ENV_FLAGS=()
[[ -n "${JASPER_HTMLUI_UPDATE_GOLDEN:-}" ]] && ENV_FLAGS+=(-e "JASPER_HTMLUI_UPDATE_GOLDEN=$JASPER_HTMLUI_UPDATE_GOLDEN")
[[ -n "${JASPER_HTMLUI_WRITE_ACTUAL:-}" ]] && ENV_FLAGS+=(-e "JASPER_HTMLUI_WRITE_ACTUAL=$JASPER_HTMLUI_WRITE_ACTUAL")
# the HTML UI performance tests' tolerance on a loaded host (compat_perf_test.cpp)
[[ -n "${JASPER_HTMLUI_PERF_SLACK:-}" ]] && ENV_FLAGS+=(-e "JASPER_HTMLUI_PERF_SLACK=$JASPER_HTMLUI_PERF_SLACK")
[[ -n "${HTMLUI_LIVE_NETWORK:-}" ]] && ENV_FLAGS+=(-e "HTMLUI_LIVE_NETWORK=$HTMLUI_LIVE_NETWORK")
# the UI effects' measuring switches (EffectPerfTest: JASPER_EFFECT_COST_ONLY, JASPER_EFFECT_PERF_*;
# EffectFluidTest: JASPER_EFFECT_FLUID_PROFILE)
for name in $(compgen -e | grep '^JASPER_EFFECT_' || true); do ENV_FLAGS+=(-e "$name=${!name}"); done

run() {
    # (the ${a[@]+...} form: an empty array under set -u fails on bash before 4.4, e.g. macOS)
    docker run --rm ${TTY_FLAGS[@]+"${TTY_FLAGS[@]}"} ${CPU_FLAGS[@]+"${CPU_FLAGS[@]}"} \
        --user "$(id -u):$(id -g)" \
        -v "$ROOT:/src" -w /src \
        -v "$MAIN/.git:$MAIN/.git:ro" \
        -v "$MAIN/$CACHE/ccache:/ccache" \
        -e HOME="/src/$CACHE/home" \
        -e CCACHE_DIR=/ccache \
        -e GRADLE_USER_HOME="/src/$CACHE/gradle" \
        ${ENV_FLAGS[@]+"${ENV_FLAGS[@]}"} \
        "$IMAGE_ID" "$@"
}

if [[ $SHELL_MODE -eq 1 ]]; then
    run bash
    exit 0
fi

CCACHE_FLAGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"

case "$TARGET" in
linux)
    BT="${BUILD_TYPE:-Release}"
    # glcore keeps the historical paths; vulkan builds next to it
    EDITOR_DIR="editor"
    BACKEND_FLAGS=""
    if [[ "$LINUX_BACKEND" == vulkan ]]; then
        EDITOR_DIR="editor-vulkan"
        BACKEND_FLAGS="-DGRAPHIC_BACKEND=vulkan"
    fi
    run bash -euo pipefail -c "
        cmake -S . -B $OUT/$EDITOR_DIR -G Ninja -DCMAKE_BUILD_TYPE=$BT $BACKEND_FLAGS \
            -DCMAKE_INSTALL_PREFIX=$OUT/$EDITOR_DIR-install $CCACHE_FLAGS
        cmake --build $OUT/$EDITOR_DIR --target $EDITOR_TARGET $JOBS_FLAG
        cmake --install $OUT/$EDITOR_DIR --strip
        # RmlUi stays inside the shared engine (engine/libs/rmlui/hidden-symbols.map)
        leaked=\$(nm -D -C $OUT/$EDITOR_DIR-install/bin/libjasperengine.so | grep -c 'Rml::' || true)
        if [[ \$leaked != 0 ]]; then
            echo \"libjasperengine.so exports \$leaked RmlUi symbols:\" >&2
            nm -D -C $OUT/$EDITOR_DIR-install/bin/libjasperengine.so | grep 'Rml::' | head -20 >&2
            exit 1
        fi
        # and so does lexbor, the compat layer's HTML parser (engine/libs/lexbor)
        leaked=\$(nm -D $OUT/$EDITOR_DIR-install/bin/libjasperengine.so | grep -cE ' (lxb|lexbor)_' || true)
        if [[ \$leaked != 0 ]]; then
            echo \"libjasperengine.so exports \$leaked lexbor symbols:\" >&2
            nm -D $OUT/$EDITOR_DIR-install/bin/libjasperengine.so | grep -E ' (lxb|lexbor)_' | head -20 >&2
            exit 1
        fi
        # and the <video> decoders: dav1d, libopus, libwebm's parser (engine/libs/{dav1d,opus,libwebm})
        leaked=\$(nm -D -C $OUT/$EDITOR_DIR-install/bin/libjasperengine.so | grep -cE ' (dav1d|opus)_|mkvparser::' || true)
        if [[ \$leaked != 0 ]]; then
            echo \"libjasperengine.so exports \$leaked video decoder symbols:\" >&2
            nm -D -C $OUT/$EDITOR_DIR-install/bin/libjasperengine.so | grep -E ' (dav1d|opus)_|mkvparser::' | head -20 >&2
            exit 1
        fi
        [[ '${SKIP_ENGINE:-0}' == 1 ]] && exit 0
        cmake -S engine -B $OUT/engine -G Ninja -DCMAKE_BUILD_TYPE=$BT $CCACHE_FLAGS
        cmake --build $OUT/engine --target $ENGINE_TARGET $JOBS_FLAG
        ccache --show-stats --verbose | grep -E 'Hits|Misses' || true
    "
    ;;
web)
    BT="${BUILD_TYPE:-Debug}"
    run bash -euo pipefail -c "
        emcmake cmake -S engine -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=$BT $CCACHE_FLAGS
        cmake --build $OUT --target $ENGINE_TARGET $JOBS_FLAG
    "
    ;;
htmlui-tests)
    BT="${BUILD_TYPE:-Release}"
    TESTS_DIR="$OUT/htmlui-tests-$HTMLUI_BACKEND"
    VK_ENV=""
    [[ "$HTMLUI_BACKEND" == vulkan ]] && VK_ENV="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    run bash -euo pipefail -c "
        cmake -S engine -B $TESTS_DIR -G Ninja -DCMAKE_BUILD_TYPE=$BT \
            -DJASPERENGINE_HTMLUI_TESTS=ON -DGRAPHIC_BACKEND=$HTMLUI_BACKEND $CCACHE_FLAGS
        cmake --build $TESTS_DIR --target jasperengine-htmlui-tests $JOBS_FLAG
        [[ '${RUN:-1}' == 0 ]] && exit 0
        set -f  # a --gtest_filter pattern is not a glob
        env $VK_ENV xvfb-run -a -s '-screen 0 1600x900x24' \
            $TESTS_DIR/tests/htmlui/jasperengine-htmlui-tests ${HTMLUI_TEST_ARGS:-}
    "
    ;;
htmlui-editor-tests)
    BT="${BUILD_TYPE:-Release}"
    run bash -euo pipefail -c "
        cmake -S . -B $OUT/editor -G Ninja -DCMAKE_BUILD_TYPE=$BT -DJASPERENGINE_HTMLUI_EDITOR_TESTS=ON \
            -DCMAKE_INSTALL_PREFIX=$OUT/editor-install $CCACHE_FLAGS
        cmake --build $OUT/editor --target jasperengine-editor jasperengine-htmlui-editor-tests $JOBS_FLAG
        [[ '${RUN:-1}' == 0 ]] && exit 0
        set -f  # a --gtest_filter pattern is not a glob
        xvfb-run -a -s '-screen 0 1600x900x24' $OUT/editor/jasperengine-htmlui-editor-tests ${HTMLUI_TEST_ARGS:-}
    "
    ;;
android)
    # Gradle writes its intermediates next to the project, so the output
    # directory is only used for the copied APK.
    run bash -euo pipefail -c "
        cd engine/workspaces/androidstudio
        CMAKE_CXX_COMPILER_LAUNCHER=ccache ./gradlew --no-daemon assembleDebug
        cp app/build/outputs/apk/debug/*.apk /src/$OUT/
    "
    ls -l "$ROOT/$OUT"/*.apk
    ;;
esac
