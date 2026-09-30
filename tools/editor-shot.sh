#!/usr/bin/env bash
# Screenshot the editor with a project open, under a virtual X server in the
# Linux build image. Run tools/build.sh linux (SKIP_ENGINE=1 is enough) first.
#
#   tools/editor-shot.sh <project-dir> [name] [seconds]
#
# The project is copied first. Writes build/shots/<name>.png and <name>.log.
# KEYS="F5" (xdotool key names, space separated) are pressed after <seconds>,
# followed by a second screenshot <name>-keys.png after KEYS_WAIT seconds.
# CLICKS="x,y x,y,b" (screen pixels; b = mouse button, 3 right, 4/5 wheel) are
# clicked before KEYS, same screenshot.
# STEPS="..." then runs space separated steps: "x,y" left click, "r:x,y" right
# click, "m:x,y" move the mouse (hover), "k:key" key press, "w:secs" wait, and
# "s" to save <name>-1.png, <name>-2.png, ... (each step waits 0.4s).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
IMAGE=jasperengine/linux-build

[[ $# -ge 1 && -f "$1/project.yaml" ]] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
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

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
    -v "$ROOT:/src" -w "/src/$WORK" \
    -e HOME="/src/$WORK/home" \
    -e KEYS="${KEYS:-}" -e KEYS_WAIT="${KEYS_WAIT:-5}" -e CLICKS="${CLICKS:-}" -e STEPS="${STEPS:-}" \
    "$IMAGE" bash -euo pipefail -c "
        Xvfb :99 -screen 0 1600x900x24 > /dev/null 2>&1 &
        export DISPLAY=:99
        sleep 1
        dbus-run-session -- /src/$EDITOR > ../$NAME.log 2>&1 &
        pid=\$!
        sleep $SECS
        kill -0 \$pid 2>/dev/null || { echo 'FAIL: editor exited early'; tail -30 ../$NAME.log; exit 1; }
        import -window root ../$NAME.png
        echo 'ok: build/shots/$NAME.png'
        if [[ -n \"\$KEYS\" || -n \"\$CLICKS\" ]]; then
            win=\$(xdotool search --onlyvisible --name . | tail -1 || true)
            [[ -n \"\$win\" ]] && xdotool windowactivate --sync \$win 2>/dev/null || true
            for c in \$CLICKS; do IFS=, read -r cx cy cb <<< \"\$c\"; xdotool mousemove \$cx \$cy click \${cb:-1}; sleep 0.7; done
            for k in \$KEYS; do xdotool key \$k; sleep 0.5; done
            sleep \$KEYS_WAIT
            kill -0 \$pid 2>/dev/null || { echo 'FAIL: editor exited after keys'; tail -30 ../$NAME.log; exit 1; }
            import -window root ../$NAME-keys.png
            echo 'ok: build/shots/$NAME-keys.png'
        fi
        if [[ -n \"\$STEPS\" ]]; then
            n=0
            for step in \$STEPS; do
                case \"\$step\" in
                    s) n=\$((n+1)); import -window root ../$NAME-\$n.png; echo \"ok: build/shots/$NAME-\$n.png\" ;;
                    w:*) sleep \"\${step#w:}\" ;;
                    k:*) xdotool key \"\${step#k:}\" ;;
                    m:*) p=\${step#m:}; xdotool mousemove \${p/,/ } ;;
                    r:*) p=\${step#r:}; xdotool mousemove \${p/,/ } click 3 ;;
                    *) xdotool mousemove \${step/,/ } click 1 ;;
                esac
                sleep 0.4
            done
            kill -0 \$pid 2>/dev/null || { echo 'FAIL: editor exited during steps'; tail -30 ../$NAME.log; exit 1; }
        fi
        kill \$pid
    "
