#!/usr/bin/env bash
# Builds the engine and editor inside the Docker images under docker/, so the
# host only needs Docker. Output goes to build/<target>/ (git-ignored); test results
# to build/test-results/. docs/testing.md describes every suite.
#
#   tools/build.sh linux [--clean]     editor + engine sample project (Release)
#   tools/build.sh web [--clean]       engine sample project for Emscripten (WEB_THREADS=1: pthreads, build/web-threads)
#   tools/build.sh android [--clean]   engine sample project APK (debug)
#   tools/build.sh unit-tests [--clean]    builds and runs jasperengine-core-tests and jasperengine-editor-tests
#   tools/build.sh htmlui-tests [--clean]  builds and runs jasperengine-htmlui-tests headless
#   tools/build.sh htmlui-editor-tests      builds the linux editor with jasperengine-htmlui-editor-tests, runs them
#   tools/build.sh mcp-tests [args...]  the MCP tests against build/linux/editor-install (run tools/build.sh linux first)
#   tools/build.sh web-gate            the web build (tools/build.sh web) in headless Chrome (tools/web/browser_gate.py)
#   tools/build.sh shell <target>      interactive shell in that target's image
#   tools/build.sh exec <target> <command...>  that command in the image (e.g. exec linux tools/ci/check-presets.sh)
#
# BUILD_TYPE overrides the CMake build type (default Release; Debug for web; RelWithDebInfo for
# unit-tests; Debug for every sanitizer build). SKIP_ENGINE=1 builds only the editor for linux.
# JOBS caps build parallelism and, with CPUS (default: DOCKER_CPUS, else JOBS), the CPUs the
# container may use (docker --cpus), which also caps what ignores JOBS: the Android gradle/ninja
# build and the test binaries. DOCKER_CPUS is the same cap under the name tools/smoke-test.sh
# passes (e.g. DOCKER_CPUS=4 on a shared host).
# GRAPHIC_BACKEND=vulkan builds the linux editor for Vulkan (build/linux/editor-vulkan), and
# runs the core tests' GPU suites on Vulkan (lavapipe) for unit-tests.
# SANITIZE=address,undefined or SANITIZE=thread (linux, unit-tests, htmlui-tests) builds with
# those sanitizers into build/linux/<dir>-<sanitizers>; UNITY=1 unity-builds (<dir>-unity);
# WERROR=1 makes warnings of our targets errors. JASPER_DOCKER_IMAGE=<id|tag> runs that image
# instead of building docker/<target>.Dockerfile (CI's prebuilt images).
# unit-tests: UNIT_TEST_ARGS goes to both test binaries (e.g. --gtest_filter=Vector3.*), RUN=0
# builds only, UNIT_SUITE=core or editor builds and runs one of the two; JASPER_TEST_UPDATE_GOLDEN=1
# rewrites the core goldens (glcore only).
# linux, web, unit-tests, htmlui-tests: CXX_STANDARD=<n> builds at another supported C++ standard
# than the default (JASPERENGINE_CXX_STANDARD), next to the default build (build/linux/editor-cxx<n>,
# build/web/cxx<n>, ...). linux first checks engine/ and editor/ with tools/check-cxx-subset.py and
# tools/check-pollers.py.
# htmlui-tests: HTMLUI_BACKEND=glcore|vulkan (default glcore), RUN=0 builds only,
# HTMLUI_TEST_ARGS goes to the test binary (e.g. --gtest_filter=Golden*),
# JASPER_HTMLUI_UPDATE_GOLDEN=1 rewrites the golden images (glcore only).
# JASPER_HTMLUI_WRITE_ACTUAL=1 keeps every compared image in the test output directory (both backends).
# htmlui-editor-tests: HTMLUI_TEST_ARGS goes to the test binary, RUN=0 builds only,
# HTMLUI_LIVE_NETWORK=1 adds the live Google Fonts test.
# mcp-tests: MCP_SUITE=ci (default; the CI = True files) or all (needs JASPER_FIXTURE, the
# fixture project, mounted read-only); arguments after the target and MCP_TEST_ARGS go to
# tools/mcp/run-tests.sh (e.g. -j 2, -k undo). Results: build/test-results/mcp.json.
# web-gate: GATE=sample-project (default, build/web) or sample-project-threads (build/web-threads);
# GATE_ARGS goes to browser_gate.py. Screenshot and transcript in build/test-results/web/.
# From a git worktree, the main checkout's ccache and .git are shared.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR_TARGET=jasperengine-editor
ENGINE_TARGET=jasperengine-project
# AC7 of docs/specs/S02-build-ci-tests.md: what an unfiltered run must at least execute
CORE_MIN_TESTS=300
EDITOR_MIN_TESTS=150
# jasperengine-htmlui-tests skips these on purpose (docs/html-ui/phase-6-acceptance.md §4.2)
HTMLUI_MAX_SKIPPED=74

