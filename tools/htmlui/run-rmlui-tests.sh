#!/usr/bin/env bash
# Builds and runs RmlUi's own unit tests (engine/libs/rmlui/Tests, doctest) against the engine's
# RmlUi fork, standalone (phase-3 spec 5.1). The engine build has no FreeType; this test build
# compiles RmlUi's default FreeType font engine from upstream 6.3 next to the fork, so the engine
# library is unchanged.
#
#   tools/htmlui/run-rmlui-tests.sh [--docker] [--clean] [-- <doctest arguments>]
#
#   --docker   build and run inside the engine's Linux build image (docker/linux.Dockerfile, the
#              image tools/build.sh uses); FreeType is then built from source (pinned, cached)
#   --clean    remove this environment's build directory first
#   doctest arguments, e.g. -- --test-case="*calc*" --success=false
#
# Environment:
#   JOBS=3                   build parallelism (BRIEF.md: at most 3, 4 when told); CPUS
#                            (default JOBS) caps the container's CPUs (docker --cpus)
#   RUN_BENCHMARKS=1         also build and run rmlui_benchmarks (BENCHMARK_ARGS: doctest
#                            arguments for them, e.g. BENCHMARK_ARGS='--test-case=*Selector*')
#   RUN=0                    build only
#   RMLUI_UPSTREAM_DIR=<dir> an existing checkout of RmlUi tag 6.3 instead of the cached clone
#   RMLUI_TESTS_FREETYPE=source  build FreeType from source even when pkg-config finds one
#   RMLUI_FORK_DIR=<dir>     another RmlUi tree laid out like engine/libs/rmlui (Include, Source/Core,
#                            Source/Debugger, Tests), e.g. a scratch copy when bisecting; inside
#                            the worktree with --docker
#
# Everything goes to build/rmlui-tests/: upstream-6.3 (the pinned upstream parts the engine does
# not vendor), freetype (source), host/ or docker/ (src: the assembled tree, symlinks into
# engine/libs/rmlui plus the upstream parts; build: the build tree). The tests run with
# <env>/src as the working directory (RmlUi's shell finds Samples/assets/rml.rcss from there).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FORK="$(cd "${RMLUI_FORK_DIR:-$ROOT/engine/libs/rmlui}" && pwd)"
WORK="$ROOT/build/rmlui-tests"
UPSTREAM="$WORK/upstream-6.3"
UPSTREAM_URL="https://github.com/mikke89/RmlUi.git"
UPSTREAM_TAG="6.3"
UPSTREAM_COMMIT="ba95ffe8bfb6370efb2cdcca927eaad4710c5413"
UPSTREAM_PARTS=2   # bump when fetch_upstream keeps more files: the cache is refreshed
FREETYPE_URL="https://github.com/freetype/freetype.git"
FREETYPE_TAG="VER-2-13-3"
JOBS="${JOBS:-3}"

DOCKER=0
IN_DOCKER=0
CLEAN=0
TEST_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --docker) DOCKER=1 ;;
        --in-docker) IN_DOCKER=1 ;;
        --clean) CLEAN=1 ;;
        --) shift; TEST_ARGS=("$@"); break ;;
        -h|--help) sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run-rmlui-tests: unknown argument $1 (doctest arguments go after --)" >&2; exit 2 ;;
    esac
    shift
done

log() { echo "run-rmlui-tests: $*"; }

