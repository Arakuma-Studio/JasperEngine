#!/usr/bin/env bash
# Builds the prebuilt Android player with Gradle (-PjasperPlayer=true, app/build.gradle) and
# collects what the editor packages a game with, without an NDK, CMake or Gradle of its own
# (docs/specs/S30-shipping.md R13), into OUT (default build/players-build/android-<flavour>):
#
#   res-flat.zip     the player's compiled resources (aapt2 .flat files, the app's and its libraries')
#   stable-ids.txt   the ids aapt2 gave them (read from the APK with aapt2 dump resources): the
#                    game's link (--stable-ids) gives the same ones, which the dex's R constants hold
#   classes*.dex     the player's code
#   lib/<abi>/*.so   the engine and the C++ runtime per ABI, as the APK holds them (stripped in
#                    a release build)
#   symbols/lib/<abi>/*.so  the same libraries before stripping (the runtime's symbols)
#
#   tools/players/android_player.sh <release|development> [OUT] [gles3] [variant]
#
# variant: a release variant of engine/player/variants.json (default full; S34 R4)
#
# Runs inside the android build image (tools/build.sh players with PLAYER_PLATFORM=android). Then
# tools/players/package_player.py --platform android --android-dir OUT makes the runtime.
set -euo pipefail

FLAVOUR="${1:?release or development}"
OUT="${2:-build/players-build/android-$FLAVOUR}"
BACKEND="${3:-gles3}"
PLAYER_VARIANT="${4:-full}"
[[ "$BACKEND" == gles3 ]] || { echo "the backend is gles3 on Android: its platform layer makes an EGL context only (got $BACKEND)" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$FLAVOUR" in
    release) TASK=assembleRelease; VARIANT=release ;;
    development) TASK=assembleDebug; VARIANT=debug ;;
    *) echo "flavour is release or development (got $FLAVOUR)" >&2; exit 1 ;;
esac
APP="$ROOT/engine/workspaces/androidstudio/app"

# no `clean`: its native step needs the CMake AGP installs only while building; the APK of an
# earlier run goes, the merged resources are rewritten by the build
rm -rf "$APP/build/outputs/apk"
# a deprecation in our Gradle files fails the build (tools/ci/check_gradle_warnings.py)
GRADLE_LOG="$(mktemp)"
(cd "$ROOT/engine/workspaces/androidstudio" && ./gradlew --no-daemon -PjasperPlayer=true -PjasperBackend="$BACKEND" -PjasperPlayerVariant="$PLAYER_VARIANT" "$TASK") 2>&1 | tee "$GRADLE_LOG"
python3 "$ROOT/tools/ci/check_gradle_warnings.py" "$GRADLE_LOG"
rm -f "$GRADLE_LOG"

rm -rf "$OUT"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

APK="$(find "$APP/build/outputs/apk/$VARIANT" -name '*.apk' | head -n 1)"
[[ -n "$APK" ]] || { echo "no APK under $APP/build/outputs/apk/$VARIANT" >&2; exit 1; }
AAPT2="$(ls -d "$ANDROID_HOME"/build-tools/*/aapt2 2>/dev/null | sort -V | tail -n 1)"
[[ -x "$AAPT2" ]] || { echo "no aapt2 under $ANDROID_HOME/build-tools" >&2; exit 1; }
"$AAPT2" dump resources "$APK" > "$OUT/resources.txt"

python3 - "$APK" "$APP/build/intermediates" "$VARIANT" "$OUT" "$ROOT/engine/platform/android/player/AndroidManifest.template.xml" <<'PY'
import re, sys, zipfile
from pathlib import Path

apk, intermediates, variant, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
# aapt2's --stable-ids format, "<package>:<type>/<name> = 0x<id>", from `aapt2 dump resources`.
# The ids are named with the package a game is linked under: the template manifest's (the
# player's APK names its table after Gradle's applicationId, which aapt2 would not match)
template = Path(sys.argv[5]).read_text()
package = re.search(r'<manifest[^>]*\spackage="([^"]+)"', template).group(1)
ids = []
for line in (out / "resources.txt").read_text().splitlines():
    found = re.match(r"\s*resource (0x[0-9a-f]{8}) ([\w.]+/[\w.$-]+)", line)
    if found:
        ids.append(f"{package}:{found.group(2)} = {found.group(1)}")
if not ids:
    sys.exit("aapt2 dump resources listed no resource ids")
(out / "stable-ids.txt").write_text("\n".join(ids) + "\n")
(out / "resources.txt").unlink()
# the code and the native libraries as the APK holds them
with zipfile.ZipFile(apk) as zipped:
    for name in zipped.namelist():
        if (name.startswith("classes") and name.endswith(".dex")) or (name.startswith("lib/") and name.endswith(".so")):
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(zipped.read(name))
# every compiled resource AGP's link of the player read: the merged ones of the variant
# (merged_res: the app's files and everyone's values) and the libraries' precompiled files
# (Gradle's transform cache), from the inputs build.gradle listed (jasperengine-player/link-inputs.txt)
flats = sorted(intermediates.glob(f"merged_res/{variant}/**/*.flat"))
if not flats:
    sys.exit(f"no merged_res/{variant} .flat files under {intermediates}")
inputs = intermediates.parent / "jasperengine-player" / "link-inputs.txt"
if not inputs.is_file():
    sys.exit(f"no {inputs}: the player build lists the link's inputs (app/build.gradle, -PjasperPlayer=true)")
for line in inputs.read_text().splitlines():
    path = Path(line)
    if "merged_res" in path.parts:
        continue
    if path.is_dir():
        flats += sorted(path.rglob("*.flat"))
    elif path.suffix == ".flat":
        flats.append(path)
names = set()
with zipfile.ZipFile(out / "res-flat.zip", "w", zipfile.ZIP_STORED) as zipped:
    for flat in flats:
        # one entry per compiled resource (libraries may compile the same file twice)
        if flat.name in names:
            continue
        names.add(flat.name)
        zipped.write(flat, flat.name)
flats = sorted(names)
# the libraries before AGP stripped them: the runtime's symbols (R23), per ABI
unstripped = sorted(intermediates.glob(f"merged_native_libs/{variant}/**/lib/*/*.so"))
for library in unstripped:
    target = out / "symbols" / "lib" / library.parent.name / library.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(library.read_bytes())
print(f"collected {len(flats)} resources, {sum(1 for _ in out.glob('classes*.dex'))} dex, "
      f"{sum(1 for _ in out.glob('lib/*/*.so'))} libraries into {out}")
PY
