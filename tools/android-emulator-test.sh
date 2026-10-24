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
#   --seconds SECONDS   with no --expect: how long the app runs before the checks (default 20)
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
# Exit 0: every expectation met (and the app did not crash). 1: an expectation missed, a forbidden
# line, or the app crashed (a FATAL EXCEPTION or a native crash of the package). 2: the test could
# not run (no KVM, the emulator did not boot, the APK did not install).
#
# EMULATOR_BOOT_TIMEOUT (default 600 s), EMULATOR_MEMORY (MB, default 3072), EMULATOR_CORES
# (default 4) and JASPER_DOCKER_IMAGE (an emulator image id/tag, skips docker build) tune it.
#
# Example (S09: the logger's logcat sink):
#   ANDROID_PROJECT=core-utilities tools/build.sh android
#   tools/android-emulator-test.sh build/android-core-utilities/app-debug.apk --ordered \
#       --expect-file tools/test-projects/core-utilities/android-expect.txt
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    sed -n '2,41p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

[[ $# -ge 1 ]] || usage
APK="$1"; shift
[[ "$APK" == -h || "$APK" == --help ]] && usage
EXPECTS=()
FORBIDS=()
ORDERED=0
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
rm -f "$ROOT/$OUT"/{logcat.txt,logcat-full.txt,screen.png,emulator.log,result.txt}

if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE_ID="$JASPER_DOCKER_IMAGE"
else
    BASE_ID="$(docker build -q -t jasperengine/android-build -f "$ROOT/docker/android.Dockerfile" "$ROOT/docker")"
    IMAGE_ID="$(docker build -q --build-arg BASE="$BASE_ID" -t jasperengine/android-emulator \
        -f "$ROOT/docker/android-emulator.Dockerfile" "$ROOT/docker")"
fi

# the expectations go in through a file: any character survives
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
    "$IMAGE_ID" bash -euo pipefail -c '
        log() { echo "android-emulator-test: $*"; }
        export ANDROID_AVD_HOME="$(mktemp -d)"
        export ANDROID_EMULATOR_HOME="$ANDROID_AVD_HOME"
        IMAGE="system-images;android-${EMULATOR_API};${EMULATOR_TAG};x86_64"
        echo no | avdmanager --silent create avd --force -n jasper -k "$IMAGE" -d pixel_6 >/dev/null
        log "booting $IMAGE"
        emulator -avd jasper -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data \
            -gpu swiftshader_indirect -accel on -memory "$MEMORY" -cores "$CORES" -no-metrics > "$OUT/emulator.log" 2>&1 &
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
        log "launching $PACKAGE"
        adb shell monkey -p "$PACKAGE" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

        # every expected line (in order with ORDERED=1) in the logcat; a crash ends the wait
        if python3 - "$OUT" "$PACKAGE" <<"PY"
import os, subprocess, sys, time
out, package = sys.argv[1], sys.argv[2]
expects = [l for l in open(os.path.join(out, "expect.txt")).read().split("\n") if l]
forbids = [l for l in open(os.path.join(out, "forbid.txt")).read().split("\n") if l]
ordered = os.environ["ORDERED"] == "1"
deadline = time.monotonic() + (float(os.environ["TIMEOUT"]) if expects else float(os.environ["SECONDS_TO_RUN"]))

def logcat(fmt):
    return subprocess.run(["adb", "logcat", "-d", "-v", fmt], capture_output=True, text=True, errors="replace").stdout

def check(lines):
    found, position = [], 0
    for text in expects:
        start = position if ordered else 0
        index = next((i for i in range(start, len(lines)) if text in lines[i]), None)
        found.append(index)
        if index is not None:
            position = index + 1
    return found

def crashed(full):
    return [l for l in full.splitlines() if ("FATAL EXCEPTION" in l or "Fatal signal" in l or ">>> " + package in l)]

while True:
    lines = logcat("tag").splitlines()
    found = check(lines)
    full = logcat("threadtime")
    crashes = crashed(full)
    if (expects and all(i is not None for i in found)) or crashes or time.monotonic() > deadline:
        break
    time.sleep(2)

open(os.path.join(out, "logcat.txt"), "w").write("\n".join(lines) + "\n")
open(os.path.join(out, "logcat-full.txt"), "w").write(full)
report, failed = [], False
for text, index in zip(expects, found):
    if index is None:
        earlier = any(text in l for l in lines)
        report.append(("OUT OF ORDER " if earlier else "MISSING ") + text)
        failed = True
    else:
        report.append("found        " + lines[index])
for text in forbids:
    hits = [l for l in lines if text in l]
    if hits:
        report.append("FORBIDDEN    " + hits[0])
        failed = True
for line in crashes[:10]:
    report.append("CRASH        " + line)
    failed = True
report.append("FAIL" if failed else "PASS")
open(os.path.join(out, "result.txt"), "w").write("\n".join(report) + "\n")
print("\n".join("android-emulator-test: " + r for r in report))
sys.exit(1 if failed else 0)
PY
        then result=0; else result=$?; fi
        adb exec-out screencap -p > "$OUT/screen.png" 2>/dev/null || true
        exit $result
    ' || status=$?

echo "android-emulator-test: results in $OUT (logcat.txt, logcat-full.txt, screen.png, emulator.log, result.txt)"
exit $status
