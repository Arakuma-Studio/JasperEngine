#!/usr/bin/env bash
# Builds the engine and editor inside the Docker images under docker/, so the
# host only needs Docker. Output goes to build/<target>/ (git-ignored); test results
# to build/test-results/. docs/testing.md describes every suite.
#
#   tools/build.sh linux [--clean]     editor + engine sample project (Release)
#   tools/build.sh web [--clean]       engine sample project for Emscripten (WEB_THREADS=1: pthreads, build/web-threads)
#   tools/build.sh android [--clean]   engine sample project APK (debug; every ABI, x86_64 for emulators)
#   tools/build.sh unit-tests [--clean]    builds and runs jasperengine-core-tests and jasperengine-editor-tests
#   tools/build.sh htmlui-tests [--clean]  builds and runs jasperengine-htmlui-tests headless
#   tools/build.sh htmlui-editor-tests      builds the linux editor with jasperengine-htmlui-editor-tests, runs them
#   tools/build.sh mcp-tests [args...]  the MCP tests against build/linux/editor-install (run tools/build.sh linux first)
#   tools/build.sh web-gate            the web build (tools/build.sh web) in headless Chrome (tools/web/browser_gate.py)
#   tools/build.sh players             the prebuilt player runtimes (S30) into build/players/<id>/:
#                                      PLAYER_PLATFORM=linux (default; PLAYER_BACKENDS="glcore gles3 vulkan")
#                                      or web (single-thread and pthreads) or android; PLAYER_FLAVOURS="release development";
#                                      PLAYER_ZIP=1 also writes the archives into build/players/archives/;
#                                      PLAYER_VARIANTS="full lite3d" the release variants (default: every one of
#                                      engine/player/variants.json; the development flavour is the full player);
#                                      android: PLAYER_ANDROID_ABIS=x86_64 builds only those ABIs (an emulator's)
#   tools/build.sh shell <target>      interactive shell in that target's image
#   tools/build.sh exec <target> <command...>  that command in the image (e.g. exec linux tools/ci/check-presets.sh)
#                                      (target runtime: the no-compiler image, docker/runtime.Dockerfile:
#                                      exec runtime python3 tools/ci/test_player_cli.py runs the S30 player CLI test)
#
# BUILD_TYPE overrides the CMake build type (default Release; Debug for web; RelWithDebInfo for
# unit-tests; Debug for every sanitizer build). SKIP_ENGINE=1 builds only the editor for linux.
# JOBS caps build parallelism and, with CPUS (default: DOCKER_CPUS, else JOBS), the CPUs the
# container may use (docker --cpus), which also caps what ignores JOBS: the Android gradle/ninja
# build and the test binaries. DOCKER_CPUS is the same cap under the name tools/smoke-test.sh
# passes (e.g. DOCKER_CPUS=4 on a shared host). KEEP_GOING=1 lets Ninja build on past a failure.
# DOCKER_GPU=1 runs the container with the host's NVIDIA GPU (see below and tools/gpu/env.sh).
# GRAPHIC_BACKEND=vulkan builds the linux editor for Vulkan (build/linux/editor-vulkan), and
# runs the core tests' GPU suites on Vulkan (lavapipe) for unit-tests.
# SANITIZE=address,undefined or SANITIZE=thread (linux, unit-tests, htmlui-tests, mcp-tests) builds with
# those sanitizers into build/linux/<dir>-<sanitizers>; UNITY=1 unity-builds (<dir>-unity);
# WERROR=1 makes warnings of our targets errors. JASPER_DOCKER_IMAGE=<id|tag> runs that image
# instead of building docker/<target>.Dockerfile (CI's prebuilt images).
# unit-tests: UNIT_TEST_ARGS goes to both test binaries (e.g. --gtest_filter=Vector3.*), RUN=0
# builds only, UNIT_SUITE=core or editor builds and runs one of the two; JASPER_TEST_UPDATE_GOLDEN=1
# rewrites the core goldens (glcore only) and the reflection snapshot; JASPER_REFLECT_WRITE_FIXTURE=1
# regenerates tools/fixtures/reflection-parity/AllComponents.scene (docs/specs/S08-reflection.md);
# JASPER_BENCH_ENTITIES sets the reflection codec benchmark's scene size (ReflectCodecBench).
# linux, web, unit-tests, htmlui-tests: CXX_STANDARD=<n> builds at another supported C++ standard
# than the default (JASPERENGINE_CXX_STANDARD), next to the default build (build/linux/editor-cxx<n>,
# build/web/cxx<n>, ...). linux first checks engine/ and editor/ with tools/check-cxx-subset.py and
# tools/check-pollers.py.
# htmlui-tests: HTMLUI_BACKEND=glcore|vulkan (default glcore), RUN=0 builds only,
# HTMLUI_TEST_ARGS goes to the test binary (e.g. --gtest_filter=Golden*),
# JASPER_HTMLUI_UPDATE_GOLDEN=1 rewrites the golden images (glcore only).
# JASPER_HTMLUI_WRITE_ACTUAL=1 keeps every compared image in the test output directory (both backends).
# JASPER_HTMLUI_S16_OFF=1 runs the HTML UI tests with S16's batching, SDF primitives and transform-aware
# text off (docs/specs/S16-ui-render-text.md AC1); JASPER_HTMLUI_BATCHING=0 / JASPER_HTMLUI_SDF_PRIMITIVES=0
# turn one of them off.
# JASPER_ZERO_COST_UPDATE=1 rewrites the built-in Lit's zero-cost hash list (ShaderZeroCostTest, unit-tests).
# htmlui-editor-tests: HTMLUI_TEST_ARGS goes to the test binary, RUN=0 builds only,
# HTMLUI_LIVE_NETWORK=1 adds the live Google Fonts test.
# mcp-tests: MCP_SUITE=ci (default; the CI = True files) or all (needs the fixture project,
# mounted read-only at /fixture: JASPER_FIXTURE, else build/fixture-unity of this or the main
# checkout, else ~/je/fixture-unity, the build servers' copy); arguments after the target and MCP_TEST_ARGS go to
# tools/mcp/run-tests.sh (e.g. -j 2, -k undo). Results: build/test-results/mcp.json. MCP_SUITE=all first
# builds the mesh-raycast web test project (build/web-mesh-raycast) its web latency gate loads.
# web, android: WEB_PROJECT=<name> / ANDROID_PROJECT=<name> build the test project
# tools/test-projects/<name>/ instead of engine/project, into build/web-<name> or
# build/android-<name> (core-utilities: the logger and Lua Timer, S09).
# android: ANDROID_ABIS="x86_64" (comma separated) builds only those ABIs (the emulator's is
# x86_64); the default is every ABI an export ships.
# web, web-gate: WASM_SIMD=0 builds (and gates) the scalar particle kernels instead of simd128
# (-DJASPERENGINE_WASM_SIMD=OFF), next to the default build (build/web*-scalar).
# web-gate: GATE=sample-project (default, build/web), sample-project-threads (build/web-threads)
# or a test project with a gate (tools/web/gates/<name>.json; build/web-<name>, WEB_PROJECT=<name>
# tools/build.sh web first; WEB_THREADS=1 gates its pthreads build, build/web-threads-<name>);
# GATE_ARGS goes to browser_gate.py. Screenshot and transcript in build/test-results/web/.
# From a git worktree, the main checkout's ccache and .git are shared.
# Inside the Linux build image (rb.sh <server> --img 'tools/build.sh ...'; no docker there) the targets
# of that image run directly with the same environment; another image's target exits 3.
# COVERAGE=1 (linux): GCC's --coverage, in build/linux/editor-coverage (tools/mcp/coverage_map.py).
# MCP_SKIP_WEB_BUILD=1 (mcp-tests): no mesh-raycast web build, for a run without test_mesh_raycast.py.
# DOCKER_GPU=1 (mcp-tests): the real-GPU lane, for a plan's chunks of the lane gpu (docs/testing.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR_TARGET=jasperengine-editor
ENGINE_TARGET=jasperengine-project
# AC7 of docs/specs/S02-build-ci-tests.md: what an unfiltered run must at least execute
CORE_MIN_TESTS=300
EDITOR_MIN_TESTS=150
# jasperengine-htmlui-tests skips these on purpose (docs/html-ui/phase-6-acceptance.md §4.2), and
# upstream's opt-in EffectFluidProfile.LiveCost (JASPER_EFFECT_FLUID_PROFILE=1)
HTMLUI_MAX_SKIPPED=75