usage() {
    sed -n '2,46p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

[[ $# -ge 1 ]] || usage
TARGET="$1"; shift
CLEAN=0
SHELL_MODE=0
EXEC_MODE=0
PASSTHROUGH=()
if [[ "$TARGET" == shell || "$TARGET" == exec ]]; then
    [[ $# -ge 1 ]] || usage
    [[ "$TARGET" == shell ]] && SHELL_MODE=1 || EXEC_MODE=1
    TARGET="$1"; shift
fi
case "$TARGET" in
    linux|web|android|unit-tests|htmlui-tests|htmlui-editor-tests|mcp-tests|web-gate) ;;
    *) usage ;;
esac
if [[ $EXEC_MODE -eq 1 ]]; then
    [[ $# -ge 1 ]] || usage
    PASSTHROUGH=("$@")
    set --
fi
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        *) [[ "$TARGET" == mcp-tests ]] || usage; PASSTHROUGH+=("$arg") ;;
    esac
done

# The tests run in the image of the build they test, next to its output
IMAGE_TARGET="$TARGET"
case "$TARGET" in
    unit-tests|htmlui-tests|htmlui-editor-tests|mcp-tests) IMAGE_TARGET=linux ;;
    web-gate) IMAGE_TARGET=web ;;
esac
IMAGE="jasperengine/${IMAGE_TARGET}-build"
OUT="build/$IMAGE_TARGET"
RESULTS="build/test-results"
CACHE="build/.cache"
# The main checkout, even when running from a git worktree.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
JOBS_FLAG=""
[[ -n "${JOBS:-}" ]] && JOBS_FLAG="-j $JOBS"

# Rebuilding an unchanged image is a no-op thanks to the layer cache. The containers run the
# image by id: other worktrees (with another Dockerfile) may retag the shared name meanwhile.
if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE_ID="$JASPER_DOCKER_IMAGE"
else
    IMAGE_ID="$(docker build -q -t "$IMAGE" -f "$ROOT/docker/$IMAGE_TARGET.Dockerfile" "$ROOT/docker")"
fi

HTMLUI_BACKEND="${HTMLUI_BACKEND:-glcore}"
LINUX_BACKEND="${GRAPHIC_BACKEND:-glcore}"
if [[ "$TARGET" == htmlui-tests && "$HTMLUI_BACKEND" != glcore && "$HTMLUI_BACKEND" != vulkan ]]; then
    echo "HTMLUI_BACKEND must be glcore or vulkan" >&2; exit 1
fi
if [[ ( "$TARGET" == linux || "$TARGET" == unit-tests ) && "$LINUX_BACKEND" != glcore && "$LINUX_BACKEND" != vulkan ]]; then
    echo "GRAPHIC_BACKEND must be glcore or vulkan for $TARGET" >&2; exit 1
fi

# SANITIZE=address,undefined | thread: the CMake list, and the suffix of the build directories
SANITIZE_LIST=""
SANITIZE_SUFFIX=""
if [[ -n "${SANITIZE:-}" ]]; then
    case "$TARGET" in
        linux|unit-tests|htmlui-tests) ;;
        *) echo "SANITIZE is for linux, unit-tests and htmlui-tests" >&2; exit 1 ;;
    esac
    IFS=', ' read -r -a sanitizers <<< "$SANITIZE"
    for s in "${sanitizers[@]}"; do
        [[ "$s" =~ ^(address|undefined|thread)$ ]] || { echo "SANITIZE: unknown sanitizer '$s' (address, undefined, thread)" >&2; exit 1; }
    done
    SANITIZE_LIST="$(IFS=';'; echo "${sanitizers[*]}")"
    SANITIZE_SUFFIX="-$(IFS='-'; echo "${sanitizers[*]}")"
