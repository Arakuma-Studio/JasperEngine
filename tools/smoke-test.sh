#!/usr/bin/env bash
# End-to-end check of a Linux build (run tools/build.sh linux first):
#
#   1. export an editor project with the command-line exporter
#   2. build the exported standalone CMake project
#   3. run the game under a virtual X server and screenshot it
#   4. open the project in the editor GUI and screenshot it
#
#   tools/smoke-test.sh <editor-project-dir>
#
# The project is copied first, since exporting re-saves its scenes. Results go
# to build/smoke/: game.png, editor.png, and the logs. Fails if the export or
# the build fails, if the game or the editor exits before its screenshot, or if
# either logs an error (missing shaders, Lua errors, failed assertions, crashes);
# the audio device Docker lacks is not an error.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
IMAGE=jasperengine/linux-build
# The main checkout's compiler cache, even when running from a git worktree.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"

[[ $# -eq 1 && -f "$1/project.yaml" ]] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
PROJECT="$(cd "$1" && pwd)"
[[ -x "$ROOT/$EDITOR" ]] || { echo "No editor at $EDITOR; run tools/build.sh linux" >&2; exit 1; }

docker build -q -t "$IMAGE" -f "$ROOT/docker/linux.Dockerfile" "$ROOT/docker" >/dev/null
docker run --rm -v "$ROOT:/src" "$IMAGE" rm -rf /src/build/smoke
mkdir -p "$ROOT/build/smoke" "$MAIN/build/.cache/ccache"
cp -r "$PROJECT" "$ROOT/build/smoke/project"

# The host's passwd/group give the container user a name, which the D-Bus
# session the editor's file dialogs need insists on.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
    -v "$ROOT:/src" -w /src/build/smoke \
    -v "$MAIN/build/.cache/ccache:/ccache" \
    -e HOME=/src/build/smoke/home \
    -e CCACHE_DIR=/ccache \
    -e EDITOR="/src/$EDITOR" \
    "$IMAGE" bash -euo pipefail -c '
        mkdir -p "$HOME"

        # Lines of a log that mean the program failed: errors, and what a crash, a failed
        # assertion or a Lua error leaves. Docker has no audio device, which is expected.
        fatal_lines() {
            grep -aE "\((ERROR|PANIC)\)|\[ERROR\]|Lua Error|[Mm]issing (precompiled )?shader|[Aa]ssertion|Aborted|Segmentation fault|terminate called|core dumped|AddressSanitizer|stack smashing" "$@" \
                | grep -avE "Failed to initialize audio output" || true
        }
        # check_log <what> <log...>: fail on the first fatal lines
        check_log() {
            local what="$1"; shift
            local lines
            lines="$(fatal_lines "$@")"
            if [[ -n "$lines" ]]; then
                echo "FAIL: $what logged errors:"
                echo "$lines" | head -20
                exit 1
            fi
        }

        echo "== export"
        # Absolute: the exporter resolves scene paths against a relative project twice.
        "$EDITOR" export --project "$PWD/project" --out "$PWD/export" --backend opengl > export.log 2>&1 \
            || { echo "FAIL: export"; tail -30 export.log; exit 1; }
        check_log export export.log

        echo "== build exported project"
        cmake -S export -B export-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > game-build.log 2>&1 \
            && cmake --build export-build >> game-build.log 2>&1 \
            || { echo "FAIL: build of the exported project"; tail -30 game-build.log; exit 1; }
        GAME="$(find export-build -maxdepth 1 -type f -executable | head -1)"
        [[ -n "$GAME" ]] || { echo "FAIL: no executable in export-build"; exit 1; }

        Xvfb :99 -screen 0 1600x900x24 > /dev/null 2>&1 &
        export DISPLAY=:99
        sleep 1

        # run <name> <log> <seconds> <command...>: start, wait, require it still runs,
        # screenshot the whole screen to <name>.png, stop it. stdout is line buffered,
        # so what it printed is in <log> when it is stopped.
        run() {
            local name="$1" log="$2" secs="$3"; shift 3
            stdbuf -oL -eL "$@" > "$log" 2>&1 &
            local pid=$!
            sleep "$secs"
            if ! kill -0 "$pid" 2>/dev/null; then
                local status=0
                wait "$pid" || status=$?
                echo "FAIL: ${name##*/} exited early (status $status)"; tail -20 "$log"; exit 1
            fi
            xwininfo -root -tree | grep -E "^ +0x[0-9a-f]+ \"" | sed "s/^ */  window: /" | head -3
            import -window root "$name.png"
            kill "$pid"
            wait "$pid" 2>/dev/null || true
            echo "ok: ${name##*/} ran (screenshot build/smoke/${name##*/}.png)"
        }

        echo "== run game"
        (cd "$(dirname "$GAME")" && run ../game ../game.log 8 "./$(basename "$GAME")")
        check_log game game.log

        # The export left this project as the most recent one in settings.yaml
        # here, so the editor reopens it. Its own log (editor.log) is written
        # next to settings.yaml, the console output goes to editor-console.log.
        echo "== run editor"
        : > editor.log
        run editor editor-console.log 20 dbus-run-session -- "$EDITOR"
        check_log editor editor.log editor-console.log

        echo "PASS"
    '