usage() {
    sed -n '2,75p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
    linux|web|android|unit-tests|htmlui-tests|htmlui-editor-tests|mcp-tests|web-gate|players) ;;
    # the no-compiler image (docker/runtime.Dockerfile): shell and exec only
    runtime) [[ $SHELL_MODE -eq 1 || $EXEC_MODE -eq 1 ]] || usage ;;
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
    players)
        case "${PLAYER_PLATFORM:-linux}" in
            web) IMAGE_TARGET=web ;;
            android) IMAGE_TARGET=android ;;
            *) IMAGE_TARGET=linux ;;
        esac ;;
esac
IMAGE="jasperengine/${IMAGE_TARGET}-build"
OUT="build/$IMAGE_TARGET"
RESULTS="build/test-results"
CACHE="build/.cache"
# The main checkout, even when running from a git worktree.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
JOBS_FLAG=""
[[ -n "${JOBS:-}" ]] && JOBS_FLAG="-j $JOBS"
# KEEP_GOING=1: Ninja builds every target it can after a failure (all the errors of one run)
[[ "${KEEP_GOING:-0}" == 1 ]] && JOBS_FLAG="$JOBS_FLAG -- -k 0"

# mcp-tests, the full suite: the web build test_mesh_raycast.py's latency gate loads (the
# mesh-raycast test project for Emscripten, in the web image), built once and kept up to date
# (ninja, ccache). Before the Linux image is picked below: while it builds, another worktree's
# image build may retag the shared name and its old image be pruned.
# (MCP_SKIP_WEB_BUILD=1: a shard of the suite without test_mesh_raycast.py, tools/mcp/shard.py)
if [[ "$TARGET" == mcp-tests && "${MCP_SUITE:-ci}" == all && "${MCP_SKIP_WEB_BUILD:-0}" != 1 && $SHELL_MODE -eq 0 && $EXEC_MODE -eq 0 ]]; then
    env -u WEB_THREADS -u WASM_SIMD -u CXX_STANDARD -u SANITIZE -u JASPER_DOCKER_IMAGE WEB_PROJECT=mesh-raycast "$ROOT/tools/build.sh" web \
        || { echo "mcp-tests: the mesh-raycast web build (WEB_PROJECT=mesh-raycast tools/build.sh web) failed" >&2; exit 1; }