fi
# TSan's shadow memory needs the low-entropy layout: kernels with vm.mmap_rnd_bits=32 (Ubuntu 22.04+
# hosts, GitHub runners) make a GCC 11 TSan binary die with "unexpected memory mapping". The test
# binaries start without ASLR (setarch -R), and Docker's default seccomp profile refuses that
# personality, so a TSan run's container has none.
TEST_WRAP=""
SECURITY_FLAGS=()
if [[ ",${SANITIZE:-}," == *,thread,* ]]; then
    TEST_WRAP="setarch x86_64 -R"
    SECURITY_FLAGS=(--security-opt seccomp=unconfined)
fi
UNITY_SUFFIX=""
[[ "${UNITY:-0}" == 1 ]] && UNITY_SUFFIX="-unity"
WERROR_VALUE=OFF
[[ "${WERROR:-0}" == 1 ]] && WERROR_VALUE=ON
# what every CMake configure of a build gets on top of its preset
OPTION_FLAGS="-DJASPERENGINE_SANITIZE='$SANITIZE_LIST'"
[[ "${UNITY:-0}" == 1 ]] && OPTION_FLAGS="$OPTION_FLAGS -DJASPERENGINE_UNITY_BUILD=ON" || OPTION_FLAGS="$OPTION_FLAGS -DJASPERENGINE_UNITY_BUILD=OFF"
# (WERROR needs WARNINGS, which the standalone engine tree has off by default)
ENGINE_WARNING_FLAGS="-DJASPERENGINE_WERROR=$WERROR_VALUE"
[[ "$WERROR_VALUE" == ON ]] && ENGINE_WARNING_FLAGS="$ENGINE_WARNING_FLAGS -DJASPERENGINE_WARNINGS=ON"
VARIANT="$SANITIZE_SUFFIX$UNITY_SUFFIX"

# What --clean removes: the whole output of the target, or only this test build
CLEAN_DIR="$OUT"
[[ "$TARGET" == htmlui-tests ]] && CLEAN_DIR="$OUT/htmlui-tests-$HTMLUI_BACKEND$VARIANT"
[[ "$TARGET" == unit-tests ]] && CLEAN_DIR="$OUT/core-tests*$VARIANT $OUT/editor-tests$VARIANT"
[[ "$TARGET" == mcp-tests || "$TARGET" == web-gate ]] && CLEAN_DIR="$RESULTS"

if [[ $CLEAN -eq 1 ]]; then
    # Files inside may belong to the container user; remove them from inside.
    docker run --rm -v "$ROOT:/src" -w /src "$IMAGE_ID" bash -c "rm -rf $CLEAN_DIR"
fi
mkdir -p "$ROOT/$OUT" "$ROOT/$RESULTS" "$ROOT/$CACHE/home" "$MAIN/$CACHE/ccache"

