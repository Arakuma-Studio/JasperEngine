#!/usr/bin/env bash
# Two fingers touching the screen of the running emulator together, then lifting together: the
# Linux multi-touch protocol (type B) through `adb shell sendevent` on the emulator's touch device.
# tools/android-emulator-test.sh --two-finger-tap runs it inside its container, after the launch.
set -euo pipefail

# the input device nodes are root's: adbd as root (the emulator's aosp_atd image allows it; the
# restart can drop the first request, so it is asked again)
for _ in $(seq 1 10); do
    [[ "$(adb shell id -u 2>/dev/null | tr -d '\r')" == 0 ]] && break
    adb root >/dev/null 2>&1 || true
    sleep 1
    adb wait-for-device
done
[[ "$(adb shell id -u | tr -d '\r')" == 0 ]] || { echo "android-two-finger-tap: adb root failed (sendevent needs it)" >&2; exit 2; }

info="$(adb shell getevent -lp | tr -d '\r')"
# The emulator makes a multi-touch device per possible display (virtio_input_multi_touch_1..11);
# the first is the main display's. Any other multi-touch device when there is none of those.
device="$(awk '/^add device/ { current = $4 } /name:/ && $2 == "\"virtio_input_multi_touch_1\"" { print current; exit }' <<< "$info")"
[[ -n "$device" ]] || device="$(awk '/^add device/ { current = $4 } /ABS_MT_POSITION_X/ && current != "" { print current; exit }' <<< "$info")"
[[ -n "$device" ]] || { echo "android-two-finger-tap: no multi-touch input device" >&2; exit 2; }

axis_max() {
    awk -v code="$1" -v wanted="$device" '
        /^add device/ { current = $4 }
        current == wanted && index($0, code) {
            for (i = 1; i <= NF; i++) if ($i == "max") { value = $(i + 1); gsub(",", "", value); print value; exit }
        }' <<< "$info"
}
max_x="$(axis_max ABS_MT_POSITION_X)"
max_y="$(axis_max ABS_MT_POSITION_Y)"
[[ -n "$max_x" && -n "$max_y" ]] || { echo "android-two-finger-tap: no position range on $device" >&2; exit 2; }
x1=$(( max_x * 3 / 10 ))
x2=$(( max_x * 7 / 10 ))
y=$(( max_y / 2 ))

event() { printf 'sendevent %s %s %s %s;' "$device" "$1" "$2" "$3"; }
# EV_ABS 3: ABS_MT_SLOT 47, ABS_MT_TRACKING_ID 57 (4294967295 = -1: lifted), ABS_MT_POSITION_X 53,
# ABS_MT_POSITION_Y 54, ABS_MT_TOUCH_MAJOR 48, ABS_MT_PRESSURE 58 (Android takes a touch with no
# pressure on a device that reports it as hovering); EV_KEY 1 BTN_TOUCH 330; EV_SYN 0 SYN_REPORT 0
finger() { printf '%s' "$(event 3 47 "$1")$(event 3 57 "$2")$(event 3 53 "$3")$(event 3 54 "$4")$(event 3 48 8)$(event 3 58 512)"; }
down="$(finger 0 101 $x1 $y)$(finger 1 102 $x2 $y)$(event 1 330 1)$(event 0 0 0)"
up="$(event 3 47 0)$(event 3 57 4294967295)$(event 3 47 1)$(event 3 57 4294967295)$(event 1 330 0)$(event 0 0 0)"
echo "android-two-finger-tap: $device at ($x1, $y) and ($x2, $y) of ${max_x}x${max_y}"
# the device's axes, for the log
awk -v wanted="$device" '/^add device/ { current = $4 } current == wanted' <<< "$info" | sed 's/^/android-two-finger-tap:   /'

adb shell "$down"
sleep 0.3
adb shell "$up"
