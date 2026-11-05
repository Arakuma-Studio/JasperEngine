#!/usr/bin/env bash
# Builds the standalone engine (cmake -S engine: the library and its sample project, what an
# exported game builds) with optional modules switched off. A Compile build and the prebuilt player
# variants leave out what a project cannot reach (docs/specs/S34-player-size.md), so every file of a
# module must compile out with its switch (guard with its #ifdef, or list the file in the module's
# source list in engine/CMakeLists.txt), and a source must include what it uses itself: a header
# that reaches it only through a module's code is gone when that module is off.
#
#   tools/check-physics-off.sh [-j N] [variant...]     (no variant: all of them)
#
# Variants: <module>-off for each module of engine/cmake/JasperPlayerVariants.cmake (3d-off and
# 2d-off are physics3d-off and physics2d-off), both-off (both physics backends), all-off (every
# module: what tools/check-changed builds when a header under engine/core changes), and
# variant-<name> for each release player of engine/player/variants.json. A module added to those
# files joins these variants without a change here.
#
# Build dirs: build/physics-off-<variant> (kept: a second run is incremental). No optimizer, and no
# debug information but in variant-<name>, which links as a release player does (LTO, split symbols):
# this checks what compiles and links, and ccache reuses every source a module's switch does not
# change between the variants; tools/build.sh players builds the real thing.
# WERROR=1 makes warnings errors, as tools/build.sh does. Exit 0 when every variant links; the
# failing variants' errors are printed.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-4}"
if [ "${1:-}" = "-j" ]; then JOBS="$2"; shift 2; fi

# "<module> <option>" per line
MODULES="$(sed -n 's/^set(JASPERENGINE_MODULE_OPTION_\([a-z0-9_]*\) \(JASPERENGINE_[A-Z0-9_]*\))$/\1 \2/p' "$ROOT/engine/cmake/JasperPlayerVariants.cmake")"
PLAYERS="$(python3 -I -c "import json, sys; print(' '.join(v['name'] for v in json.load(open(sys.argv[1]))['variants']))" "$ROOT/engine/player/variants.json")"
if [ -z "$MODULES" ] || [ -z "$PLAYERS" ]; then echo "no modules or player variants found"; exit 2; fi
option_of() { awk -v module="$1" '$1 == module { print $2 }' <<< "$MODULES"; }

VARIANTS=("$@")
if [ ${#VARIANTS[@]} -eq 0 ]; then
    VARIANTS=(all-off both-off)
    for module in $(awk '{ print $1 }' <<< "$MODULES"); do VARIANTS+=("$module-off"); done
    for player in $PLAYERS; do VARIANTS+=("variant-$player"); done
fi
LAUNCHER=()
command -v ccache > /dev/null && LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
WARNINGS=()
[ "${WERROR:-0}" = 1 ] && WARNINGS=(-DJASPERENGINE_WARNINGS=ON -DJASPERENGINE_WERROR=ON)

status=0
for variant in "${VARIANTS[@]}"; do
    # (a player needs a Release tree: a Debug one is a development player, always the full one)
    type=Debug
    case "$variant" in
        3d-off) flags=(-DJASPERENGINE_PHYSICS_2D=ON -DJASPERENGINE_PHYSICS_3D=OFF) ;;
        2d-off) flags=(-DJASPERENGINE_PHYSICS_2D=OFF -DJASPERENGINE_PHYSICS_3D=ON) ;;
        both-off) flags=(-DJASPERENGINE_PHYSICS_2D=OFF -DJASPERENGINE_PHYSICS_3D=OFF) ;;
        all-off)
            flags=()
            for option in $(awk '{ print $2 }' <<< "$MODULES"); do flags+=("-D$option=OFF"); done
            ;;
        variant-*)
            player="${variant#variant-}"
            if [[ " $PLAYERS " != *" $player "* ]]; then echo "unknown player variant: $player ($PLAYERS)"; exit 2; fi
            type=Release
            flags=(-DJASPERENGINE_PLAYER=ON "-DJASPERENGINE_PLAYER_VARIANT=$player")
            ;;
        *-off)
            option="$(option_of "${variant%-off}")"
            if [ -z "$option" ]; then echo "unknown variant: $variant"; exit 2; fi
            flags=("-D$option=OFF")
            ;;
        *) echo "unknown variant: $variant (<module>-off, 3d-off, 2d-off, both-off, all-off, variant-<player>)"; exit 2 ;;
    esac
    dir="$ROOT/build/physics-off-$variant"
    log="$dir.log"
    if ! cmake -S "$ROOT/engine" -B "$dir" -G Ninja "-DCMAKE_BUILD_TYPE=$type" -DCMAKE_C_FLAGS_DEBUG=-O0 -DCMAKE_CXX_FLAGS_DEBUG=-O0 \
            "-DCMAKE_C_FLAGS_RELEASE=-O0 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG" \
            ${LAUNCHER[@]+"${LAUNCHER[@]}"} ${WARNINGS[@]+"${WARNINGS[@]}"} "${flags[@]}" > "$log" 2>&1; then
        echo "FAIL $variant: configure (see $log)"; tail -20 "$log"; status=1; continue
    fi
    if cmake --build "$dir" -j "$JOBS" -- -k 0 >> "$log" 2>&1; then
        warnings=$(grep -c "warning:" "$log" || true)
        echo "PASS $variant (warnings: $warnings)"
    else
        echo "FAIL $variant (see $log):"
        grep -E "error:|undefined reference|^FAILED" "$log" | sort -u | head -40
        status=1
    fi
done
exit $status
