#!/usr/bin/env bash
# iOS packaging end to end on a Mac (docs/specs/S47-packaging-tests.md R4): what a user pressing Build
# gets, run for real. tools/fixtures/unified-lighting goes through the editor with the prebuilt iOS
# player, then:
#
#   1. Simulator: the editor's Xcode project built by xcodebuild into an unsigned .app; a simulator
#      device of this script's own (created, booted headless, deleted after: no window) installs and
#      launches it, the game draws with Metal, reaches its ready line and a screenshot shows it (not a blank
#      screen; compared with tools/fixtures/unified-lighting/golden/ios/screen.png with
#      IOS_PACKAGING_GOLDEN=1, the renderer the golden was taken on).
#   2. Device, no team: the build compiles and links for arm64 iOS, stops before signing, warns
#      W-IOS-NO-TEAM and keeps the Xcode project.
#   3. Device, a team this Mac has no account for: the build fails on signing, and says what to do.
#   4. IPA: an unsigned archive of the same project, then -exportArchive as far as it goes without an
#      Apple developer account (it stops at signing; what it said is in logs/export-archive.log).
#   5. publish prepare validates the simulator and the device folders.
#
#   tools/players/ios_player.sh development build/players-build/ios-development
#   python3 tools/players/package_player.py --platform ios --arch arm64 --backend metal --flavour development \
#       --variant full --ios-dir build/players-build/ios-development --out build/players
#   JASPER_EDITOR=<the editor binary> tools/ci/ios_packaging.sh [release|development]
#
# Xcode: DEVELOPER_DIR when set. Otherwise the editor finds it by itself, which step 1 also checks when
# xcode-select is on the Command Line Tools. JASPER_PLAYERS_DIR (default build/players). Results in
# build/ios-packaging/. Exit 0: every step as above. 1: a step failed. 2: it could not run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
FLAVOUR="${1:-development}"
[[ "$FLAVOUR" == release || "$FLAVOUR" == development ]] || { echo "flavour is release or development" >&2; exit 2; }
EDITOR="${JASPER_EDITOR:-}"
[[ -x "$EDITOR" ]] || { echo "set JASPER_EDITOR to the editor binary (got '$EDITOR')" >&2; exit 2; }
export JASPER_PLAYERS_DIR="${JASPER_PLAYERS_DIR:-$ROOT/build/players}"
[[ -d "$JASPER_PLAYERS_DIR/ios-arm64-metal-$FLAVOUR" ]] || { echo "no ios-arm64-metal-$FLAVOUR runtime in $JASPER_PLAYERS_DIR" >&2; exit 2; }

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }

OUT="$ROOT/build/ios-packaging"
rm -rf "$OUT"
mkdir -p "$OUT/logs"
DEV_FLAG=""
[[ "$FLAVOUR" == development ]] && DEV_FLAG="--development"

# this script's own tools (simctl, xcodebuild) need a full Xcode whatever xcode-select says; the
# editor, when DEVELOPER_DIR was not set, is left to find it (see EDITOR_ENV)
EDITOR_ENV=(env)
if [[ -z "${DEVELOPER_DIR:-}" ]]; then
    case "$(xcode-select -p 2>/dev/null)" in
        *CommandLineTools*|"")
            [[ -d /Applications/Xcode.app ]] || { echo "no Xcode: xcode-select is on the Command Line Tools and /Applications has no Xcode.app" >&2; exit 2; }
            export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
            EDITOR_ENV=(env -u DEVELOPER_DIR)
            ;;
    esac
fi
xcodebuild -version | tee "$OUT/logs/xcode-version.txt"

make_project() {
    local name="$1"; shift
    cp -R tools/fixtures/unified-lighting "$OUT/$name"
    {
        echo 'ios:'
        echo '  bundleIdentifier: com.jasperengine.packaging'
        printf '%s\n' "$@"
    } >> "$OUT/$name/project.yaml"
}
build() {  # project, output, more arguments
    local project="$1" output="$2"; shift 2
    "${EDITOR_ENV[@]}" "$EDITOR" build --project "$OUT/$project" --platform ios --method prebuilt $DEV_FLAG --out "$OUT/$output" "$@" \
        > "$OUT/logs/$output.log" 2>&1
}
make_project game
python3 tools/ci/freeze_unified_lighting.py "$OUT/game"
make_project team '  teamId: ABCDE12345'

# ---- 1. the simulator
build game sim --ios-simulator || { tail -n 40 "$OUT/logs/sim.log"; fail "the simulator build"; }
APP="$(sed -n 's/^executable: //p' "$OUT/logs/sim.log" | head -n 1)"
[[ -d "$APP" && "$APP" == *.app ]] || fail "the simulator build gave no .app ($APP)"
BUNDLE="$(plutil -extract CFBundleIdentifier raw "$APP/Info.plist")"
[[ "$BUNDLE" == com.jasperengine.packaging ]] || fail "the .app's bundle id is $BUNDLE"
ok "xcodebuild made $APP for the simulator"