fi
# Rebuilding an unchanged image is a no-op thanks to the layer cache. The image is tagged with
# a hash of its Dockerfile and of what that COPYs or ADDs from docker/ (jasperengine/<target>-build:
# <12 hex>), and :latest names the newest build (for people): worktrees with different Dockerfiles
# each keep theirs, instead of retagging one name back and forth and leaving the other image
# untagged, for a prune to delete under a job that still runs it. The containers run it by id.
image_hash() {
    local file="$ROOT/docker/$1.Dockerfile"
    {
        cat "$file"
        # the build-context sources of COPY and ADD (not a --from stage's)
        awk 'toupper($1) == "COPY" || toupper($1) == "ADD" {
                 from = 0; for (i = 2; i <= NF; i++) if ($i ~ /^--from/) from = 1
                 if (!from) for (i = 2; i < NF; i++) if ($i !~ /^--/) print $i
             }' "$file" | while read -r source; do
            (cd "$ROOT/docker" && find "./$source" -type f -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum)
        done
    } | sha256sum | cut -c1-12
}
# Already inside the Linux build image (rb.sh --img, tools/check-changed there): no Docker; the
# targets of that image run here directly, with the same environment (IN_IMAGE=1)
IN_IMAGE=0
if [[ -f /.dockerenv ]] && ! command -v docker >/dev/null 2>&1; then
    IN_IMAGE=1
    [[ "$IMAGE_TARGET" == linux ]] || { echo "tools/build.sh $TARGET needs the $IMAGE_TARGET image: run it on the host (this is the linux image)" >&2; exit 3; }
fi
if [[ $IN_IMAGE == 1 ]]; then
    IMAGE_ID=""
elif [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE_ID="$JASPER_DOCKER_IMAGE"
else
    IMAGE_ID="$(docker build -q -t "$IMAGE:$(image_hash "$IMAGE_TARGET")" -t "$IMAGE:latest" \
        -f "$ROOT/docker/$IMAGE_TARGET.Dockerfile" "$ROOT/docker")"
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
        linux|unit-tests|htmlui-tests|mcp-tests) ;;
        *) echo "SANITIZE is for linux, unit-tests, htmlui-tests and mcp-tests" >&2; exit 1 ;;
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
# COVERAGE=1 (linux): GCC's --coverage, next to the default tree (build/linux/editor-coverage): the
# MCP suite's coverage map (tools/mcp/coverage_map.py) is recorded with it
COVERAGE_SUFFIX=""
if [[ "${COVERAGE:-0}" == 1 ]]; then
    [[ "$TARGET" == linux ]] || { echo "COVERAGE=1 is for linux" >&2; exit 1; }
    COVERAGE_SUFFIX="-coverage"
fi
WERROR_VALUE=OFF
[[ "${WERROR:-0}" == 1 ]] && WERROR_VALUE=ON
# what every CMake configure of a build gets on top of its preset
OPTION_FLAGS="-DJASPERENGINE_SANITIZE='$SANITIZE_LIST'"
[[ "${UNITY:-0}" == 1 ]] && OPTION_FLAGS="$OPTION_FLAGS -DJASPERENGINE_UNITY_BUILD=ON" || OPTION_FLAGS="$OPTION_FLAGS -DJASPERENGINE_UNITY_BUILD=OFF"
# (WERROR needs WARNINGS, which the standalone engine tree has off by default)
ENGINE_WARNING_FLAGS="-DJASPERENGINE_WERROR=$WERROR_VALUE"
[[ "$WERROR_VALUE" == ON ]] && ENGINE_WARNING_FLAGS="$ENGINE_WARNING_FLAGS -DJASPERENGINE_WARNINGS=ON"
VARIANT="$SANITIZE_SUFFIX$UNITY_SUFFIX$COVERAGE_SUFFIX"
[[ -n "$COVERAGE_SUFFIX" ]] && OPTION_FLAGS="$OPTION_FLAGS -DCMAKE_C_FLAGS=--coverage -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage"

# What --clean removes: the whole output of the target, or only this test build
CLEAN_DIR="$OUT"
[[ "$TARGET" == htmlui-tests ]] && CLEAN_DIR="$OUT/htmlui-tests-$HTMLUI_BACKEND$VARIANT"
[[ "$TARGET" == unit-tests ]] && CLEAN_DIR="$OUT/core-tests*$VARIANT $OUT/editor-tests$VARIANT"
[[ "$TARGET" == mcp-tests || "$TARGET" == web-gate ]] && CLEAN_DIR="$RESULTS"

if [[ $CLEAN -eq 1 ]]; then
    # Files inside may belong to the container user; remove them from inside.
    if [[ $IN_IMAGE == 1 ]]; then (cd "$ROOT" && rm -rf $CLEAN_DIR)
    else docker run --rm -v "$ROOT:/src" -w /src "$IMAGE_ID" bash -c "rm -rf $CLEAN_DIR"; fi
