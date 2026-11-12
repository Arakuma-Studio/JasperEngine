#!/usr/bin/env bash
# The macOS lane's steps (docs/superpowers/specs/2026-11-12-ci-design.md §12, contract C5), with
# Apple Clang and Ninja (ninja on PATH: brew install ninja), from any directory.
#
#   tools/ci/macos.sh editor                 the Release editor (preset macos-release, or macos-universal-release
#                                            with ARCHS=universal; ARCHS=arm64 is the default) and
#                                            jasperengine-editor-tests, installed into instdir/ (EDITOR_TESTS=0:
#                                            no tests; a release editor, which package needs). The installed
#                                            JasperEngine.app is ad-hoc signed again: --strip voids the linker's
#                                            signature, and arm64 runs nothing unsigned
#   tools/ci/macos.sh engine                 the engine sample project and jasperengine-core-tests (Metal,
#                                            preset engine-macos-tests)
#   tools/ci/macos.sh tests                  the CPU unit tests (-*Gpu*) through tools/ci/gtest-gate.py; XML in
#                                            build/test-results/{core,editor}-macos.xml
#   tools/ci/macos.sh xcode <macos|ios>      xcodebuild of engine/workspaces/xcode (Debug; the iOS simulator SDK)
#   tools/ci/macos.sh compile-smoke <project-dir> [backend]  a Compile build of the project, as Build with
#                                            Method Compile makes it (tools/ci/compile-smoke.sh)
#   tools/ci/macos.sh package <version>      dist/JasperEngine-<version>-macos-universal.zip (the signed .app,
#                                            ditto) and .dmg (the .app and an Applications link)
#   tools/ci/macos.sh check-cli [zip]        tools/ci/check-call-cli.py on the editor of instdir/, or of that
#                                            zip unpacked
#
# Trees: build/macos/editor, build/macos/engine, build/macos/xcode-<sdk>. Logs:
# build/test-results/macos-<step>.log (macos-xcode-<macos|ios>.log). JASPERENGINE_VERSION, JOBS,
# WERROR=1 and ccache on PATH as in tools/ci/platform-common.sh; XCODE_JOBS caps xcodebuild's jobs.
# Exit 2 on bad arguments, 1 when a step fails.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=platform-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/platform-common.sh"

usage() { ci_usage "$(ci_header "${BASH_SOURCE[0]}")"; }

EDITOR_TREE=build/macos/editor
ENGINE_TREE=build/macos/engine
INSTALL="$ROOT/instdir"
APP="$INSTALL/bin/JasperEngine.app"
EDITOR_BIN="$APP/Contents/MacOS/JasperEngine"

need_ninja() {
    command -v ninja >/dev/null 2>&1 || { echo "ninja is not on PATH (brew install ninja)" >&2; exit 1; }
}

# Ad-hoc signature (no identity: the release is unsigned, §12.5): the code in Contents/MacOS first,
# then the bundle, which seals everything else
sign_app() {
    local app="$1" code
    codesign --remove-signature "$app" 2>/dev/null || true
    while IFS= read -r code; do
        codesign --force --sign - --timestamp=none "$code"
    done < <(find "$app/Contents/MacOS" -type f -name '*.dylib')
    codesign --force --sign - --timestamp=none "$app"
    verify_app "$app"
}

# a broken signature fails; --strict's extra rules only warn, as in the old CI job, until a run shows
# the bundle passes them
verify_app() {
    codesign --verify --verbose=2 "$1"
    codesign --verify --strict --verbose=2 "$1" || echo "::warning::codesign --verify --strict rejects $1"
}

