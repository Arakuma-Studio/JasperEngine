#!/usr/bin/env bash
# tools/with-xvfb.sh [--screen WxHxD] command...
#
# Runs a command under a private Xvfb (DISPLAY set to it) and stops the server afterwards; the
# command's exit status is the script's. Replaces xvfb-run, whose start races: it traps SIGUSR1
# and then `wait`s for Xvfb's ready signal, so a server that signals before the wait begins
# leaves it waiting for the server to exit, forever (a test target that never starts its tests).
# Here Xvfb picks a free display itself and writes its number to a pipe (-displayfd); the wait
# for it is bounded: XVFB_TIMEOUT seconds (default 30), then the script fails with exit 2.
set -euo pipefail

SCREEN="1600x900x24"
if [[ "${1:-}" == --screen ]]; then
    SCREEN="$2"
    shift 2
fi
[[ $# -gt 0 ]] || { echo "usage: tools/with-xvfb.sh [--screen WxHxD] command..." >&2; exit 2; }
TIMEOUT="${XVFB_TIMEOUT:-30}"

dir="$(mktemp -d)"
mkfifo "$dir/display"
# read-write: the open does not wait for a writer, so a server that never starts cannot block it
exec 4<>"$dir/display"
Xvfb -displayfd 3 -screen 0 "$SCREEN" -nolisten tcp 3>"$dir/display" >"$dir/xvfb.log" 2>&1 &
server=$!
stop() {
    kill "$server" 2>/dev/null || true
    wait "$server" 2>/dev/null || true
    rm -rf "$dir"
}
trap stop EXIT

display=""
if ! read -r -t "$TIMEOUT" -u 4 display || [[ ! "$display" =~ ^[0-9]+$ ]]; then
    echo "with-xvfb: Xvfb gave no display within ${TIMEOUT}s" >&2
    cat "$dir/xvfb.log" >&2 || true
    exit 2
fi
exec 4<&-

status=0
DISPLAY=":$display" "$@" || status=$?
exit "$status"