RUNTIME="$(xcrun simctl list runtimes -j | python3 -c "
import json, sys
ios = [r for r in json.load(sys.stdin)['runtimes'] if r['platform'] == 'iOS' and r['isAvailable']]
print(ios[-1]['identifier'] if ios else '')")"
[[ -n "$RUNTIME" ]] || { echo "no iOS simulator runtime: Xcode > Settings > Components" >&2; exit 2; }
DEVICE_TYPE="$(xcrun simctl list devicetypes -j | python3 -c "
import json, os, sys
names = [t for t in json.load(sys.stdin)['devicetypes'] if t['name'].startswith('iPhone')]
wanted = os.environ.get('IOS_PACKAGING_DEVICE', 'iPhone 17')
exact = [t for t in names if t['name'] == wanted]
print((exact or names[-1:])[0]['identifier'])")"
UDID="$(xcrun simctl create "je-packaging-$$" "$DEVICE_TYPE" "$RUNTIME")"
cleanup() {
    xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true
    xcrun simctl delete "$UDID" >/dev/null 2>&1 || true
}
trap cleanup EXIT
echo "simulator device $UDID: $DEVICE_TYPE on $RUNTIME"
xcrun simctl boot "$UDID"
xcrun simctl bootstatus "$UDID" -b > "$OUT/logs/simulator-boot.log" 2>&1 || fail "the simulator did not boot"
xcrun simctl install "$UDID" "$APP" || fail "simctl install"
ok "installed"
# the engine logs to the unified log on iOS (AppleLogSink); stdout has only what scripts print
EXECUTABLE="$(plutil -extract CFBundleExecutable raw "$APP/Info.plist")"
xcrun simctl spawn "$UDID" log stream --level info --style compact --predicate "process == \"$EXECUTABLE\"" > "$OUT/logs/game-log.txt" 2>&1 &
LOG_STREAM=$!
cleanup() {
    kill "$LOG_STREAM" >/dev/null 2>&1 || true
    xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true
    xcrun simctl delete "$UDID" >/dev/null 2>&1 || true
}
sleep 2
# (the launched app's environment comes from SIMCTL_CHILD_)
SIMCTL_CHILD_JASPERENGINE_READY_SIGNAL=1 xcrun simctl launch --stdout="$OUT/logs/game-stdout.txt" --stderr="$OUT/logs/game-stderr.txt" \
    "$UDID" "$BUNDLE" > "$OUT/logs/launch.txt" 2>&1 || { cat "$OUT/logs/launch.txt"; fail "simctl launch"; }
ok "launched: $(cat "$OUT/logs/launch.txt")"
waited=0
until grep -qs "Engine ready:" "$OUT/logs/game-log.txt"; do
    sleep 2
    waited=$((waited + 2))
    if (( waited > 300 )); then
        tail -n 20 "$OUT/logs/game-log.txt" "$OUT/logs/game-stdout.txt" "$OUT/logs/game-stderr.txt" || true
        fail "no 'Engine ready:' line after ${waited}s"
    fi
done
grep -q "Graphics: Metal" "$OUT/logs/game-log.txt" || fail "the game does not say it draws with Metal"
ok "the engine is ready after ${waited}s: $(grep -m1 "Engine ready:" "$OUT/logs/game-log.txt" | sed 's/.*Engine ready/Engine ready/')"
# ('Engine presented:' is Android's: here some seconds of frames after the ready line)
sleep 5
xcrun simctl io "$UDID" screenshot "$OUT/screen.png" > "$OUT/logs/screenshot.txt" 2>&1 || { cat "$OUT/logs/screenshot.txt"; fail "simctl io screenshot"; }
xcrun simctl spawn "$UDID" launchctl list > "$OUT/logs/launchctl.txt" 2>&1 || true
grep -q "UIKitApplication:$BUNDLE" "$OUT/logs/launchctl.txt" || fail "the game is no longer running"
GOLDEN_ARGS=()
if [[ "${IOS_PACKAGING_GOLDEN:-0}" == 1 ]]; then
    mkdir -p tools/fixtures/unified-lighting/golden/ios
    GOLDEN_ARGS=(--golden tools/fixtures/unified-lighting/golden/ios/screen.png)
fi
python3 tools/android-screen-check.py "$OUT/screen.png" ${GOLDEN_ARGS[@]+"${GOLDEN_ARGS[@]}"} | tee "$OUT/logs/screen-check.txt" || fail "the screen check"
xcrun simctl terminate "$UDID" "$BUNDLE" || true
cleanup
trap - EXIT
ok "the game runs on the simulator and shows its scene: $OUT/screen.png"

