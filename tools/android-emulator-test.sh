#!/usr/bin/env bash
# Install an APK on a headless Android emulator, launch it, check its logcat and what it shows.
#
#   tools/android-emulator-test.sh <apk or aab> [options]
#
#   An .aab goes through bundletool (--bundletool): build-apks for the connected device, signed
#   with the debug key, then install-apks, as Google Play would serve it.
#   --bundletool JAR    the bundletool jar for an .aab
#   --expect TEXT       a `logcat -v tag` line must contain TEXT, e.g. 'W/JasperEngine: [render] lag'
#                       (priority letter V D I W E F, then the tag: the priority and tag are checked
#                       with the text). Repeatable. {scene} stands for the run's --scene.
#   --expect-file FILE  one --expect per line ('#' comments and blank lines are skipped)
#   --ordered           the expected lines must arrive in the order given
#   --forbid TEXT       fail if a line contains TEXT (repeatable; default: none)
#   --timeout SECONDS   how long to wait for the expected lines after the launch (default 120)
#   --no-ready          do not require the engine's ready line (below)
#   --seconds SECONDS   with --no-ready and no --expect: how long the app runs before the checks
#                       (default 20)
#   --expect-native-crash require our report and the OS tombstone; a deliberate crash passes
#                       (implies --no-ready: the fixture may crash before its ready line)
#   --two-finger-tap    TAP_AFTER seconds (--tap-after, default 10) after the launch, two fingers
#                       touch the screen together and lift (tools/android-two-finger-tap.sh)
#   --tap-after SECONDS when --two-finger-tap taps
#   --relaunch          once the expected lines are in, change the font scale: Android relaunches
#                       the activity in the same process (the manifest does not handle fontScale),
#                       and the engine starts again in it. The run then needs a new view (the
#                       engine's 'Graphics:' line), the process still running and no crash; the
#                       font scale is restored after. The screenshot is the one before it: the
#                       relaunched scene is still loading after the new view.
#   --scene NAME        launch the app once per --scene (repeatable), with the property
#                       debug.jasperengine.scene set to NAME (a test project picks what it shows
#                       by it: tools/test-projects/render-gate); every run is checked on its own
#   --golden DIR        check what the screen shows: each run waits for the engine's
#                       'I/JasperEngine: Engine presented:' line (30 frames presented after the
#                       ready line), screenshots the device and fails when the screen is black or
#                       one colour (nothing drawn over the clear colour) or differs from
#                       DIR/<scene>.png (DIR/screen.png without --scene) beyond the tolerances of
#                       tools/android-screen-check.py. JASPER_UPDATE_GOLDEN=1 writes the goldens
#                       from the screenshots instead (they must still pass the content check).
#   --out DIR           results (default build/android-emulator): logcat.txt (-v tag, the run),
#                       logcat-full.txt (-v threadtime, everything), screen.png (<scene>.png),
#                       result.txt, stacks.txt on a failure; in DIR/<scene>/ for each --scene.
#                       emulator.log and the overall result.txt in DIR
#
# Runs in docker/android-emulator.Dockerfile (the Android build image plus the emulator and an
# x86_64 system image, EMULATOR_API / EMULATOR_TAG, default 34 / aosp_atd), with --device /dev/kvm:
# the host needs KVM (Linux, /dev/kvm) and Docker; the golden check runs on the host (python3).
# The emulator is headless (-no-window) and renders on the CPU: EMULATOR_GPU (default
# swangle_indirect: ANGLE on SwiftShader Vulkan, GLES 3.1, the goldens' renderer) is passed to
# -gpu; swiftshader_indirect is the older GLES 3.0 translator (docs/testing.md, "The emulator's
# GPU"). The AVD starts from a wiped userdata each run and stops at the end. The APK needs the x86_64 ABI (tools/build.sh android builds
# every ABI; ANDROID_ABIS=x86_64 only that one). The app is launched with its LAUNCHER intent;
# the package comes from the APK (aapt2).
#
# Unless --no-ready, the run also expects the engine's own line 'I/JasperEngine: Engine ready:'
# (the property debug.jasperengine.ready_signal, which the runner sets, turns it on as
# JASPERENGINE_READY_SIGNAL does on the desktop): the native library loaded, the engine started
# and drew its scenes. An app that never gets there fails, and the native stacks of its threads
# (debuggerd -b) are written to stacks.txt.
#
# Every run fails on what the GLES path logs when it goes wrong, whatever the expectations: an
# OpenGL error (NativeEngine reads glGetError after every frame; a debug build's sokol also
# asserts on one after each call), a shader that did not compile or link (sokol's GL backend,
# the runtime shader compiler), an incomplete framebuffer, an EGL error or a failed swap, a
# sokol panic, and libEGL's "call to OpenGL ES API with no current context".
#
# Exit 0: every expectation met (and the app did not crash). 1: an expectation missed, a forbidden
# or GLES error line, the app crashed (a FATAL EXCEPTION or a native crash of the package), or a
# screen check failed. 2: the test could not run (no KVM, the emulator did not boot, the APK did
# not install).
#
# EMULATOR_BOOT_TIMEOUT (default 600 s), EMULATOR_MEMORY (MB, default 3072), EMULATOR_CORES
# (default 4), EMULATOR_TMPFS_GB (the tmpfs the AVD lives on, default 10: only what the run
# writes takes memory) and JASPER_DOCKER_IMAGE (an emulator image id/tag, skips docker build)
# tune it.
#
# Examples (S09: the logger's logcat sink; BF3: rendering against goldens):
#   ANDROID_PROJECT=core-utilities tools/build.sh android
#   tools/android-emulator-test.sh build/android-core-utilities/app-debug.apk --ordered \
#       --expect-file tools/test-projects/core-utilities/android-expect.txt
#   ANDROID_PROJECT=render-gate ANDROID_ABIS=x86_64 tools/build.sh android
#   tools/android-emulator-test.sh build/android-render-gate/app-debug.apk \
#       --expect-file tools/test-projects/render-gate/android-expect.txt \
#       --scene lit3d --scene ui2d --scene htmlui --golden tools/test-projects/render-gate/golden
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    sed -n '2,79p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

