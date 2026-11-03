# Source inside the build image on a GPU host (JIMG_GPU=1 rb.sh H --img '. tools/gpu/env.sh && ...',
# or tools/build.sh with DOCKER_GPU=1). The NVIDIA container runtime mounts the driver's libraries
# but not their loader manifests, so the Vulkan and EGL loaders would only see Mesa. This writes
# the two manifests and points the loaders at them, and, when tools/gpu/xorg-headless.sh runs,
# points DISPLAY at its headless NVIDIA X server so GLX is NVIDIA's.
#   JE_GPU_DISPLAY=97 (default) | none (keep DISPLAY, e.g. for xvfb-run + Vulkan only)
_je_gpu_icd="${HOME:-/tmp}/.je-gpu-icd"
mkdir -p "$_je_gpu_icd"
if [ -e /usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0 ]; then
  printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libGLX_nvidia.so.0","api_version":"1.4"}}\n' > "$_je_gpu_icd/nvidia_icd.json"
  printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0"}}\n' > "$_je_gpu_icd/10_nvidia.json"
  export VK_ICD_FILENAMES="$_je_gpu_icd/nvidia_icd.json"
  export __EGL_VENDOR_LIBRARY_FILENAMES="$_je_gpu_icd/10_nvidia.json"
else
  echo "[gpu-env] no NVIDIA libraries in this container (run with JIMG_GPU=1 / DOCKER_GPU=1)" >&2
fi
_je_gpu_disp="${JE_GPU_DISPLAY:-97}"
if [ "$_je_gpu_disp" != none ]; then
  _je_gpu_sock="$(cd "$(dirname "${BASH_SOURCE[0]:-tools/gpu/env.sh}")/../.." && pwd)/build/.x11"
  if [ -S "$_je_gpu_sock/X$_je_gpu_disp" ]; then
    # Xlib looks in /tmp/.X11-unix; the image's /tmp is private to this container
    # (a real directory holding links to its sockets: Xvfb can't make its own socket under a symlink,
    # and this run's other tests are on llvmpipe through their own Xvfb)
    [ -L /tmp/.X11-unix ] && rm -f /tmp/.X11-unix
    mkdir -p /tmp/.X11-unix
    chmod 1777 /tmp/.X11-unix 2>/dev/null || true
    for _je_gpu_x in "$_je_gpu_sock"/X*; do
      [ -S "$_je_gpu_x" ] && ln -sf "$_je_gpu_x" "/tmp/.X11-unix/$(basename "$_je_gpu_x")"
    done
    export DISPLAY=":$_je_gpu_disp"
    # benches must not throttle on a vsync of a display that does not exist
    export __GL_SYNC_TO_VBLANK=0 vblank_mode=0
  else
    echo "[gpu-env] no headless X server at $_je_gpu_sock/X$_je_gpu_disp (tools/gpu/xorg-headless.sh start)" >&2
  fi
fi
unset _je_gpu_icd _je_gpu_disp _je_gpu_sock