# ---- 2. a device, no team
build game device-noteam || { tail -n 40 "$OUT/logs/device-noteam.log"; fail "the device build without a team"; }
grep -q "W-IOS-NO-TEAM" "$OUT/logs/device-noteam.log" || fail "no W-IOS-NO-TEAM warning"
grep -q "Team ID" "$OUT/logs/device-noteam.log" || fail "the warning does not name the Team ID setting"
DEVICE_APP="$(sed -n 's/^executable: //p' "$OUT/logs/device-noteam.log" | head -n 1)"
[[ -d "$DEVICE_APP" ]] || fail "the device build gave no .app ($DEVICE_APP)"
BINARY="$DEVICE_APP/$(plutil -extract CFBundleExecutable raw "$DEVICE_APP/Info.plist")"
lipo -archs "$BINARY" | grep -q arm64 || fail "not an arm64 binary: $(lipo -archs "$BINARY")"
xcrun vtool -show-build "$BINARY" | grep -q "platform IOS" || fail "not an iOS device binary"
if codesign -dv "$DEVICE_APP" 2>&1 | grep -q "TeamIdentifier=[A-Z0-9]"; then fail "signed although no team was set"; fi
[[ -d "$OUT/device-noteam/UnifiedLighting/JasperEnginePlayer.xcodeproj" ]] || fail "no Xcode project in the output"
ok "no team: compiled and linked for a device, not signed, W-IOS-NO-TEAM, the Xcode project kept"

# ---- 3. a device, a team without an account here: the signing's message
if build team device-team; then
    IPA="$(sed -n 's/^executable: //p' "$OUT/logs/device-team.log" | head -n 1)"
    [[ -f "$IPA" ]] || fail "a team build that passed gave no .ipa ($IPA)"
    ok "this Mac can sign for the team: $IPA"
else
    grep -Eq "Xcode has no account for the team|no certificate or provisioning profile|signing needs a team" "$OUT/logs/device-team.log" ||
        { tail -n 30 "$OUT/logs/device-team.log"; fail "the signing failure does not say what to do"; }
    ok "a team this Mac has no account for: $(grep -Em1 "Xcode has no account for the team|no certificate or provisioning profile|signing needs a team" "$OUT/logs/device-team.log")"
fi

# ---- 4. the IPA path, as far as it goes without an account
PROJECT="$OUT/device-noteam/UnifiedLighting/JasperEnginePlayer.xcodeproj"
ARCHIVE="$OUT/ipa/Game.xcarchive"
mkdir -p "$OUT/ipa"
xcodebuild -project "$PROJECT" -scheme JasperEnginePlayer -configuration Release -destination generic/platform=iOS -archivePath "$ARCHIVE" \
    archive CODE_SIGNING_ALLOWED=NO > "$OUT/logs/archive.log" 2>&1 || { tail -n 30 "$OUT/logs/archive.log"; fail "an unsigned archive"; }
[[ -d "$ARCHIVE/Products/Applications" ]] || fail "the archive holds no application"
ok "an unsigned archive of the exported project: $ARCHIVE"
cat > "$OUT/ipa/ExportOptions.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>method</key>
	<string>development</string>
	<key>teamID</key>
	<string>ABCDE12345</string>
</dict>
</plist>
PLIST
if xcodebuild -exportArchive -archivePath "$ARCHIVE" -exportOptionsPlist "$OUT/ipa/ExportOptions.plist" -exportPath "$OUT/ipa/export" \
    > "$OUT/logs/export-archive.log" 2>&1; then
    ok "exportArchive made an .ipa"
else
    echo "NOT RUN TO THE END: -exportArchive stops here without an Apple developer account: $(grep -m1 "error:" "$OUT/logs/export-archive.log" || tail -n 1 "$OUT/logs/export-archive.log")"
fi

# ---- 5. what publish checks
cat > "$OUT/game/publishing.json" <<JSON
{"schemaVersion": 1, "releasesDir": "../releases", "targets": {
  "simulator": {"platform": "ios", "arch": "arm64", "flavour": "$FLAVOUR", "buildDir": "../sim"},
  "device": {"platform": "ios", "arch": "arm64", "flavour": "$FLAVOUR", "buildDir": "../device-noteam"}}}
JSON
"$EDITOR" publish prepare --project "$OUT/game" --release s47 > "$OUT/logs/publish-prepare.log" 2>&1 ||
    { tail -n 30 "$OUT/logs/publish-prepare.log"; fail "publish prepare refused the iOS builds"; }
ok "publish prepare validates the iOS builds"

echo "PASS: iOS packaging (simulator run and shown; device to signing; IPA as far as it goes without an account)"
