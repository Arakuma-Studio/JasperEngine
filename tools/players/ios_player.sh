#!/usr/bin/env bash
# Builds the prebuilt iOS player (docs/specs/S30-shipping.md R14) on a Mac with Xcode, into OUT
# (default build/players-build/ios-<flavour>):
#
#   JasperEnginePlayer.xcframework   the engine and the player, main() included, as one static
#                                    library per SDK: iphoneos arm64, iphonesimulator arm64+x86_64
#   template/                        the Xcode project a build stages the game into
#                                    (engine/platform/apple/ios/player-template, its storyboards,
#                                    the asset catalog, the engine's PrivacyInfo.xcprivacy and
#                                    include/JasperNative.h for the game's native plugins)
#
#   tools/players/ios_player.sh <release|development> [OUT] [variant]
#
# variant: a release variant of engine/player/variants.json (default full; S34 R4).
# Then tools/players/package_player.py --platform ios --arch arm64 --backend metal --ios-dir OUT.
set -euo pipefail

FLAVOUR="${1:?release or development}"
OUT="${2:-build/players-build/ios-$FLAVOUR}"
VARIANT="${3:-full}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$FLAVOUR" in
    release) DEV=OFF ;;
    development) DEV=ON ;;
    *) echo "flavour is release or development (got $FLAVOUR)" >&2; exit 1 ;;
esac
command -v xcodebuild >/dev/null || { echo "ios_player.sh needs Xcode (xcodebuild)" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
LIBS=()
for sdk in iphoneos iphonesimulator; do
    archs="arm64"
    [[ "$sdk" == iphonesimulator ]] && archs="arm64;x86_64"
    tree="$ROOT/build/players-build/ios-$sdk-$FLAVOUR-$VARIANT"
    cmake -S "$ROOT/engine" -B "$tree" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES="$archs" -DJASPERENGINE_PLAYER=ON -DGRAPHIC_BACKEND=metal \
        -DJASPERENGINE_DEVELOPMENT_BUILD="$DEV" -DJASPERENGINE_PLAYER_VARIANT="$VARIANT"
    cmake --build "$tree" --config Release --target jasperengine-player -- CODE_SIGNING_ALLOWED=NO
    # every static library of the tree (its libraries', and the engine's in Release/), merged into one;
    # nothing under build/: Xcode's intermediates hold the same objects again (per-architecture copies,
    # object libraries such as dav1d's), which -all_load would link twice. (No mapfile: a Mac's
    # /bin/bash is 3.2.)
    archives=()
    while IFS= read -r archive; do archives+=("$archive"); done < <(find "$tree" \( -path "*Release-$sdk*" -o -path "$tree/Release/*" \) -name '*.a' -not -path "$tree/build/*" | sort)
    repeated="$(for archive in "${archives[@]}"; do basename "$archive"; done | sort | uniq -d)"
    [[ -z "$repeated" ]] || { echo "the same library twice in $tree: $repeated" >&2; exit 1; }
    [[ " ${archives[*]} " == *"/libjasperengine.a "* ]] || { echo "no libjasperengine.a in $tree" >&2; exit 1; }
    [[ ${#archives[@]} -gt 0 ]] || { echo "no static libraries in $tree" >&2; exit 1; }
    mkdir -p "$tree/merged"
    libtool -static -o "$tree/merged/libJasperEnginePlayer.a" "${archives[@]}"
    LIBS+=(-library "$tree/merged/libJasperEnginePlayer.a")
done
xcodebuild -create-xcframework "${LIBS[@]}" -output "$OUT/JasperEnginePlayer.xcframework"

cp -R "$ROOT/engine/platform/apple/ios/player-template" "$OUT/template"
cp "$ROOT/engine/platform/apple/ios/Main.storyboard" "$ROOT/engine/platform/apple/ios/LaunchScreen.storyboard" "$OUT/template/"
cp -R "$ROOT/engine/workspaces/xcode/Assets.xcassets" "$OUT/template/Assets.xcassets"
# what a game's Objective-C/Swift native plugins import (the export compiles them into the template)
mkdir -p "$OUT/template/include"
cp "$ROOT/engine/platform/apple/JasperNative.h" "$OUT/template/include/"
echo "ios player: $OUT"