fi
mkdir -p "$ROOT/$OUT" "$ROOT/$RESULTS"
[[ $IN_IMAGE == 1 ]] || mkdir -p "$ROOT/$CACHE/home" "$MAIN/$CACHE/ccache"

TTY_FLAGS=()
[[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
CPU_FLAGS=()
CPUS="${CPUS:-${DOCKER_CPUS:-${JOBS:-}}}"
[[ -n "$CPUS" ]] && CPU_FLAGS=(--cpus "$CPUS")
ENV_FLAGS=()
# DOCKER_GPU=1 (opt-in, a host with the NVIDIA container runtime): the host GPU in the container, and
# tools/with-xvfb.sh (unit tests, htmlui tests) sources tools/gpu/env.sh: the NVIDIA Vulkan/EGL
# loader manifests and, while tools/gpu/xorg-headless.sh runs, its headless NVIDIA X server instead
# of a Mesa Xvfb (docs/perf/gpu-campaign.md). Goldens stay the llvmpipe/lavapipe CI reference.
if [[ "${DOCKER_GPU:-0}" == 1 ]]; then
    CPU_FLAGS+=(--gpus all -e NVIDIA_DRIVER_CAPABILITIES=all)
    ENV_FLAGS+=(-e DOCKER_GPU=1)
    [[ -n "${JE_GPU_DISPLAY:-}" ]] && ENV_FLAGS+=(-e "JE_GPU_DISPLAY=$JE_GPU_DISPLAY")
fi
for name in JASPER_HTMLUI_UPDATE_GOLDEN JASPER_HTMLUI_WRITE_ACTUAL JASPER_TEST_UPDATE_GOLDEN JASPER_TEST_WRITE_ACTUAL \
            JASPER_TEST_KEEP_TEMP JASPER_REFLECT_WRITE_FIXTURE JASPER_BENCH_ENTITIES JASPER_HTMLUI_PERF_SLACK JASPER_TEST_PERF_SLACK \
            HTMLUI_LIVE_NETWORK JASPER_CHROME JASPER_ZERO_COST_UPDATE JASPER_PICK_SIMD \
            JASPER_HTMLUI_S16_OFF JASPER_HTMLUI_BATCHING JASPER_HTMLUI_SDF_PRIMITIVES JASPERENGINE_GPU_TIMER JASPER_HTMLUI_PROFILE \
            JASPER_TEST_UPDATE_FIXTURES JASPER_SLOW_BUILD JASPER_SLOW_TESTS JASPER_BUILD_TIMEOUT JASPER_MCP_PERF_SLACK; do
    # (JASPER_HTMLUI_PERF_SLACK: the HTML UI performance tests' tolerance on a loaded host, compat_perf_test.cpp;
    # JASPER_TEST_PERF_SLACK: the core tests' wall-clock budgets are enforced only when it is set;
    # JASPER_PICK_SIMD: the picking BVH's slab test path, scalar|avx2|neon, S26 AC2;
    # JASPER_TEST_UPDATE_FIXTURES: the editor's TextureFormatFixtures writes the GPU format test's cooks, S14;
    # JASPER_SLOW_BUILD, JASPER_SLOW_TESTS, JASPER_BUILD_TIMEOUT, JASPER_MCP_PERF_SLACK: the MCP suite's opt-ins)
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
# ccache with precompiled headers (JASPERENGINE_PCH, docs/testing.md "Build speed"): it can only
# cache a unit that uses one when told not to look for #defines and time macros it cannot see
# through the header; no unit of a Linux build uses __DATE__ or __TIME__ (the web image's build
# id does, and is not given this)
[[ "$IMAGE_TARGET" == linux ]] && ENV_FLAGS+=(-e "CCACHE_SLOPPINESS=pch_defines,time_macros")
MOUNT_FLAGS=()
# mcp-tests: the fixture project the default-fixture tests copy (the places
# tools/mcp/harness.py default_fixture() looks in), mounted where the harness finds it
if [[ "$TARGET" == mcp-tests && -z "${JASPER_FIXTURE:-}" ]]; then
    for candidate in "$ROOT/build/fixture-unity" "$MAIN/build/fixture-unity" "$HOME/je/fixture-unity"; do
        if [[ -f "$candidate/project.yaml" ]]; then JASPER_FIXTURE="$candidate"; break; fi
    done
fi
if [[ "$TARGET" == mcp-tests && -n "${JASPER_FIXTURE:-}" ]]; then
    MOUNT_FLAGS+=(-v "$JASPER_FIXTURE:/fixture:ro" -e JASPER_FIXTURE=/fixture)
fi
# the UI effects' measuring switches (EffectPerfTest: JASPER_EFFECT_COST_ONLY, JASPER_EFFECT_PERF_*;
# EffectFluidTest: JASPER_EFFECT_FLUID_PROFILE)
for name in $(compgen -e | grep '^JASPER_EFFECT_' || true); do ENV_FLAGS+=(-e "$name=${!name}"); done
# the MCP suite's switches (JASPER_MCP_CALL_LOG, JASPER_MCP_BUILD_JOBS, ...: tools/mcp/harness.py,
# tools/mcp/build_cache.py) and ccache's own (CCACHE_STATSLOG, CCACHE_LOGFILE, ...; not CCACHE_DIR,
# which names the shared cache's mount below)
for name in $(compgen -e | grep -E '^(JASPER_MCP_|CCACHE_)' || true); do
    [[ "$name" == CCACHE_DIR || "$name" == JASPER_MCP_SHARED_BUILD_CACHE ]] || ENV_FLAGS+=(-e "$name=${!name}")
done
# JASPER_MCP_SHARED_BUILD_CACHE=<host dir> (mcp-tests): the machine's shared cache of the MCP suite's
# prebuilt player runtimes (tools/mcp/build_cache.py), mounted at /mcp-build-cache: every worktree
# and run of the machine reuses a runtime of its content key instead of building its own (the
# build servers' rb.sh sets ~/je/mcp-build-cache)
if [[ "$TARGET" == mcp-tests && -n "${JASPER_MCP_SHARED_BUILD_CACHE:-}" ]]; then
    mkdir -p "$JASPER_MCP_SHARED_BUILD_CACHE"
    MOUNT_FLAGS+=(-v "$(cd "$JASPER_MCP_SHARED_BUILD_CACHE" && pwd):/mcp-build-cache" -e JASPER_MCP_SHARED_BUILD_CACHE=/mcp-build-cache)
fi
# A non-root uid has no entry in the image's /etc/passwd; dbus (dbus-run-session, used by the MCP
# tests and tool docs) and getpwuid() callers then fail ("User ??? unknown"). Lend the host's tables
# (whose home directory the container lacks: the Android Gradle plugin, which would take Java's
# user.home from them, gets ANDROID_USER_HOME below).
if [[ "$(uname -s)" == Linux && "$(id -u)" != 0 && -r /etc/passwd && -r /etc/group ]]; then
    MOUNT_FLAGS+=(-v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro)
fi

run() {
    if [[ $IN_IMAGE == 1 ]]; then
        # the container's own mounts and HOME/CCACHE_DIR (jimg's); the run's -e variables as docker would set them
        local assignments=() i=0
        while [[ $i -lt ${#ENV_FLAGS[@]} ]]; do
            [[ "${ENV_FLAGS[$i]}" == -e ]] && assignments+=("${ENV_FLAGS[$((i + 1))]}") && i=$((i + 2)) && continue
            i=$((i + 1))
        done
        (cd "$ROOT" && env ${assignments[@]+"${assignments[@]}"} "$@")
        return $?
    fi
    # (the ${a[@]+...} form: an empty array under set -u fails on bash before 4.4, e.g. macOS)
    docker run --rm ${TTY_FLAGS[@]+"${TTY_FLAGS[@]}"} ${CPU_FLAGS[@]+"${CPU_FLAGS[@]}"} \
        --user "$(id -u):$(id -g)" \
        -v "$ROOT:/src" -w /src \
        -v "$MAIN/.git:$MAIN/.git:ro" \
        -v "$MAIN/$CACHE/ccache:/ccache" \
        -e HOME="/src/$CACHE/home" \
        -e CCACHE_DIR=/ccache \
        -e GRADLE_USER_HOME="/src/$CACHE/gradle" \
        -e ANDROID_USER_HOME="/src/$CACHE/home/.android" \
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
# the standalone engine tree's development and test builds (linux's sample project, unit-tests,
# htmlui-tests) with precompiled headers; exports, web, Android and players keep it off
DEV_ENGINE_FLAGS="-DJASPERENGINE_PCH=ON"
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
    # (a sanitizer's reports need the symbols)
    INSTALL_STRIP="--strip"
    [[ -n "$SANITIZE_LIST" ]] && INSTALL_STRIP=""
    run bash -euo pipefail -c "
        python3 tools/check-cxx-subset.py
        python3 tools/check-pollers.py
        cmake --preset $PRESET -B $OUT/$EDITOR_DIR -DCMAKE_BUILD_TYPE=$BT $OPTION_FLAGS \
            -DJASPERENGINE_WERROR=$WERROR_VALUE \
            -DCMAKE_INSTALL_PREFIX=$OUT/$EDITOR_DIR-install $CCACHE_FLAGS
        cmake --build $OUT/$EDITOR_DIR --target $EDITOR_TARGET $JOBS_FLAG
        cmake --install $OUT/$EDITOR_DIR $INSTALL_STRIP
        # (a sanitizer build ships nothing, and at -O0 exports the std templates it instantiates on these types)
        if [[ -z '$SANITIZE_LIST' ]]; then
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
        fi
        [[ '${SKIP_ENGINE:-0}' == 1 ]] && exit 0
        cmake --preset engine-linux-release -S engine -B $OUT/$ENGINE_DIR -DCMAKE_BUILD_TYPE=$BT $OPTION_FLAGS \
            $ENGINE_WARNING_FLAGS $DEV_ENGINE_FLAGS $CCACHE_FLAGS
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
    PROJECT_FLAGS=""
    if [[ -n "${WEB_PROJECT:-}" ]]; then
        [[ "$WEB_PROJECT" =~ ^[A-Za-z0-9_-]+$ && -d "$ROOT/tools/test-projects/$WEB_PROJECT" ]] \
            || { echo "WEB_PROJECT must name a directory of tools/test-projects (got $WEB_PROJECT)" >&2; exit 1; }
        # WEB_THREADS=1: build/web-threads-<name>
        WEB_DIR="$WEB_DIR-$WEB_PROJECT"
        PROJECT_FLAGS="-DPROJECT_ROOT=/src/tools/test-projects/$WEB_PROJECT"
    fi
    [[ -n "$CXX_SUFFIX" ]] && WEB_DIR="$WEB_DIR/cxx$CXX_STANDARD"
    SIMD_FLAGS=""
    if [[ "${WASM_SIMD:-1}" == 0 ]]; then
        WEB_DIR="$WEB_DIR-scalar"
        SIMD_FLAGS="-DJASPERENGINE_WASM_SIMD=OFF"
    fi
    run bash -euo pipefail -c "
        cmake --preset $PRESET -S engine -B $WEB_DIR -DCMAKE_BUILD_TYPE=$BT $PROJECT_FLAGS $OPTION_FLAGS $SIMD_FLAGS $CCACHE_FLAGS
        cmake --build $WEB_DIR --target $ENGINE_TARGET $JOBS_FLAG
    "
    ;;
players)
    PLAYER_PLATFORM="${PLAYER_PLATFORM:-linux}"
    PLAYER_FLAVOURS="${PLAYER_FLAVOURS:-release development}"
    PLAYERS_OUT=build/players
    ZIP_FLAG=""
    [[ "${PLAYER_ZIP:-0}" == 1 ]] && ZIP_FLAG="--zip $PLAYERS_OUT/archives"
    for flavour in $PLAYER_FLAVOURS; do
        [[ "$flavour" =~ ^(release|development)$ ]] || { echo "PLAYER_FLAVOURS lists release and development (got $flavour)" >&2; exit 1; }
    done
    # the release variants (docs/specs/S34-player-size.md R4): name:suffix pairs
    ALL_VARIANTS="$(python3 -c "import json; print(' '.join(v['name'] + ':' + v['suffix'] for v in json.load(open('$ROOT/engine/player/variants.json'))['variants']))")"
    RELEASE_VARIANTS=""
    for pair in $ALL_VARIANTS; do
        if [[ -z "${PLAYER_VARIANTS:-}" || " $PLAYER_VARIANTS " == *" ${pair%%:*} "* ]]; then RELEASE_VARIANTS="$RELEASE_VARIANTS $pair"; fi
    done
    for name in ${PLAYER_VARIANTS:-}; do
        [[ " $ALL_VARIANTS" == *" $name:"* ]] || { echo "PLAYER_VARIANTS: $name is not in engine/player/variants.json ($ALL_VARIANTS)" >&2; exit 1; }
    done
    case "$PLAYER_PLATFORM" in
    linux)
        PLAYER_BACKENDS="${PLAYER_BACKENDS:-glcore gles3 vulkan}"
        for backend in $PLAYER_BACKENDS; do
            [[ "$backend" =~ ^(glcore|gles3|vulkan)$ ]] || { echo "PLAYER_BACKENDS lists glcore, gles3 and vulkan (got $backend)" >&2; exit 1; }
        done
        # (one runtime's build and packaging: tools/players/build_linux_player.sh, which the MCP
        # suite's shared build cache runs too)
        BUILD_PLAYER_FLAGS=""
        [[ -n "${JOBS:-}" ]] && BUILD_PLAYER_FLAGS="--jobs $JOBS"
        [[ "${KEEP_GOING:-0}" == 1 ]] && BUILD_PLAYER_FLAGS="$BUILD_PLAYER_FLAGS --keep-going"
        run bash -euo pipefail -c "
            for flavour in $PLAYER_FLAVOURS; do
                variants='$RELEASE_VARIANTS'; [[ \$flavour == development ]] && variants='full:'
                for pair in \$variants; do
                    variant=\${pair%%:*} suffix=\${pair#*:}
                    for backend in $PLAYER_BACKENDS; do
                        tools/players/build_linux_player.sh $BUILD_PLAYER_FLAGS $ZIP_FLAG --variant \$variant \$backend \$flavour \
                            build/players-build/linux-x64-\$backend-\$flavour\${suffix:+-\$suffix} $PLAYERS_OUT -- $OPTION_FLAGS $ENGINE_WARNING_FLAGS $CCACHE_FLAGS
                    done
                done
            done
        "
        ;;
    web)
        run bash -euo pipefail -c "
            for flavour in $PLAYER_FLAVOURS; do
                variants='$RELEASE_VARIANTS'; [[ \$flavour == development ]] && variants='full:'
                for pair in \$variants; do
                    variant=\${pair%%:*} suffix=\${pair#*:}
                    id=web-wasm32-gles3-\$flavour\${suffix:+-\$suffix}
                    dev=OFF; bt=Release; [[ \$flavour == development ]] && { dev=ON; }
                    for threads in OFF ON; do
                        tree=build/players-build/\$id-threads\$threads
                        cmake --preset engine-web-release -S engine -B \$tree -DCMAKE_BUILD_TYPE=\$bt -DJASPERENGINE_PLAYER=ON \
                            -DGRAPHIC_BACKEND=gles3 -DEMSCRIPTEN_THREAD_SUPPORT=\$threads -DJASPERENGINE_DEVELOPMENT_BUILD=\$dev \
                            -DJASPERENGINE_PLAYER_VARIANT=\$variant $OPTION_FLAGS $CCACHE_FLAGS
                        cmake --build \$tree --target jasperengine-player $JOBS_FLAG
                    done
                    python3 tools/players/package_player.py --platform web --arch wasm32 --backend gles3 --flavour \$flavour \
                        --variant \$variant --build build/players-build/\$id-threadsOFF --build-mt build/players-build/\$id-threadsON \
                        --out $PLAYERS_OUT $ZIP_FLAG
                done
            done
        "
        ;;
    android)
        PLAYER_BACKENDS="${PLAYER_BACKENDS:-gles3}"
        for backend in $PLAYER_BACKENDS; do
            [[ "$backend" == gles3 ]] || { echo "PLAYER_BACKENDS is gles3 for android: its platform layer makes an EGL context only (got $backend)" >&2; exit 1; }
        done
        run bash -euo pipefail -c "
            for flavour in $PLAYER_FLAVOURS; do
                variants='$RELEASE_VARIANTS'; [[ \$flavour == development ]] && variants='full:'
                for pair in \$variants; do
                    variant=\${pair%%:*} suffix=\${pair#*:}
                    for backend in $PLAYER_BACKENDS; do
                        dir=build/players-build/android-\$backend-\$flavour\${suffix:+-\$suffix}
                        CMAKE_CXX_COMPILER_LAUNCHER=ccache JASPERENGINE_ANDROID_ABIS=${PLAYER_ANDROID_ABIS:-} \
                            tools/players/android_player.sh \$flavour \$dir \$backend \$variant
                        python3 tools/players/package_player.py --platform android --arch universal --backend \$backend --flavour \$flavour \
                            --variant \$variant --android-dir \$dir --symbols \$dir/symbols --out $PLAYERS_OUT $ZIP_FLAG
                    done
                done
            done
        "
        ;;
    *) echo "PLAYER_PLATFORM is linux, web or android here (Windows, macOS and iOS runtimes have no build target)" >&2; exit 1 ;;
    esac
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
                $OPTION_FLAGS -DJASPERENGINE_WARNINGS=ON -DJASPERENGINE_WERROR=$WERROR_VALUE $DEV_ENGINE_FLAGS $CCACHE_FLAGS
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
            env $VK_ENV tools/with-xvfb.sh $TEST_WRAP \
                $CORE_DIR/tests/core/jasperengine-core-tests --gtest_output=xml:$RESULTS/$CORE_RESULT.xml ${UNIT_TEST_ARGS:-} || status=1
            $GTEST_GATE --xml $RESULTS/$CORE_RESULT.xml --min-tests $CORE_MIN --max-skipped 0 || status=1
        fi
        if [[ $EDITOR == 1 ]]; then
            rm -f $RESULTS/$EDITOR_RESULT.xml
            tools/with-xvfb.sh $TEST_WRAP \
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
            -DJASPERENGINE_HTMLUI_TESTS=ON -DGRAPHIC_BACKEND=$HTMLUI_BACKEND $OPTION_FLAGS $ENGINE_WARNING_FLAGS $DEV_ENGINE_FLAGS $CCACHE_FLAGS
        cmake --build $TESTS_DIR --target jasperengine-htmlui-tests $JOBS_FLAG
        [[ '${RUN:-1}' == 0 ]] && exit 0
        set -f  # a --gtest_filter pattern is not a glob
        rm -f $RESULTS/$RESULT.xml
        status=0
        env $VK_ENV tools/with-xvfb.sh $TEST_WRAP \
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
        tools/with-xvfb.sh $OUT/editor/jasperengine-htmlui-editor-tests \
            --gtest_output=xml:$RESULTS/htmlui-editor.xml ${HTMLUI_TEST_ARGS:-} || status=1
        # the live Google Fonts test skips without HTMLUI_LIVE_NETWORK=1
        $GTEST_GATE --xml $RESULTS/htmlui-editor.xml --max-skipped 1 || status=1
        exit \$status
    "
    ;;
mcp-tests)
    SUITE="${MCP_SUITE:-ci}"
    [[ "$SUITE" == ci || "$SUITE" == all ]] || { echo "MCP_SUITE must be ci or all" >&2; exit 1; }
    [[ "$SUITE" == all && -z "${JASPER_FIXTURE:-}" ]] && { echo "MCP_SUITE=all needs the fixture project: JASPER_FIXTURE, build/fixture-unity or ~/je/fixture-unity" >&2; exit 1; }
    # (SANITIZE: the install of that build, `SANITIZE=... tools/build.sh linux`; TSan's start needs its wrapper)
    EDITOR_INSTALL="build/linux/editor$SANITIZE_SUFFIX-install"
    [[ -x "$ROOT/$EDITOR_INSTALL/bin/jasperengine-editor" ]] || { echo "No editor at $EDITOR_INSTALL; run ${SANITIZE:+SANITIZE=$SANITIZE }tools/build.sh linux" >&2; exit 1; }
    [[ -n "$SANITIZE_SUFFIX" ]] && ENV_FLAGS+=(-e "JASPER_EDITOR=${TEST_WRAP:+$TEST_WRAP }/src/$EDITOR_INSTALL/bin/jasperengine-editor")
    # (no leak check here: an editor's exit reports what the GL driver it unloaded kept, frames of an unmapped
    # module that no suppression can name; the unit tests hold the leak check)
    [[ -n "$SANITIZE_SUFFIX" ]] && ENV_FLAGS+=(-e "ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1")
    SUITE_FLAGS="--suite $SUITE"
    [[ "$SUITE" == all ]] && SUITE_FLAGS=""
    # (each argument quoted once more: the command runs through bash -c)
    ARGS="$(printf '%q ' ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}) ${MCP_TEST_ARGS:-}"
    # DOCKER_GPU=1: the real-GPU lane (docs/testing.md): the chunks a plan puts on the lane "gpu" run
    # on the host's NVIDIA GPU through tools/gpu/env.sh's headless X server(s) (JE_GPU_DISPLAY,
    # JASPER_MCP_GPU_DISPLAYS); every other test of the run on llvmpipe, as everywhere
    GPU_LANE=""
    [[ "${DOCKER_GPU:-0}" == 1 ]] && GPU_LANE=". tools/gpu/env.sh && export JASPER_MCP_GPU_LANE=1 &&"
    run bash -euo pipefail -c "
        rm -f $RESULTS/mcp.json
        $GPU_LANE dbus-run-session -- tools/mcp/run-tests.sh $SUITE_FLAGS --json $RESULTS/mcp.json $ARGS
    "
    ;;
web-gate)
    NAME="${GATE:-sample-project}"
    case "$NAME" in
        sample-project) WEB_DIR="$OUT" ;;
        sample-project-threads) WEB_DIR="$OUT-threads" ;;
        *)
            [[ "$NAME" =~ ^[A-Za-z0-9_-]+$ && -f "$ROOT/tools/web/gates/$NAME.json" ]] \
                || { echo "GATE must be sample-project, sample-project-threads or a tools/web/gates/<name>.json" >&2; exit 1; }
            WEB_DIR="$OUT-$NAME"
            # a test project's pthreads build (WEB_THREADS=1 WEB_PROJECT=<name> tools/build.sh web)
            [[ "${WEB_THREADS:-0}" == 1 ]] && WEB_DIR="$OUT-threads-$NAME" ;;
    esac
    SHOT_NAME="$NAME"
    if [[ "${WASM_SIMD:-1}" == 0 ]]; then
        WEB_DIR="$WEB_DIR-scalar"
        SHOT_NAME="$NAME-scalar"
    fi
    [[ -f "$ROOT/$WEB_DIR/$ENGINE_TARGET.html" ]] || { echo "No web build at $WEB_DIR; run tools/build.sh web" >&2; exit 1; }
    run bash -euo pipefail -c "
        mkdir -p $RESULTS/web
        python3 tools/web/browser_gate.py --root $WEB_DIR --page $ENGINE_TARGET.html --until-ready \
            --baseline tools/web/gates/$NAME.json --screenshot $RESULTS/web/$SHOT_NAME.png \
            --transcript $RESULTS/web/$SHOT_NAME.log ${GATE_ARGS:-}
    "
    ;;
