#!/usr/bin/env bash
# The prebuilt Android path end to end (docs/specs/S30-shipping.md R13, the Definition of Done's
# emulator check): tools/fixtures/unified-lighting built into an APK by the editor in the Android
# image (the SDK's aapt2, zipalign and apksigner and a JDK: no NDK, CMake or Gradle) with the
# android-universal-gles3-<flavour> runtime, then installed and run on the emulator
# (tools/android-emulator-test.sh: the ready line, and a screen that is not one colour).
#
#   tools/build.sh linux                                   # the editor install
#   PLAYER_PLATFORM=android PLAYER_ANDROID_ABIS=x86_64 PLAYER_FLAVOURS=development tools/build.sh players
#   tools/ci/android_prebuilt.sh [release|development]     # default development
#
# Needs KVM for the emulator (servers A and C). Results in build/android-prebuilt/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
FLAVOUR="${1:-development}"
[[ "$FLAVOUR" == release || "$FLAVOUR" == development ]] || { echo "flavour is release or development" >&2; exit 2; }
[[ -d "build/players/android-universal-gles3-$FLAVOUR" ]] || { echo "no android-universal-gles3-$FLAVOUR runtime in build/players" >&2; exit 2; }
[[ -x build/linux/editor-install/bin/jasperengine-editor ]] || { echo "no editor install: tools/build.sh linux" >&2; exit 2; }

OUT=build/android-prebuilt
rm -rf "$OUT"
mkdir -p "$OUT"
cp -r tools/fixtures/unified-lighting "$OUT/project"
# the emulator's ABI only (the runtime above was built for it)
cat >> "$OUT/project/project.yaml" <<'YAML'
android:
  architectures:
    armeabi-v7a: false
    arm64-v8a: false
    x86: false
YAML
DEV_FLAG=""
[[ "$FLAVOUR" == development ]] && DEV_FLAG="--development"
tools/build.sh exec android bash -c "dbus-run-session -- env JASPER_PLAYERS_DIR=/src/build/players \
    build/linux/editor-install/bin/jasperengine-editor build --project $OUT/project --platform android --android-format apk $DEV_FLAG --out $OUT/game \
    | tee $OUT/build.log"
grep -q "^method: prebuilt" "$OUT/build.log" || { echo "FAIL: not built with the prebuilt player" >&2; exit 1; }
APK="$(sed -n 's/^executable: //p' "$OUT/build.log" | head -1)"
APK="${APK#/src/}"
[[ -f "$APK" ]] || { echo "FAIL: no APK ($APK)" >&2; exit 1; }
echo "APK: $APK ($(stat -c %s "$APK") bytes)"
tools/android-emulator-test.sh "$APK" --out "$OUT/emulator" --timeout 600
