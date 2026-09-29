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
# to build/smoke/: game.png, editor.png, and the logs. Fails if either program
# exits before its screenshot is taken.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
IMAGE=jasperengine/linux-build

[[ $# -eq 1 && -f "$1/project.yaml" ]] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
PROJECT="$(cd "$1" && pwd)"
[[ -x "$ROOT/$EDITOR" ]] || { echo "No editor at $EDITOR; run tools/build.sh linux" >&2; exit 1; }

docker build -q -t "$IMAGE" -f "$ROOT/docker/linux.Dockerfile" "$ROOT/docker" >/dev/null
docker run --rm -v "$ROOT:/src" "$IMAGE" rm -rf /src/build/smoke
mkdir -p "$ROOT/build/smoke"
cp -r "$PROJECT" "$ROOT/build/smoke/project"

# The host's passwd/group give the container user a name, which the D-Bus
# session the editor's file dialogs need insists on.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
    -v "$ROOT:/src" -w /src/build/smoke \
    -e HOME=/src/build/smoke/home \
    -e CCACHE_DIR=/src/build/.cache/ccache \
    -e EDITOR="/src/$EDITOR" \
    "$IMAGE" bash -euo pipefail -c '
        mkdir -p "$HOME"

        echo "== export"
        # Absolute: the exporter resolves scene paths against a relative project twice.
        "$EDITOR" export --project "$PWD/project" --out "$PWD/export" --backend opengl > export.log 2>&1 \
            || { tail -30 export.log; exit 1; }

        echo "== build exported project"
        cmake -S export -B export-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > game-build.log 2>&1 \
            && cmake --build export-build >> game-build.log 2>&1 \
            || { tail -30 game-build.log; exit 1; }
        GAME="$(find export-build -maxdepth 1 -type f -executable | head -1)"
        [[ -n "$GAME" ]] || { echo "no executable in export-build"; exit 1; }

        Xvfb :99 -screen 0 1600x900x24 > /dev/null 2>&1 &
        export DISPLAY=:99
        sleep 1

        # run <name> <seconds> <command...>: start, wait, require it still runs,
        # screenshot the whole screen, stop it.
        run() {
            local name="$1" secs="$2"; shift 2
            "$@" > "$name.log" 2>&1 &
            local pid=$!
            sleep "$secs"
            if ! kill -0 "$pid" 2>/dev/null; then
                wait "$pid" || true
                echo "FAIL: ${name##*/} exited early"; tail -20 "$name.log"; exit 1
            fi
            xwininfo -root -tree | grep -E "^ +0x[0-9a-f]+ \"" | sed "s/^ */  window: /" | head -3
            import -window root "$name.png"
            kill "$pid"
            echo "ok: ${name##*/} (screenshot build/smoke/${name##*/}.png)"
        }

        echo "== run game"
        (cd "$(dirname "$GAME")" && run ../game 8 "./$(basename "$GAME")")

        # The export left this project as the most recent one in settings.yaml
        # here, so the editor reopens it.
        echo "== run editor"
        run editor 20 dbus-run-session -- "$EDITOR"
    '
