# Real-GPU performance campaign

The rendering streams' GPU budgets, measured on a real GPU with the S15 GPU pass timers
(`capture_profile` + `get_gpu_passes`, or `jasperengine-editor benchmark --gpu-passes`). Until this
campaign every stream could only be checked on software drivers (llvmpipe, lavapipe, SwiftShader).
The goldens stay the llvmpipe/lavapipe CI reference; GPU numbers are recorded here and in each
stream's spec.

## Machine

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 3080 Ti (GA102, 12 GB), PCIe 4.0 x16 |
| Driver | 595.99.02 (GL 4.6.0 NVIDIA 595.99.02, Vulkan 1.4.329) |
| CPU / RAM | AMD Ryzen, 24 threads / 94 GB |
| OS | Ubuntu 24.04 host; builds and runs in the `linux` build image (Ubuntu 22.04) |
| Host | server H (`rb.sh H`), a user's desktop PC: its KDE session runs on the same GPU, so the GPU is shared (the median of 5 runs absorbs its load) |

The budgets name a "mid desktop" GPU (GTX 1660 / RX 5500 class; S10 also names 1440p for its
desktop tier). The RTX 3080 Ti is roughly 4-5x a GTX 1660 in shader throughput and bandwidth, so a
number here that is within budget is not proof for the reference class. The tables give the
measured value and, where it decides, the headroom.

## GPU access (headless)

**Vulkan** needs no display server. The NVIDIA container runtime (`--gpus all`,
`NVIDIA_DRIVER_CAPABILITIES=all`) mounts the driver's libraries but not the loader manifests, so the
Vulkan and EGL loaders only see Mesa; `tools/gpu/env.sh` writes the two manifests
(`libGLX_nvidia.so.0` for Vulkan, `libEGL_nvidia.so.0` for EGL) and points `VK_ICD_FILENAMES` and
`__EGL_VENDOR_LIBRARY_FILENAMES` at them.

**OpenGL** goes through GLX on Linux (the editor, the player and the test context), and a Mesa
Xvfb gives Mesa's GLX. EGL on the NVIDIA device works headless (`EGL_EXT_platform_device`, desktop
GL, pbuffers), but the engine has no EGL path on desktop Linux, and adding one would not exercise
the GLX code (and the GLX worker context of S32) that ships. So GL runs on a **separate headless
Xorg on the NVIDIA GPU**, `tools/gpu/xorg-headless.sh`:

- Xorg runs in its own unprivileged container (`docker/gpu-xorg.Dockerfile`: Ubuntu 22.04 and
  `xserver-xorg-core` only) with `--gpus all`; the host's NVIDIA Xorg driver and GLX module
  (`/usr/lib/x86_64-linux-gnu/nvidia/xorg`) are mounted read-only, since they must match the
  kernel driver;
- `tools/gpu/xorg-nvidia-headless.conf`: `UseDisplayDevice "none"` (the driver logs "enabling
  NoScanout": nothing is scanned out), `AllowEmptyInitialConfiguration`, no input devices
  (`AutoAddDevices false`), no VT switching, a 3840x2160 virtual screen;
- the container has no host `/dev/tty*`, no `/dev/input` and no `/dev/dri`, so it cannot reach the
  host's console, VTs or desktop session; the desktop's own Xorg (`:0`, vt2) is untouched;
- display `:97`; the socket goes to `<worktree>/build/.x11/X97`, which the build image sees as
  `/src/build/.x11`; `tools/gpu/env.sh` links `/tmp/.X11-unix` to it inside the build container and
  sets `DISPLAY=:97` (and vsync off: `__GL_SYNC_TO_VBLANK=0`).

```sh
rb.sh H tools/gpu/xorg-headless.sh start          # once; `stop` when done, `status`
JIMG_GPU=1 rb.sh H --img '. tools/gpu/env.sh && glxinfo -B | grep renderer'
#   OpenGL renderer string: NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2
JIMG_GPU=1 rb.sh H --img '. tools/gpu/env.sh && JASPER_XVFB=none uv run ... tools/perf/gpu_campaign.py shadows'
DOCKER_GPU=1 rb.sh H tools/build.sh unit-tests    # tools/with-xvfb.sh uses :97 instead of an Xvfb
```

With `JASPER_XVFB=none` the MCP harness gives the editor the caller's `DISPLAY` (`:97`).
`get_render_stats.gpu` reports `backend` and `device`; every bench below records the device it ran
on (GL: `NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2`; Vulkan: `NVIDIA GeForce RTX 3080 Ti`).

Nothing on the host was changed for this: the X server, its image and its socket live in Docker
and under `~/je`.

## Method

- Release editors (`tools/build.sh linux`, GL and `GRAPHIC_BACKEND=vulkan`), WERROR.
- Each case: a new editor over MCP, vsync off, the Game view maximised on Play and sized to the
  case's resolution with `tools/perf/gpu_view.py fit_view` (the X window is resized until the view
  renders exactly W x H), shader creation idle, then 300 frames recorded with GPU timing; each pass
  label's median GPU ms per frame. 5 runs, the median of the runs.
- Scripts: `tools/perf/gpu_campaign.py` (S10, S11, S13, S20), `tools/performance/atmosphere_bench.py
  --gpu` (S12), `tools/perf/async_shaders_bench.py` (S32), `tools/perf/profiler_overhead.py` (S15),
  `tools/performance/vat_crowd.py --gpu-passes` (VAT, S21's skinned crowd).

## Results

(below)
