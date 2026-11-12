#!/usr/bin/env bash
# The Windows lane's steps (docs/superpowers/specs/2026-11-12-ci-design.md §12, contract C5), with
# MSVC and Ninja. Runs under Git Bash inside an MSVC developer environment (ilammy/msvc-dev-cmd,
# arch x64, or a Developer Command Prompt's environment), from any directory: it works in the
# checkout it belongs to.
#
#   tools/ci/windows.sh editor                     the Release editor (preset windows-msvc-release) with
#                                                  jasperengine-editor-cmd and jasperengine-editor-tests,
#                                                  installed into instdir/ (EDITOR_TESTS=0: no tests; a
#                                                  release editor, which package needs)
#   tools/ci/windows.sh engine [d3d11|glcore|vulkan]  the engine sample project and jasperengine-core-tests
#                                                  of that backend (default glcore, engine/CMakeLists.txt's
#                                                  for Windows; vulkan needs the Vulkan SDK, VULKAN_SDK)
#   tools/ci/windows.sh tests [backend]            the CPU unit tests (-*Gpu*: no device here) of the editor
#                                                  tree and that engine tree, through tools/ci/gtest-gate.py;
#                                                  XML in build/test-results/{core,editor}-windows.xml
#   tools/ci/windows.sh compile-smoke <project-dir> [backend]  a Compile build of the project with MSVC,
#                                                  as Build with Method Compile makes it (tools/ci/compile-smoke.sh)
#   tools/ci/windows.sh package <version>          dist/JasperEngine-<version>-windows-x64.zip of instdir/bin
#   tools/ci/windows.sh check-cli [zip]            tools/ci/check-call-cli.py on jasperengine-editor-cmd.exe of
#                                                  instdir/, or of that zip unpacked
#
# Trees: build/windows/editor, build/windows/engine-<backend>. Logs: build/test-results/windows-<step>.log
# (windows-engine-<backend>.log). JASPERENGINE_VERSION, JOBS, WERROR=1 and ccache on PATH as in
# tools/ci/platform-common.sh. Exit 2 on bad arguments, 1 when a step fails.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=platform-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/platform-common.sh"

usage() { ci_usage "$(ci_header "${BASH_SOURCE[0]}")"; }

EDITOR_TREE=build/windows/editor
INSTALL="$ROOT/instdir"

need_msvc() {
    command -v cl >/dev/null 2>&1 || { echo "cl is not on PATH: run inside an MSVC developer environment (ilammy/msvc-dev-cmd, arch x64)" >&2; exit 1; }
    command -v ninja >/dev/null 2>&1 || { echo "ninja is not on PATH (Visual Studio's CMake tools or choco install ninja)" >&2; exit 1; }
}

check_backend() {
    [[ "$1" =~ ^(d3d11|glcore|vulkan)$ ]] || ci_usage "backend: d3d11, glcore or vulkan (got $1)"
}

step_editor() {
    need_msvc
    local targets=(jasperengine-editor jasperengine-editor-cmd) tests=ON
    if [[ "${EDITOR_TESTS:-1}" == 0 ]]; then tests=OFF; else targets+=(jasperengine-editor-tests); fi
    cmake --preset windows-msvc-release -B "$EDITOR_TREE" -DCMAKE_BUILD_TYPE=Release \
        -DJASPERENGINE_TESTS:BOOL="$tests" -DJASPERENGINE_WERROR="$WERROR_VALUE" \
        "-DCMAKE_INSTALL_PREFIX:PATH=$(ci_native "$INSTALL")" \
        ${EDITOR_VERSION_ARGS[@]+"${EDITOR_VERSION_ARGS[@]}"} ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
    cmake --build "$EDITOR_TREE" --target "${targets[@]}" ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
    # (jasperengine-editor-cmd is EXCLUDE_FROM_ALL: installed because it was built above)
    rm -rf "$INSTALL"
    cmake --install "$EDITOR_TREE" --strip
    "$PY" tools/ci/check-notices.py --installed "$(ci_native "$INSTALL/bin")"
    [[ -f "$INSTALL/bin/jasperengine-editor-cmd.exe" ]] || { echo "no jasperengine-editor-cmd.exe in $INSTALL/bin" >&2; exit 1; }
    ccache --show-stats 2>/dev/null | grep -E 'Hits|Misses' || true
}

