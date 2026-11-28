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
#   tools/ci/macos.sh xcode <macos|ios>      xcodebuild of engine/workspaces/xcode (Debug; the iOS simulator SDK);
#                                            XCODE_CAS=<dir>: through Xcode's compilation cache in that directory,
#                                            without debug symbols (the ios job's cache entry)
#   tools/ci/macos.sh compile-smoke <project-dir> [backend]  a Compile build of the project, as Build with
#                                            Method Compile makes it (tools/ci/compile-smoke.sh)
#   tools/ci/macos.sh package <version>      dist/JasperEngine-<version>-macos-universal.zip (the signed .app,
#                                            ditto) and .dmg (the .app and an Applications link). With every
#                                            variable of DEVELOPER_ID_VARS set: the .app Developer ID signed
#                                            (hardened runtime, editor/resources/macos/JasperEngine.entitlements),
#                                            the .app and the .dmg notarized and stapled; with none, ad-hoc
#   tools/ci/macos.sh signing-cleanup        deletes package's temporary keychain and API key (after a cancel)
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

# Developer ID signing (package): the .p12 and its password, the team, and the App Store Connect API
# key notarytool signs in with; all set (the release workflows' secrets) or none (forks, PRs: ad-hoc)
DEVELOPER_ID_VARS=(MACOS_SIGN_P12_BASE64 MACOS_SIGN_P12_PASSWORD MACOS_SIGN_TEAM_ID APPLE_API_KEY_P8_BASE64 APPLE_API_KEY_ID APPLE_API_ISSUER_ID)
SIGN_DIR="${RUNNER_TEMP:-$ROOT/build/macos}/developer-id"
KEYCHAIN="$SIGN_DIR/signing.keychain-db"
API_KEY=""
ENTITLEMENTS="$ROOT/editor/resources/macos/JasperEngine.entitlements"
IDENTITY=""

