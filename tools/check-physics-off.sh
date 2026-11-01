#!/usr/bin/env bash
# Builds the standalone engine (cmake -S engine: the library and its sample project, what an
# exported game builds) with each physics backend switched off: 3D off, 2D off, both off. The
# Exporter writes JASPERENGINE_PHYSICS_2D / _3D OFF for projects that do not use them, so every
# file of a backend must compile out with its switch (guard with #ifdef JASPERENGINE_PHYSICS_3D /
# _2D, or list the file in the backend's source list in engine/CMakeLists.txt).
#
#   tools/check-physics-off.sh [-j N] [variant...]     variants: 3d-off 2d-off both-off (default all)
#
# Build dirs: build/physics-off-<variant> (kept: a second run is incremental). Exit 0 when every
# variant links; the failing variants' errors are printed.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS=4
if [ "${1:-}" = "-j" ]; then JOBS="$2"; shift 2; fi
VARIANTS=("$@")
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=(3d-off 2d-off both-off)
LAUNCHER=()
command -v ccache > /dev/null && LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)

status=0
for variant in "${VARIANTS[@]}"; do
    case "$variant" in
        3d-off) flags=(-DJASPERENGINE_PHYSICS_2D=ON -DJASPERENGINE_PHYSICS_3D=OFF) ;;
        2d-off) flags=(-DJASPERENGINE_PHYSICS_2D=OFF -DJASPERENGINE_PHYSICS_3D=ON) ;;
        both-off) flags=(-DJASPERENGINE_PHYSICS_2D=OFF -DJASPERENGINE_PHYSICS_3D=OFF) ;;
        *) echo "unknown variant: $variant (3d-off, 2d-off, both-off)"; exit 2 ;;
    esac
    dir="$ROOT/build/physics-off-$variant"
    log="$dir.log"
    if ! cmake -S "$ROOT/engine" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "${LAUNCHER[@]}" "${flags[@]}" > "$log" 2>&1; then
        echo "FAIL $variant: configure (see $log)"; tail -20 "$log"; status=1; continue
    fi
    if cmake --build "$dir" -j "$JOBS" -- -k 0 >> "$log" 2>&1; then
        warnings=$(grep -c "warning:" "$log" || true)
        echo "PASS $variant (warnings: $warnings)"
    else
        echo "FAIL $variant (see $log):"
        grep -E "error:|^FAILED" "$log" | sort -u | head -40
        status=1
    fi
done
exit $status
