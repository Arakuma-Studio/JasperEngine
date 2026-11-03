#!/usr/bin/env bash
# tools/gpu/xorg-headless.sh start|stop|status [display]  — a headless Xorg on the host's NVIDIA GPU,
# for real-GPU GL runs of the editor and the engine (GLX), next to a desktop that uses the same GPU.
#
# The X server runs in its own unprivileged container (docker/gpu-xorg.Dockerfile) with --gpus all:
#   - the host's NVIDIA Xorg driver and GLX module are mounted read-only (they match its kernel driver);
#   - no display device (UseDisplayDevice none), no input devices, no VT switching
#     (tools/gpu/xorg-nvidia-headless.conf); the container has no host /dev/tty* and no /dev/input,
#     so the host's console and desktop session are never touched;
#   - its socket goes to <worktree>/build/.x11/X<display>, which the build image sees as
#     /src/build/.x11; tools/gpu/env.sh (sourced inside the image) points DISPLAY at it.
# JE_GPU_SCREEN=WxH sets its screen (default 3840x2160; the MCP suite's GPU lane uses 1600x900, the
# size of the Xvfb an editor makes itself, so a test's windows and views come out the same size).
# Default display :97. Run it on the GPU host from the worktree (rb.sh H tools/gpu/xorg-headless.sh start),
# then: JIMG_GPU=1 rb.sh H --img '. tools/gpu/env.sh && <command>'. Stop it when done.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ACTION="${1:-status}"
DISP="${2:-97}"
NAME="je-gpu-xorg-$DISP"
SOCKDIR="$ROOT/build/.x11"
NVXORG="${NVIDIA_XORG_DIR:-/usr/lib/x86_64-linux-gnu/nvidia/xorg}"

running() { [[ "$(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null || true)" == true ]]; }

case "$ACTION" in
  start)
    if running; then echo "[xorg-headless] $NAME already running (:$DISP)"; exit 0; fi
    [[ -f "$NVXORG/nvidia_drv.so" ]] || { echo "[xorg-headless] no NVIDIA Xorg driver in $NVXORG" >&2; exit 1; }
    HASH="$(sha256sum "$ROOT/docker/gpu-xorg.Dockerfile" | cut -c1-12)"
    IMG="jasperengine/gpu-xorg:$HASH"
    docker image inspect "$IMG" >/dev/null 2>&1 || docker build -q -t "$IMG" -f "$ROOT/docker/gpu-xorg.Dockerfile" "$ROOT/docker" >/dev/null
    mkdir -p "$SOCKDIR"; rm -f "$SOCKDIR/X$DISP" "$SOCKDIR/xorg-$DISP.log"
    SCREEN="${JE_GPU_SCREEN:-3840x2160}"
    [[ "$SCREEN" =~ ^[0-9]+x[0-9]+$ ]] || { echo "[xorg-headless] JE_GPU_SCREEN must be WxH (got $SCREEN)" >&2; exit 2; }
    CONF="$SOCKDIR/xorg-$DISP.conf"
    sed "s/Virtual 3840 2160/Virtual ${SCREEN%x*} ${SCREEN#*x}/" "$ROOT/tools/gpu/xorg-nvidia-headless.conf" > "$CONF"
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    docker run -d --rm --name "$NAME" --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all \
      -v "$NVXORG:/nvidia-xorg:ro" \
      -v "$CONF:/etc/X11/xorg-headless.conf:ro" \
      -v "$SOCKDIR:/tmp/.X11-unix" \
      "$IMG" Xorg ":$DISP" -config /etc/X11/xorg-headless.conf -noreset -nolisten tcp -ac \
        -sharevts -novtswitch -keeptty -logfile "/tmp/.X11-unix/xorg-$DISP.log" >/dev/null
    for _ in $(seq 1 100); do
      [[ -S "$SOCKDIR/X$DISP" ]] && { echo "[xorg-headless] $NAME up: socket $SOCKDIR/X$DISP"; exit 0; }
      running || break
      sleep 0.2
    done
    echo "[xorg-headless] Xorg :$DISP did not come up; log:" >&2
    cat "$SOCKDIR/xorg-$DISP.log" 2>/dev/null | grep -E '\(EE\)|\(WW\)|Fatal' >&2 || docker logs "$NAME" >&2 || true
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    exit 1 ;;
  stop)
    docker rm -f "$NAME" >/dev/null 2>&1 && echo "[xorg-headless] $NAME stopped" || echo "[xorg-headless] $NAME not running"
    # the socket and the log belong to the container's root (and Xorg made the directory sticky)
    rm -f "$SOCKDIR/xorg-$DISP.conf"
    if [[ -e "$SOCKDIR/X$DISP" || -e "$SOCKDIR/xorg-$DISP.log" ]]; then
        IMG="jasperengine/gpu-xorg:$(sha256sum "$ROOT/docker/gpu-xorg.Dockerfile" | cut -c1-12)"
        docker run --rm -v "$SOCKDIR:/x" "$IMG" rm -f "/x/X$DISP" "/x/xorg-$DISP.log" 2>/dev/null || rm -f "$SOCKDIR/X$DISP"
    fi ;;
  status)
    if running; then echo "[xorg-headless] $NAME running (:$DISP)"; else echo "[xorg-headless] $NAME not running"; exit 1; fi ;;
  *) echo "usage: $0 start|stop|status [display]" >&2; exit 2 ;;
esac