# --- 1. the upstream parts (host side: needs git and the network once) ----------------------
# Only what the build needs is kept: Source/Core/FontEngineDefault, CMake, Samples/shell,
# Samples/assets, Samples/basic/{demo,benchmark}/data (documents the tests load),
# Backends/RmlUi_Backend.h, LICENSE.txt.
fetch_upstream() {
    if [[ -f "$UPSTREAM/COMMIT" && "$(cat "$UPSTREAM/COMMIT")" == "$UPSTREAM_COMMIT $UPSTREAM_PARTS" ]]; then
        return
    fi
    local from tmp=""
    if [[ -n "${RMLUI_UPSTREAM_DIR:-}" ]]; then
        from="$RMLUI_UPSTREAM_DIR"
        if [[ -d "$from/.git" ]]; then
            local commit
            commit="$(git -C "$from" rev-parse HEAD)"
            [[ "$commit" == "$UPSTREAM_COMMIT" ]] || { echo "run-rmlui-tests: $from is at $commit, not tag $UPSTREAM_TAG ($UPSTREAM_COMMIT)" >&2; exit 1; }
        fi
    else
        tmp="$WORK/upstream-clone"
        rm -rf "$tmp"
        log "cloning RmlUi $UPSTREAM_TAG"
        git clone --quiet --depth 1 -b "$UPSTREAM_TAG" "$UPSTREAM_URL" "$tmp"
        local commit
        commit="$(git -C "$tmp" rev-parse HEAD)"
        [[ "$commit" == "$UPSTREAM_COMMIT" ]] || { echo "run-rmlui-tests: tag $UPSTREAM_TAG is $commit, expected $UPSTREAM_COMMIT" >&2; exit 1; }
        from="$tmp"
    fi
    rm -rf "$UPSTREAM"
    mkdir -p "$UPSTREAM/Source/Core" "$UPSTREAM/Samples" "$UPSTREAM/Backends"
    cp -rp "$from/Source/Core/FontEngineDefault" "$UPSTREAM/Source/Core/"
    cp -rp "$from/CMake" "$UPSTREAM/"
    cp -rp "$from/Samples/shell" "$from/Samples/assets" "$UPSTREAM/Samples/"
    mkdir -p "$UPSTREAM/Samples/basic/demo" "$UPSTREAM/Samples/basic/benchmark"
    cp -rp "$from/Samples/basic/demo/data" "$UPSTREAM/Samples/basic/demo/"
    cp -rp "$from/Samples/basic/benchmark/data" "$UPSTREAM/Samples/basic/benchmark/"
    cp -p "$from/Backends/RmlUi_Backend.h" "$UPSTREAM/Backends/"
    cp -p "$from/LICENSE.txt" "$UPSTREAM/"
    echo "$UPSTREAM_COMMIT $UPSTREAM_PARTS" > "$UPSTREAM/COMMIT"
    [[ -n "$tmp" ]] && rm -rf "$tmp"
    return 0
}

fetch_freetype() {
    if [[ -f "$WORK/freetype/CMakeLists.txt" ]]; then
        return
    fi
    log "cloning FreeType $FREETYPE_TAG"
    rm -rf "$WORK/freetype"
    git clone --quiet --depth 1 -b "$FREETYPE_TAG" "$FREETYPE_URL" "$WORK/freetype"
}

