#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# T2 section 6: the noise golden table on the Android ABIs, on this host. In the Android image:
#
#   tools/build.sh exec android tools/noise/cross_check.sh [--red-contract]
#
# Builds tools/noise/golden with the NDK's clang and the player's release flags (-O2, ThinLTO, the
# noise flags), static, for each ABI, and runs it: x86 and x86_64 natively, the ARM two under
# qemu-user. A row that cannot run prints UNVERIFIED and never counts as a pass. A static bionic binary
# that will not start under qemu falls back to Ubuntu's cross compiler: that proves the ISA and the
# flags, not NDK clang, and the row says so.
#
# --red-contract builds x86-64 with -ffp-contract=fast -mfma: the table must catch it, so this mode
# exits non-zero when the run is red, and 3 when the control failed to go red.
set -uo pipefail

NDK_VERSION=28.2.13676358
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/noise-cross"
SOURCE="$ROOT/tools/noise/golden"
RED=0
[[ "${1:-}" == --red-contract ]] && RED=1

find_ndk() {
    for candidate in "${ANDROID_NDK_HOME:-}" "${ANDROID_HOME:-/opt/android-sdk}/ndk/$NDK_VERSION" "$HOME/noise-android-sdk/ndk/$NDK_VERSION"; do
        if [[ -n "$candidate" && -f "$candidate/build/cmake/android.toolchain.cmake" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

if ! NDK="$(find_ndk)"; then
    # The SDK inside the image is thrown away with the container: keep this NDK in the mounted cache
    if command -v sdkmanager >/dev/null; then
        echo "installing NDK $NDK_VERSION under $HOME/noise-android-sdk" >&2
        yes | sdkmanager --sdk_root="$HOME/noise-android-sdk" --licenses >/dev/null 2>&1
        sdkmanager --sdk_root="$HOME/noise-android-sdk" "ndk;$NDK_VERSION" >/dev/null || true
    fi
    NDK="$(find_ndk)" || { echo "UNVERIFIED (no NDK $NDK_VERSION: run in the Android image)"; exit 2; }
fi
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
CMAKE="$(command -v cmake || true)"
for candidate in "${ANDROID_HOME:-/opt/android-sdk}"/cmake/*/bin/cmake "$HOME"/noise-android-sdk/cmake/*/bin/cmake; do
    [[ -x "$candidate" ]] && CMAKE="$candidate"
done
if [[ -z "$CMAKE" ]]; then
    sdkmanager --sdk_root="$HOME/noise-android-sdk" "cmake;3.22.1" >/dev/null || true
    CMAKE="$HOME/noise-android-sdk/cmake/3.22.1/bin/cmake"
fi
[[ -x "$CMAKE" ]] || { echo "UNVERIFIED (no cmake in the image)"; exit 2; }
NINJA_DIR="$(dirname "$CMAKE")"
export PATH="$NINJA_DIR:$PATH"
command -v ninja >/dev/null || { echo "UNVERIFIED (no ninja in the image)"; exit 2; }

build_ndk() {  # abi extra-flags extra-cmake...
    local abi="$1" flags="$2"
    shift 2
    local dir="$OUT/$abi"
    # a clean tree each run (the SDK's CMake 3.22 has no --fresh)
    rm -rf "$dir"
    "$CMAKE" -S "$SOURCE" -B "$dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG $flags" \
        -DJASPERENGINE_NOISE_STATIC=ON "$@" >"$dir.configure.log" 2>&1 \
        && "$CMAKE" --build "$dir" >"$dir.build.log" 2>&1
}

# Prints the tool's report and sets RUN_STATUS: 0 pass, 1 mismatch, 126 could not start
run_tool() {  # runner binary
    local runner="$1" binary="$2" output
    if [[ -n "$runner" ]]; then
        output="$("$runner" "$binary" --all-backends --report - 2>&1)"
    else
        output="$("$binary" --all-backends --report - 2>&1)"
    fi
    RUN_STATUS=$?
    echo "$output"
    if [[ $RUN_STATUS -ne 0 && $RUN_STATUS -ne 1 ]] || ! grep -q '^noise-golden ' <<<"$output"; then
        RUN_STATUS=126
    fi
}

if [[ $RED -eq 1 ]]; then
    mkdir -p "$OUT"
    if ! grep -q ' fma ' /proc/cpuinfo; then
        echo "UNVERIFIED (this CPU has no FMA, so contraction cannot show)"
        exit 2
    fi
    build_ndk x86_64 "-flto=thin" -DJASPERENGINE_NOISE_RED_CONTRACT=ON || { tail -20 "$OUT/x86_64.build.log"; exit 2; }
    run_tool "" "$OUT/x86_64/jasperengine-noise-golden"
    if [[ $RUN_STATUS -eq 1 ]]; then
        echo "red-contract: the table caught contraction"
        exit 1
    fi
    echo "red-contract: the control did NOT go red"
    exit 3
fi

mkdir -p "$OUT"
failures=0
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    runner=""
    case "$abi" in
        arm64-v8a) runner=qemu-aarch64-static ;;
        armeabi-v7a) runner=qemu-arm-static ;;
    esac
    if [[ -n "$runner" ]] && ! command -v "$runner" >/dev/null; then
        echo "cross-check $abi: UNVERIFIED (no qemu-user in the image)"
        continue
    fi
    if ! build_ndk "$abi" "-flto=thin"; then
        echo "cross-check $abi: FAILED to build (see $OUT/$abi.build.log)"
        tail -5 "$OUT/$abi.build.log"
        failures=$((failures + 1))
        continue
    fi
    run_tool "$runner" "$OUT/$abi/jasperengine-noise-golden"
    if [[ $RUN_STATUS -eq 0 ]]; then
        echo "cross-check $abi: passed (NDK clang, static bionic${runner:+, $runner})"
    elif [[ $RUN_STATUS -eq 1 ]]; then
        echo "cross-check $abi: FAILED: a difference is a kernel bug"
        failures=$((failures + 1))
    elif [[ -n "$runner" ]]; then
        # T2 section 9: a static bionic binary may not start under qemu-user
        cross=""
        case "$abi" in
            arm64-v8a) cross=aarch64-linux-gnu ;;
            armeabi-v7a) cross=arm-linux-gnueabihf ;;
        esac
        if command -v "$cross-g++" >/dev/null; then
            dir="$OUT/$abi-ubuntu"
            rm -rf "$dir"
            if "$CMAKE" -S "$SOURCE" -B "$dir" -G Ninja -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER="$cross-gcc" -DCMAKE_CXX_COMPILER="$cross-g++" \
                    -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_NOISE_STATIC=ON >"$dir.configure.log" 2>&1 && "$CMAKE" --build "$dir" >"$dir.build.log" 2>&1; then
                run_tool "$runner" "$dir/jasperengine-noise-golden"
                if [[ $RUN_STATUS -eq 0 ]]; then
                    echo "cross-check $abi: passed on Ubuntu's $cross-g++ under $runner: ISA and flags verified, NDK clang UNVERIFIED (static bionic would not start)"
                elif [[ $RUN_STATUS -eq 1 ]]; then
                    echo "cross-check $abi: FAILED on Ubuntu's $cross-g++ under $runner"
                    failures=$((failures + 1))
                else
                    echo "cross-check $abi: UNVERIFIED (would not start under $runner)"
                fi
                continue
            fi
            echo "cross-check $abi: UNVERIFIED (static bionic would not start under $runner, and $cross-g++ failed to build: see $dir.build.log)"
            continue
        fi
        echo "cross-check $abi: UNVERIFIED (static bionic would not start under $runner, and no cross compiler)"
    else
        echo "cross-check $abi: UNVERIFIED (static bionic would not start here)"
    fi
done
exit $((failures > 0 ? 1 : 0))