[[ $# -ge 1 ]] || usage
APK="$1"; shift
[[ "$APK" == -h || "$APK" == --help ]] && usage
EXPECTS=()
FORBIDS=()
SCENES=()
GOLDEN=""
ORDERED=0
EXPECT_NATIVE_CRASH=0
READY=1
TIMEOUT=120
SECONDS_TO_RUN=20
TWO_FINGER_TAP=0
TAP_AFTER=10
RELAUNCH=0
OUT="build/android-emulator"
BUNDLETOOL=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --expect) EXPECTS+=("$2"); shift 2 ;;
        --expect-file)
            while IFS= read -r line || [[ -n "$line" ]]; do
                [[ -z "${line// }" || "$line" == \#* ]] && continue
                EXPECTS+=("$line")
            done < "$2"
            shift 2 ;;
        --ordered) ORDERED=1; shift ;;
        --expect-native-crash) EXPECT_NATIVE_CRASH=1; READY=0; shift ;;
        --no-ready) READY=0; shift ;;
        --forbid) FORBIDS+=("$2"); shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
        --two-finger-tap) TWO_FINGER_TAP=1; shift ;;
        --tap-after) TAP_AFTER="$2"; shift 2 ;;
        --relaunch) RELAUNCH=1; shift ;;
        --scene)
            [[ "$2" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "android-emulator-test: --scene takes a name of letters, digits, _ and - (got $2)" >&2; exit 2; }
            SCENES+=("$2"); shift 2 ;;
        --golden) GOLDEN="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --bundletool) BUNDLETOOL="$2"; shift 2 ;;
        *) usage ;;
    esac
done

[[ -f "$APK" ]] || { echo "android-emulator-test: no APK at $APK" >&2; exit 2; }
if [[ "$APK" == *.aab ]]; then
    [[ -f "$BUNDLETOOL" ]] || { echo "android-emulator-test: an .aab needs --bundletool <jar> (got '$BUNDLETOOL')" >&2; exit 2; }
