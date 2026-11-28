#!/usr/bin/env bash
# Patch And Run on a real emulator (docs/specs/build-and-run-on-device.md AC10).
#
#   tools/ci/android_patch_run.sh [--out build/android-patch-run]
#
# Builds a Development Build of a copy of tools/fixtures/tween-curve with the prebuilt Android
# development runtime (build/players/android-universal-gles3-development, built first when missing:
# PLAYER_PLATFORM=android PLAYER_FLAVOURS=development PLAYER_ANDROID_ABIS=x86_64 tools/build.sh players)
# and runs it with `jasperengine-editor build --run-device emulator-5554` on a headless emulator
# (docker/android-emulator.Dockerfile, KVM). Then a Lua string edit and the same command again,
# which must patch: logcat shows the new string, the installed APK's SHA-256 (`pm path`, then
# sha256sum on the device) is the same as before (no reinstall), and the patch step's time is
# reported against the 15 s budget (a [bench] line; JASPER_MCP_PERF_SLACK=<factor> enforces it, as
# perfSlack() does). The editor (build/linux/editor-install, tools/build.sh linux) runs inside the
# emulator container, which has the Android SDK and the JDK the build needs; the libraries it needs
# beyond that image come from the Linux build image (both Ubuntu 22.04).
#
# Exit 0 pass, 1 fail, 2 could not run (no KVM, no editor, the emulator did not boot).

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT=build/android-patch-run
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "android_patch_run: unknown argument $1" >&2; exit 2 ;;
    esac
done
[[ "$OUT" =~ ^build/[A-Za-z0-9_./-]+$ ]] || { echo "android_patch_run: --out must be under build/" >&2; exit 2; }
[[ -e /dev/kvm ]] || { echo "android_patch_run: no /dev/kvm (the emulator needs KVM)" >&2; exit 2; }
EDITOR="$ROOT/build/linux/editor-install/bin/jasperengine-editor"
[[ -x "$EDITOR" ]] || { echo "android_patch_run: no editor at $EDITOR (tools/build.sh linux)" >&2; exit 2; }

RUNTIME=android-universal-gles3-development
if [[ ! -f "$ROOT/build/players/$RUNTIME/player-info.json" ]]; then
    echo "android_patch_run: building the $RUNTIME runtime (x86_64)"
    (cd "$ROOT" && PLAYER_PLATFORM=android PLAYER_FLAVOURS=development PLAYER_BACKENDS=gles3 PLAYER_ANDROID_ABIS=x86_64 tools/build.sh players)
fi

rm -rf "${ROOT:?}/$OUT"
mkdir -p "$ROOT/$OUT/libs" "$ROOT/build/.cache/home"
# the project: a copy outside the repository's fixtures, its Lua printing a marker we change
cp -R "$ROOT/tools/fixtures/tween-curve" "$ROOT/$OUT/project"
printf 'print("patch-run marker one")\n' | cat - "$ROOT/$OUT/project/TweenCurveCheck.lua" > "$ROOT/$OUT/check.lua"
mv "$ROOT/$OUT/check.lua" "$ROOT/$OUT/project/TweenCurveCheck.lua"

# the editor's libraries the emulator image lacks, from the Linux build image
LINUX_IMAGE="${JASPER_LINUX_IMAGE:-jasperengine/linux-build}"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src "$LINUX_IMAGE" bash -c '
    ldd build/linux/editor-install/bin/jasperengine-editor | awk "/=> \// {print \$3}" | while read -r lib; do
        case "$lib" in */libc.so*|*/libm.so*|*/libpthread.so*|*/libdl.so*|*/ld-linux*) ;; *) cp -L "$lib" "'"$OUT"'/libs/" ;; esac
    done'

