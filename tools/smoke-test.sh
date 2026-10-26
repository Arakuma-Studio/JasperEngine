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
#
# SMOKE_GAME_ENV="NAME=value ..." is passed to the game's environment.
# SMOKE_EXPORT_ARGS="..." is added to the export command (e.g. --development-build).
# SMOKE_GAME_ARGS="..." is passed to the game's command line (e.g. --profile-capture=../capture.jetrace.json).
# SMOKE_GAME_SECONDS="ready" (the default) screenshots the game once it logs that
# every scene is loaded and no shader is still compiling (JASPERENGINE_READY_SIGNAL),
# SMOKE_READY_SETTLE seconds later (2), waiting up to SMOKE_READY_TIMEOUT seconds
# (300; past it the screenshot is taken anyway, with a warning). Whole seconds after
# it starts instead ("8"), or several ("8 14 20", "ready 30"), give
# game-<seconds>.png each, game.png the last.
# SMOKE_GAME_RUNS="<name> NAME=value ...; <name> ..." runs the exported game again
# once per entry, with SMOKE_GAME_ENV plus the entry's variables, on the same
# schedule: game-<name>.png and game-<name>.log, checked like the first run (a
# project that picks its scene from the environment shows each scene from one build).
#
# SMOKE_BACKEND selects the game's graphics backend: opengl (the default: GLSL 4.10,
# desktop GL on Mesa llvmpipe), vulkan (SPIR-V, sokol's Vulkan backend on Mesa's
# lavapipe) or gles3 (GLSL ES 3.00, sokol's GLES3 backend, the one Android and the web
# use, in an OpenGL ES context on llvmpipe). The last two run in the image of
# docker/linux-backends.Dockerfile; the editor run is the same for all. lavapipe makes a
# driver shader per Vulkan pipeline and recompiles them every frame once a frame uses
# more than its variant caches hold (~1000 pipelines: the whole shader-library fixture
# takes minutes per frame there), so run such a project a few groups at a time.
# SMOKE_DIR=build/<name> writes there instead of build/smoke, so several smoke tests
# can run at once from one checkout. JASPER_DOCKER_IMAGE=<id|tag> runs that image (built from
# docker/linux.Dockerfile, or linux-backends for SMOKE_BACKEND=vulkan|gles3) instead of building
# it (CI's prebuilt images).
# The exported project must configure as a user's game does: without the repository's presets,
# warnings as errors or sanitizers (docs/testing.md); the test fails otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
IMAGE=jasperengine/linux-build
BACKEND="${SMOKE_BACKEND:-opengl}"
OUT="${SMOKE_DIR:-build/smoke}"
case "$BACKEND" in
    opengl) GRAPHIC_BACKEND=glcore; EXPORT_BACKEND=opengl; GAME_FLAGS="" ;;
    # lavapipe's descriptors (280 bytes) exceed sokol's default room for one (256)
    vulkan) GRAPHIC_BACKEND=vulkan; EXPORT_BACKEND=vulkan; GAME_FLAGS="-D_SG_VK_MAX_DESCRIPTOR_DATA_SIZE=320" ;;
    gles3) GRAPHIC_BACKEND=gles3; EXPORT_BACKEND=opengles; GAME_FLAGS="" ;;
    *) echo "SMOKE_BACKEND must be opengl, vulkan or gles3 (got $BACKEND)" >&2; exit 1 ;;
esac
[[ "$OUT" =~ ^build/[A-Za-z0-9_.-]+$ ]] || { echo "SMOKE_DIR must be build/<name> (got $OUT)" >&2; exit 1; }
# The main checkout's compiler cache, even when running from a git worktree.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"

