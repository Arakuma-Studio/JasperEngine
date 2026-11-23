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
#                                                  for Windows; vulkan needs the Vulkan SDK, VULKAN_SDK);
#                                                  ENGINE_DEBUG_INFO=1 with debug information (/Z7, PDBs)
#   tools/ci/windows.sh tests [backend]            the CPU unit tests (-*Gpu*: no device here) of the editor
#                                                  tree and that engine tree, through tools/ci/gtest-gate.py;
#                                                  XML in build/test-results/{core,editor}-windows.xml
#                                                  (TEST_SUITES=core or editor: that suite alone, as the
#                                                  lane's engine-glcore and editor jobs run them;
#                                                  TEST_TIMEOUT=<s>: a suite still running then is
#                                                  stopped and fails, googletest having no per-test one)
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

check_backend() {
    [[ "$1" =~ ^(d3d11|glcore|vulkan)$ ]] || ci_usage "backend: d3d11, glcore or vulkan (got $1)"
}

step_editor() {
    ci_msvc
    local targets=(jasperengine-editor jasperengine-editor-cmd) tests=ON
    if [[ "${EDITOR_TESTS:-1}" == 0 ]]; then tests=OFF; else targets+=(jasperengine-editor-tests); fi
    cmake --preset windows-msvc-release -B "$EDITOR_TREE" -DCMAKE_BUILD_TYPE=Release \
        -DJASPERENGINE_TESTS:BOOL="$tests" -DJASPERENGINE_WERROR="$WERROR_VALUE" \
        "-DCMAKE_INSTALL_PREFIX:PATH=$(ci_native "$INSTALL")" "${MSVC_LINKER_ARGS[@]}" \
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
    local backend="$1" debug=()
    ci_msvc
    # Release, not the preset's RelWithDebInfo, whose /Zi shared PDB ccache cannot cache.
    # ENGINE_DEBUG_INFO=1 (the tree whose core tests run): the debug information in the objects
    # instead (/Z7: CMP0141's Embedded), which ccache caches, and /DEBUG links the PDB the crash,
    # hang and logger tests' backtraces name their frames from
    if [[ "${ENGINE_DEBUG_INFO:-0}" == 1 ]]; then
        debug=(-DCMAKE_POLICY_DEFAULT_CMP0141=NEW -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
            "-DCMAKE_EXE_LINKER_FLAGS=/machine:x64 /DEBUG" "-DCMAKE_SHARED_LINKER_FLAGS=/machine:x64 /DEBUG")
    fi
    cmake --preset engine-windows-msvc-tests -S engine -B "build/windows/engine-$backend" -DCMAKE_BUILD_TYPE=Release \
        ${debug[@]+"${debug[@]}"} \
        -DGRAPHIC_BACKEND="$backend" -DJASPERENGINE_WERROR="$WERROR_VALUE" "${MSVC_LINKER_ARGS[@]}" ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
    cmake --build "build/windows/engine-$backend" --target jasperengine-project jasperengine-core-tests ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
}

min_tests() { sed -n "s/^$1=//p" tools/build.sh; }

step_tests() {
    local backend="$1" status=0 suite
    local core="build/windows/engine-$backend/tests/core/jasperengine-core-tests.exe"
    local editor="$EDITOR_TREE/jasperengine-editor-tests.exe"
    local suites="${TEST_SUITES:-core editor}"
    [[ "$suites" =~ ^(core|editor|core\ editor)$ ]] || ci_usage "TEST_SUITES: core, editor or \"core editor\" (got '$suites')"
    if [[ " $suites " == *" core "* ]]; then
        [[ -f "$core" ]] || { echo "no $core: run tools/ci/windows.sh engine $backend first" >&2; exit 1; }
    fi
    if [[ " $suites " == *" editor "* ]]; then
        [[ -f "$editor" ]] || { echo "no $editor: run tools/ci/windows.sh editor (without EDITOR_TESTS=0) first" >&2; exit 1; }
    fi
    mkdir -p "$RESULTS"
    # the GPU suites need a device (Linux runs them on llvmpipe/lavapipe, docs/testing.md); the
    # minimum counts are tools/build.sh's (AC7 of S02), the skip limit its 0
    for suite in $suites; do
        local binary="$core" minimum=CORE_MIN_TESTS
        [[ "$suite" == editor ]] && binary="$editor" minimum=EDITOR_MIN_TESTS
        rm -f "$RESULTS/$suite-windows.xml"
        local run=("$binary" --gtest_filter=-*Gpu* --gtest_output=xml:"$RESULTS/$suite-windows.xml")
        if [[ -n "${TEST_TIMEOUT:-}" ]]; then
            # (the last "[ RUN      ]" line of the log names the test that did not finish)
            local code=0
            timeout --kill-after=30 "$TEST_TIMEOUT" "${run[@]}" || code=$?
            if [[ $code != 0 ]]; then
                status=1
                [[ $code == 124 || $code == 137 ]] && echo "$suite tests: stopped after $TEST_TIMEOUT s" >&2
            fi
        else
            "${run[@]}" || status=1
        fi
        "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/$suite-windows.xml" --min-tests "$(min_tests "$minimum")" --max-skipped 0 \
            --allow-skip-reason 'count(s)? allocations|allocation counter' || status=1
    done
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
        ci_msvc
        export CI_LOG_COMMAND
        tools/ci/compile-smoke.sh "$INSTALL/bin/jasperengine-editor-cmd.exe" windows "$@" ;;
    package) [[ $# == 1 ]] || usage; ci_logged windows-package step_package "$1" ;;
    check-cli) [[ $# -le 1 ]] || usage; ci_logged windows-check-cli step_check_cli "${1:-}" ;;
    *) usage ;;
esac