developer_id_set() {
    local name present=() missing=()
    for name in "${DEVELOPER_ID_VARS[@]}"; do
        if [[ -n "${!name:-}" ]]; then present+=("$name"); else missing+=("$name"); fi
    done
    [[ ${#present[@]} -gt 0 ]] || return 1
    [[ ${#missing[@]} == 0 ]] || { echo "Developer ID signing needs every one of ${DEVELOPER_ID_VARS[*]}; not set: ${missing[*]}" >&2; exit 1; }
}

decode_base64() {
    "$PY" -I -c 'import base64, os, sys; sys.stdout.buffer.write(base64.b64decode(os.environ[sys.argv[1]]))' "$1"
}

# A keychain of this job's own (random password, ours first in the user's search list so codesign
# finds the chain), the .p12 imported with codesign allowed to use its key without a prompt
signing_begin() {
    signing_cleanup
    mkdir -p "$SIGN_DIR"
    chmod 700 "$SIGN_DIR"
    local password p12="$SIGN_DIR/developer-id.p12" keychain
    password="$(uuidgen)"
    security create-keychain -p "$password" "$KEYCHAIN"
    security set-keychain-settings -lut 21600 "$KEYCHAIN"
    security unlock-keychain -p "$password" "$KEYCHAIN"
    decode_base64 MACOS_SIGN_P12_BASE64 > "$p12"
    security import "$p12" -k "$KEYCHAIN" -f pkcs12 -P "$MACOS_SIGN_P12_PASSWORD" -T /usr/bin/codesign -T /usr/bin/security
    rm -f "$p12"
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$password" "$KEYCHAIN" > /dev/null
    security list-keychains -d user | sed -e 's/^[[:space:]]*"//' -e 's/"$//' > "$SIGN_DIR/search-list"
    local search=("$KEYCHAIN")
    while IFS= read -r keychain; do [[ -z "$keychain" ]] || search+=("$keychain"); done < "$SIGN_DIR/search-list"
    security list-keychains -d user -s "${search[@]}"
    # the identity by its SHA-1, of the configured team (a .p12 can hold more than one)
    IDENTITY="$(security find-identity -v -p codesigning "$KEYCHAIN" \
        | awk -v team="($MACOS_SIGN_TEAM_ID)\"" '/"Developer ID Application: / && index($0, team) { print $2; exit }')"
    [[ -n "$IDENTITY" ]] || { echo "the .p12 holds no valid Developer ID Application identity of the team MACOS_SIGN_TEAM_ID" >&2; exit 1; }
    # (the file name notarytool's own key lookup expects)
    API_KEY="$SIGN_DIR/AuthKey_$APPLE_API_KEY_ID.p8"
    decode_base64 APPLE_API_KEY_P8_BASE64 > "$API_KEY"
    chmod 600 "$API_KEY"
    echo "signing with the Developer ID Application identity $IDENTITY"
}

signing_cleanup() {
    if [[ -f "$SIGN_DIR/search-list" ]]; then
        local keychain search=()
        while IFS= read -r keychain; do [[ -z "$keychain" ]] || search+=("$keychain"); done < "$SIGN_DIR/search-list"
        security list-keychains -d user -s ${search[@]+"${search[@]}"} || true
    fi
    if [[ -f "$KEYCHAIN" ]]; then security delete-keychain "$KEYCHAIN" || true; fi
    rm -rf "$SIGN_DIR"
}

# Every Mach-O file and nested bundle of the .app but its main executable, the deepest first: a
# bundle's signature seals what it holds, so the inside is signed before it
nested_code() {
    local app="$1" main path magic
    main="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
    while IFS= read -r -d '' path; do
        if [[ -d "$path" ]]; then
            printf '%s\n' "$path"
            continue
        fi
        [[ "$path" != "$main" ]] || continue
        magic="$(head -c 4 "$path" | od -An -tx1 | tr -d ' \n')"
        case "$magic" in
            cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca) printf '%s\n' "$path" ;;
        esac
    done < <(find "$app/Contents" \( -type d \( -name '*.framework' -o -name '*.app' -o -name '*.bundle' -o -name '*.xpc' -o -name '*.appex' \) \) -print0 \
                 -o -type f -print0) \
        | awk -F/ '{ print NF "\t" $0 }' | sort -t $'\t' -k1,1nr -k2 | cut -f2-
}

sign_developer_id() {
    local app="$1" code
    while IFS= read -r code; do
        echo "codesign $code"
        codesign --force --options runtime --timestamp --keychain "$KEYCHAIN" --sign "$IDENTITY" "$code"
    done < <(nested_code "$app")
    codesign --force --options runtime --timestamp --keychain "$KEYCHAIN" --entitlements "$ENTITLEMENTS" --sign "$IDENTITY" "$app"
    codesign --verify --deep --strict --verbose=2 "$app"
    codesign --display --entitlements - --verbose=2 "$app" 2>&1
}

# notarytool's verdict on the file (its log printed when it is not Accepted), then the ticket stapled
# to what Gatekeeper checks offline
notarize() {
    local file="$1" staple="$2" result="$SIGN_DIR/notary.json" id status
    local auth=(--key "$API_KEY" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID")
    echo "notarizing $file"
    xcrun notarytool submit "$file" "${auth[@]}" --wait --timeout 2h --output-format json > "$result" || true
    cat "$result"
    echo
    id="$("$PY" -I -c 'import json, sys; print(json.load(open(sys.argv[1])).get("id", ""))' "$result" 2>/dev/null || true)"
    status="$("$PY" -I -c 'import json, sys; print(json.load(open(sys.argv[1])).get("status", ""))' "$result" 2>/dev/null || true)"
    if [[ "$status" != Accepted ]]; then
        echo "::error::notarization of $(basename "$file") ended ${status:-without a verdict}"
        [[ -z "$id" ]] || xcrun notarytool log "$id" "${auth[@]}" || true
        exit 1
    fi
    xcrun stapler staple "$staple"
    xcrun stapler validate "$staple"
}

# Gatekeeper's own verdict, which must name the notarization
gatekeeper_accepts() {
    local verdict
    verdict="$(spctl --assess -vvv "$@" 2>&1)" || { echo "$verdict"; echo "::error::Gatekeeper rejects ${*: -1}"; exit 1; }
    echo "$verdict"
    grep -q 'source=Notarized Developer ID' <<< "$verdict" || { echo "::error::${*: -1} is not accepted as notarized Developer ID code"; exit 1; }
}

step_editor() {
    need_ninja
    local preset archs
    case "${ARCHS:-arm64}" in
        arm64) preset=macos-release; archs=arm64 ;;
        universal) preset=macos-universal-release; archs="x86_64;arm64" ;;
        *) echo "ARCHS is arm64 or universal (got $ARCHS)" >&2; exit 2 ;;
    esac
    local targets=(jasperengine-editor) tests=ON pch=()
    # JASPERENGINE_PCH=OFF in CI, as tools/build.sh
    if [[ -n "${JASPERENGINE_PCH:-}" ]]; then pch=(-DJASPERENGINE_PCH="$JASPERENGINE_PCH"); fi
    if [[ "${EDITOR_TESTS:-1}" == 0 ]]; then tests=OFF; else targets+=(jasperengine-editor-tests); fi
    cmake --preset "$preset" -B "$EDITOR_TREE" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$archs" \
        -DJASPERENGINE_TESTS:BOOL="$tests" -DJASPERENGINE_WERROR="$WERROR_VALUE" "-DCMAKE_INSTALL_PREFIX:PATH=$INSTALL" \
        ${EDITOR_VERSION_ARGS[@]+"${EDITOR_VERSION_ARGS[@]}"} ${LAUNCHERS[@]+"${LAUNCHERS[@]}"} ${pch[@]+"${pch[@]}"}
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
    # what arm64 and Apple's own HTTP stack do not have: the 8-wide (AVX2) curl noise, the curl
    # backend's missing-library seam
    "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/core-macos.xml" --min-tests "$(min_tests CORE_MIN_TESTS)" --max-skipped 0 \
        --allow-skip-reason 'count(s)? allocations|allocation counter' --allow-skip-reason 'no 8-wide curl noise on this processor' || status=1
    "$PY" tools/ci/gtest-gate.py --xml "$RESULTS/editor-macos.xml" --min-tests "$(min_tests EDITOR_MIN_TESTS)" --max-skipped 0 \
        --allow-skip-reason 'count(s)? allocations|allocation counter' --allow-skip-reason "the missing library is the curl backend's \\(Linux\\)" || status=1
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
    local jobs=() caching=()
    [[ -n "${XCODE_JOBS:-}" ]] && jobs=(-jobs "$XCODE_JOBS")
    # Xcode 26's compilation cache, outside DerivedData so a fresh tree hits it (both SDKs share it). The
    # checkout's path is mapped, so a cache made in another checkout hits too (only the SDK, toolchain and
    # Xcode are by default). No debug symbols: on the Mac, iOS's cache was 888 MB compressed with them,
    # both SDKs' 392 MB without, and every object still compiles and links (a warm iOS build 30 s, cold 4.5 min)
    if [[ -n "${XCODE_CAS:-}" ]]; then
        mkdir -p "$XCODE_CAS"
        caching=(COMPILATION_CACHE_ENABLE_CACHING=YES "COMPILATION_CACHE_CAS_PATH=$XCODE_CAS" GCC_GENERATE_DEBUGGING_SYMBOLS=NO
            CLANG_ENABLE_PREFIX_MAPPING=YES "CLANG_OTHER_PREFIX_MAPPINGS=$ROOT=/^src")
    fi
    # (only "JasperEngine macOS" has a shared scheme; xcodebuild makes the iOS one from its target. No
    # signing: this checks that the project compiles and links, and a simulator build needs none)
    (cd engine/workspaces/xcode && /usr/bin/xcodebuild -list -project JasperEngine.xcodeproj | sed -n '/Schemes:/,$p')
    (cd engine/workspaces/xcode && /usr/bin/xcodebuild build -project JasperEngine.xcodeproj -scheme "$scheme" -sdk "$sdk" \
        -configuration Debug -derivedDataPath "$ROOT/build/macos/xcode-$sdk" ${jobs[@]+"${jobs[@]}"} CODE_SIGNING_ALLOWED=NO \
        ${caching[@]+"${caching[@]}"})
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
    local developer_id=0
    if developer_id_set; then
        developer_id=1
        trap signing_cleanup EXIT
        signing_begin
        sign_developer_id "$APP"
        # the .app's own ticket first, so the zip and the dmg both carry a stapled .app
        ditto -c -k --sequesterRsrc --keepParent "$APP" "$SIGN_DIR/JasperEngine.zip"
        notarize "$SIGN_DIR/JasperEngine.zip" "$APP"
    else
        echo "ad-hoc signed: Developer ID secrets not set"
    fi
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
    if [[ $developer_id == 1 ]]; then
        codesign --force --timestamp --keychain "$KEYCHAIN" --sign "$IDENTITY" "$base.dmg"
        notarize "$base.dmg" "$base.dmg"
        codesign --verify --deep --strict --verbose=2 "$APP"
        gatekeeper_accepts --type execute "$APP"
        gatekeeper_accepts --type open --context context:primary-signature "$base.dmg"
        # the zip's .app as a user unpacks it
        ditto -x -k "$base.zip" "$SIGN_DIR/unpacked"
        gatekeeper_accepts --type execute "$SIGN_DIR/unpacked/JasperEngine.app"
        signing_cleanup
        trap - EXIT
    fi
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
    signing-cleanup) [[ $# == 0 ]] || usage; signing_cleanup ;;
    check-cli) [[ $# -le 1 ]] || usage; ci_logged macos-check-cli step_check_cli "${1:-}" ;;
    *) usage ;;
esac
