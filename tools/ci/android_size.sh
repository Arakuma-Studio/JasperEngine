#!/usr/bin/env bash
# What a shipped Android build of tools/fixtures/unified-lighting weighs (docs/specs/S30-shipping.md,
# shipped size): the release android-universal-gles3 runtime (its stripped libraries and the
# symbols split off them); the editor's release packages of every ABI from it (the universal APK,
# one APK per ABI, and the App Bundle with Google Play's download size per ABI) next to a compiled
# arm64-v8a build of the same project (its exported source project built by Gradle), each broken
# down by tools/players/apk_size.py; the x86_64 APK then runs on the emulator.
#
#   tools/build.sh linux                     # the editor install
#   tools/ci/android_size.sh [--skip-players] [--skip-prebuilt] [--skip-compiled]
#
# Needs KVM for the emulator (servers A and C). Results in build/android-size/ (sizes.json).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
PLAYERS=1
PREBUILT=1
COMPILED=1
for arg in "$@"; do
    case "$arg" in
        --skip-players) PLAYERS=0 ;;
        --skip-prebuilt) PREBUILT=0 ;;
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
build() {  # name, method, package format
    step "build $1 ($2, $3)"
    tools/build.sh exec android bash -c "dbus-run-session -- env JASPER_PLAYERS_DIR=/src/build/players \
        build/linux/editor-install/bin/jasperengine-editor build --project $OUT/$1 --platform android --method $2 \
        --android-format $3 --out $OUT/$1-out" > "$OUT/$1.log" 2>&1
    local code=$?
    tail -n 6 "$OUT/$1.log"
    [[ $code == 0 ]] || { echo "FAIL: $1 (exit $code)" >&2; return 1; }
    grep -q "^method: $2" "$OUT/$1.log" || { echo "FAIL: $1 was not built with method $2" >&2; return 1; }
    [[ -f "$(apk "$1")" ]] || { echo "FAIL: $1 made no package" >&2; return 1; }
}
apk() { sed -n 's/^executable: //p' "$OUT/$1.log" | head -1 | sed 's|^/src/||'; }
artifacts() { sed -n 's/^artifact: //p' "$OUT/$1.log" | sed 's|^/src/||'; }

FAILED=0
APKS=()
EMULATOR_APK=""
if [[ $PREBUILT == 1 ]]; then
    # every ABI (the project's default), as the three packages: the universal APK (an explicit
    # choice), one APK per ABI, and the App Bundle (the default) with Google Play's download size
    # per ABI (bundletool get-size over the device APKs it serves)
    project prebuilt-universal ""
    project prebuilt-per-abi ""
    project prebuilt-aab ""
    if build prebuilt-universal prebuilt apk; then APKS+=("$(apk prebuilt-universal)"); else FAILED=1; fi
    if build prebuilt-per-abi prebuilt apk-per-abi; then
        while read -r artifact; do
            APKS+=("$artifact")
            [[ "$artifact" == *-x86_64.apk ]] && EMULATOR_APK="$artifact"
        done < <(artifacts prebuilt-per-abi)
    else
        FAILED=1
    fi
    if build prebuilt-aab prebuilt aab; then
        step "Google Play's download size per ABI (bundletool get-size total --dimensions=ABI)"
        AAB="$(apk prebuilt-aab)"
        tools/build.sh exec android bash -c "set -e; JAR=\$(find / -name 'bundletool*.jar' -not -path '/proc/*' 2>/dev/null | head -n 1 || true); \
            [ -n \"\$JAR\" ] || { echo 'no bundletool jar' >&2; exit 1; }; KS=\$HOME/.android/debug.keystore; \
            java -jar \$JAR build-apks --bundle=/src/$AAB --output=/src/$OUT/game.apks --overwrite --ks=\$KS --ks-pass=pass:android \
                --ks-key-alias=androiddebugkey --key-pass=pass:android; \
            java -jar \$JAR get-size total --apks=/src/$OUT/game.apks --dimensions=ABI" | tee "$OUT/download-size.csv" || FAILED=1
        echo "the bundle itself: $(stat -c %s "$AAB") bytes"
    else
        FAILED=1
    fi
