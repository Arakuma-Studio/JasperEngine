# shellcheck shell=bash
# (the variables are the sourcing scripts')
# shellcheck disable=SC2034
# What the platform scripts of CI share (tools/ci/windows.sh, macos.sh, players-windows.sh,
# players-apple.sh, package-linux.sh, compile-smoke.sh; docs/superpowers/specs/2026-11-12-ci-design.md
# §12, contract C5). Sourced, never run. Sets ROOT (the checkout, the working directory from here
# on), RESULTS, PY and LAUNCHERS, and gives:
#
#   ci_logged <log name> <command...>   runs the command with its output also in
#                                       build/test-results/<log name>.log; its status is the call's
#   ci_usage <text>                     prints the usage and exits 2 (bad arguments)
#   ci_native <path>                    the path as native tools take it (C:/... under Git Bash)
#   ci_zip_dir <dir> <zip>              zips the contents of dir (mode bits kept: a Unix executable stays one)
#   cache_has <CMakeCache.txt> <line>   whether a build tree was configured with that cache entry
#   ci_player_variants <flavour>        the player variants a flavour builds, as name:suffix pairs
#   ci_check_flavours "<flavours>"      whether that is a list of release and development
#
# Environment: JASPERENGINE_VERSION (the version stamped into editors and players; default git describe,
# as CMakeLists.txt asks), JOBS (build parallelism), WERROR=1 (warnings of our targets are errors),
# ccache on PATH (the compiler launcher, CCACHE_DIR as .github/actions/ccache-restore sets it).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
RESULTS=build/test-results

# The command a log starts with ("$ <command>", what tools/ci/report.py takes as the repro): the
# script as run from the checkout, its arguments, and the variables of this file that change what it
# does. A script that runs another one for its step exports it, so the log names the outer command.
if [[ -z "${CI_LOG_COMMAND:-}" ]]; then
    CI_LOG_COMMAND=""
    for _name in ARCHS EDITOR_TESTS ENGINE_DEBUG_INFO JASPERENGINE_VERSION TEST_SUITES WERROR XCODE_JOBS CMAKE_GENERATOR; do
        [[ -n "${!_name:-}" ]] && CI_LOG_COMMAND+="$_name=$(printf '%q' "${!_name}") "
    done
    unset _name
    CI_LOG_COMMAND+="$(printf '%q ' "tools/ci/$(basename "$0")" "$@")"
    CI_LOG_COMMAND="${CI_LOG_COMMAND% }"
fi

# Git Bash on a Windows runner has `python` (python3 there can be the Store's stub that only opens
# the Store); macOS and Linux have python3
if command -v python3 >/dev/null 2>&1 && python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' >/dev/null 2>&1; then
    PY=python3
else
    PY=python
fi

LAUNCHERS=()
if command -v ccache >/dev/null 2>&1; then
    LAUNCHERS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

JOBS_ARGS=()
[[ -n "${JOBS:-}" ]] && JOBS_ARGS=(-j "$JOBS")
# KEEP_GOING=1, as tools/build.sh: Ninja builds on past a failure, so one run shows every error
[[ "${KEEP_GOING:-0}" == 1 ]] && JOBS_ARGS+=(-- -k 0)

WERROR_VALUE=OFF
[[ "${WERROR:-0}" == 1 ]] && WERROR_VALUE=ON

# -DJASPERENGINE_EDITOR_VERSION for an editor tree, --engine-version for package_player.py: empty
# without JASPERENGINE_VERSION, so both fall back to the same git describe
EDITOR_VERSION_ARGS=()
PLAYER_VERSION_ARGS=()
if [[ -n "${JASPERENGINE_VERSION:-}" ]]; then
    EDITOR_VERSION_ARGS=("-DJASPERENGINE_EDITOR_VERSION:STRING=$JASPERENGINE_VERSION")
    PLAYER_VERSION_ARGS=(--engine-version "$JASPERENGINE_VERSION")
fi

# the header comment of a script (its usage), without the #! line
ci_header() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$1"
}

ci_usage() {
    printf '%s\n' "$1" >&2
    exit 2
}

# cache_has <CMakeCache.txt> <exact line>: whatever line ends the file was written with
cache_has() {
    tr -d '\r' < "$1" | grep -qxF -- "$2"
}

# MSVC_LINKER_ARGS: CMAKE_LINKER pinned to the link.exe beside cl. Under Git Bash /usr/bin/link (GNU
# coreutils) is on PATH too; CMake looks beside the compiler first (CMakeFindBinUtils.cmake), so this
# only makes that choice explicit and checks it
ci_msvc() {
    local cl
    cl="$(command -v cl 2>/dev/null || true)"
    [[ -n "$cl" ]] || { echo "cl is not on PATH: run inside an MSVC developer environment (ilammy/msvc-dev-cmd, arch x64)" >&2; exit 1; }
    [[ -f "$(dirname "$cl")/link.exe" ]] || { echo "no link.exe beside $cl" >&2; exit 1; }
    MSVC_LINKER_ARGS=("-DCMAKE_LINKER=$(ci_native "$(dirname "$cl")/link.exe")")
    command -v ninja >/dev/null 2>&1 || { echo "ninja is not on PATH (Visual Studio's CMake tools or choco install ninja)" >&2; exit 1; }
}

ci_native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}

ci_logged() {
    local log="$RESULTS/$1.log"
    shift
    mkdir -p "$RESULTS"
    echo "\$ $CI_LOG_COMMAND" > "$log"
    echo "log: $log" >&2
    local status=0
    set +e
    ( set -e; "$@" ) 2>&1 | tee -a "$log"
    status=${PIPESTATUS[0]}
    set -e
    # the warnings per target of a build log, beside the job's other summaries (docs/testing.md "Warnings")
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && grep -q '^\[[0-9]*/[0-9]*\] ' "$log"; then
        "$PY" tools/ci/warning-summary.py "$log" >> "$GITHUB_STEP_SUMMARY" || true
    fi
    [[ $status == 0 ]] || echo "FAILED (exit $status); log: $log" >&2
    return "$status"
}

ci_zip_dir() {
    "$PY" -I - "$(ci_native "$1")" "$(ci_native "$2")" <<'PYZIP'
import os, sys, zipfile
from pathlib import Path
source, target = Path(sys.argv[1]), Path(sys.argv[2])
target.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(source.rglob("*")):
        if path.is_file():
            archive.write(path, path.relative_to(source).as_posix())
print(f"zip: {target} ({target.stat().st_size} bytes)")
PYZIP
}

# The release variants of engine/player/variants.json as name:suffix pairs (S34 R4); the development
# flavour is always the full player
ci_player_variants() {
    local flavour="$1"
    if [[ "$flavour" == development ]]; then
        echo "full:"
    else
        "$PY" -I -c "import json, sys; print(' '.join(v['name'] + ':' + v['suffix'] for v in json.load(open(sys.argv[1]))['variants']))" \
            engine/player/variants.json
    fi
}

ci_check_flavours() {
    local flavour
    [[ -n "$1" ]] || return 1
    for flavour in $1; do
        [[ "$flavour" == release || "$flavour" == development ]] || return 1
    done
}
