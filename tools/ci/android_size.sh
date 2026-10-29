#!/usr/bin/env bash
# What a shipped Android build of tools/fixtures/unified-lighting weighs (docs/specs/S30-shipping.md,
# shipped size): the release android-universal-gles3 runtime (its stripped libraries and the
# symbols split off them), release APKs built from it by the editor (every ABI, arm64-v8a only,
# armeabi-v7a only) next to a compiled arm64-v8a build of the same project, each broken down by
# tools/players/apk_size.py; the release prebuilt APK then runs on the emulator.
#
#   tools/build.sh linux                     # the editor install
#   tools/ci/android_size.sh [--skip-players] [--skip-compiled]
#
# Needs KVM for the emulator (servers A and C). Results in build/android-size/ (sizes.json).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
PLAYERS=1
COMPILED=1
for arg in "$@"; do
    case "$arg" in
        --skip-players) PLAYERS=0 ;;
        --skip-compiled) COMPILED=0 ;;
        *) echo "unknown argument $arg" >&2; exit 2 ;;
    esac
done
step() { echo; echo "=== $*"; }
[[ -x build/linux/editor-install/bin/jasperengine-editor ]] || { echo "no editor install: tools/build.sh linux" >&2; exit 2; }

if [[ $PLAYERS == 1 ]]; then
    step "release player, every ABI, gles3"
    PLAYER_PLATFORM=android PLAYER_BACKENDS=gles3 PLAYER_FLAVOURS=release tools/build.sh players || { echo "FAIL: the release player" >&2; exit 1; }
fi
RUNTIME=build/players/android-universal-gles3-release
[[ -d "$RUNTIME" ]] || { echo "no $RUNTIME" >&2; exit 2; }
step "the runtime: shipped libraries (stripped) and their symbols (unstripped)"
find "$RUNTIME" -name '*.so' -printf '%s %P\n' | sort -k2

OUT=build/android-size
rm -rf "$OUT"
mkdir -p "$OUT"
project() {  # name, the ABIs it ships (comma separated; empty: the project's default)
    cp -r tools/fixtures/unified-lighting "$OUT/$1"
    [[ -z "$2" ]] && return
    printf 'android:\n  architectures:\n' >> "$OUT/$1/project.yaml"
    for abi in armeabi-v7a arm64-v8a x86 x86_64; do
        if [[ ",$2," == *",$abi,"* ]]; then echo "    $abi: true"; else echo "    $abi: false"; fi
    done >> "$OUT/$1/project.yaml"
}
build() {  # name, method
    step "build $1 ($2)"
    tools/build.sh exec android bash -c "dbus-run-session -- env JASPER_PLAYERS_DIR=/src/build/players \
        build/linux/editor-install/bin/jasperengine-editor build --project $OUT/$1 --platform android --method $2 --out $OUT/$1-out" \
        > "$OUT/$1.log" 2>&1
    local code=$?
    tail -n 6 "$OUT/$1.log"
    grep -q "^method: $2" "$OUT/$1.log" || { echo "FAIL: $1 was not built with method $2 (exit $code)" >&2; return 1; }
}
apk() { sed -n 's/^executable: //p' "$OUT/$1.log" | head -1 | sed 's|^/src/||'; }

FAILED=0
project prebuilt-all ""
project prebuilt-arm64 arm64-v8a
project prebuilt-armv7 armeabi-v7a
APKS=()
for name in prebuilt-all prebuilt-arm64 prebuilt-armv7; do
    if build "$name" prebuilt; then APKS+=("$(apk "$name")"); else FAILED=1; fi
done

step "the player's code: what the dex holds"
tools/build.sh exec android bash -c "set -e; DEXDUMP=\$(ls -d \$ANDROID_HOME/build-tools/*/dexdump | sort -V | tail -1); rm -rf /tmp/dex; mkdir /tmp/dex; \
    unzip -q -o /src/${APKS[0]} 'classes*.dex' -d /tmp/dex; for f in /tmp/dex/classes*.dex; do \$DEXDUMP \$f; done > /tmp/dex/dump.txt; \
    ls -l /tmp/dex/classes*.dex; echo classes: \$(grep -c 'Class descriptor' /tmp/dex/dump.txt); \
    grep -A3 'Class descriptor.*/GameActivity;' /tmp/dex/dump.txt | head -4; \
    grep 'Class descriptor' /tmp/dex/dump.txt | sed -E 's/.*L([a-z0-9]+\\/[a-z0-9]+)\\/.*/\\1/' | sort | uniq -c | sort -rn | head -12"

if [[ ${#APKS[@]} -gt 0 ]]; then
    step "the release prebuilt APK on the emulator"
    tools/android-emulator-test.sh "${APKS[0]}" --out "$OUT/emulator" --timeout 600 || FAILED=1
fi

if [[ $COMPILED == 1 ]]; then
    project compiled-arm64 arm64-v8a
    if build compiled-arm64 compile; then APKS+=("$(apk compiled-arm64)"); else FAILED=1; fi
fi

step "sizes"
python3 tools/players/apk_size.py "${APKS[@]}" --json "$OUT/sizes.json" || FAILED=1
exit $FAILED
