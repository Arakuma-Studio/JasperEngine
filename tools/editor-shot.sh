#!/usr/bin/env bash
# Screenshot the editor with a project open, under a virtual X server in the
# Linux build image. Run tools/build.sh linux (SKIP_ENGINE=1 is enough) first.
#
#   tools/editor-shot.sh <project-dir> [name] [seconds]
#
# The project is copied first. Writes build/shots/<name>.png and <name>.log
# after <seconds>. Then, if CLICKS or KEYS are set, their steps run (CLICKS
# first) and <name>-keys.png is saved KEYS_WAIT seconds later. STEPS run after
# that. All three take the same space separated steps:
#   X,Y or X,Y,B   click (button B: 1 left, 3 right, 4/5 wheel up/down)
#   r:X,Y          right click          m:X,Y       move the mouse (hover)
#   dbl:X,Y        double click (opens a scene or prefab in the Project window)
#   run:SCRIPT     run bash SCRIPT (a path under the repo) from the run directory, whose
#                  project/ is the open project: a change made outside the editor
#   drag:X1,Y1,X2,Y2  left drag         wheel:X,Y,N scroll N notches (N<0: up)
#   k:K, key:K, K  press xdotool key K (e.g. F5, ctrl+s, space)
#   kd:K, ku:K     hold / release key K, for chords the editor must see across
#                  frames (e.g. kd:ctrl kd:z w:0.3 ku:z ku:ctrl)
#   w:N, sleep:N   wait N seconds
#   shot:TAG       save <name>-TAG.png  s   save <name>-1.png, <name>-2.png, ...
# EXPORT_DIR=build/x (under the repo) sets the default Build Settings output.
# EXTRA_SETTINGS (YAML) is appended to the run's settings.yaml (e.g. a window to open at start).
# SHOT_GPU=1 (the GPU host, after tools/gpu/xorg-headless.sh start with JE_GPU_SCREEN=1600x900) runs
# the editor on its NVIDIA X server instead of an Xvfb, so the canvas renders on the real GPU.
# SHOT_FONTS=<dir> copies that directory's font files into the run's ~/.local/share/fonts, where
# fontconfig (and so the editor's system font search) finds them: e.g. a CJK font the image lacks.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
IMAGE=jasperengine/linux-build

