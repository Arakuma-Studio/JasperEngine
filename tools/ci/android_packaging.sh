#!/usr/bin/env bash
# Android packaging end to end (docs/specs/S47-packaging-tests.md R1-R3): what a user pressing Build
# gets, run for real. tools/fixtures/unified-lighting (x86_64 only: the emulator's ABI) goes through
# the editor in the Android image with the prebuilt player, then:
#
#   1. bundletool: the cached jar is removed, so an App Bundle build downloads it from the real URL
#      (pinned SHA-256 checked); a second build uses the cache.
#   2. Signing: the debug key by default; a release keystore (JKS with its own key password, and a
#      PKCS12 one with an EC key) signs an APK and a bundle; a wrong store password, a wrong key
#      password and an unknown alias each fail with their own message; no password reaches a log, a
#      package or the output folder.
#   3. publish prepare validates the APK and the bundle (signed, aligned, the content there).
#   4. The emulator (tools/android-emulator-test.sh; needs KVM): the APK installs, runs, relaunches and
#      its screen is compared with tools/fixtures/unified-lighting/golden/android/screen.png.
#      The bundle goes through bundletool build-apks and install-apks and is compared the same way.
#      A texture that fails to load fails the run: the picture would still look like a game.
#
#   tools/build.sh linux                                   # the editor install
#   PLAYER_PLATFORM=android PLAYER_ANDROID_ABIS=x86_64 PLAYER_FLAVOURS=development tools/build.sh players
#   tools/ci/android_packaging.sh [release|development]    # default development
#
# ANDROID_PACKAGING_SOURCE=1 adds the source export, which compiles the engine (about half an hour):
# the editor's export built with the project's own Gradle into a debug APK and bundle, both run and
# compared on the emulator as well. Gradle signs them with the debug key: a source export's release key
# is set up in Android Studio, as the Gradle project has no signing config of the project's keystore.
#
# ANDROID_PACKAGING_NO_EMULATOR=1 stops after step 3 (no KVM here). Results in build/android-packaging/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
FLAVOUR="${1:-development}"
[[ "$FLAVOUR" == release || "$FLAVOUR" == development ]] || { echo "flavour is release or development" >&2; exit 2; }
[[ -d "build/players/android-universal-gles3-$FLAVOUR" ]] || { echo "no android-universal-gles3-$FLAVOUR runtime in build/players" >&2; exit 2; }
[[ -x build/linux/editor-install/bin/jasperengine-editor ]] || { echo "no editor install: tools/build.sh linux" >&2; exit 2; }

OUT=build/android-packaging
rm -rf "$OUT"
mkdir -p "$OUT/logs"
DEV_FLAG=""
[[ "$FLAVOUR" == development ]] && DEV_FLAG="--development"

# a project copy; its android: block, with the emulator's ABI only, then the lines given
make_project() {
    local name="$1"; shift
    cp -r tools/fixtures/unified-lighting "$OUT/$name"
    {
        echo 'android:'
        echo '  packageName: com.jasperengine.packaging'
        echo '  architectures:'
        echo '    armeabi-v7a: false'
        echo '    arm64-v8a: false'
        echo '    x86: false'
        echo '    x86_64: true'
        printf '%s\n' "$@"
    } >> "$OUT/$name/project.yaml"
}
make_project debug
python3 tools/ci/freeze_unified_lighting.py "$OUT/debug"
# the emulator's renderer has no ASTC: without ETC2 cooks these textures fail to load there
printf 'texture:\n  androidEtc2Fallback: true\n' >> "$OUT/debug/project.yaml"
make_project release '  keystore:' "    path: ../release.jks" '    alias: game'
make_project release-ec '  keystore:' "    path: ../release-ec.p12" '    alias: ec'
make_project wrong-alias '  keystore:' "    path: ../release.jks" '    alias: nosuchkey'

# what the checks grep for in every log and output: not real secrets, and in no file if all is well
STORE_SECRET=st0re-S47-do-not-log
KEY_SECRET=k3y-S47-do-not-log