TTY_FLAGS=()
[[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
CPU_FLAGS=()
CPUS="${CPUS:-${DOCKER_CPUS:-${JOBS:-}}}"
[[ -n "$CPUS" ]] && CPU_FLAGS=(--cpus "$CPUS")
ENV_FLAGS=()
for name in JASPER_HTMLUI_UPDATE_GOLDEN JASPER_HTMLUI_WRITE_ACTUAL JASPER_TEST_UPDATE_GOLDEN JASPER_TEST_WRITE_ACTUAL \
            JASPER_TEST_KEEP_TEMP JASPER_HTMLUI_PERF_SLACK HTMLUI_LIVE_NETWORK JASPER_CHROME; do
    # (JASPER_HTMLUI_PERF_SLACK: the HTML UI performance tests' tolerance on a loaded host, compat_perf_test.cpp)
    [[ -n "${!name:-}" ]] && ENV_FLAGS+=(-e "$name=${!name}")
done
# Sanitizer runtime options for every run (tools/sanitizers/*.supp: third-party code only)
SUPP=/src/tools/sanitizers
ENV_FLAGS+=(
    -e "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1"
    -e "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=$SUPP/ubsan.supp"
    -e "LSAN_OPTIONS=suppressions=$SUPP/lsan.supp"
    -e "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=$SUPP/tsan.supp"
)
MOUNT_FLAGS=()
# mcp-tests MCP_SUITE=all: the fixture project the default-fixture tests copy
if [[ "$TARGET" == mcp-tests && -n "${JASPER_FIXTURE:-}" ]]; then
    MOUNT_FLAGS+=(-v "$JASPER_FIXTURE:/fixture:ro" -e JASPER_FIXTURE=/fixture)
fi

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
        -e UV_CACHE_DIR="/src/$CACHE/uv" \
        ${ENV_FLAGS[@]+"${ENV_FLAGS[@]}"} \
        ${MOUNT_FLAGS[@]+"${MOUNT_FLAGS[@]}"} \
        ${SECURITY_FLAGS[@]+"${SECURITY_FLAGS[@]}"} \
        "$IMAGE_ID" "$@"
}

if [[ $SHELL_MODE -eq 1 ]]; then
    run bash
    exit 0
fi
if [[ $EXEC_MODE -eq 1 ]]; then
    run "${PASSTHROUGH[@]}"
    exit $?
fi

CCACHE_FLAGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
# googletest XML results through the no-silent-green gate (tools/ci/gtest-gate.py)
GTEST_GATE="python3 tools/ci/gtest-gate.py"

# A non-default standard gets build directories of its own, so the default trees keep theirs
CXX_STANDARD_FLAGS=""
CXX_SUFFIX=""
if [[ -n "${CXX_STANDARD:-}" ]]; then
    if [[ ! "$CXX_STANDARD" =~ ^[0-9]+$ ]]; then
        echo "CXX_STANDARD must be a number (e.g. 23)" >&2; exit 1
    fi
    CXX_STANDARD_FLAGS="-DJASPERENGINE_CXX_STANDARD=$CXX_STANDARD"
    CXX_SUFFIX="-cxx$CXX_STANDARD"
fi
OPTION_FLAGS="$OPTION_FLAGS $CXX_STANDARD_FLAGS"
VARIANT="$CXX_SUFFIX$VARIANT"

case "$TARGET" in
linux)
    BT="${BUILD_TYPE:-Release}"
    [[ -n "$SANITIZE_LIST" ]] && BT="${BUILD_TYPE:-Debug}"
    # glcore keeps the historical paths; vulkan, sanitizer and unity builds go next to them
    EDITOR_DIR="editor"
    PRESET=linux-release
    if [[ "$LINUX_BACKEND" == vulkan ]]; then
        EDITOR_DIR="editor-vulkan"
        PRESET=linux-vulkan-release
    fi
    EDITOR_DIR="$EDITOR_DIR$VARIANT"
    ENGINE_DIR="engine$VARIANT"
    run bash -euo pipefail -c "
        python3 tools/check-cxx-subset.py
        python3 tools/check-pollers.py
        cmake --preset $PRESET -B $OUT/$EDITOR_DIR -DCMAKE_BUILD_TYPE=$BT $OPTION_FLAGS \
            -DJASPERENGINE_WERROR=$WERROR_VALUE \
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
        cmake --preset engine-linux-release -S engine -B $OUT/$ENGINE_DIR -DCMAKE_BUILD_TYPE=$BT $OPTION_FLAGS \
            $ENGINE_WARNING_FLAGS $CCACHE_FLAGS
        cmake --build $OUT/$ENGINE_DIR --target $ENGINE_TARGET $JOBS_FLAG
        ccache --show-stats --verbose | grep -E 'Hits|Misses' || true
    "
    ;;
web)
    BT="${BUILD_TYPE:-Debug}"
    PRESET=engine-web-debug
    WEB_DIR="$OUT"
    if [[ "${WEB_THREADS:-0}" == 1 ]]; then
        PRESET=engine-web-threads
        WEB_DIR="$OUT-threads"
    fi
    [[ -n "$CXX_SUFFIX" ]] && WEB_DIR="$WEB_DIR/cxx$CXX_STANDARD"
    run bash -euo pipefail -c "
        cmake --preset $PRESET -S engine -B $WEB_DIR -DCMAKE_BUILD_TYPE=$BT $OPTION_FLAGS $CCACHE_FLAGS
        cmake --build $WEB_DIR --target $ENGINE_TARGET $JOBS_FLAG
    "
    ;;