[[ $# -eq 1 && -f "$1/project.yaml" ]] || { sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
PROJECT="$(cd "$1" && pwd)"
[[ -x "$ROOT/$EDITOR" ]] || { echo "No editor at $EDITOR; run tools/build.sh linux" >&2; exit 1; }

if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE="$JASPER_DOCKER_IMAGE"
else
    docker build -q -t "$IMAGE" -f "$ROOT/docker/linux.Dockerfile" "$ROOT/docker" >/dev/null
    if [[ "$BACKEND" != opengl ]]; then
        IMAGE=jasperengine/linux-backends
        docker build -q -t "$IMAGE" -f "$ROOT/docker/linux-backends.Dockerfile" "$ROOT/docker" >/dev/null
    fi
fi
docker run --rm -v "$ROOT:/src" "$IMAGE" rm -rf "/src/$OUT"
mkdir -p "$ROOT/$OUT" "$MAIN/build/.cache/ccache"
cp -r "$PROJECT" "$ROOT/$OUT/project"

# DOCKER_CPUS caps the CPUs of the run (and llvmpipe/lavapipe's thread count) on a shared host.
CPU_FLAGS=()
[[ -n "${DOCKER_CPUS:-}" ]] && CPU_FLAGS=(--cpus "$DOCKER_CPUS" -e LP_NUM_THREADS="$DOCKER_CPUS")

# The host's passwd/group give the container user a name, which the D-Bus
# session the editor's file dialogs need insists on.
# (the ${a[@]+...} form: an empty array under set -u fails on bash before 4.4, e.g. macOS)
docker run --rm ${CPU_FLAGS[@]+"${CPU_FLAGS[@]}"} \
    --user "$(id -u):$(id -g)" \
    -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
    -v "$ROOT:/src" -w "/src/$OUT" \
    -v "$MAIN/build/.cache/ccache:/ccache" \
    -e HOME="/src/$OUT/home" \
    -e OUT="$OUT" \
    -e EXPORT_BACKEND="$EXPORT_BACKEND" \
    -e GRAPHIC_BACKEND="$GRAPHIC_BACKEND" \
    -e GAME_FLAGS="$GAME_FLAGS" \
    -e CCACHE_DIR=/ccache \
    -e EDITOR="/src/$EDITOR" \
    -e SMOKE_GAME_ENV="${SMOKE_GAME_ENV:-}" \
    -e SMOKE_EXPORT_ARGS="${SMOKE_EXPORT_ARGS:-}" \
    -e SMOKE_GAME_ARGS="${SMOKE_GAME_ARGS:-}" \
    -e SMOKE_GAME_RUNS="${SMOKE_GAME_RUNS:-}" \
    -e SMOKE_GAME_SECONDS="${SMOKE_GAME_SECONDS:-ready}" \
    -e SMOKE_READY_TIMEOUT="${SMOKE_READY_TIMEOUT:-300}" \
    -e SMOKE_READY_SETTLE="${SMOKE_READY_SETTLE:-2}" \
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
        # Relative paths, as a user types them (relative to the current directory)
        # shellcheck disable=SC2086
        "$EDITOR" export --project project --out export --backend "$EXPORT_BACKEND" $SMOKE_EXPORT_ARGS > export.log 2>&1 \
            || { echo "FAIL: export"; tail -30 export.log; exit 1; }
        check_log export export.log

        echo "== build exported project"
        cmake -S export -B export-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGRAPHIC_BACKEND="$GRAPHIC_BACKEND" \
            -DCMAKE_C_FLAGS="$GAME_FLAGS" -DCMAKE_CXX_FLAGS="$GAME_FLAGS" \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > game-build.log 2>&1 \
            && cmake --build export-build >> game-build.log 2>&1 \
            || { echo "FAIL: build of the exported project"; tail -30 game-build.log; exit 1; }
        GAME="$(find export-build -maxdepth 1 -type f -executable | head -1)"
        [[ -n "$GAME" ]] || { echo "FAIL: no executable in export-build"; exit 1; }
        # an exported game builds as a user would: no presets, no Werror, no sanitizer, no warnings
        [[ ! -e export/CMakePresets.json ]] || { echo "FAIL: the export carries CMakePresets.json"; exit 1; }
        if grep -E "^JASPERENGINE_(WERROR|WARNINGS|UNITY_BUILD)(:BOOL)?=(ON|TRUE|1)$|^JASPERENGINE_SANITIZE(:STRING)?=.+$" export-build/CMakeCache.txt; then
            echo "FAIL: the exported project configured with the repository build options above"; exit 1
        fi

        Xvfb :99 -screen 0 1600x900x24 > /dev/null 2>&1 &
        export DISPLAY=:99
        sleep 1

        # run <name> <log> <seconds...> <command...>: start, wait, require it still runs,
        # screenshot the whole screen to <name>.png, stop it. Several seconds (one
        # argument, space separated) screenshot at each to <name>-<seconds>.png, the last
        # copied to <name>.png; "ready" waits for the engine ready line in <log> (see the
        # header), then SMOKE_READY_SETTLE seconds. stdout is line buffered, so what it
        # printed is in <log> when it is stopped.
        run() {
            local name="$1" log="$2" secs="$3"; shift 3
            stdbuf -oL -eL "$@" > "$log" 2>&1 &
            local pid=$! elapsed=0 shot out
            for shot in $secs; do
                if [[ "$shot" == ready ]]; then
                    while ! grep -aqs "Engine ready:" "$log" && (( elapsed < SMOKE_READY_TIMEOUT )) && kill -0 "$pid" 2>/dev/null; do
                        sleep 1
                        elapsed=$((elapsed + 1))
                    done
                    if grep -aqs "Engine ready:" "$log"; then
                        echo "ready after ${elapsed}s"
                    else
                        echo "WARN: ${name##*/} not ready after ${elapsed}s, screenshot anyway"
                    fi
                    sleep "$SMOKE_READY_SETTLE"
                    elapsed=$((elapsed + SMOKE_READY_SETTLE))
                    shot="$elapsed"
                else
                    sleep "$((shot > elapsed ? shot - elapsed : 0))"
                    elapsed="$shot"
                fi
                if ! kill -0 "$pid" 2>/dev/null; then
                    local status=0
                    wait "$pid" || status=$?
                    echo "FAIL: ${name##*/} exited early (status $status)"; tail -20 "$log"; exit 1
                fi
                [[ "$secs" == "$shot" || "$secs" == ready ]] && out="$name" || out="$name-$shot"
                import -window root "$out.png"
            done
            [[ "$out" == "$name" ]] || cp "$out.png" "$name.png"
            xwininfo -root -tree | grep -E "^ +0x[0-9a-f]+ \"" | sed "s/^ */  window: /" | head -3
            kill "$pid"
            wait "$pid" 2>/dev/null || true
            echo "ok: ${name##*/} ran (screenshot $OUT/${name##*/}.png)"
        }

        echo "== run game"
        # word splitting of SMOKE_GAME_ENV is wanted: one NAME=value per word
        # shellcheck disable=SC2086
        (cd "$(dirname "$GAME")" && run ../game ../game.log "$SMOKE_GAME_SECONDS" env JASPERENGINE_READY_SIGNAL=1 $SMOKE_GAME_ENV "./$(basename "$GAME")" $SMOKE_GAME_ARGS)
        check_log game game.log

        # the extra runs: "<name> NAME=value ..." entries separated by ";"
        IFS=";" read -ra runs <<< "$SMOKE_GAME_RUNS"
        for entry in "${runs[@]}"; do
            read -ra words <<< "$entry"
            (( ${#words[@]} )) || continue
            label="${words[0]}"
            [[ "$label" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "FAIL: SMOKE_GAME_RUNS name \"$label\""; exit 1; }
            echo "== run game ($label)"
            # shellcheck disable=SC2086
            (cd "$(dirname "$GAME")" && run "../game-$label" "../game-$label.log" "$SMOKE_GAME_SECONDS" env JASPERENGINE_READY_SIGNAL=1 $SMOKE_GAME_ENV "${words[@]:1}" "./$(basename "$GAME")")
            check_log "game ($label)" "game-$label.log"
        done

        # The export left this project as the most recent one in settings.yaml
        # here, so the editor reopens it. Its own log (editor.log) is written
        # next to settings.yaml, the console output goes to editor-console.log.
        echo "== run editor"
        : > editor.log
        run editor editor-console.log 20 dbus-run-session -- "$EDITOR"
        check_log editor editor.log editor-console.log

        echo "PASS"
    '