IMAGE="${JASPER_DOCKER_IMAGE:-jasperengine/android-emulator}"
status=0
docker run --rm \
    --device /dev/kvm --group-add "$(stat -c %g /dev/kvm)" \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT:/src" -w /src \
    -e HOME=/src/build/.cache/home -e OUT="/src/$OUT" -e PERF_SLACK="${JASPER_MCP_PERF_SLACK:-0}" \
    --tmpfs "/avd:rw,exec,size=${EMULATOR_TMPFS_GB:-10}g,mode=1777" \
    "$IMAGE" bash -euo pipefail -c '
        log() { echo "android_patch_run: $*"; }
        export ANDROID_AVD_HOME=/avd ANDROID_EMULATOR_HOME=/avd
        echo no | avdmanager --silent create avd --force -n jasper -k "system-images;android-${EMULATOR_API};${EMULATOR_TAG};x86_64" -d pixel_6 >/dev/null
        emulator -avd jasper -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data -gpu swangle_indirect -accel on \
            -memory 3072 -cores 4 -no-metrics > "$OUT/emulator.log" 2>&1 &
        EMULATOR_PID=$!
        trap "adb emu kill >/dev/null 2>&1 || true; sleep 2; kill $EMULATOR_PID >/dev/null 2>&1 || true" EXIT
        start=$SECONDS
        until [[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d "\r")" == 1 ]]; do
            kill -0 "$EMULATOR_PID" 2>/dev/null || { log "the emulator exited while booting"; tail -30 "$OUT/emulator.log"; exit 2; }
            (( SECONDS - start > 600 )) && { log "not booted after 600 s"; exit 2; }
            sleep 3
        done
        log "booted in $((SECONDS - start)) s"
        adb shell input keyevent 82 >/dev/null 2>&1 || true
        export LD_LIBRARY_PATH="$OUT/libs" JASPER_PLAYERS_DIR=/src/build/players
        cd "$OUT"
        build() {
            env -u DISPLAY /src/build/linux/editor-install/bin/jasperengine-editor build --project project --platform android --development \
                --out game-android --run-device emulator-5554 --json > "$1.json" 2> "$1.log" || { log "$1 failed"; tail -40 "$1.log"; cat "$1.json"; exit 1; }
        }
        package() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"installed\"])" "$1.json"; }
        apk_sha() {
            local pkg path
            pkg="$(adb shell pm list packages -3 | tr -d "\r" | sed "s/package://" | head -1)"
            path="$(adb shell pm path "$pkg" | tr -d "\r" | sed "s/package://" | head -1)"
            adb shell sha256sum "$path" | tr -d "\r" | cut -d" " -f1
        }
        adb logcat -c
        build first
        [[ "$(package first)" == full ]] || { log "the first run did not install in full: $(cat first.json)"; exit 1; }
        sleep 10
        adb logcat -d -v tag > logcat-first.txt
        grep -q "patch-run marker one" logcat-first.txt || { log "the first run did not print its marker"; tail -40 logcat-first.txt; exit 1; }
        before="$(apk_sha)"
        sed -i "s/patch-run marker one/patch-run marker two/" project/TweenCurveCheck.lua
        adb logcat -c
        t0=$(date +%s.%N)
        build second
        t1=$(date +%s.%N)
        [[ "$(package second)" == patched ]] || { log "the second run did not patch: $(cat second.json)"; exit 1; }
        ok=0
        for _ in $(seq 1 30); do
            adb logcat -d -v tag > logcat-second.txt
            if grep -q "patch-run marker two" logcat-second.txt; then ok=1; break; fi
            sleep 1
        done
        (( ok )) || { log "the patched run does not print the new string"; tail -60 logcat-second.txt; exit 1; }
        grep -q "patch-run marker one" logcat-second.txt && { log "the patched run still prints the old string"; exit 1; }
        after="$(apk_sha)"
        [[ "$before" == "$after" ]] || { log "the APK changed ($before -> $after): it was reinstalled"; exit 1; }
        seconds=$(python3 -c "print(round($t1 - $t0, 2))")
        # (the whole command: the cook is in it too, so this bounds the time from the end of the cook to the game ready)
        echo "[bench] Patch And Run (cook, push, restart) of a one-file change: ${seconds} s (budget 15 s)"
        grep -h "run-as\|cp -R\|denied" second.log | tail -5 || true
        if python3 -c "import sys; s=float(sys.argv[1]); sys.exit(0 if s <= 0 or $seconds <= 15 * s else 1)" "$PERF_SLACK"; then
            log "PASS (APK $before unchanged, the new string in logcat)"
        else
            log "FAIL: the patch took ${seconds} s, over 15 s x $PERF_SLACK"; exit 1
        fi
    ' || status=$?
exit "$status"