unit-tests)
    BT="${BUILD_TYPE:-RelWithDebInfo}"
    CORE_PRESET=engine-linux-tests
    EDITOR_PRESET=linux-ci
    case "$SANITIZE_LIST" in
        "") ;;
        thread) CORE_PRESET=engine-linux-tsan; EDITOR_PRESET=linux-tsan; BT="${BUILD_TYPE:-Debug}" ;;
        *) CORE_PRESET=engine-linux-asan; EDITOR_PRESET=linux-asan; BT="${BUILD_TYPE:-Debug}" ;;
    esac
    CORE_DIR="$OUT/core-tests$VARIANT"
    CORE_RESULT="core$VARIANT"
    VK_ENV=""
    if [[ "$LINUX_BACKEND" == vulkan ]]; then
        [[ -z "$SANITIZE_LIST" ]] && CORE_PRESET=engine-linux-tests-vulkan
        CORE_DIR="$OUT/core-tests-vulkan$VARIANT"
        CORE_RESULT="core-vulkan$VARIANT"
        VK_ENV="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    fi
    EDITOR_DIR="$OUT/editor-tests$VARIANT"
    EDITOR_RESULT="editor$VARIANT"
    # a filtered run proves only that something ran; an unfiltered one has to reach AC7's counts
    CORE_MIN=$CORE_MIN_TESTS
    EDITOR_MIN=$EDITOR_MIN_TESTS
    if [[ "${UNIT_TEST_ARGS:-}" == *--gtest_filter* ]]; then CORE_MIN=1; EDITOR_MIN=1; fi
    # (UNIT_SUITES=core|editor: the name S03's interim target used)
    UNIT_SUITE="${UNIT_SUITE:-${UNIT_SUITES:-all}}"
    [[ "$UNIT_SUITE" == "core editor" ]] && UNIT_SUITE=all
    [[ "$UNIT_SUITE" =~ ^(all|core|editor)$ ]] || { echo "UNIT_SUITE must be all, core or editor" >&2; exit 1; }
    CORE=0; EDITOR=0
    [[ "$UNIT_SUITE" != editor ]] && CORE=1
    [[ "$UNIT_SUITE" != core ]] && EDITOR=1
    run bash -euo pipefail -c "
        if [[ $CORE == 1 ]]; then
            cmake --preset $CORE_PRESET -S engine -B $CORE_DIR -DCMAKE_BUILD_TYPE=$BT -DGRAPHIC_BACKEND=$LINUX_BACKEND \
                $OPTION_FLAGS -DJASPERENGINE_WARNINGS=ON -DJASPERENGINE_WERROR=$WERROR_VALUE $CCACHE_FLAGS
            cmake --build $CORE_DIR --target jasperengine-core-tests $JOBS_FLAG
        fi
        if [[ $EDITOR == 1 ]]; then
            cmake --preset $EDITOR_PRESET -B $EDITOR_DIR -DCMAKE_BUILD_TYPE=$BT $OPTION_FLAGS \
                -DJASPERENGINE_WERROR=$WERROR_VALUE $CCACHE_FLAGS
            cmake --build $EDITOR_DIR --target jasperengine-editor-tests $JOBS_FLAG
        fi
        [[ '${RUN:-1}' == 0 ]] && exit 0
        set -f  # a --gtest_filter pattern is not a glob
        status=0
        if [[ $CORE == 1 ]]; then
            rm -f $RESULTS/$CORE_RESULT.xml
            env $VK_ENV xvfb-run -a -s '-screen 0 1600x900x24' $TEST_WRAP \
                $CORE_DIR/tests/core/jasperengine-core-tests --gtest_output=xml:$RESULTS/$CORE_RESULT.xml ${UNIT_TEST_ARGS:-} || status=1
            $GTEST_GATE --xml $RESULTS/$CORE_RESULT.xml --min-tests $CORE_MIN --max-skipped 0 || status=1
        fi
        if [[ $EDITOR == 1 ]]; then
            rm -f $RESULTS/$EDITOR_RESULT.xml
            xvfb-run -a -s '-screen 0 1600x900x24' $TEST_WRAP \
                $EDITOR_DIR/jasperengine-editor-tests --gtest_output=xml:$RESULTS/$EDITOR_RESULT.xml ${UNIT_TEST_ARGS:-} || status=1
            $GTEST_GATE --xml $RESULTS/$EDITOR_RESULT.xml --min-tests $EDITOR_MIN --max-skipped 0 || status=1
        fi
        exit \$status
    "
    ;;
