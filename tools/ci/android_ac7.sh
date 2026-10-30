#!/usr/bin/env bash
# AC7 (docs/specs/S30-shipping.md): an Android game from the prebuilt player with the SDK's tools
# alone. tools/fixtures/unified-lighting (arm64-v8a and x86_64 selected) is built by the editor in
# the Android image as each package format: the App Bundle, one APK per ABI, the universal APK,
# with `cmake` and `ndk-build` replaced by stand-ins that fail and log, and no NDK. Then:
#   - no stand-in ran;
#   - apksigner verify --verbose: v2 signed; zipalign -c -P 16 4 passes;
#   - aapt2 dump badging: the project's applicationId, versionCode and label;
#   - bundletool validate passes on the .aab;
#   - unzip -l: only the selected ABIs (the universal APK both; each per-ABI APK its own).
#
#   tools/build.sh linux                                   # the editor install
#   PLAYER_PLATFORM=android PLAYER_FLAVOURS=release tools/build.sh players   # or players install
#   tools/ci/android_ac7.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
[[ -d build/players/android-universal-gles3-release ]] || { echo "no android-universal-gles3-release runtime in build/players" >&2; exit 2; }
[[ -x build/linux/editor-install/bin/jasperengine-editor ]] || { echo "no editor install: tools/build.sh linux" >&2; exit 2; }
OUT=build/android-ac7
rm -rf "$OUT"
mkdir -p "$OUT/stubs"
cp -r tools/fixtures/unified-lighting "$OUT/project"
cat >> "$OUT/project/project.yaml" <<'YAML'
android:
  packageName: com.jasperengine.acseven
  versionCode: 7
  versionName: "7.0"
  architectures:
    armeabi-v7a: false
    arm64-v8a: true
    x86: false
    x86_64: true
YAML
for tool in cmake ndk-build; do
    printf '#!/bin/sh\necho "AC7: %s was run: $*" >> /src/%s/stubs/ran.log\nexit 99\n' "$tool" "$OUT" > "$OUT/stubs/$tool"
    chmod +x "$OUT/stubs/$tool"
done
tools/build.sh exec android bash -euo pipefail -c "
    export ANDROID_NDK_HOME=/nonexistent ANDROID_NDK_ROOT=/nonexistent PATH=/src/$OUT/stubs:\$PATH
    for format in aab apk-per-abi apk; do
        dbus-run-session -- env JASPER_PLAYERS_DIR=/src/build/players build/linux/editor-install/bin/jasperengine-editor build \
            --project $OUT/project --platform android --method prebuilt --android-format \$format --out $OUT/\$format > $OUT/\$format.log 2>&1 \
            || { tail -n 30 $OUT/\$format.log; echo \"FAIL: the \$format build\"; exit 1; }
        grep -q '^method: prebuilt' $OUT/\$format.log || { echo \"FAIL: \$format was not built with the prebuilt player\"; exit 1; }
    done
    BT=\$(ls -d \$ANDROID_HOME/build-tools/* | sort -V | tail -n 1)
    check_apk() {  # apk, the ABIs it must hold (space separated)
        \$BT/apksigner verify --verbose \"\$1\" | grep -q 'Verified using v2 scheme (APK Signature Scheme v2): true' \
            || { echo \"FAIL: \$1 is not v2 signed\"; exit 1; }
        \$BT/zipalign -c -P 16 4 \"\$1\" || { echo \"FAIL: \$1 is not aligned (zipalign -c -P 16 4)\"; exit 1; }
        badging=\$(\$BT/aapt2 dump badging \"\$1\")
        echo \"\$badging\" | grep -q \"package: name='com.jasperengine.acseven' versionCode='7' versionName='7.0'\" \
            || { echo \"FAIL: \$1 badging: \$(echo \"\$badging\" | head -n 1)\"; exit 1; }
        echo \"\$badging\" | grep -q \"application-label:'\" || { echo \"FAIL: \$1 has no label\"; exit 1; }
        abis=\$(unzip -l \"\$1\" | sed -n 's|.* lib/\\([^/]*\\)/.*|\\1|p' | sort -u | paste -sd' ')
        [[ \"\$abis\" == \"\$2\" ]] || { echo \"FAIL: \$1 holds the ABIs '\$abis', not '\$2'\"; exit 1; }
        echo \"ok: \$(basename \"\$1\"): v2 signed, aligned, com.jasperengine.acseven 7, ABIs \$abis\"
    }
    check_apk $OUT/apk/UnifiedLighting.apk 'arm64-v8a x86_64'
    check_apk $OUT/apk-per-abi/UnifiedLighting-arm64-v8a.apk 'arm64-v8a'
    check_apk $OUT/apk-per-abi/UnifiedLighting-x86_64.apk 'x86_64'
    [[ \$(ls $OUT/apk-per-abi/*.apk | wc -l) == 2 ]] || { echo 'FAIL: not one APK per selected ABI'; exit 1; }
    JAR=\$(find / -name 'bundletool*.jar' -not -path '/proc/*' 2>/dev/null | head -n 1)
    [[ -n \"\$JAR\" ]] || { echo 'FAIL: no bundletool jar'; exit 1; }
    java -jar \"\$JAR\" validate --bundle=$OUT/aab/UnifiedLighting.aab > $OUT/aab-validate.txt || { cat $OUT/aab-validate.txt; echo 'FAIL: bundletool validate'; exit 1; }
    abis=\$(unzip -l $OUT/aab/UnifiedLighting.aab | sed -n 's|.* base/lib/\\([^/]*\\)/.*|\\1|p' | sort -u | paste -sd' ')
    [[ \"\$abis\" == 'arm64-v8a x86_64' ]] || { echo \"FAIL: the bundle holds the ABIs '\$abis'\"; exit 1; }
    echo \"ok: UnifiedLighting.aab: bundletool validate passes, ABIs \$abis\"
"
if [[ -s "$OUT/stubs/ran.log" ]]; then
    cat "$OUT/stubs/ran.log"
    echo "FAIL: the build ran a tool a prebuilt Android build must not need" >&2
    exit 1
fi
echo "PASS: AC7 (no NDK, CMake or Gradle)"
