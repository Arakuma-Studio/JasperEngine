#!/usr/bin/env bash
# A Compile build of a project, the editor's Build with Method Compile (Build Settings > Build):
# `jasperengine-editor build --method compile`, which exports the project and builds its generated
# CMake project with this machine's compiler (the engine modules the project needs, S34). A CLI
# subcommand runs before the editor's backend starts, so it opens no window and needs no GPU (the
# design's §16.4 question). The CI lanes of Windows (MSVC) and macOS (Apple Clang) run it through
# `tools/ci/windows.sh compile-smoke` and `tools/ci/macos.sh compile-smoke`; on Linux it runs in the
# linux image (`tools/build.sh exec linux tools/ci/compile-smoke.sh ...`), and the smoke lane's
# `SMOKE_METHOD=build tools/smoke-test.sh` runs the same command and then the game.
#
#   tools/ci/compile-smoke.sh <editor binary> <windows|linux|macos> <project-dir> [backend]
#
# backend: the build's graphics API (default: the platform's first, d3d11 / glcore / metal).
# The project is copied to build/ci-compile-smoke/<name>/ first (a build re-saves its scenes). Fails
# when the build fails, makes no executable, or logs an error (the patterns tools/smoke-test.sh
# fails a build on). CMAKE_GENERATOR defaults to Ninja, the generator the editor loads the MSVC
# environment for on Windows (CommandRunner::msvcEnvironment); ccache on PATH is the compiler
# launcher (CMake reads CMAKE_<LANG>_COMPILER_LAUNCHER from the environment).
# Log: build/test-results/<platform>-compile-smoke.log.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=platform-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/platform-common.sh"

USAGE="usage: tools/ci/compile-smoke.sh <editor binary> <windows|linux|macos> <project-dir> [backend]"
[[ $# -ge 3 && $# -le 4 ]] || ci_usage "$USAGE"
EDITOR="$1" PLATFORM="$2" PROJECT="$3" BACKEND="${4:-}"
[[ "$PLATFORM" =~ ^(windows|linux|macos)$ ]] || ci_usage "$USAGE (platform: $PLATFORM)"
[[ -f "$EDITOR" ]] || ci_usage "no editor at $EDITOR: build and install it first"
[[ -f "$PROJECT/project.yaml" ]] || ci_usage "$PROJECT is not a project (no project.yaml)"
EDITOR="$(cd "$(dirname "$EDITOR")" && pwd)/$(basename "$EDITOR")"

smoke() {
    local name scratch
    name="$(basename "$(cd "$PROJECT" && pwd)")"
    scratch="$ROOT/build/ci-compile-smoke/$name"
    rm -rf "$scratch"
    mkdir -p "$scratch"
    cp -R "$PROJECT" "$scratch/project"

    export CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
    if command -v ccache >/dev/null 2>&1; then
        export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
    fi
    local args=(build --project project --platform "$PLATFORM" --method compile --out game-out)
    [[ -n "$BACKEND" ]] && args+=(--backend "$BACKEND")
    echo "== $EDITOR ${args[*]} (in $scratch; CMAKE_GENERATOR=$CMAKE_GENERATOR)"
    local status=0
    # the working directory holds what the editor writes beside it (settings.yaml, editor.log); the
    # output streams as it comes (an engine compile is the step's half hour)
    set +e
    (cd "$scratch" && "$EDITOR" "${args[@]}") 2>&1 | tee "$scratch/build.log"
    status=${PIPESTATUS[0]}
    set -e
    [[ $status == 0 ]] || { echo "FAIL: the Compile build exited $status"; return 1; }

    grep -q '^method: compile' "$scratch/build.log" || { echo "FAIL: the build did not compile (no 'method: compile' line)"; return 1; }
    local lines
    lines="$(grep -aE 'JASPERENGINE CRASH:|\((ERROR|PANIC)\)|\[ERROR\]|Lua Error|[Mm]issing (precompiled )?shader|[Aa]ssertion|Aborted|Segmentation fault|terminate called|core dumped' \
        "$scratch/build.log" | grep -avE 'Failed to initialize audio output' || true)"
    [[ -z "$lines" ]] || { echo "FAIL: the build logged errors:"; echo "$lines" | head -20; return 1; }
    local game
    game="$(sed -n 's/^executable: //p' "$scratch/build.log" | head -1 | tr -d '\r')"
    [[ -n "$game" ]] || { echo "FAIL: the build printed no 'executable:' line"; return 1; }
    # (the editor prints a native path; Git Bash reads C:\... through cygpath)
    command -v cygpath >/dev/null 2>&1 && game="$(cygpath -u "$game")"
    [[ "$game" == /* ]] || game="$scratch/$game"
    [[ -e "$game" ]] || { echo "FAIL: no executable at $game"; return 1; }
    echo "ok: $PLATFORM Compile build of $name: $game ($(du -sh "$game" | cut -f1))"
}

ci_logged "$PLATFORM-compile-smoke" smoke
