#!/usr/bin/env bash
# End-to-end check of a Linux build (run tools/build.sh linux first):
#
#   1. build the game: with a prebuilt player (SMOKE_METHOD=prebuilt, the default: jasperengine-editor
#      build, no compiler, in the image of docker/runtime.Dockerfile) or from source
#      (SMOKE_METHOD=compile: export the project, build the exported CMake project)
#   2. run the game under a virtual X server and screenshot it (prebuilt: the player's own
#      --headless --after-ready --quit-after 30 --fixed-fps 60 --screenshot --fail-on-error run)
#   3. open the project in the editor GUI and screenshot it
#   4. bake a navmesh and query a path through the editor's MCP tools (tools/mcp/nav_smoke.py)
#   (and after 2, record 30 frames of the project's start scene to PNG from the shell:
#   `jasperengine-editor call --project ... --wait-job record_game`, S27 AC26)
#
# SMOKE_METHOD=prebuilt needs the player runtimes (tools/build.sh players, build/players;
# SMOKE_PLAYERS_DIR=build/<dir> for others). SMOKE_QUIT_AFTER (30) frames are run there.
# SMOKE_PLAY=1 (the default for prebuilt) runs a compiled game the same way, with its own
# frame as game.png (tools/ci/parity.sh compares the two methods' frames).
#
#   tools/smoke-test.sh <editor-project-dir>
#
# The project is copied first, since exporting re-saves its scenes. Results go
# to build/smoke/: game.png, editor.png, and the logs. Fails if the export or
# the build fails, if the game or the editor exits before its screenshot, or if
# either logs an error (missing shaders, Lua errors, failed assertions, crashes);
# the audio device Docker lacks is not an error.
#
# A smoke-expect.txt in the project lists lines (one per line, '#' comments) the game's log must
# hold after its run (tools/fixtures/tween-curve: the tween's curve checked in the exported game);
# such a project runs 120 frames unless SMOKE_QUIT_AFTER says otherwise.
# SMOKE_METHOD=build builds as the editor's Build does with Method Compile (jasperengine-editor build
# --method compile): only the engine modules the project needs (S34), unlike compile's source export.
# A smoke-export-expect.txt lists lines that build's generated CMakeLists.txt must hold (build only),
# a smoke-build-expect.txt lines only that build's game logs (a source export and a prebuilt player
# keep every module, so the same game does not log them),
# and a smoke-golden.json ({"image", "tolerance", "max_fraction"}) an image the game's own frame
# (SMOKE_PLAY=1) must match (tools/ci/smoke_golden.py; tools/fixtures/title3d-fixed: a Realtime 3D
# title drawn from its cooked mesh in a build without the mesher, against the editor's frame).
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
# can run at once from one checkout. JASPER_DOCKER_IMAGE=<id|tag> runs that image instead of building
# it: docker/runtime.Dockerfile's for prebuilt, else linux's (linux-backends' for SMOKE_BACKEND=vulkan
# or gles3); CI passes its GHCR images there (.github/actions/image).
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

METHOD="${SMOKE_METHOD:-prebuilt}"
[[ "$METHOD" == prebuilt || "$METHOD" == compile || "$METHOD" == build ]] || { echo "SMOKE_METHOD must be prebuilt, compile or build (got $METHOD)" >&2; exit 1; }
PLAYERS="${SMOKE_PLAYERS_DIR:-build/players}"
[[ "$PLAYERS" =~ ^build/[A-Za-z0-9_./-]+$ ]] || { echo "SMOKE_PLAYERS_DIR must be build/<dir> (got $PLAYERS)" >&2; exit 1; }

