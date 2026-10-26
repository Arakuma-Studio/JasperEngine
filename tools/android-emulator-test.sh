#!/usr/bin/env bash
# Install an APK on a headless Android emulator, launch it and check its logcat.
#
#   tools/android-emulator-test.sh <apk> [options]
#
#   --expect TEXT       a `logcat -v tag` line must contain TEXT, e.g. 'W/JasperEngine: [render] lag'
#                       (priority letter V D I W E F, then the tag: the priority and tag are checked
#                       with the text). Repeatable.
#   --expect-file FILE  one --expect per line ('#' comments and blank lines are skipped)
#   --ordered           the expected lines must arrive in the order given
#   --forbid TEXT       fail if a line contains TEXT (repeatable; default: none)
#   --timeout SECONDS   how long to wait for the expected lines after the launch (default 120)
#   --no-ready          do not require the engine's ready line (below)
#   --seconds SECONDS   with --no-ready and no --expect: how long the app runs before the checks
#                       (default 20)
#   --out DIR           results (default build/android-emulator): logcat.txt (-v tag, the run),
#                       logcat-full.txt (-v threadtime, everything), screen.png, emulator.log,
#                       result.txt
#
# Runs in docker/android-emulator.Dockerfile (the Android build image plus the emulator and an
# x86_64 system image, EMULATOR_API / EMULATOR_TAG, default 34 / aosp_atd), with --device /dev/kvm:
# the host needs KVM (Linux, /dev/kvm) and Docker. The emulator is headless (-no-window), renders
# with SwiftShader (GLES 3), starts from a wiped userdata each run and stops at the end. The APK
# needs the x86_64 ABI (tools/build.sh android builds every ABI). The app is launched with its
# LAUNCHER intent; the package comes from the APK (aapt2).
#
# Unless --no-ready, the run also expects the engine's own line 'I/JasperEngine: Engine ready:'
# (the property debug.jasperengine.ready_signal, which the runner sets, turns it on as
# JASPERENGINE_READY_SIGNAL does on the desktop): the native library loaded, the engine started
# and drew its scenes. An app that never gets there fails, and the native stacks of its threads
# (debuggerd -b) are written to stacks.txt.
#
# Exit 0: every expectation met (and the app did not crash). 1: an expectation missed, a forbidden
# line, or the app crashed (a FATAL EXCEPTION or a native crash of the package). 2: the test could
# not run (no KVM, the emulator did not boot, the APK did not install).
#
# EMULATOR_BOOT_TIMEOUT (default 600 s), EMULATOR_MEMORY (MB, default 3072), EMULATOR_CORES
# (default 4), EMULATOR_TMPFS_GB (the tmpfs the AVD lives on, default 10: only what the run
# writes takes memory) and JASPER_DOCKER_IMAGE (an emulator image id/tag, skips docker build)
# tune it.
#
# Example (S09: the logger's logcat sink):
#   ANDROID_PROJECT=core-utilities tools/build.sh android
#   tools/android-emulator-test.sh build/android-core-utilities/app-debug.apk --ordered \
#       --expect-file tools/test-projects/core-utilities/android-expect.txt
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

[[ $# -ge 1 ]] || usage
APK="$1"; shift
[[ "$APK" == -h || "$APK" == --help ]] && usage
EXPECTS=()
FORBIDS=()
ORDERED=0
READY=1
TIMEOUT=120
SECONDS_TO_RUN=20
OUT="build/android-emulator"
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
        --no-ready) READY=0; shift ;;
        --forbid) FORBIDS+=("$2"); shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) usage ;;
    esac
done

[[ -f "$APK" ]] || { echo "android-emulator-test: no APK at $APK" >&2; exit 2; }
[[ -e /dev/kvm ]] || { echo "android-emulator-test: no /dev/kvm here: the emulator needs KVM (servers A and C have it)" >&2; exit 2; }
[[ "$OUT" =~ ^build/[A-Za-z0-9_./-]+$ ]] || { echo "android-emulator-test: --out must be under build/ (got $OUT)" >&2; exit 2; }