htmlui-tests)
    BT="${BUILD_TYPE:-Release}"
    [[ -n "$SANITIZE_LIST" ]] && BT="${BUILD_TYPE:-Debug}"
    TESTS_DIR="$OUT/htmlui-tests-$HTMLUI_BACKEND$VARIANT"
    RESULT="htmlui-$HTMLUI_BACKEND$VARIANT"
    VK_ENV=""
    [[ "$HTMLUI_BACKEND" == vulkan ]] && VK_ENV="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    run bash -euo pipefail -c "
        cmake --preset engine-linux-release -S engine -B $TESTS_DIR -DCMAKE_BUILD_TYPE=$BT \
            -DJASPERENGINE_HTMLUI_TESTS=ON -DGRAPHIC_BACKEND=$HTMLUI_BACKEND $OPTION_FLAGS $ENGINE_WARNING_FLAGS $CCACHE_FLAGS
        cmake --build $TESTS_DIR --target jasperengine-htmlui-tests $JOBS_FLAG
        [[ '${RUN:-1}' == 0 ]] && exit 0
        set -f  # a --gtest_filter pattern is not a glob
        rm -f $RESULTS/$RESULT.xml
        status=0
        env $VK_ENV xvfb-run -a -s '-screen 0 1600x900x24' $TEST_WRAP \
            $TESTS_DIR/tests/htmlui/jasperengine-htmlui-tests --gtest_output=xml:$RESULTS/$RESULT.xml ${HTMLUI_TEST_ARGS:-} || status=1
        $GTEST_GATE --xml $RESULTS/$RESULT.xml --max-skipped $HTMLUI_MAX_SKIPPED || status=1
        exit \$status
    "
    ;;
htmlui-editor-tests)
    BT="${BUILD_TYPE:-Release}"
    run bash -euo pipefail -c "
        cmake --preset linux-release -B $OUT/editor -DCMAKE_BUILD_TYPE=$BT -DJASPERENGINE_HTMLUI_EDITOR_TESTS=ON \
            $OPTION_FLAGS -DJASPERENGINE_WERROR=$WERROR_VALUE -DCMAKE_INSTALL_PREFIX=$OUT/editor-install $CCACHE_FLAGS
        cmake --build $OUT/editor --target jasperengine-editor jasperengine-htmlui-editor-tests $JOBS_FLAG
        [[ '${RUN:-1}' == 0 ]] && exit 0
        set -f  # a --gtest_filter pattern is not a glob
        rm -f $RESULTS/htmlui-editor.xml
        status=0
        xvfb-run -a -s '-screen 0 1600x900x24' $OUT/editor/jasperengine-htmlui-editor-tests \
            --gtest_output=xml:$RESULTS/htmlui-editor.xml ${HTMLUI_TEST_ARGS:-} || status=1
        # the live Google Fonts test skips without HTMLUI_LIVE_NETWORK=1
        $GTEST_GATE --xml $RESULTS/htmlui-editor.xml --max-skipped 1 || status=1
        exit \$status
    "
    ;;
mcp-tests)
    SUITE="${MCP_SUITE:-ci}"
    [[ "$SUITE" == ci || "$SUITE" == all ]] || { echo "MCP_SUITE must be ci or all" >&2; exit 1; }
    [[ "$SUITE" == all && -z "${JASPER_FIXTURE:-}" ]] && { echo "MCP_SUITE=all needs JASPER_FIXTURE (the fixture project)" >&2; exit 1; }
    [[ -x "$ROOT/build/linux/editor-install/bin/jasperengine-editor" ]] || { echo "No editor at build/linux/editor-install; run tools/build.sh linux" >&2; exit 1; }
    SUITE_FLAGS="--suite $SUITE"
    [[ "$SUITE" == all ]] && SUITE_FLAGS=""
    # (each argument quoted once more: the command runs through bash -c)
    ARGS="$(printf '%q ' ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}) ${MCP_TEST_ARGS:-}"
    run bash -euo pipefail -c "
        rm -f $RESULTS/mcp.json
        dbus-run-session -- tools/mcp/run-tests.sh $SUITE_FLAGS --json $RESULTS/mcp.json $ARGS
    "
    ;;
web-gate)
    NAME="${GATE:-sample-project}"
    case "$NAME" in
        sample-project) WEB_DIR="$OUT" ;;
        sample-project-threads) WEB_DIR="$OUT-threads" ;;
        *) echo "GATE must be sample-project or sample-project-threads" >&2; exit 1 ;;
    esac
    [[ -f "$ROOT/$WEB_DIR/$ENGINE_TARGET.html" ]] || { echo "No web build at $WEB_DIR; run tools/build.sh web" >&2; exit 1; }
    run bash -euo pipefail -c "
        mkdir -p $RESULTS/web
        python3 tools/web/browser_gate.py --root $WEB_DIR --page $ENGINE_TARGET.html --until-ready \
            --baseline tools/web/gates/$NAME.json --screenshot $RESULTS/web/$NAME.png \
            --transcript $RESULTS/web/$NAME.log ${GATE_ARGS:-}
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