step_editor() {
    need_ninja
    local preset archs
    case "${ARCHS:-arm64}" in
        arm64) preset=macos-release; archs=arm64 ;;
        universal) preset=macos-universal-release; archs="x86_64;arm64" ;;
        *) echo "ARCHS is arm64 or universal (got $ARCHS)" >&2; exit 2 ;;
    esac
    local targets=(jasperengine-editor) tests=ON
    if [[ "${EDITOR_TESTS:-1}" == 0 ]]; then tests=OFF; else targets+=(jasperengine-editor-tests); fi
    cmake --preset "$preset" -B "$EDITOR_TREE" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$archs" \
        -DJASPERENGINE_TESTS:BOOL="$tests" -DJASPERENGINE_WERROR="$WERROR_VALUE" "-DCMAKE_INSTALL_PREFIX:PATH=$INSTALL" \
        ${EDITOR_VERSION_ARGS[@]+"${EDITOR_VERSION_ARGS[@]}"} ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
    cmake --build "$EDITOR_TREE" --target "${targets[@]}" ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
    rm -rf "$INSTALL"
    cmake --install "$EDITOR_TREE" --strip
    [[ -x "$EDITOR_BIN" ]] || { echo "no $EDITOR_BIN after the install" >&2; find "$INSTALL" -maxdepth 4 | head -40 >&2; exit 1; }
    sign_app "$APP"
    lipo -archs "$EDITOR_BIN"
    "$PY" tools/ci/check-notices.py --installed "$APP/Contents/Resources"
    ccache --show-stats 2>/dev/null | grep -E 'Hits|Misses' || true
}

step_engine() {
    need_ninja
    cmake --preset engine-macos-tests -S engine -B "$ENGINE_TREE" -DJASPERENGINE_WERROR="$WERROR_VALUE" ${LAUNCHERS[@]+"${LAUNCHERS[@]}"}
    cmake --build "$ENGINE_TREE" --target jasperengine-project jasperengine-core-tests ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"}
}

min_tests() { sed -n "s/^$1=//p" tools/build.sh; }

step_tests() {
    local status=0
    local core="$ENGINE_TREE/tests/core/jasperengine-core-tests" editor="$EDITOR_TREE/jasperengine-editor-tests"
    [[ -x "$core" ]] || { echo "no $core: run tools/ci/macos.sh engine first" >&2; exit 1; }
    [[ -x "$editor" ]] || { echo "no $editor: run tools/ci/macos.sh editor (without EDITOR_TESTS=0) first" >&2; exit 1; }
    mkdir -p "$RESULTS"
    rm -f "$RESULTS/core-macos.xml" "$RESULTS/editor-macos.xml"
    # the GPU suites need a device the runner's VM does not offer to tests (Linux runs them,
    # docs/testing.md); minimum counts and skip limit as tools/build.sh unit-tests
    "$core" --gtest_filter=-*Gpu* --gtest_output=xml:"$RESULTS/core-macos.xml" || status=1
    "$editor" --gtest_filter=-*Gpu* --gtest_output=xml:"$RESULTS/editor-macos.xml" || status=1
    "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/core-macos.xml" --min-tests "$(min_tests CORE_MIN_TESTS)" --max-skipped 0 \
        --allow-skip-reason 'count(s)? allocations|allocation counter' || status=1
    "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/editor-macos.xml" --min-tests "$(min_tests EDITOR_MIN_TESTS)" --max-skipped 0 \
        --allow-skip-reason 'count(s)? allocations|allocation counter' || status=1
    return "$status"
}

step_xcode() {
    local which="$1" sdk scheme
    case "$which" in
        macos) sdk=macosx; scheme="JasperEngine macOS" ;;
        ios) sdk=iphonesimulator; scheme="JasperEngine iOS" ;;
    esac
    /usr/bin/xcodebuild -version
    # the workspace's project group (contents.xcworkspacedata: ../../../project), which the old CI
    # job created before building; nothing in it is built
    mkdir -p "$ROOT/project/assets"
    local jobs=()
    [[ -n "${XCODE_JOBS:-}" ]] && jobs=(-jobs "$XCODE_JOBS")
    # (only "JasperEngine macOS" has a shared scheme; xcodebuild makes the iOS one from its target. No
    # signing: this checks that the project compiles and links, and a simulator build needs none)
    (cd engine/workspaces/xcode && /usr/bin/xcodebuild -list -project JasperEngine.xcodeproj | sed -n '/Schemes:/,$p')
    (cd engine/workspaces/xcode && /usr/bin/xcodebuild build -project JasperEngine.xcodeproj -scheme "$scheme" -sdk "$sdk" \
        -configuration Debug -derivedDataPath "$ROOT/build/macos/xcode-$sdk" ${jobs[@]+"${jobs[@]}"} CODE_SIGNING_ALLOWED=NO)
}

