#!/usr/bin/env bash
# Screenshot the editor with a project open, under a virtual X server in the
# Linux build image. Run tools/build.sh linux (SKIP_ENGINE=1 is enough) first.
#
#   tools/editor-shot.sh <project-dir> [name] [seconds]
#
# The project is copied first. Writes build/shots/<name>.png and <name>.log.
# KEYS="F5" (xdotool key names, space separated) are pressed after <seconds>,
# followed by a second screenshot <name>-keys.png after KEYS_WAIT seconds.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
IMAGE=jasperengine/linux-build

[[ $# -ge 1 && -f "$1/project.yaml" ]] || { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
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
    -e KEYS="${KEYS:-}" -e KEYS_WAIT="${KEYS_WAIT:-5}" \
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
        if [[ -n \"\$KEYS\" ]]; then
            win=\$(xdotool search --onlyvisible --name . | tail -1 || true)
            [[ -n \"\$win\" ]] && xdotool windowactivate --sync \$win 2>/dev/null || true
            for k in \$KEYS; do xdotool key \$k; sleep 0.5; done
            sleep \$KEYS_WAIT
            kill -0 \$pid 2>/dev/null || { echo 'FAIL: editor exited after keys'; tail -30 ../$NAME.log; exit 1; }
            import -window root ../$NAME-keys.png
            echo 'ok: build/shots/$NAME-keys.png'
        fi
        kill \$pid
    "
