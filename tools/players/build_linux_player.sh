#!/usr/bin/env bash
# Builds one Linux prebuilt player runtime (S30) and packages it into a players directory.
# Runs inside the linux build image. The one recipe both `tools/build.sh players` and the MCP
# suite's shared build cache (tools/mcp/build_cache.py) use, so the suite's runtimes are CI's.
#
#   tools/players/build_linux_player.sh [options] <backend> <flavour> <build-dir> <players-dir> [-- <cmake args>...]
#
#   backend     glcore, gles3 or vulkan
#   flavour     release or development (Development Build: the profiler and --profile)
#   build-dir   the player's CMake build tree
#   players-dir where the runtime goes (<players-dir>/linux-x64-<backend>-<flavour>/)
#   options     --jobs N (cmake --build -j N), --keep-going (Ninja -k 0), --zip DIR (also the
#               archives, package_player.py --zip), --engine-version V (the editor version the
#               runtime is locked to; default: git describe, as the editor's CMake asks)
#   cmake args  added to the configure (tools/build.sh: its option, warning and ccache flags)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JOBS_ARGS=()
KEEP_GOING=()
PACKAGE_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS_ARGS=(-j "$2"); shift 2 ;;
        --keep-going) KEEP_GOING=(-- -k 0); shift ;;
        --zip) PACKAGE_ARGS+=(--zip "$2"); shift 2 ;;
        --engine-version) PACKAGE_ARGS+=(--engine-version "$2"); shift 2 ;;
        -*) echo "build_linux_player.sh: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[[ $# -ge 4 ]] || { sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }
backend="$1" flavour="$2" tree="$3" out="$4"
shift 4
[[ "${1:-}" == -- ]] && shift
[[ "$backend" =~ ^(glcore|gles3|vulkan)$ ]] || { echo "backend: glcore, gles3 or vulkan (got $backend)" >&2; exit 2; }
[[ "$flavour" =~ ^(release|development)$ ]] || { echo "flavour: release or development (got $flavour)" >&2; exit 2; }

dev=OFF
[[ "$flavour" == development ]] && dev=ON
cd "$ROOT"
# Release code with debug information, split off by the link (JASPERENGINE_SPLIT_SYMBOLS, R23):
# the player ships stripped with a debug link, symbols/ is the runtime's own archive
cmake --preset engine-linux-release -S engine -B "$tree" -DCMAKE_BUILD_TYPE=Release \
    -DJASPERENGINE_PLAYER=ON -DGRAPHIC_BACKEND="$backend" -DJASPERENGINE_DEVELOPMENT_BUILD="$dev" "$@"
cmake --build "$tree" --target jasperengine-player ${JOBS_ARGS[@]+"${JOBS_ARGS[@]}"} ${KEEP_GOING[@]+"${KEEP_GOING[@]}"}
pkg="$tree/package"
rm -rf "$pkg" && mkdir -p "$pkg/symbols"
cp "$tree/jasperengine-player" "$pkg/jasperengine-player"
cp "$tree/symbols/jasperengine-player.debug" "$pkg/symbols/"
python3 tools/players/package_player.py --platform linux --arch x64 --backend "$backend" --flavour "$flavour" \
    --build "$pkg" --symbols "$pkg/symbols" --out "$out" ${PACKAGE_ARGS[@]+"${PACKAGE_ARGS[@]}"}