step_package() {
    local version="$1" cache="$EDITOR_TREE/CMakeCache.txt"
    [[ -x "$EDITOR_BIN" && -f "$cache" ]] || { echo "no editor in $INSTALL: run tools/ci/macos.sh editor first" >&2; exit 1; }
    cache_has "$cache" 'JASPERENGINE_TESTS:BOOL=OFF' || { echo "the editor was built with its tests: build it with EDITOR_TESTS=0 to package it" >&2; exit 1; }
    cache_has "$cache" "JASPERENGINE_EDITOR_VERSION:STRING=$version" \
        || { echo "the editor is not stamped $version ($(grep '^JASPERENGINE_EDITOR_VERSION' "$cache" || echo 'git describe')): build it with JASPERENGINE_VERSION=$version" >&2; exit 1; }
    local archs
    archs="$(lipo -archs "$EDITOR_BIN")"
    [[ " $archs " == *" x86_64 "* && " $archs " == *" arm64 "* ]] || { echo "the editor holds $archs, not both slices: build it with ARCHS=universal" >&2; exit 1; }
    verify_app "$APP"
    local base="dist/JasperEngine-$version-macos-universal"
    mkdir -p dist
    rm -f "$base.zip" "$base.dmg"
    # ditto keeps the executable bits, symlinks and the signature a plain zip of the tree would lose
    ditto -c -k --sequesterRsrc --keepParent "$APP" "$base.zip"
    local stage=build/macos/dmg-stage
    rm -rf "$stage" && mkdir -p "$stage"
    ditto "$APP" "$stage/JasperEngine.app"
    ln -s /Applications "$stage/Applications"
    hdiutil create -volname JasperEngine -srcfolder "$stage" -ov -format UDZO "$base.dmg"
    rm -rf "$stage"
    ls -la dist
}

step_check_cli() {
    local editor="$EDITOR_BIN"
    if [[ -n "${1:-}" ]]; then
        [[ -f "$1" ]] || ci_usage "no zip at $1"
        local where="$ROOT/build/macos/check-cli"
        rm -rf "$where" && mkdir -p "$where"
        ditto -x -k "$1" "$where"
        editor="$where/JasperEngine.app/Contents/MacOS/JasperEngine"
        verify_app "$where/JasperEngine.app"
    fi
    "$PY" tools/ci/check-call-cli.py "$editor"
}

[[ $# -ge 1 ]] || usage
STEP="$1"
shift
case "$STEP" in
    editor) [[ $# == 0 ]] || usage; ci_logged macos-editor step_editor ;;
    engine) [[ $# == 0 ]] || usage; ci_logged macos-engine step_engine ;;
    tests) [[ $# == 0 ]] || usage; ci_logged macos-tests step_tests ;;
    xcode)
        [[ $# == 1 && ( "$1" == macos || "$1" == ios ) ]] || usage
        ci_logged "macos-xcode-$1" step_xcode "$1" ;;
    compile-smoke)
        [[ $# -ge 1 && $# -le 2 ]] || usage
        need_ninja
        export CI_LOG_COMMAND
        tools/ci/compile-smoke.sh "$EDITOR_BIN" macos "$@" ;;
    package) [[ $# == 1 ]] || usage; ci_logged macos-package step_package "$1" ;;
    check-cli) [[ $# -le 1 ]] || usage; ci_logged macos-check-cli step_check_cli "${1:-}" ;;
    *) usage ;;
esac