[[ $# -eq 1 && -f "$1/project.yaml" ]] || { sed -n '2,64p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
PROJECT="$(cd "$1" && pwd)"
[[ -z "${SMOKE_QUIT_AFTER:-}" && -f "$PROJECT/smoke-expect.txt" ]] && SMOKE_QUIT_AFTER=120
[[ -x "$ROOT/$EDITOR" ]] || { echo "No editor at $EDITOR; run tools/build.sh linux" >&2; exit 1; }

if [[ "$METHOD" == prebuilt ]]; then
    [[ -d "$ROOT/$PLAYERS" ]] || { echo "No player runtimes in $PLAYERS; run tools/build.sh players (or SMOKE_METHOD=compile)" >&2; exit 1; }
fi
if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
    IMAGE="$JASPER_DOCKER_IMAGE"
elif [[ "$METHOD" == prebuilt ]]; then
    # a user's computer: no compiler (the image refuses to build with one)
    IMAGE=jasperengine/runtime-build
    docker build -q -t "$IMAGE" -f "$ROOT/docker/runtime.Dockerfile" "$ROOT/docker" >/dev/null
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
    -e CMAKE_C_COMPILER_LAUNCHER=ccache -e CMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -e EDITOR="/src/$EDITOR" \
    -e SMOKE_GAME_ENV="${SMOKE_GAME_ENV:-}" \
    -e SMOKE_EXPORT_ARGS="${SMOKE_EXPORT_ARGS:-}" \
    -e SMOKE_GAME_ARGS="${SMOKE_GAME_ARGS:-}" \
    -e SMOKE_GAME_RUNS="${SMOKE_GAME_RUNS:-}" \
    -e SMOKE_GAME_SECONDS="${SMOKE_GAME_SECONDS:-ready}" \
    -e SMOKE_READY_TIMEOUT="${SMOKE_READY_TIMEOUT:-300}" \
    -e SMOKE_READY_SETTLE="${SMOKE_READY_SETTLE:-2}" \
    -e SMOKE_METHOD="$METHOD" \
    -e SMOKE_PLAY="${SMOKE_PLAY:-$([[ "$METHOD" == prebuilt ]] && echo 1 || echo 0)}" \
    -e SMOKE_QUIT_AFTER="${SMOKE_QUIT_AFTER:-30}" \
    -e JASPER_PLAYERS_DIR="/src/$PLAYERS" \
    "$IMAGE" bash -euo pipefail -c '
        mkdir -p "$HOME"

        # Lines of a log that mean the program failed: errors, and what a crash, a failed
        # assertion or a Lua error leaves. Docker has no audio device, which is expected.
        fatal_lines() {
            grep -aE "JASPERENGINE CRASH:|\((ERROR|PANIC)\)|\[ERROR\]|Lua Error|[Mm]issing (precompiled )?shader|[Aa]ssertion|Aborted|Segmentation fault|terminate called|core dumped|AddressSanitizer|stack smashing" "$@" \
                | grep -avE "Failed to initialize audio output" || true
        }
        # check_log <what> <log...>: fail on the first fatal lines, and on a hang watchdog report
        # (a warning to run_game, but the smoke runs must have none: docs/crash-diagnostics.md)
        check_log() {
            local what="$1"; shift
            local lines
            lines="$(fatal_lines "$@")"
            if [[ -n "$lines" ]]; then
                echo "FAIL: $what logged errors:"
                echo "$lines" | head -20
                exit 1
            fi
            lines="$(grep -a "JASPERENGINE HANG:" "$@" || true)"
            if [[ -n "$lines" ]]; then
                echo "FAIL: $what reported a hang:"
                echo "$lines" | head -20
                exit 1
            fi
        }

        if [[ "$SMOKE_METHOD" == build ]]; then
        echo "== build (Compile: the modules the project needs)"
        # shellcheck disable=SC2086
        "$EDITOR" build --project project --platform linux --backend "$EXPORT_BACKEND" --out game-out --method compile \
            $SMOKE_EXPORT_ARGS > build.log 2>&1 || { echo "FAIL: build"; tail -30 build.log; exit 1; }
        check_log build build.log
        GAME="$(sed -n "s/^executable: //p" build.log | head -1)"
        [[ -x "$GAME" ]] || { echo "FAIL: no executable ($GAME)"; exit 1; }
        # smoke-export-expect.txt: lines the generated CMakeLists of the build must hold (module switches)
        if [[ -f project/smoke-export-expect.txt ]]; then
            TREE="$(sed -n "s/.*Building in the shared compile tree //p" build.log | head -1)"
            while IFS= read -r line; do
                [[ -z "$line" || "$line" == "#"* ]] && continue
                grep -qF -- "$line" "$TREE/CMakeLists.txt" || { echo "FAIL: the build does not hold: $line"; exit 1; }
            done < project/smoke-export-expect.txt
            echo "ok: the build holds project/smoke-export-expect.txt"
        fi
        elif [[ "$SMOKE_METHOD" == prebuilt ]]; then
        echo "== build (prebuilt player)"
        # shellcheck disable=SC2086
        "$EDITOR" build --project project --platform linux --backend "$EXPORT_BACKEND" --out game-out --method prebuilt \
            $SMOKE_EXPORT_ARGS > build.log 2>&1 || { echo "FAIL: build"; tail -30 build.log; exit 1; }
        check_log build build.log
        grep -q "^method: prebuilt" build.log || { echo "FAIL: the build did not use the prebuilt player"; cat build.log; exit 1; }
        GAME="$(sed -n "s/^executable: //p" build.log | head -1)"
        [[ -x "$GAME" ]] || { echo "FAIL: no executable ($GAME)"; exit 1; }
        else
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

        # play <name> <env...> -- <args...>: the prebuilt player runs itself (R17) and writes its
        # own frame; its exit code is the verdict (0, or 2-7: docs/shipping.md)
        play() {
            local name="$1"; shift
            local envs=()
            while [[ $# -gt 0 && "$1" != -- ]]; do envs+=("$1"); shift; done
            shift
            local status=0
            (cd "$(dirname "$GAME")" && timeout 900 env JASPERENGINE_READY_SIGNAL=1 ${envs[@]+"${envs[@]}"} stdbuf -oL -eL \
                "./$(basename "$GAME")" --headless --after-ready --quit-after "$SMOKE_QUIT_AFTER" --fixed-fps 60 \
                --screenshot "/src/$OUT/$name.png" --fail-on-error "$@" > "/src/$OUT/$name.log" 2>&1) || status=$?
            if [[ $status -ne 0 ]]; then
                echo "FAIL: ${name} exited with $status"; tail -20 "$name.log"; exit 1
            fi
            [[ -s "$name.png" ]] || { echo "FAIL: ${name} wrote no screenshot"; tail -20 "$name.log"; exit 1; }
            echo "ok: ${name} ran (screenshot $OUT/${name}.png)"
        }

        echo "== run game"
        # word splitting of SMOKE_GAME_ENV is wanted: one NAME=value per word
        if [[ "$SMOKE_PLAY" == 1 ]]; then
            # shellcheck disable=SC2086
            play game $SMOKE_GAME_ENV -- $SMOKE_GAME_ARGS
        else
            # shellcheck disable=SC2086
            (cd "$(dirname "$GAME")" && run ../game ../game.log "$SMOKE_GAME_SECONDS" env JASPERENGINE_READY_SIGNAL=1 $SMOKE_GAME_ENV "./$(basename "$GAME")" $SMOKE_GAME_ARGS)
        fi
        check_log game game.log
        if [[ -f project/smoke-expect.txt ]]; then
            while IFS= read -r line; do
                [[ -z "$line" || "$line" == "#"* ]] && continue
                grep -aqF -- "$line" game.log || { echo "FAIL: the game did not log: $line"; tail -20 game.log; exit 1; }
            done < project/smoke-expect.txt
            echo "ok: the game logged project/smoke-expect.txt"
        fi
        if [[ "$SMOKE_METHOD" == build && -f project/smoke-build-expect.txt ]]; then
            while IFS= read -r line; do
                [[ -z "$line" || "$line" == "#"* ]] && continue
                grep -aqF -- "$line" game.log || { echo "FAIL: the game did not log: $line"; tail -20 game.log; exit 1; }
            done < project/smoke-build-expect.txt
            echo "ok: the game logged project/smoke-build-expect.txt"
        fi
        # smoke-golden.json: the own frame of the game against an image (tools/ci/smoke_golden.py)
        if [[ -f project/smoke-golden.json ]]; then
            [[ "$SMOKE_PLAY" == 1 ]] || { echo "FAIL: project/smoke-golden.json compares the frame the game writes itself: run with SMOKE_PLAY=1"; exit 1; }
            python3 /src/tools/ci/smoke_golden.py project/smoke-golden.json game.png game-golden-diff.png \
                || { echo "FAIL: the frame of the game is not its golden ($OUT/game-golden-diff.png)"; exit 1; }
        fi

        # the extra runs: "<name> NAME=value ..." entries separated by ";"
        IFS=";" read -ra runs <<< "$SMOKE_GAME_RUNS"
        for entry in "${runs[@]}"; do
            read -ra words <<< "$entry"
            (( ${#words[@]} )) || continue
            label="${words[0]}"
            [[ "$label" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "FAIL: SMOKE_GAME_RUNS name \"$label\""; exit 1; }
            echo "== run game ($label)"
            if [[ "$SMOKE_PLAY" == 1 ]]; then
                # shellcheck disable=SC2086
                play "game-$label" $SMOKE_GAME_ENV "${words[@]:1}" --
            else
                # shellcheck disable=SC2086
                (cd "$(dirname "$GAME")" && run "../game-$label" "../game-$label.log" "$SMOKE_GAME_SECONDS" env JASPERENGINE_READY_SIGNAL=1 $SMOKE_GAME_ENV "${words[@]:1}" "./$(basename "$GAME")")
            fi
            check_log "game ($label)" "game-$label.log"
        done

        # S27: the Recorder from a shell, through a one-shot headless editor (S31 call), before the
        # editor GUI opens the project (its lock would outlive the stopped GUI here)
        echo "== record"
        dbus-run-session -- "$EDITOR" call --project project --wait-job --timeout 900 record_game --frames 30 --format png --width 320 --height 180 \
            --output "$PWD/recording" > record.log 2>&1 || { echo "FAIL: record"; tail -30 record.log; exit 1; }
        frames="$(find recording -name "frame_*.png" | wc -l)"
        [[ "$frames" -eq 30 ]] || { echo "FAIL: record wrote $frames frames, not 30"; tail -30 record.log; exit 1; }
        echo "ok: record wrote 30 frames ($OUT/recording)"

        # The export left this project as the most recent one in settings.yaml
        # here, so the editor reopens it. Its own log (editor.log) is written
        # next to settings.yaml, the console output goes to editor-console.log.
        echo "== run editor"
        : > editor.log
        run editor editor-console.log 20 dbus-run-session -- "$EDITOR"
        # llvmpipe compiles a variant at its first draw, on the render thread (no worker context on
        # a software GL), so a scene of many Lit variants can hold its first frame past the 3 s of
        # the watchdog: only that, on the first open, and then a clean open with the Mesa disk
        # cache warm (tools/ci/smoke_jit_hangs.py, docs/testing.md). Any other hang fails as ever.
        if grep -aq "JASPERENGINE HANG:" editor-console.log && python3 /src/tools/ci/smoke_jit_hangs.py editor-console.log; then
            echo "note: the first open stalled in the llvmpipe JIT: $(grep -a "recovered after" editor-console.log | sed "s/.*recovered after //" | paste -sd, -); opening again with its cache warm"
            # (run stops the D-Bus session; the editor it started holds the project lock: by its PID)
            first="$(python3 -c "import json; print(json.load(open(\"project/.jasperengine/user/editor.lock\"))[\"pid\"])")"
            kill "$first"
            for _ in $(seq 1 40); do kill -0 "$first" 2>/dev/null || break; sleep 0.5; done
            kill -9 "$first" 2>/dev/null || true
            cp editor-console.log editor-first-console.log
            cp editor.log editor-first.log
            : > editor.log
            run editor editor-console.log 20 dbus-run-session -- "$EDITOR"
        fi
        check_log editor editor.log editor-console.log

        # S19: a NavMesh Surface baked and a path queried through the MCP tools, in a new project
        echo "== navigation"
        UV="$(command -v uv || echo "$HOME/.local/bin/uv")"
        (cd /src && JASPER_EDITOR="$EDITOR" dbus-run-session -- "$UV" run --quiet --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/nav_smoke.py) || { echo "FAIL: navigation"; exit 1; }

        echo "PASS"
    '