if [[ $IN_DOCKER -eq 0 ]]; then
    mkdir -p "$WORK"
    fetch_upstream
    if [[ $DOCKER -eq 1 ]]; then
        fetch_freetype
        # the image and mounts of tools/build.sh (from a worktree, the main checkout's ccache)
        MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
        mkdir -p "$MAIN/build/.cache/ccache" "$ROOT/build/.cache/home"
        IMAGE_ID="$(docker build -q -t jasperengine/linux-build -f "$ROOT/docker/linux.Dockerfile" "$ROOT/docker")"
        ARGS=(--in-docker)
        [[ $CLEAN -eq 1 ]] && ARGS+=(--clean)
        TTY_FLAGS=()
        [[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
        exec docker run --rm "${TTY_FLAGS[@]}" --cpus "${CPUS:-$JOBS}" --user "$(id -u):$(id -g)" \
            -v "$ROOT:/src" -w /src -v "$MAIN/build/.cache/ccache:/ccache" \
            -e HOME=/src/build/.cache/home -e CCACHE_DIR=/ccache \
            -e JOBS="$JOBS" -e RUN_BENCHMARKS="${RUN_BENCHMARKS:-0}" -e BENCHMARK_ARGS="${BENCHMARK_ARGS:-}" \
            -e RUN="${RUN:-1}" -e RMLUI_TESTS_FREETYPE="${RMLUI_TESTS_FREETYPE:-}" \
            ${RMLUI_FORK_DIR:+-e RMLUI_FORK_DIR="/src/${FORK#"$ROOT"/}"} \
            "$IMAGE_ID" tools/htmlui/run-rmlui-tests.sh "${ARGS[@]}" -- "${TEST_ARGS[@]}"
    fi
fi

[[ -f "$UPSTREAM/COMMIT" ]] || { echo "run-rmlui-tests: $UPSTREAM is missing (run the script on the host first)" >&2; exit 1; }

ENV_NAME=host
[[ $IN_DOCKER -eq 1 ]] && ENV_NAME=docker
# one build tree per environment and per RmlUi tree: switching RMLUI_FORK_DIR in one tree would
# keep objects newer than the other tree's sources
BUILD="$WORK/$ENV_NAME"
[[ -n "${RMLUI_FORK_DIR:-}" ]] && BUILD="$WORK/$ENV_NAME-$(basename "$FORK")-$(echo "$FORK" | cksum | cut -d' ' -f1)"
[[ $CLEAN -eq 1 ]] && rm -rf "$BUILD"
mkdir -p "$BUILD"

# --- 2. the source tree ------------------------------------------------------------------------
# Relative symlinks into the fork (they resolve on the host and in the container). Source/Core is
# a real directory: the upstream font engine is copied into it, so its "../X.h" includes reach the
# fork's headers; every fork entry is a link, so a fork edit rebuilds incrementally.
SRC="$BUILD/src"
rm -rf "$SRC"
mkdir -p "$SRC/Source/Core" "$SRC/Samples" "$SRC/Backends"
ln -sr "$ROOT/tools/htmlui/rmlui-tests/CMakeLists.txt" "$SRC/CMakeLists.txt"
ln -sr "$FORK/Include" "$SRC/Include"
ln -sr "$FORK/Tests" "$SRC/Tests"
ln -sr "$FORK/Source/Debugger" "$SRC/Source/Debugger"
for entry in "$FORK/Source/Core/"*; do
    ln -sr "$entry" "$SRC/Source/Core/$(basename "$entry")"
done
[[ -e "$FORK/Source/Core/FontEngineDefault" ]] && { echo "run-rmlui-tests: the fork has its own FontEngineDefault; update this script" >&2; exit 1; }
cp -rp "$UPSTREAM/Source/Core/FontEngineDefault" "$SRC/Source/Core/"
ln -sr "$UPSTREAM/CMake" "$SRC/CMake"
ln -sr "$UPSTREAM/Samples/shell" "$SRC/Samples/shell"
ln -sr "$UPSTREAM/Samples/assets" "$SRC/Samples/assets"
ln -sr "$UPSTREAM/Samples/basic" "$SRC/Samples/basic"
ln -sr "$UPSTREAM/Backends/RmlUi_Backend.h" "$SRC/Backends/RmlUi_Backend.h"
ln -sr "$ROOT/tools/htmlui/rmlui-tests/RmlUi_Backend_Null.cpp" "$SRC/Backends/RmlUi_Backend_Null.cpp"

# --- 3. FreeType --------------------------------------------------------------------------------
CMAKE_ARGS=()
if [[ "${RMLUI_TESTS_FREETYPE:-}" == source ]] || ! pkg-config --exists freetype2 2>/dev/null; then
    [[ -f "$WORK/freetype/CMakeLists.txt" ]] || fetch_freetype
    FT_INSTALL="$BUILD/freetype-install"
    if [[ ! -f "$FT_INSTALL/lib/libfreetype.a" && ! -f "$FT_INSTALL/lib64/libfreetype.a" ]]; then
        log "building FreeType $FREETYPE_TAG from source"
        cmake -S "$WORK/freetype" -B "$BUILD/freetype-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
            -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DCMAKE_INSTALL_PREFIX="$FT_INSTALL" >/dev/null
        cmake --build "$BUILD/freetype-build" -j "$JOBS" >/dev/null
        cmake --install "$BUILD/freetype-build" >/dev/null
    fi
    CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$FT_INSTALL")
    log "FreeType: $FREETYPE_TAG built from source"
else
    log "FreeType: system $(pkg-config --modversion freetype2) (pkg-config)"
fi

# --- 4. configure and build ------------------------------------------------------------------
GENERATOR="Unix Makefiles"
command -v ninja >/dev/null && GENERATOR=Ninja
command -v ccache >/dev/null && CMAKE_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
cmake -S "$SRC" -B "$BUILD/build" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release "${CMAKE_ARGS[@]}" >/dev/null
TARGETS=(rmlui_unit_tests)
[[ "${RUN_BENCHMARKS:-0}" == 1 ]] && TARGETS+=(rmlui_benchmarks)
cmake --build "$BUILD/build" -j "$JOBS" --target "${TARGETS[@]}"

[[ "${RUN:-1}" == 0 ]] && exit 0

# --- 5. run ----------------------------------------------------------------------------------------
cd "$SRC"
status=0
log "rmlui_unit_tests ${TEST_ARGS[*]:-}"
"$BUILD/build/Tests/Source/UnitTests/rmlui_unit_tests" "${TEST_ARGS[@]}" || status=$?
if [[ "${RUN_BENCHMARKS:-0}" == 1 ]]; then
    log "rmlui_benchmarks ${BENCHMARK_ARGS:-}"
    # shellcheck disable=SC2086
    "$BUILD/build/Tests/Source/Benchmarks/rmlui_benchmarks" ${BENCHMARK_ARGS:-} || status=$?
fi
exit $status