[[ $# -ge 1 && -f "$1/project.yaml" ]] || { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
PROJECT="$(cd "$1" && pwd)"
NAME="${2:-editor}"
SECS="${3:-15}"
[[ -x "$ROOT/$EDITOR" ]] || { echo "No editor at $EDITOR; run tools/build.sh linux" >&2; exit 1; }

WORK="build/shots/$NAME.run"
docker run --rm -v "$ROOT:/src" "$IMAGE" rm -rf "/src/$WORK"
mkdir -p "$ROOT/$WORK/home"
cp -r "$PROJECT" "$ROOT/$WORK/project"
# The editor reads settings.yaml from its working directory when writable.
cat > "$ROOT/$WORK/settings.yaml" <<EOF
last_project_path: /src/$WORK/project
recent_projects:
  - /src/$WORK/project
window:
  width: 1600
  height: 900
  maximized: true
EOF
if [[ -n "${EXPORT_DIR:-}" ]]; then
    printf 'export:\n  default_dir: /src/%s\n' "$EXPORT_DIR" >> "$ROOT/$WORK/settings.yaml"
fi
if [[ -n "${EXTRA_SETTINGS:-}" ]]; then
    printf '%s\n' "$EXTRA_SETTINGS" >> "$ROOT/$WORK/settings.yaml"
fi
if [[ -n "${SHOT_FONTS:-}" ]]; then
    mkdir -p "$ROOT/$WORK/home/.local/share/fonts"
    cp "$SHOT_FONTS"/*.[ot]t[fc] "$ROOT/$WORK/home/.local/share/fonts/"
fi

# Runs inside the container, from $WORK.
cat > "$ROOT/$WORK/run.sh" <<'EOF'
set -euo pipefail
shots=0
alive() { kill -0 "$pid" 2>/dev/null || { echo "FAIL: editor exited $1"; tail -30 "../$NAME.log"; exit 1; }; }
shot() { import -window root "../$NAME$1.png"; echo "ok: build/shots/$NAME$1.png"; }
steps() {
    for t in $1; do
        case "$t" in
            s) shots=$((shots + 1)); shot "-$shots" ;;
            shot:*) shot "-${t#shot:}" ;;
            w:*) sleep "${t#w:}" ;;
            sleep:*) sleep "${t#sleep:}" ;;
            k:*) xdotool key "${t#k:}" ;;
            key:*) xdotool key "${t#key:}" ;;
            kd:*) xdotool keydown "${t#kd:}" ;;
            ku:*) xdotool keyup "${t#ku:}" ;;
            m:*) IFS=, read -r x y <<< "${t#m:}"; xdotool mousemove "$x" "$y" ;;
            r:*) IFS=, read -r x y <<< "${t#r:}"; xdotool mousemove "$x" "$y"; sleep 0.2; xdotool click 3 ;;
            click:*) IFS=, read -r x y <<< "${t#click:}"; xdotool mousemove "$x" "$y"; sleep 0.2; xdotool click 1 ;;
            dbl:*) IFS=, read -r x y <<< "${t#dbl:}"; xdotool mousemove "$x" "$y"; sleep 0.2; xdotool click --repeat 2 --delay 80 1 ;;
            run:*) bash "/src/${t#run:}" ;;
            wheel:*) IFS=, read -r x y n <<< "${t#wheel:}"; xdotool mousemove "$x" "$y"; sleep 0.2
                if (( n > 0 )); then xdotool click --repeat "$n" --delay 100 5; else xdotool click --repeat $(( -n )) --delay 100 4; fi ;;
            drag:*) IFS=, read -r x1 y1 x2 y2 <<< "${t#drag:}"
                xdotool mousemove "$x1" "$y1"; sleep 0.3; xdotool mousedown 1; sleep 0.3
                for i in 1 2 3 4 5 6 7 8 9 10; do
                    xdotool mousemove $(( x1 + (x2 - x1) * i / 10 )) $(( y1 + (y2 - y1) * i / 10 )); sleep 0.1
                done
                sleep 0.3; xdotool mouseup 1 ;;
            [0-9]*,[0-9]*) IFS=, read -r x y b <<< "$t"; xdotool mousemove "$x" "$y"; sleep 0.2; xdotool click "${b:-1}" ;;
            *) xdotool key "$t" ;;
        esac
        sleep 0.5
    done
}

if [[ "${SHOT_GPU:-0}" == 1 ]]; then
    # the GPU host's headless NVIDIA X server (tools/gpu/xorg-headless.sh, JE_GPU_SCREEN=1600x900)
    . /src/tools/gpu/env.sh
else
    Xvfb :99 -screen 0 1600x900x24 > /dev/null 2>&1 &
    export DISPLAY=:99
fi
sleep 1
dbus-run-session -- "/src/$EDITOR" > "../$NAME.log" 2>&1 &
pid=$!
sleep "$SECS"
alive early
shot ""
if [[ -n "$CLICKS$KEYS$STEPS" ]]; then
    win=$(xdotool search --onlyvisible --name . | tail -1 || true)
    [[ -n "$win" ]] && xdotool windowactivate --sync "$win" 2>/dev/null || true
fi
if [[ -n "$CLICKS$KEYS" ]]; then
    steps "$CLICKS"
    steps "$KEYS"
    sleep "$KEYS_WAIT"
    alive "after keys"
    shot "-keys"
fi
if [[ -n "$STEPS" ]]; then
    steps "$STEPS"
    alive "during steps"
fi
kill "$pid"
EOF

GPU_ARGS=()
[[ "${SHOT_GPU:-0}" == 1 ]] && GPU_ARGS=(--gpus all -e SHOT_GPU=1)
docker run --rm "${GPU_ARGS[@]}" \
    --user "$(id -u):$(id -g)" \
    -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
    -v "$ROOT:/src" -w "/src/$WORK" \
    -e HOME="/src/$WORK/home" \
    -e NAME="$NAME" -e SECS="$SECS" -e EDITOR="$EDITOR" \
    -e KEYS="${KEYS:-}" -e KEYS_WAIT="${KEYS_WAIT:-5}" -e CLICKS="${CLICKS:-}" -e STEPS="${STEPS:-}" \
    "$IMAGE" bash run.sh