# The APK and the results are seen through the /src mount
APK_ABS="$(cd "$(dirname "$APK")" && pwd)/$(basename "$APK")"
case "$APK_ABS" in
    "$ROOT"/*) APK_IN="/src/${APK_ABS#"$ROOT"/}" ;;
    *) mkdir -p "$ROOT/$OUT"; cp "$APK_ABS" "$ROOT/$OUT/app.apk"; APK_IN="/src/$OUT/app.apk" ;;
esac
mkdir -p "$ROOT/$OUT" "$ROOT/build/.cache/home"
rm -f "$ROOT/$OUT"/{logcat.txt,logcat-full.txt,screen.png,emulator.log,result.txt,stacks.txt}

if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE_ID="$JASPER_DOCKER_IMAGE"
else
    # (by tag: BuildKit resolves FROM by name; the emulator image is built right after)
    docker build -q -t jasperengine/android-build -f "$ROOT/docker/android.Dockerfile" "$ROOT/docker" >/dev/null
    IMAGE_ID="$(docker build -q -t jasperengine/android-emulator \
        -f "$ROOT/docker/android-emulator.Dockerfile" "$ROOT/docker")"
fi

# the expectations go in through a file: any character survives (the ready line last: it comes
# after what the scenes log as they start)
(( READY )) && EXPECTS+=("I/JasperEngine: Engine ready:")
printf '%s\n' ${EXPECTS[@]+"${EXPECTS[@]}"} > "$ROOT/$OUT/expect.txt"
printf '%s\n' ${FORBIDS[@]+"${FORBIDS[@]}"} > "$ROOT/$OUT/forbid.txt"

TTY_FLAGS=()
[[ -t 0 && -t 1 ]] && TTY_FLAGS=(-it)
status=0
docker run --rm ${TTY_FLAGS[@]+"${TTY_FLAGS[@]}"} \
    --device /dev/kvm --group-add "$(stat -c %g /dev/kvm)" \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT:/src" -w /src \
    -e HOME=/src/build/.cache/home \
    -e APK="$APK_IN" -e OUT="/src/$OUT" -e ORDERED="$ORDERED" -e TIMEOUT="$TIMEOUT" -e SECONDS_TO_RUN="$SECONDS_TO_RUN" \
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
        log "booting $IMAGE"
        emulator -avd jasper -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data \
            -gpu swiftshader_indirect -accel on -memory "$MEMORY" -cores "$CORES" -no-metrics \
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
        log "booted in $((SECONDS - start))s"
        # no screen lock, no animations: the app is in front at once
        adb shell input keyevent 82 >/dev/null 2>&1 || true
        adb shell settings put global window_animation_scale 0 >/dev/null 2>&1 || true
        adb shell settings put global transition_animation_scale 0 >/dev/null 2>&1 || true

        AAPT2="$(ls -d $ANDROID_HOME/build-tools/*/aapt2 | tail -1)"
        PACKAGE="$("$AAPT2" dump packagename "$APK" | tr -d "\r")"
        [[ -n "$PACKAGE" ]] || { log "no package name in $APK"; exit 2; }
        log "installing $PACKAGE"
        adb install -r -g "$APK" > "$OUT/install.log" 2>&1 || { log "install failed"; cat "$OUT/install.log"; exit 2; }

        adb logcat -c
        adb shell setprop debug.jasperengine.ready_signal 1
        log "launching $PACKAGE"
        adb shell monkey -p "$PACKAGE" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

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
                }" "$OUT/expect.txt" "$OUT/logcat.txt"
        }
        if grep -q . "$OUT/expect.txt"; then WAIT=$TIMEOUT; else WAIT=$SECONDS_TO_RUN; fi
        start=$SECONDS
        while true; do
            adb logcat -d -v tag > "$OUT/logcat.txt"
            adb logcat -d -v threadtime > "$OUT/logcat-full.txt"
            crashes="$(grep -E "FATAL EXCEPTION|Fatal signal|>>> $PACKAGE <<<" "$OUT/logcat-full.txt" | head -10 || true)"
            if grep -q . "$OUT/expect.txt" && check > /dev/null; then break; fi
            [[ -n "$crashes" ]] && break
            (( SECONDS - start > WAIT )) && break
            sleep 2
        done
        result=0
        check > "$OUT/result.txt" || result=1
        # no expectation (--no-ready): the app must at least still be running after its seconds
        if ! grep -q . "$OUT/expect.txt" && [[ -z "$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d "\r")" ]]; then
            echo "NOT RUNNING  $PACKAGE exited within ${WAIT}s" >> "$OUT/result.txt"
            result=1
        fi
        while IFS= read -r forbidden; do
            [[ -z "$forbidden" ]] && continue
            hit="$(grep -F -- "$forbidden" "$OUT/logcat.txt" | head -1 || true)"
            if [[ -n "$hit" ]]; then echo "FORBIDDEN    $hit" >> "$OUT/result.txt"; result=1; fi
        done < "$OUT/forbid.txt"
        if [[ -n "$crashes" ]]; then
            echo "$crashes" | sed "s/^/CRASH        /" >> "$OUT/result.txt"
            result=1
        fi
        if (( result )); then
            # where the app is: the native stack of every thread (a hang loading the library, in
            # JNI_OnLoad, in a static constructor...)
            pid="$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d "\r")"
            if [[ -n "$pid" ]]; then
                # (debuggerd wants root: the emulator images are userdebug builds)
                adb root >/dev/null 2>&1 && adb wait-for-device || true
                adb shell debuggerd -b "$pid" > "$OUT/stacks.txt" 2>&1 || true
                echo "STACKS       the app (pid $pid) is still running: its threads are in stacks.txt" >> "$OUT/result.txt"
            else
                echo "NO PROCESS   $PACKAGE is not running" >> "$OUT/result.txt"
            fi
            echo FAIL >> "$OUT/result.txt"
        else
            echo PASS >> "$OUT/result.txt"
        fi
        sed "s/^/android-emulator-test: /" "$OUT/result.txt"
        adb exec-out screencap -p > "$OUT/screen.png" 2>/dev/null || true
        exit $result
    ' || status=$?

echo "android-emulator-test: results in $OUT (logcat.txt, logcat-full.txt, screen.png, emulator.log, result.txt, stacks.txt on a failure)"
exit $status