fi

step "the player's code: what the dex holds"
[[ $PREBUILT == 1 && ${#APKS[@]} -gt 0 ]] && tools/build.sh exec android bash -c "set -e; DEXDUMP=\$(ls -d \$ANDROID_HOME/build-tools/*/dexdump | sort -V | tail -1); rm -rf /tmp/dex; mkdir /tmp/dex; \
    unzip -q -o /src/${APKS[0]} 'classes*.dex' -d /tmp/dex; for f in /tmp/dex/classes*.dex; do \$DEXDUMP \$f; done > /tmp/dex/dump.txt; \
    ls -l /tmp/dex/classes*.dex; echo classes: \$(grep -c 'Class descriptor' /tmp/dex/dump.txt); \
    grep -A3 'Class descriptor.*/GameActivity;' /tmp/dex/dump.txt | head -4; \
    grep 'Class descriptor' /tmp/dex/dump.txt | sed -E 's/.*L([a-z0-9]+\\/[a-z0-9]+)\\/.*/\\1/' | sort | uniq -c | sort -rn | head -12"

if [[ $PREBUILT == 1 ]]; then
    step "the release prebuilt x86_64 APK (one APK per ABI) on the emulator"
    if [[ -f "$EMULATOR_APK" ]]; then
        tools/android-emulator-test.sh "$EMULATOR_APK" --out "$OUT/emulator" --timeout 600 --relaunch || FAILED=1
    else
        echo "FAIL: no x86_64 APK" >&2
        FAILED=1
    fi
fi

if [[ $COMPILED == 1 ]]; then
    # a Compile build of Android is the exported source project built by Gradle (the editor's
    # build command makes the prebuilt one): release, arm64-v8a, the gles3 shaders (./gradlew: the
    # export makes it executable)
    step "compiled arm64-v8a: export, then Gradle assembleRelease"
    project compiled-arm64 arm64-v8a
    tools/build.sh exec android bash -c "dbus-run-session -- build/linux/editor-install/bin/jasperengine-editor export         --project $OUT/compiled-arm64 --out $OUT/compiled-arm64-export --backend opengles" > "$OUT/compiled-arm64.log" 2>&1
    code=$?
    tail -n 4 "$OUT/compiled-arm64.log"
    if [[ $code == 0 ]]; then
        tools/build.sh exec android bash -c "cd $OUT/compiled-arm64-export/workspaces/androidstudio &&             env JASPERENGINE_ANDROID_ABIS=arm64-v8a CMAKE_CXX_COMPILER_LAUNCHER=ccache ./gradlew --no-daemon -q assembleRelease"             >> "$OUT/compiled-arm64.log" 2>&1
        code=$?
    fi
    COMPILED_APK="$(find "$OUT/compiled-arm64-export/workspaces/androidstudio/app/build/outputs/apk/release" -name '*.apk' 2>/dev/null | head -n 1)"
    if [[ $code == 0 && -f "$COMPILED_APK" ]]; then
        APKS+=("$COMPILED_APK")
        COMPILED_LIB="$(find "$OUT/compiled-arm64-export/workspaces/androidstudio/app/build/intermediates" -path '*stripped_native_libs*arm64-v8a/libjasperengine-android.so' | head -n 1)"
        [[ -n "$COMPILED_LIB" ]] && echo "compiled stripped library: $(stat -c %s "$COMPILED_LIB") $COMPILED_LIB"
    else
        echo "FAIL: the compiled build (exit $code)" >&2
        tail -n 30 "$OUT/compiled-arm64.log"
        FAILED=1
    fi
fi

step "sizes"
[[ ${#APKS[@]} -gt 0 ]] && python3 tools/players/apk_size.py "${APKS[@]}" --json "$OUT/sizes.json" || FAILED=1
exit $FAILED
