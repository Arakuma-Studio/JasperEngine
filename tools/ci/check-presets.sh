#!/usr/bin/env bash
# Checks the CMake presets (CMakePresets.json, engine/CMakePresets.json) and the build options'
# configure-time errors (docs/testing.md). Runs inside a build image, from the repository root:
#
#   tools/ci/check-presets.sh          linux image: every D3 preset is listed in both trees, every
#                                      linux-* and engine-linux-* preset configures (--fresh), and
#                                      JASPERENGINE_SANITIZE="address;thread" fails with its message
#   tools/ci/check-presets.sh web      web image: the engine-web-* presets configure, and any
#                                      sanitizer on Emscripten fails with its message
#
# The configure trees go to build/check-presets/ and are removed afterwards.
set -euo pipefail

MODE="${1:-linux}"
OUT=build/check-presets
rm -rf "$OUT"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
failures=0

fail() {
    echo "check-presets: FAIL: $*" >&2
    failures=$((failures + 1))
}

# configure <dir> <args...>: configure into $OUT/<dir>, output in $OUT/<dir>.log
configure() {
    local dir="$1"; shift
    cmake "$@" -B "$OUT/$dir" > "$OUT/$dir.log" 2>&1
}

# expect_failure <dir> <message> <args...>: the configure must fail and print the message
expect_failure() {
    local dir="$1" message="$2"; shift 2
    if configure "$dir" "$@"; then
        fail "$dir configured, expected the error '$message'"
    elif ! grep -qF "$message" "$OUT/$dir.log"; then
        fail "$dir failed without '$message':"
        tail -20 "$OUT/$dir.log" >&2
    else
        echo "check-presets: $dir fails as expected: $message"
    fi
}

# the D3 leaves (docs/specs/S02-build-ci-tests.md) of both trees
ROOT_PRESETS="linux-release linux-debug linux-vulkan-release linux-asan linux-tsan linux-unity linux-ci
    windows-msvc-release windows-msvc-debug windows-msvc-asan windows-vs2022
    macos-release macos-universal-release macos-asan"
ENGINE_PRESETS="engine-linux-release engine-linux-debug engine-linux-tests engine-linux-tests-vulkan
    engine-linux-asan engine-linux-tsan engine-windows-msvc-tests engine-macos-tests
    engine-web-debug engine-web-release engine-web-threads"

# CMake reads both files (a schema or inheritance error fails here); the names come from the JSON,
# since --list-presets leaves out what this host cannot generate (windows-vs2022 on Linux)
cmake --list-presets > /dev/null 2>&1 || fail "cmake cannot read CMakePresets.json"
cmake -S engine --list-presets > /dev/null 2>&1 || fail "cmake cannot read engine/CMakePresets.json"
preset_names() {
    python3 -c 'import json, sys; print("\n".join(p["name"] for p in json.load(open(sys.argv[1]))["configurePresets"]))' "$1"
}
listed_root="$(preset_names CMakePresets.json)"
listed_engine="$(preset_names engine/CMakePresets.json)"
for preset in $ROOT_PRESETS; do
    grep -qxF "$preset" <<< "$listed_root" || fail "CMakePresets.json does not list $preset"
done
for preset in $ENGINE_PRESETS; do
    grep -qxF "$preset" <<< "$listed_engine" || fail "engine/CMakePresets.json does not list $preset"
done
echo "check-presets: both trees list their $(wc -w <<< "$ROOT_PRESETS $ENGINE_PRESETS") presets"

SANITIZER_MESSAGE="JASPERENGINE_SANITIZE: thread and address cannot be combined"
WEB_SANITIZER_MESSAGE="JASPERENGINE_SANITIZE is for desktop builds"

if [[ "$MODE" == linux ]]; then
    for preset in $ROOT_PRESETS; do
        [[ "$preset" == linux-* ]] || continue
        if configure "$preset" --preset "$preset" --fresh; then
            echo "check-presets: $preset configures"
        else
            fail "$preset does not configure:"; tail -30 "$OUT/$preset.log" >&2
        fi
        rm -rf "${OUT:?}/$preset"
    done
    for preset in $ENGINE_PRESETS; do
        [[ "$preset" == engine-linux-* ]] || continue
        if configure "$preset" -S engine --preset "$preset" --fresh; then
            echo "check-presets: $preset configures"
        else
            fail "$preset does not configure:"; tail -30 "$OUT/$preset.log" >&2
        fi
        rm -rf "${OUT:?}/$preset"
    done
    expect_failure editor-address-thread "$SANITIZER_MESSAGE" --preset linux-debug --fresh "-DJASPERENGINE_SANITIZE=address;thread"
    expect_failure engine-address-thread "$SANITIZER_MESSAGE" -S engine --preset engine-linux-debug --fresh "-DJASPERENGINE_SANITIZE=address,thread"
    expect_failure engine-unknown "JASPERENGINE_SANITIZE: unknown sanitizer 'memory'" -S engine --preset engine-linux-debug --fresh -DJASPERENGINE_SANITIZE=memory
    expect_failure editor-noise-off "The editor needs JASPERENGINE_NOISE" --preset linux-debug --fresh -DJASPERENGINE_NOISE=OFF
    expect_failure engine-werror "JASPERENGINE_WERROR needs JASPERENGINE_WARNINGS=ON" -S engine --preset engine-linux-debug --fresh -DJASPERENGINE_WERROR=ON
elif [[ "$MODE" == web ]]; then
    for preset in $ENGINE_PRESETS; do
        [[ "$preset" == engine-web-* ]] || continue
        if configure "$preset" -S engine --preset "$preset" --fresh; then
            echo "check-presets: $preset configures"
        else
            fail "$preset does not configure:"; tail -30 "$OUT/$preset.log" >&2
        fi
        rm -rf "${OUT:?}/$preset"
    done
    for sanitizer in address undefined thread; do
        expect_failure "web-$sanitizer" "$WEB_SANITIZER_MESSAGE" -S engine --preset engine-web-debug --fresh -DJASPERENGINE_SANITIZE=$sanitizer
    done
else
    echo "usage: tools/ci/check-presets.sh [linux|web]" >&2
    exit 2
fi

if [[ $failures != 0 ]]; then
    echo "check-presets: $failures failures" >&2
    exit 1
fi
echo "check-presets: PASS"
