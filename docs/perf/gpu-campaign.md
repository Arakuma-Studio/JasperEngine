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

Medians of 5 runs unless noted; "software" is the earlier figure on the benchmark server E
(llvmpipe/lavapipe) for comparison. GPU clocks during the captures: 1.7-1.9 GHz (boost 2.1 GHz),
recorded per case (`_clock_mhz`); light frames (under ~0.3 ms) let the GPU clock down, which is
why S11 is measured at 4K.

### S32 asynchronous shaders (R21, R22)

`tools/perf/async_shaders_bench.py --runs 5` (10 toggles, the editor focused and its vsync off).

| | R21 max main-thread creation (≤ 4 ms) | R22 worst / idle frame (≤ 2) | software (E) |
|---|---|---|---|
| GL `worker`, warm draws on (new default) | **0.014 ms** | 16.4 (worst 11-18.7 ms, idle 1.13 ms) | `parallel` 385.7 ms / 10.0 |
| GL `worker`, warm draws off | 0.015 ms | 15.7 | |
| GL `parallel` (compile threads fixed) | 8.83 ms | 10.6 | |
| GL `sync` | 2192 ms | 1477 | 375.5 ms / 12.9 |
| GL control: the same edits, no creation | 0.011 ms | 8.7 | |
| Vulkan `thread` | **0.227 ms** | 3.9 (worst 3.7 ms) | 0.105 ms / 16.0 |
| Vulkan `sync` | 799 ms | 838 | 298.4 ms / 21.3 |

R21 holds on GL `worker` and Vulkan. R22 does not: on NVIDIA GL the worker's GLSL front end (about
8 ms per new variant) holds the render thread's GL calls, and an unwarmed first draw compiles
another ~8 ms; the no-creation control shows that one edit already costs the editor ~10 ms
against a 1.1 ms idle frame. Fixed: `parallel` mode got no driver compile threads on NVIDIA (a
2.1 s link on the render thread, now 8.8 ms). Defaults: GL Auto = `worker`, warm draws on for desktop
GL, off on GLES. Details: `docs/specs/S32-async-shaders.md` (Progress).

### S10 shadows (AC14)

`tools/perf/gpu_campaign.py shadows`, the bench fixture, desktop tiers at 2560x1440, web and mobile
tiers at 1920x1080 (budgets for their own devices). GL:

| Tier | Receiver, sun soft | Static, cached | Walking, cached + 20 dynamic | software |
|---|---|---|---|---|
| Low | 0.061 ms (≤ 1.0) | 0.000 (≤ 0.05) | 1.209 (≤ 1.5) | not timed (no GPU timers then) |
| Medium | 0.095 (≤ 1.5) | 0.000 | 0.664 (≤ 2.0) | |
| High | 0.183 (≤ 0.6) | 0.000 | **0.328** (≤ 1.0; was 7.174) | |
| Ultra | 0.233 (≤ 1.2) | 0.000 | **0.527** (≤ 1.5; was 8.789) | |

Fixed: High and Ultra had no static shadow layer (their D32 atlases filled the memory row) and
refitted their cascades every frame, so a walking camera redrew all 2,000 static casters each
frame. They now freeze their fits and have memory rows of 192 / 576 MB
(`docs/rendering/shadow-budgets.md`). Vulkan (the frames it timed, below): Ultra walking 0.167 ms of
shadow passes, High walking 0.122 ms.

### S12 atmosphere (AC10)

`tools/performance/atmosphere_bench.py --gpu --runs 5`, 1920x1080, the Atmosphere/* passes each
feature adds (height fog and the Sky Composite have no pass of their own: the camera pass's GPU
time over its baseline's). GL / Vulkan:

| Feature | GL | Vulkan | Budget | software (E, relative method) |
|---|---|---|---|---|
| Height fog (over linear) | 0.003 ms | - | ≤ 0.10 | within |
| Sky Composite | 0.008 ms | - | ≤ 0.15 | within |
| Volumetric fog Low (mobile budget) | 0.094 ms | - | ≤ 1.5 | 2-10x over |
| Volumetric fog Medium | 0.391 ms | 0.349 | ≤ 1.0 | over |
| Volumetric fog High | 0.761 ms | 0.662 | ≤ 1.8 | over |
| Clouds Low (mobile budget) | 0.067 ms | - | ≤ 0.6 | within |
| Clouds Medium | 0.184 ms | - | ≤ 0.8 | over |
| Clouds High | 0.307 ms | 0.319 | ≤ 2.5 | over |

All within budget on this card; the software method's 2-10x misses were its own (fragment-bound
passes on a CPU). VRAM unchanged (11.8 MB Medium/High, 3.8 MB clouds High). Scaled to a GTX 1660
class (4-5x slower) Volumetric High would be about 3-3.8 ms against 1.8: to be confirmed there.

### S13 post FX

GL, the post-processing fixture at 1920x1080: the whole Game view 0.25-0.27 ms with any preset;
`film` and `vhs` cost -0.002 / -0.008 ms over `mobile` (within noise). The R14 budget is a
Mali-G57 / Adreno 610 phone's and stays **pending** (no phone here).

### VAT and S21 GPU skinning

`tools/performance/vat_crowd.py --gpu-passes` (benchmark CLI, Game view 752x362, 300 frames):

| Heroes | Case | GL frame / GPU | Vulkan frame / GPU |
|---|---|---|---|
| 1,000 | skinned (Animation Players) | 155.8 / 28.76 ms | 145.0 / 2.90 ms |
| 1,000 | VAT Vertices | 1.09 / 0.67 ms | 1.00 / 0.58 ms |
| 1,000 | VAT Bones | 1.07 / 0.79 ms | 1.01 / 0.55 ms |
| 2,000 | skinned | 589.1 / 61.17 ms | 563.5 / 6.62 ms |
| 2,000 | VAT Vertices / Bones | 1.49 / 1.33, 1.72 / 1.54 ms | 1.36 / 1.18, 1.26 / 1.09 ms |
| 5,000 | skinned | 1632.9 / 101.8 ms | 1564.6 / 19.1 ms |
| 5,000 | VAT Vertices / Bones | 3.65 / 3.34, 4.17 / 3.83 ms | 3.27 / 2.96, 3.03 / 2.75 ms |

VAT draws 5,000 heroes in 3-4 ms of GPU on either backend. The skinned crowd is CPU-bound
(posing; 126-1624 ms of CPU) and its GL GPU time is ~10x Vulkan's: the bone matrices go to the
GPU as one texture per hero on GL (an upload per hero per frame) and as storage buffers on Vulkan.
S21 has no GPU budget; recorded for its follow-up.

### Known measurement limits

- **Vulkan pass timers in the editor drop the Game view's passes on light frames.** Captures of
  frames under ~0.5 ms (the S10 receiver and static cases, S13, S20, the light S12 configurations)
  have no Game-view pass in any run, while heavy ones (S12 volumetric/clouds High, S10 Ultra) are
  timed in every run. The benchmark CLI's frame GPU time is not affected (VAT above). Likely the
  timer's per-commit results arriving after the ring drops them (`sgj_poll`, `SGJ_MAX_AGE`) at
  1,000+ uncapped frames a second with three commits each; open for S15. The benches use only the
  runs a capture timed, and GL is the reference.
- **The editor draws the Scene view behind a maximised Game view**: the benches keep the Game
  view unmaximised (its tab hides the Scene view) and size it with `fit_view`.