android)
    # Gradle writes its intermediates next to the project, so the output
    # directory is only used for the copied APK.
    APK_DIR="$OUT"
    PROJECT_ENV=""
    if [[ -n "${ANDROID_PROJECT:-}" ]]; then
        [[ "$ANDROID_PROJECT" =~ ^[A-Za-z0-9_-]+$ && -d "$ROOT/tools/test-projects/$ANDROID_PROJECT" ]] \
            || { echo "ANDROID_PROJECT must name a directory of tools/test-projects (got $ANDROID_PROJECT)" >&2; exit 1; }
        APK_DIR="$OUT-$ANDROID_PROJECT"
        # app/build.gradle builds this project instead of engine/project
        PROJECT_ENV="JASPERENGINE_PROJECT_ROOT=/src/tools/test-projects/$ANDROID_PROJECT"
    fi
    if [[ -n "${ANDROID_ABIS:-}" ]]; then
        [[ "$ANDROID_ABIS" =~ ^(arm64-v8a|armeabi-v7a|x86|x86_64)(,(arm64-v8a|armeabi-v7a|x86|x86_64))*$ ]] \
            || { echo "ANDROID_ABIS must list arm64-v8a, armeabi-v7a, x86, x86_64 (got $ANDROID_ABIS)" >&2; exit 1; }
        # app/build.gradle builds only these
        PROJECT_ENV="$PROJECT_ENV JASPERENGINE_ANDROID_ABIS=$ANDROID_ABIS"
    fi
    mkdir -p "$ROOT/$APK_DIR"
    # Gradle's output also goes to gradle.log: a deprecation in our Gradle files fails the build
    # (tools/ci/check_gradle_warnings.py; the template sets --warning-mode all)
    run bash -euo pipefail -c "
        cd engine/workspaces/androidstudio
        env $PROJECT_ENV CMAKE_CXX_COMPILER_LAUNCHER=ccache ./gradlew --no-daemon assembleDebug 2>&1 | tee /src/$APK_DIR/gradle.log
        cp app/build/outputs/apk/debug/*.apk /src/$APK_DIR/
        python3 /src/tools/ci/check_gradle_warnings.py /src/$APK_DIR/gradle.log
    "
    ls -l "$ROOT/$APK_DIR"/*.apk
    ;;
esac