fi
[[ -e /dev/kvm ]] || { echo "android-emulator-test: no /dev/kvm here: the emulator needs KVM (servers A and C have it)" >&2; exit 2; }
[[ "$OUT" =~ ^build/[A-Za-z0-9_./-]+$ ]] || { echo "android-emulator-test: --out must be under build/ (got $OUT)" >&2; exit 2; }
if [[ -n "$GOLDEN" ]]; then
    (( READY )) || { echo "android-emulator-test: --golden needs the ready line (no --no-ready)" >&2; exit 2; }
    command -v python3 >/dev/null || { echo "android-emulator-test: --golden needs python3 on the host" >&2; exit 2; }
    mkdir -p "$GOLDEN"
    GOLDEN="$(cd "$GOLDEN" && pwd)"
fi

# The APK and the results are seen through the /src mount
APK_ABS="$(cd "$(dirname "$APK")" && pwd)/$(basename "$APK")"
case "$APK_ABS" in
    "$ROOT"/*) APK_IN="/src/${APK_ABS#"$ROOT"/}" ;;
    *) mkdir -p "$ROOT/$OUT"; cp "$APK_ABS" "$ROOT/$OUT/app.${APK_ABS##*.}"; APK_IN="/src/$OUT/app.${APK_ABS##*.}" ;;
esac
BUNDLETOOL_IN=""
if [[ -n "$BUNDLETOOL" ]]; then
    mkdir -p "$ROOT/$OUT"
    cp "$BUNDLETOOL" "$ROOT/$OUT/bundletool.jar"
    BUNDLETOOL_IN="/src/$OUT/bundletool.jar"
fi
mkdir -p "$ROOT/$OUT" "$ROOT/build/.cache/home"
rm -f "$ROOT/$OUT"/{logcat.txt,logcat-full.txt,screen.png,emulator.log,result.txt,stacks.txt}
rm -f "$ROOT/$OUT"/screen.*.png
for scene in ${SCENES[@]+"${SCENES[@]}"}; do rm -rf "${ROOT:?}/$OUT/$scene"; done

if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE_ID="$JASPER_DOCKER_IMAGE"
else
    # (by tag: BuildKit resolves FROM by name; the emulator image is built right after)
    docker build -q -t jasperengine/android-build -f "$ROOT/docker/android.Dockerfile" "$ROOT/docker" >/dev/null
    IMAGE_ID="$(docker build -q -t jasperengine/android-emulator \
        -f "$ROOT/docker/android-emulator.Dockerfile" "$ROOT/docker")"
fi

# the expectations go in through a file: any character survives (the ready line last: it comes
# after what the scenes log as they start; the presented line after it)
(( READY )) && EXPECTS+=("I/JasperEngine: Engine ready:")
[[ -n "$GOLDEN" ]] && EXPECTS+=("I/JasperEngine: Engine presented:")
printf '%s\n' ${EXPECTS[@]+"${EXPECTS[@]}"} > "$ROOT/$OUT/expect-all.txt"
printf '%s\n' ${FORBIDS[@]+"${FORBIDS[@]}"} > "$ROOT/$OUT/forbid.txt"

TTY_FLAGS=()
[[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
status=0
docker run --rm ${TTY_FLAGS[@]+"${TTY_FLAGS[@]}"} \
    --device /dev/kvm --group-add "$(stat -c %g /dev/kvm)" \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT:/src" -w /src \
    -e HOME=/src/build/.cache/home \
    -e APK="$APK_IN" -e BUNDLETOOL="$BUNDLETOOL_IN" -e OUT="/src/$OUT" -e ORDERED="$ORDERED" -e TIMEOUT="$TIMEOUT" -e SECONDS_TO_RUN="$SECONDS_TO_RUN" \
    -e TWO_FINGER_TAP="$TWO_FINGER_TAP" -e TAP_AFTER="$TAP_AFTER" -e RELAUNCH="$RELAUNCH" \
    -e EXPECT_NATIVE_CRASH="$EXPECT_NATIVE_CRASH" \
    -e SCENES="${SCENES[*]+"${SCENES[*]}"}" -e SCREENSHOT_ON_PRESENT="$([[ -n "$GOLDEN" ]] && echo 1 || echo 0)" \
    -e GPU="${EMULATOR_GPU:-swangle_indirect}" \
    -e BOOT_TIMEOUT="${EMULATOR_BOOT_TIMEOUT:-600}" -e MEMORY="${EMULATOR_MEMORY:-3072}" -e CORES="${EMULATOR_CORES:-4}" \
    --tmpfs "/avd:rw,exec,size=${EMULATOR_TMPFS_GB:-10}g,mode=1777" \
    "$IMAGE_ID" bash -euo pipefail -c '
        log() { echo "android-emulator-test: $*"; }
        # on a tmpfs: the emulator wants room for its sparse userdata image (about 7 GB), of
        # which a run writes a few hundred MB; neither the host disk nor the container layer pays
        export ANDROID_AVD_HOME=/avd
        export ANDROID_EMULATOR_HOME=/avd
        IMAGE="system-images;android-${EMULATOR_API};${EMULATOR_TAG};x86_64"
        echo no | avdmanager --silent create avd --force -n jasper -k "$IMAGE" -d pixel_6 >/dev/null
        log "booting $IMAGE (-gpu $GPU)"
        emulator -avd jasper -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data \
            -gpu "$GPU" -accel on -memory "$MEMORY" -cores "$CORES" -no-metrics \
            > "$OUT/emulator.log" 2>&1 &
        EMULATOR_PID=$!
        cleanup() {
            adb emu kill >/dev/null 2>&1 || true
            sleep 2
            kill "$EMULATOR_PID" >/dev/null 2>&1 || true
            wait "$EMULATOR_PID" 2>/dev/null || true
        }
        trap cleanup EXIT

        start=$SECONDS
        until [[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d "\r")" == 1 ]]; do
            if ! kill -0 "$EMULATOR_PID" 2>/dev/null; then
                log "the emulator exited while booting"; tail -30 "$OUT/emulator.log"; exit 2
            fi
            if (( SECONDS - start > BOOT_TIMEOUT )); then
                log "not booted after ${BOOT_TIMEOUT}s"; tail -30 "$OUT/emulator.log"; exit 2
            fi
            sleep 3
        done
        log "booted in $((SECONDS - start))s ($(grep -m1 "Graphics API Version" "$OUT/emulator.log" | sed "s/.*Graphics API Version //"))"
        if [[ "$EXPECT_NATIVE_CRASH" == 1 ]]; then
            adb root >/dev/null 2>&1 || true
            adb wait-for-device
        fi
        # no screen lock, no animations: the app is in front at once
        adb shell input keyevent 82 >/dev/null 2>&1 || true
        adb shell settings put global window_animation_scale 0 >/dev/null 2>&1 || true
        adb shell settings put global transition_animation_scale 0 >/dev/null 2>&1 || true

        AAPT2="$(ls -d $ANDROID_HOME/build-tools/*/aapt2 | tail -1)"
        if [[ "$APK" == *.aab ]]; then
            KS="$HOME/.android/debug.keystore"
            if [[ ! -f "$KS" ]]; then
                mkdir -p "$HOME/.android"
                keytool -genkeypair -keystore "$KS" -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 \
                    -validity 10000 -dname "CN=Android Debug,O=Android,C=US" 2> /dev/null
            fi
            PACKAGE="$(java -jar "$BUNDLETOOL" dump manifest --bundle="$APK" --xpath=/manifest/@package | tr -d "\r")"
            [[ -n "$PACKAGE" ]] || { log "no package name in $APK"; exit 2; }
            log "bundletool build-apks for the device, then install-apks: $PACKAGE"
            {
                java -jar "$BUNDLETOOL" build-apks --bundle="$APK" --output="$OUT/app.apks" --overwrite --connected-device --adb="$(command -v adb)" \
                    --ks="$KS" --ks-pass=pass:android --ks-key-alias=androiddebugkey --key-pass=pass:android &&
                java -jar "$BUNDLETOOL" install-apks --apks="$OUT/app.apks" --adb="$(command -v adb)"
            } > "$OUT/install.log" 2>&1 || { log "bundletool failed"; cat "$OUT/install.log"; exit 2; }
            unzip -l "$OUT/app.apks" >> "$OUT/install.log"
        else
            PACKAGE="$("$AAPT2" dump packagename "$APK" | tr -d "\r")"
            [[ -n "$PACKAGE" ]] || { log "no package name in $APK"; exit 2; }
            log "installing $PACKAGE"
            adb install -r -g "$APK" > "$OUT/install.log" 2>&1 || { log "install failed"; cat "$OUT/install.log"; exit 2; }
        fi
        adb shell setprop debug.jasperengine.ready_signal 1

        # what the GLES path logs when it goes wrong (NativeEngine, sokol GL, ShaderPool, libEGL)
        GLES_ERRORS="^[A-Z]/JasperEngine: .*(OpenGL error|OPENGL ERRORS|shader compilation failed|shader linking failed|GL_SHADER_COMPILATION_FAILED|GL_SHADER_LINKING_FAILED|framebuffer completeness check failed|GL_FRAMEBUFFER_STATUS|failed to compile at runtime|Shader build failed|Shader resource creation failed|EGL error|SwappyGL_swap failed|\(PANIC\))|^E/libEGL *: call to OpenGL ES API with no current context"

        # every expected line (in order with ORDERED=1) in the logcat; a crash ends the wait
        # check: one line per expectation (found / OUT OF ORDER / MISSING), exit 1 if one is missing
        check() {
            awk -v ordered="$ORDERED" "
                FILENAME == ARGV[1] { if (length(\$0)) expected[++n] = \$0; next }
                { lines[++m] = \$0 }
                END {
                    position = 1; failed = 0
                    for (i = 1; i <= n; i++) {
                        found = 0
                        for (j = (ordered == 1 ? position : 1); j <= m; j++) if (index(lines[j], expected[i])) { found = j; break }
                        if (found) { position = found + 1; print \"found        \" lines[found]; continue }
                        anywhere = 0
                        for (j = 1; j <= m; j++) if (index(lines[j], expected[i])) anywhere = 1
                        print (anywhere ? \"OUT OF ORDER \" : \"MISSING      \") expected[i]
                        failed = 1
                    }
                    exit failed
                }" "$DIR/expect.txt" "$DIR/logcat.txt"
        }

        # one launch: SCENE (empty: no --scene) in DIR, its screenshot SHOT; 1 when it failed
        run() {
            mkdir -p "$DIR"
            # (another file: without --scene DIR is OUT, and sed would empty what it reads)
            sed "s/{scene}/$SCENE/g" "$OUT/expect-all.txt" > "$DIR/expect.txt"
            adb shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
            adb logcat -c
            adb shell setprop debug.jasperengine.scene "\"$SCENE\""
            log "launching $PACKAGE${SCENE:+ (scene $SCENE)}"
            adb shell monkey -p "$PACKAGE" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
            if [[ "$TWO_FINGER_TAP" == 1 ]]; then
                ( sleep "$TAP_AFTER"; bash /src/tools/android-two-finger-tap.sh > "$DIR/tap.log" 2>&1 ) &
            fi

            if grep -q . "$DIR/expect.txt"; then WAIT=$TIMEOUT; else WAIT=$SECONDS_TO_RUN; fi
            start=$SECONDS
            while true; do
                adb logcat -d -v tag > "$DIR/logcat.txt"
                adb logcat -d -v threadtime > "$DIR/logcat-full.txt"
                crashes="$(grep -E "FATAL EXCEPTION|Fatal signal|>>> $PACKAGE <<<" "$DIR/logcat-full.txt" | head -10 || true)"
                if grep -q . "$DIR/expect.txt" && check > /dev/null; then break; fi
                [[ -n "$crashes" ]] && break
                (( SECONDS - start > WAIT )) && break
                sleep 2
            done
            # what the screen shows now: with --golden, at least 30 frames after the ready line
            adb exec-out screencap -p > "$DIR/$SHOT" 2>/dev/null || true
            # an activity relaunched in the same process: the engine starts again in it
            relaunched=""
            if [[ "$RELAUNCH" == 1 && -z "$crashes" ]]; then
                views="$(grep -c "I/JasperEngine: Graphics:" "$DIR/logcat.txt" || true)"
                pid="$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d "\r")"
                log "relaunching the activity in the same process (font scale 1.15)"
                adb shell settings put system font_scale 1.15
                start=$SECONDS
                while true; do
                    sleep 2
                    adb logcat -d -v tag > "$DIR/logcat.txt"
                    adb logcat -d -v threadtime > "$DIR/logcat-full.txt"
                    crashes="$(grep -E "FATAL EXCEPTION|Fatal signal|>>> $PACKAGE <<<" "$DIR/logcat-full.txt" | head -10 || true)"
                    [[ -n "$crashes" ]] && break
                    (( $(grep -c "I/JasperEngine: Graphics:" "$DIR/logcat.txt" || true) > views )) && break
                    (( SECONDS - start > TIMEOUT )) && break
                done
                # frames of the new instance before the checks
                [[ -z "$crashes" ]] && sleep 10
                adb shell settings put system font_scale 1.0
                after="$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d "\r")"
                if (( $(grep -c "I/JasperEngine: Graphics:" "$DIR/logcat.txt" || true) <= views )); then
                    relaunched="MISSING      a new view after the relaunch (I/JasperEngine: Graphics:)"
                elif [[ -z "$after" ]]; then
                    relaunched="NOT RUNNING  $PACKAGE after the relaunch"
                elif [[ "$after" != "$pid" ]]; then
                    relaunched="MISSING      a relaunch in the same process (pid $pid became $after)"
                else
                    relaunched="found        relaunched in process $pid: a new view, still running"
                fi
            fi
            adb logcat -d -v tag > "$DIR/logcat.txt"
            adb logcat -d -v threadtime > "$DIR/logcat-full.txt"

            result=0
            : > "$DIR/result.txt"
            if [[ "$EXPECT_NATIVE_CRASH" == 1 ]]; then
                # debuggerd writes after the first fatal log line; wait for the report and
                # tombstone rather than ending the test when the signal is first observed.
                for attempt in $(seq 1 30); do
                    adb logcat -d -v tag > "$DIR/logcat.txt"
                    adb logcat -d -v threadtime > "$DIR/logcat-full.txt"
                    adb shell run-as "$PACKAGE" find files -name crash.txt > "$DIR/crash-paths.txt" 2>/dev/null || true
                    adb shell ls /data/tombstones > "$DIR/tombstones.txt" 2>/dev/null || true
                    if grep -q "JASPERENGINE CRASH:" "$DIR/logcat.txt" && grep -q "crash.txt" "$DIR/crash-paths.txt" &&
                       grep -q tombstone_ "$DIR/tombstones.txt"; then break; fi
                    sleep 1
                done
                REPORT_PATH="$(head -1 "$DIR/crash-paths.txt" | tr -d "\r")"
                if [[ -n "$REPORT_PATH" ]]; then
                    adb exec-out run-as "$PACKAGE" cat "$REPORT_PATH" > "$DIR/crash-report.txt" 2>/dev/null || true
                fi
                # An aosp_atd image runs adbd as root. Preserve the OS artifact too, so the
                # package and faulting thread can be checked against our saved report.
                adb pull /data/tombstones "$DIR/tombstones" >/dev/null 2>&1 || true
                if grep -q "JASPERENGINE CRASH:" "$DIR/logcat.txt" && grep -q "signal:" "$DIR/crash-report.txt" &&
                   grep -q "^#0 libjasperengine-android.so+0x" "$DIR/crash-report.txt" &&
                   grep -R -q "$PACKAGE" "$DIR/tombstones"; then
                    echo "found        engine report and debuggerd tombstone" >> "$DIR/result.txt"
                else
                    echo "MISSING      engine report, game-module frame or matching debuggerd tombstone" >> "$DIR/result.txt"
                    result=1
                fi
            fi
            # A native fault can be observed before our logcat lines have been collected.
            # Check the final snapshot after the report/tombstone wait, not the early one.
            check >> "$DIR/result.txt" || result=1
            if [[ -n "$relaunched" ]]; then
                echo "$relaunched" >> "$DIR/result.txt"
                [[ "$relaunched" == found* ]] || result=1
            fi
            # no expectation (--no-ready): the app must at least still be running after its seconds
            if [[ "$EXPECT_NATIVE_CRASH" != 1 ]] && ! grep -q . "$DIR/expect.txt" &&
               [[ -z "$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d "\r")" ]]; then
                echo "NOT RUNNING  $PACKAGE exited within ${WAIT}s" >> "$DIR/result.txt"
                result=1
            fi
            while IFS= read -r forbidden; do
                [[ -z "$forbidden" ]] && continue
                hit="$(grep -F -- "$forbidden" "$DIR/logcat.txt" | head -1 || true)"
                if [[ -n "$hit" ]]; then echo "FORBIDDEN    $hit" >> "$DIR/result.txt"; result=1; fi
            done < "$OUT/forbid.txt"
            gles="$(grep -E -- "$GLES_ERRORS" "$DIR/logcat.txt" | head -10 || true)"
            if [[ -n "$gles" ]]; then
                echo "$gles" | sed "s/^/GLES ERROR   /" >> "$DIR/result.txt"
                result=1
            fi
            if [[ "$EXPECT_NATIVE_CRASH" != 1 && -n "$crashes" ]]; then
                echo "$crashes" | sed "s/^/CRASH        /" >> "$DIR/result.txt"
                result=1
            fi
            if (( result )); then
                # where the app is: the native stack of every thread (a hang loading the library, in
                # JNI_OnLoad, in a static constructor...)
                pid="$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d "\r")"
                if [[ -n "$pid" ]]; then
                    # (debuggerd wants root: the emulator images are userdebug builds)
                    adb root >/dev/null 2>&1 && adb wait-for-device || true
                    adb shell debuggerd -b "$pid" > "$DIR/stacks.txt" 2>&1 || true
                    echo "STACKS       the app (pid $pid) is still running: its threads are in stacks.txt" >> "$DIR/result.txt"
                else
                    echo "NO PROCESS   $PACKAGE is not running" >> "$DIR/result.txt"
                fi
                echo FAIL >> "$DIR/result.txt"
            else
                echo PASS >> "$DIR/result.txt"
            fi
            sed "s/^/android-emulator-test: ${SCENE:+[$SCENE] }/" "$DIR/result.txt"
            return $result
        }

        failed=0
        if [[ -z "$SCENES" ]]; then
            SCENE="" DIR="$OUT" SHOT=screen.png
            run || failed=1
        else
            for SCENE in $SCENES; do
                DIR="$OUT/$SCENE" SHOT="$SCENE.png"
                run || failed=1
            done
        fi
        exit $failed
    ' || status=$?

# the screenshots against their goldens (the content check first: a black or clear-colour-only
# screen fails whatever the golden says)
if [[ -n "$GOLDEN" && $status -ne 2 ]]; then
    if (( ${#SCENES[@]} )); then
        shots=()
        for scene in "${SCENES[@]}"; do shots+=("$scene:$ROOT/$OUT/$scene/$scene.png"); done
    else
        shots=("screen:$ROOT/$OUT/screen.png")
    fi
    for shot in "${shots[@]}"; do
        name="${shot%%:*}"; png="${shot#*:}"
        dir="$(dirname "$png")"
        if [[ ! -s "$png" ]]; then
            echo "SCREEN       no screenshot for $name" | tee -a "$dir/result.txt" | sed "s/^/android-emulator-test: [$name] /"
            status=1
            continue
        fi
        screen_status=0
        python3 "$ROOT/tools/android-screen-check.py" "$png" --golden "$GOLDEN/$name.png" > "$dir/screen-check.txt" || screen_status=$?
        sed "s/^/SCREEN       /" "$dir/screen-check.txt" >> "$dir/result.txt"
        sed "s/^/android-emulator-test: [$name] SCREEN /" "$dir/screen-check.txt"
        if (( screen_status )); then
            echo "FAIL (screen)" >> "$dir/result.txt"
            (( status == 0 )) && status=1
        fi
    done
fi

if (( ${#SCENES[@]} )); then
    for scene in "${SCENES[@]}"; do
        echo "$scene: $(grep -E "^(PASS|FAIL)" "$ROOT/$OUT/$scene/result.txt" 2>/dev/null | tr "\n" " ")"
    done > "$ROOT/$OUT/result.txt"
fi
echo "android-emulator-test: $( (( status == 0 )) && echo PASS || echo FAIL)"
echo "android-emulator-test: results in $OUT (logcat.txt, logcat-full.txt, the screenshots, emulator.log, result.txt, stacks.txt on a failure${SCENES[*]+; per scene in $OUT/<scene>/})"
exit $status
