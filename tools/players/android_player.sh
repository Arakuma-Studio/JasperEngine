#!/usr/bin/env bash
# Builds the prebuilt Android player with Gradle (-PjasperPlayer=true, app/build.gradle) and
# collects what the editor packages a game with, without an NDK, CMake or Gradle of its own
# (docs/specs/S30-shipping.md R13), into OUT (default build/players-build/android-<flavour>):
#
#   res-flat.zip     the player's compiled resources (aapt2 .flat files, the app's and its libraries')
#   stable-ids.txt   the ids aapt2 gave them (--emit-ids): the game's link gives the same ones, which
#                    the dex's R constants hold
#   classes*.dex     the player's code
#   lib/<abi>/*.so   the engine and the C++ runtime per ABI
#
#   tools/players/android_player.sh <release|development> [OUT] [gles3|vulkan]
#
# Runs inside the android build image (tools/build.sh players with PLAYER_PLATFORM=android). Then
# tools/players/package_player.py --platform android --android-dir OUT makes the runtime.
set -euo pipefail

FLAVOUR="${1:?release or development}"
OUT="${2:-build/players-build/android-$FLAVOUR}"
BACKEND="${3:-gles3}"
[[ "$BACKEND" == gles3 || "$BACKEND" == vulkan ]] || { echo "backend is gles3 or vulkan (got $BACKEND)" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$FLAVOUR" in
    release) TASK=assembleRelease; VARIANT=release ;;
    development) TASK=assembleDebug; VARIANT=debug ;;
    *) echo "flavour is release or development (got $FLAVOUR)" >&2; exit 1 ;;
esac
APP="$ROOT/engine/workspaces/androidstudio/app"

(cd "$ROOT/engine/workspaces/androidstudio" && ./gradlew --no-daemon -PjasperPlayer=true -PjasperBackend="$BACKEND" clean "$TASK")

rm -rf "$OUT"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

APK="$(find "$APP/build/outputs/apk/$VARIANT" -name '*.apk' | head -n 1)"
[[ -n "$APK" ]] || { echo "no APK under $APP/build/outputs/apk/$VARIANT" >&2; exit 1; }
IDS="$APP/build/jasperengine-player/stable-ids.txt"
[[ -s "$IDS" ]] || { echo "aapt2 wrote no $IDS (androidResources --emit-ids)" >&2; exit 1; }
cp "$IDS" "$OUT/stable-ids.txt"

python3 - "$APK" "$APP/build/intermediates" "$VARIANT" "$OUT" <<'PY'
import sys, zipfile
from pathlib import Path

apk, intermediates, variant, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
# the code and the native libraries as the APK holds them
with zipfile.ZipFile(apk) as zipped:
    for name in zipped.namelist():
        if (name.startswith("classes") and name.endswith(".dex")) or (name.startswith("lib/") and name.endswith(".so")):
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(zipped.read(name))
# the merged, compiled resources of the variant (AGP: merged_res/<variant>/merge<Variant>Resources)
flats = sorted(p for p in intermediates.glob(f"merged_res/{variant}/**/*.flat"))
if not flats:
    sys.exit(f"no merged_res/{variant} .flat files under {intermediates}")
with zipfile.ZipFile(out / "res-flat.zip", "w", zipfile.ZIP_STORED) as zipped:
    for flat in flats:
        zipped.write(flat, flat.name)
print(f"collected {len(flats)} resources, {sum(1 for _ in out.glob('classes*.dex'))} dex, "
      f"{sum(1 for _ in out.glob('lib/*/*.so'))} libraries into {out}")
PY