step_engine() {
    local backend="$1"
    need_msvc
    # Release, not the preset's RelWithDebInfo: MSVC's /Zi debug information goes to PDBs, which
    # ccache cannot cache
    cmake --preset engine-windows-msvc-tests -S engine -B "build/windows/engine-$backend" -DCMAKE_BUILD_TYPE=Release \
        -DGRAPHIC_BACKEND="$backend" -DJASPERENGINE_WERROR="$WERROR_VALUE" ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
    cmake --build "build/windows/engine-$backend" --target jasperengine-project jasperengine-core-tests ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
}

min_tests() { sed -n "s/^$1=//p" tools/build.sh; }

step_tests() {
    local backend="$1" status=0
    local core="build/windows/engine-$backend/tests/core/jasperengine-core-tests.exe"
    local editor="$EDITOR_TREE/jasperengine-editor-tests.exe"
    [[ -f "$core" ]] || { echo "no $core: run tools/ci/windows.sh engine $backend first" >&2; exit 1; }
    [[ -f "$editor" ]] || { echo "no $editor: run tools/ci/windows.sh editor (without EDITOR_TESTS=0) first" >&2; exit 1; }
    mkdir -p "$RESULTS"
    rm -f "$RESULTS/core-windows.xml" "$RESULTS/editor-windows.xml"
    # the GPU suites need a device (Linux runs them on llvmpipe/lavapipe, docs/testing.md); the
    # minimum counts are tools/build.sh's (AC7 of S02), the skip limit its 0
    "$core" --gtest_filter=-*Gpu* --gtest_output=xml:"$RESULTS/core-windows.xml" || status=1
    "$editor" --gtest_filter=-*Gpu* --gtest_output=xml:"$RESULTS/editor-windows.xml" || status=1
    "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/core-windows.xml" --min-tests "$(min_tests CORE_MIN_TESTS)" --max-skipped 0 || status=1
    "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/editor-windows.xml" --min-tests "$(min_tests EDITOR_MIN_TESTS)" --max-skipped 0 || status=1
    return "$status"
}

step_package() {
    local version="$1" cache="$EDITOR_TREE/CMakeCache.txt"
    [[ -f "$INSTALL/bin/jasperengine-editor.exe" && -f "$cache" ]] || { echo "no editor in $INSTALL: run tools/ci/windows.sh editor first" >&2; exit 1; }
    # a release ships what its name says, and without the unit tests' seams in the engine (JASPERENGINE_TESTS
    # compiles test hooks into jasperengine.dll: engine/CMakeLists.txt)
    cache_has "$cache" 'JASPERENGINE_TESTS:BOOL=OFF' || { echo "the editor was built with its tests: build it with EDITOR_TESTS=0 to package it" >&2; exit 1; }
    cache_has "$cache" "JASPERENGINE_EDITOR_VERSION:STRING=$version" \
        || { echo "the editor is not stamped $version ($(grep '^JASPERENGINE_EDITOR_VERSION' "$cache" || echo 'git describe')): build it with JASPERENGINE_VERSION=$version" >&2; exit 1; }
    local zip="dist/JasperEngine-$version-windows-x64.zip"
    rm -f "$zip"
    ci_zip_dir "$INSTALL/bin" "$zip"
}

step_check_cli() {
    local where="$INSTALL"
    if [[ -n "${1:-}" ]]; then
        [[ -f "$1" ]] || ci_usage "no zip at $1"
        where="$ROOT/build/windows/check-cli"
        rm -rf "$where" && mkdir -p "$where"
        "$PY" -I -m zipfile -e "$(ci_native "$1")" "$(ci_native "$where")"
    fi
    "$PY" tools/ci/check-call-cli.py "$(ci_native "$where")"
}

[[ $# -ge 1 ]] || usage
STEP="$1"
shift
case "$STEP" in
    editor) [[ $# == 0 ]] || usage; ci_logged windows-editor step_editor ;;
    engine)
        [[ $# -le 1 ]] || usage
        backend="${1:-glcore}"; check_backend "$backend"
        ci_logged "windows-engine-$backend" step_engine "$backend" ;;
    tests)
        [[ $# -le 1 ]] || usage
        backend="${1:-glcore}"; check_backend "$backend"
        ci_logged windows-tests step_tests "$backend" ;;
    compile-smoke)
        [[ $# -ge 1 && $# -le 2 ]] || usage
        need_msvc
        tools/ci/compile-smoke.sh "$INSTALL/bin/jasperengine-editor-cmd.exe" windows "$@" ;;
    package) [[ $# == 1 ]] || usage; ci_logged windows-package step_package "$1" ;;
    check-cli) [[ $# -le 1 ]] || usage; ci_logged windows-check-cli step_check_cli "${1:-}" ;;
    *) usage ;;
esac