tools/build.sh exec android bash -euo pipefail -c "
    OUT=$OUT DEV_FLAG='$DEV_FLAG' STORE_SECRET=$STORE_SECRET KEY_SECRET=$KEY_SECRET
    BT=\$(ls -d \$ANDROID_HOME/build-tools/* | sort -V | tail -n 1)
    fail() { echo \"FAIL: \$*\"; exit 1; }
    ok() { echo \"ok: \$*\"; }

    # one editor build: project, format, output, log (the environment the caller set)
    build() {
        dbus-run-session -- env JASPER_PLAYERS_DIR=/src/build/players build/linux/editor-install/bin/jasperengine-editor build \
            --project \$OUT/\$1 --platform android --method prebuilt --android-format \$2 \$DEV_FLAG --out \$OUT/\$3 > \$OUT/logs/\$3.log 2>&1
    }
    must_build() { build \"\$@\" || { tail -n 30 \$OUT/logs/\$3.log; fail \"the \$2 build of \$1\"; }; grep -q '^method: prebuilt' \$OUT/logs/\$3.log || fail \"\$3 was not prebuilt\"; }
    # the build must fail, and say this
    must_refuse() {
        local text=\"\$1\"; shift
        if build \"\$@\"; then fail \"\$3 was built\"; fi
        grep -qF -- \"\$text\" \$OUT/logs/\$3.log || { tail -n 20 \$OUT/logs/\$3.log; fail \"\$3 does not say: \$text\"; }
        ok \"\$3 refused: \$(grep -F -- \"\$text\" \$OUT/logs/\$3.log | head -n 1)\"
    }

    # ---- 1. bundletool: the real download, then the cache
    JAR=\$HOME/.cache/jasperengine/tools/bundletool-all-1.17.2.jar
    rm -f \"\$JAR\" \"\$JAR.part\"
    must_build debug aab debug-aab
    [[ -f \"\$JAR\" ]] || fail 'the bundle build did not keep bundletool in the cache'
    echo \"2d4ad908faea64047c1cc9cb747e6aa667c6ab192e09607bd16b67246a8cd6ae  \$JAR\" | sha256sum -c - || fail 'the cached bundletool has another hash'
    STAMP=\$(stat -c %Y \"\$JAR\")
    ok \"bundletool downloaded from the real URL (\$(stat -c %s \"\$JAR\") bytes), SHA-256 as pinned\"
    must_build debug aab debug-aab-again
    [[ \$(stat -c %Y \"\$JAR\") == \"\$STAMP\" ]] || fail 'the second bundle build fetched bundletool again'
    ok 'the second bundle build used the cached bundletool'
    cp \"\$JAR\" \$OUT/bundletool.jar

    # ---- 2. signing
    must_build debug apk debug-apk
    grep -q W-ANDROID-DEBUG-KEY \$OUT/logs/debug-apk.log && [[ -z \"\$DEV_FLAG\" ]] || [[ -n \"\$DEV_FLAG\" ]] || fail 'a release build signed with the debug key does not warn'
    \$BT/apksigner verify --print-certs \$OUT/debug-apk/UnifiedLighting.apk | grep -q 'CN=Android Debug' || fail 'the default APK is not signed with the debug key'
    jarsigner -verify -certs -verbose \$OUT/debug-aab/UnifiedLighting.aab > \$OUT/logs/debug-aab-verify.txt 2>&1 || fail 'the bundle does not verify'
    grep -q 'CN=Android Debug' \$OUT/logs/debug-aab-verify.txt || fail 'the default bundle is not signed with the debug key'
    ok 'the debug key signs by default (APK and bundle)'

    keytool -genkeypair -keystore \$OUT/release.jks -storetype JKS -storepass \"\$STORE_SECRET\" -keypass \"\$KEY_SECRET\" -alias game \
        -keyalg RSA -keysize 2048 -validity 1000 -dname 'CN=S47 Release,O=JasperEngine,C=US' 2> /dev/null
    keytool -genkeypair -keystore \$OUT/release-ec.p12 -storetype PKCS12 -storepass \"\$STORE_SECRET\" -alias ec \
        -keyalg EC -groupname secp256r1 -validity 1000 -dname 'CN=S47 EC,O=JasperEngine,C=US' 2> /dev/null

    export JASPER_ANDROID_KS_PASS=\"\$STORE_SECRET\" JASPER_ANDROID_KEY_PASS=\"\$KEY_SECRET\"
    must_build release apk release-apk
    \$BT/apksigner verify --print-certs \$OUT/release-apk/UnifiedLighting.apk | grep -q 'CN=S47 Release' || fail 'the APK is not signed with the release key'
    must_build release aab release-aab
    jarsigner -verify -certs -verbose \$OUT/release-aab/UnifiedLighting.aab > \$OUT/logs/release-aab-verify.txt 2>&1 || fail 'the release bundle does not verify'
    grep -q 'CN=S47 Release' \$OUT/logs/release-aab-verify.txt || fail 'the bundle is not signed with the release key'
    ok 'a release keystore signs the APK and the bundle'

    # (PKCS12: one password for the store and the key)
    export JASPER_ANDROID_KEY_PASS=\"\$STORE_SECRET\"
    must_build release-ec aab release-ec-aab
    must_build release-ec apk release-ec-apk
    jarsigner -verify -certs -verbose \$OUT/release-ec-aab/UnifiedLighting.aab > \$OUT/logs/release-ec-aab-verify.txt 2>&1 || fail 'the EC bundle does not verify'
    grep -q 'CN=S47 EC' \$OUT/logs/release-ec-aab-verify.txt || fail 'the bundle is not signed with the EC key'
    \$BT/apksigner verify --print-certs \$OUT/release-ec-apk/UnifiedLighting.apk | grep -q 'CN=S47 EC' || fail 'the APK is not signed with the EC key'
    ok 'an EC key signs the APK and the bundle'

    export JASPER_ANDROID_KS_PASS=wrong-\$STORE_SECRET JASPER_ANDROID_KEY_PASS=\"\$KEY_SECRET\"
    must_refuse 'the password of the keystore' release apk refused-store-apk
    must_refuse 'the password of the keystore' release aab refused-store-aab
    export JASPER_ANDROID_KS_PASS=\"\$STORE_SECRET\" JASPER_ANDROID_KEY_PASS=wrong-\$KEY_SECRET
    must_refuse 'the password of the key game' release apk refused-key-apk
    must_refuse 'the password of the key game' release aab refused-key-aab
    export JASPER_ANDROID_KS_PASS=\"\$STORE_SECRET\" JASPER_ANDROID_KEY_PASS=\"\$KEY_SECRET\"
    must_refuse 'has no key named nosuchkey' wrong-alias apk refused-alias
    unset JASPER_ANDROID_KS_PASS JASPER_ANDROID_KEY_PASS
    must_refuse 'needs its password' release apk refused-no-password

    # no password in a log, a package or anything the builds left (the project copies list only paths)
    if grep -rlaF -e \"\$STORE_SECRET\" -e \"\$KEY_SECRET\" \$OUT --exclude=release.jks --exclude=release-ec.p12; then fail 'a password was written out'; fi
    if find \$OUT -name '.ks-pass' -o -name '.key-pass' | grep .; then fail 'a password file was left behind'; fi
    ok 'no password in any log, package or output'

    # ---- 3. what publish checks
    # (the debug project's output folders hold what was just built; prepare validates each)
    cat > \$OUT/debug/publishing.json <<JSON
{\"schemaVersion\": 1, \"releasesDir\": \"../releases\", \"targets\": {
  \"apk\": {\"platform\": \"android\", \"arch\": \"universal\", \"flavour\": \"$FLAVOUR\", \"buildDir\": \"../debug-apk\"},
  \"aab\": {\"platform\": \"android\", \"arch\": \"universal\", \"flavour\": \"$FLAVOUR\", \"buildDir\": \"../debug-aab\"}}}
JSON
    dbus-run-session -- build/linux/editor-install/bin/jasperengine-editor publish prepare --project \$OUT/debug --release s47 > \$OUT/logs/publish-prepare.log 2>&1 \
        || { tail -n 30 \$OUT/logs/publish-prepare.log; fail 'publish prepare refused the APK or the bundle'; }
    ok 'publish prepare validates the APK and the bundle'
"
echo "ok: the packaging, signing and bundletool checks passed"

[[ "${ANDROID_PACKAGING_NO_EMULATOR:-0}" == 1 ]] && { echo "PASS (no emulator: ANDROID_PACKAGING_NO_EMULATOR=1)"; exit 0; }

# ---- 4. on the emulator: the same picture from the APK and from the bundle
GOLDEN=tools/fixtures/unified-lighting/golden/android
FORBID=(--forbid "Failed to load cooked texture")
tools/android-emulator-test.sh "$OUT/debug-apk/UnifiedLighting.apk" --out "$OUT/emulator-apk" --timeout 600 --relaunch --golden "$GOLDEN" "${FORBID[@]}"
tools/android-emulator-test.sh "$OUT/debug-aab/UnifiedLighting.aab" --bundletool "$OUT/bundletool.jar" --out "$OUT/emulator-aab" --timeout 600 --golden "$GOLDEN" "${FORBID[@]}"
# (a run that lost its expectations would pass on whatever the screen held after some seconds)
for run in emulator-apk emulator-aab; do
    grep -q "^found .*Engine presented:" "$OUT/$run/result.txt" || { echo "FAIL: $run never saw 'Engine presented:'" >&2; exit 1; }
done

# ---- the source export, when asked for: the same picture from what Gradle builds
if [[ "${ANDROID_PACKAGING_SOURCE:-0}" == 1 ]]; then
    tools/build.sh exec android bash -euo pipefail -c "
        BT=\$(ls -d \$ANDROID_HOME/build-tools/* | sort -V | tail -n 1)
        dbus-run-session -- build/linux/editor-install/bin/jasperengine-editor export --project $OUT/debug --out $OUT/source --backend opengles \
            > $OUT/logs/source-export.log 2>&1 || { tail -n 30 $OUT/logs/source-export.log; echo 'FAIL: the source export'; exit 1; }
        cd $OUT/source/workspaces/androidstudio
        JASPERENGINE_ANDROID_ABIS=x86_64 CMAKE_CXX_COMPILER_LAUNCHER=ccache ./gradlew --no-daemon assembleDebug bundleDebug \
            > /src/$OUT/logs/source-gradle.log 2>&1 || { tail -n 40 /src/$OUT/logs/source-gradle.log; echo 'FAIL: Gradle on the source export'; exit 1; }
        \$BT/apksigner verify --print-certs app/build/outputs/apk/debug/app-debug.apk | grep -q 'CN=Android Debug' || { echo 'FAIL: the APK of the source export is not signed'; exit 1; }
        jarsigner -verify app/build/outputs/bundle/debug/app-debug.aab | grep -q 'jar verified' || { echo 'FAIL: the bundle of the source export is not signed'; exit 1; }
    "
    echo "ok: the source export builds with Gradle: a signed APK and bundle"
    SOURCE="$OUT/source/workspaces/androidstudio/app/build/outputs"
    # (both, whatever the first says: each is its own evidence)
    source_status=0
    tools/android-emulator-test.sh "$SOURCE/apk/debug/app-debug.apk" --out "$OUT/emulator-source-apk" --timeout 600 --golden "$GOLDEN" "${FORBID[@]}" || source_status=1
    tools/android-emulator-test.sh "$SOURCE/bundle/debug/app-debug.aab" --bundletool "$OUT/bundletool.jar" --out "$OUT/emulator-source-aab" --timeout 600 --golden "$GOLDEN" "${FORBID[@]}" || source_status=1
    for run in emulator-source-apk emulator-source-aab; do
        grep -q "^found .*Engine presented:" "$OUT/$run/result.txt" || { echo "FAIL: $run never saw 'Engine presented:'" >&2; exit 1; }
    done
    (( source_status == 0 )) || { echo "FAIL: the source export's runs on the emulator (result.txt in each)" >&2; exit 1; }
fi
echo "PASS: Android packaging (APK and bundle: built, signed, installed, run, compared)"
