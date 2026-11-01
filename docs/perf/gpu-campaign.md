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

`tools/perf/async_shaders_bench.py --runs 5` (10 toggles, the editor focused and its vsync off),
re-measured in perf2 over every frame of the toggles: the first campaign's capture windows had
gaps and missed the worst frames, so its GL R22 (16.4x, worst 18.7 ms) was wrong. Frames that ran
an MCP tool (the edit, the bench's own calls) are apart (`get_frame_times` `tool`). Medians of 5:

| | R21 max main-thread creation (≤ 4 ms) | R22 worst / idle frame (≤ max(2, 1 + 4 ms / idle), cache warm) |
|---|---|---|
| GL `worker`, drivers' shader caches off (every run cold) | **0.016 ms** | 676 (755-828 ms) |
| GL `worker`, driver cache on (run 1 cold, runs 2-5 warm) | 0.016 ms | **1.69** (a second batch 2.06) |
| the same before the compile-start delay | 0.014 ms | 9.06 (9-11 ms every toggle) |
| GL, the same recording with no edit (`--idle`) | | 2.02 |
| Vulkan `thread` | **0.24 ms** | 3.7 (2.8-4.4 ms) |
| Vulkan with the GL fix's delay (not kept: no measurable change) | 0.22 ms | 3.0, a second batch 3.7 |
| Vulkan `--control` / `--idle` | | 1.5 / 2.2 |
| GL `sync` / Vulkan `sync` (red controls, first campaign) | 2192 / 799 ms | 1477 / 838 |

- **NVIDIA GL recompiles every program on the context that first draws it**, ~0.75 s per stage
  (fragment, then vertex; the driver's debug output: "being recompiled based on GL state ... not
  found in the disk cache"). Warm draws (on or off), a warm-draw state matched cap for cap,
  program binaries, `parallel` and `__GL_THREADED_OPTIMIZATIONS` do not avoid it; `sync` pays it
  inside its 2.1 s link. The driver's disk cache (its default) does: R22 1.69 with it warm.
- **Fixed: the ~10 ms stall per variant.** A compile or warm draw the GL worker starts within
  ~50 ms of its request held the render thread's next GL calls for 8-10 ms; the worker now starts
  it 60 ms after the request (GL 9.06 -> 1.69; hardware GL only).
- **Vulkan's 3.7** is a few frames per run of 2.5-3.8 ms of main-thread work in the editor's
  render while the driver compiles a pipeline on the worker; no creation work on the render thread
  (R21 0.22 ms), the mean frame unchanged, against a noise floor of 2.2 with no edit at all.
- The 2x bound sat at this machine's own noise at a 1 ms frame (2.0-2.2 with no edit): R22 is now
  max(2x idle, idle + 4 ms) with the driver cache warm (`docs/specs/S32-async-shaders.md` R22,
  approved by the user 2026-11-01; Vulkan and GL meet it). Details there (Progress, perf2).

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

### S11 materials (AC11, R13)

`tools/perf/gpu_campaign.py pbr --size 3840 2160` on the generated `build/pbr-extended` (200
spheres a scene), the Game view's GPU time; at 1080p a 0.24 ms frame let the GPU clock down mid-run
(1065-1695 MHz, ratios of ±40 % noise), so the scenes are measured at 4K, where it holds 1.7+ GHz.
The ratio is the whole view's, so it slightly understates a lobe's per-sphere cost.

| Scene | GPU (Standard) | vs Lit Baseline | Budget | High / Standard |
|---|---|---|---|---|
| Lit Baseline | 0.648 ms | | | 1.079 (≤ 1.05: **over**) |
| Lobes Off | 0.647 | -0.2 % | ≤ +2 % | 1.082 |
| Clearcoat | 0.788 | +21.6 % | ≤ +25 % | 1.025 |
| Sheen | 0.755 | +16.5 % **over** | ≤ +15 % | 1.103 |
| Anisotropy | 0.720 | +11.1 % | ≤ +15 % | 1.122 |
| Iridescence | 0.823 | +27.0 % | ≤ +30 % | 1.075 |
| Specular/IOR | 0.705 | +8.8 % **over** | ≤ +3 % | 1.054 |
| Transmission | 1.982 | +205.9 % (with the scene-colour copy, which the budget excludes) | ≤ +20 % | 1.047 |
| Hex Terrain / Terrain Baseline | 6.056 / 1.173 | **5.16x** | ≤ 1.35x | |

Software (E) could not see these (vertex-bound on llvmpipe: every lobe within 1 %).

**perf2** (5 runs GL, 3 Vulkan, 4K unless said; the same scenes):

| | GL | Vulkan | Budget |
|---|---|---|---|
| Sheen | +16.0 % | **+7.9 %** | ≤ +15 % |
| Specular/IOR | +8.3 % (+0.054 ms) | +3.8 % (+0.036 ms) | ≤ +3 % |
| Hex Terrain / Terrain Baseline | 6.41 -> **3.67 / 1.14 ms (3.2x)** | 8.11 -> **3.28 / 1.11 (3.0x)** | ≤ 1.35x |

- **Fixed (hex terrain, output unchanged):** a hex layer's taps (`HexTaps`, 13 vec2 and the
  barycentrics) were made once and kept live through the layer for its surface and normal maps; the
  NVIDIA compiler allocated registers for them over the whole layer loop even where those maps are
  never sampled. Made where each map samples them (the same inputs, so the same values): GL 5.94
  -> 3.67 ms, Vulkan 8.11 -> 3.28 ms. Also exact: a side plane with no weight is not fetched
  (6.41 -> 6.08), and a hex layer no longer samples the flat projection its hex colour overwrites
  (6.08 -> 5.94).
- What is left of the hex terrain, measured by removing parts: the hex taps themselves are cheap
  (one tap instead of three: -0.24 ms; no `pow` in the weights, a trivial hash, no lattice
  colouring: within noise; no rotation: -0.45 ms); the variant with every hex call removed still
  takes 2.63 ms against 1.15: a built-in Lit variant with extra defines (a custom shader
  reference, `RenderSystem::getSubmeshShaderReference`) costs more than the default variant with
  the same code. The same shows for the lobes: at 1080p Lobe Specular is +0.07 ms over Lobes Off,
  more than at 4K (+0.054 ms), so it is not shading; with the lobe shading removed (only the block
  and the define left) it is still +0.045 ms. That per-variant cost is the next lead for Sheen,
  Specular and hex; at the shader level they are within budget on Vulkan (Sheen) or a few
  hundredths of a millisecond from it.
- Tried and not kept: Sheen's view terms once a fragment and a single-evaluation Charlie lambda
  (+24.7 %, slower than +16.0 %); a 3-vec4 lobe block for Specular (no change).

### S20 particles (R23)

`gpu_campaign.py particles`: the reference stack (curl noise High, colour and size over lifetime,
billboards, additive) at 50,000 a second with 1.5-2.5 s lives, 0.05-0.15 m wide (3-10 px at 14 m):
**92,344 live particles, 0.245 ms** of GPU at 1080p over the empty scene (≤ 1.5 ms for 100 k), one
draw. Mobile (10 k, Adreno/Mali) pending.

### S16 HTML UI (R22-R24)

`HtmlUIRenderPerf` on the NVIDIA GL (5 runs; the harness has no GPU pass timers, so "frame" is the
wall time of a frame started with the GPU idle): B2 `list-500` 1,115 -> 3 draws, engine 1.40 ->
0.30 ms (21 %, R22 ≤ 50 %), frame 3.10 -> 0.36 ms; B6 the same; B3 `text-wall` 39 -> 1 draw, frame
0.085 -> 0.057 ms. R24 (B2's fragment cost ≤ baseline) has no pass-level GPU number; the frame is
8.6x faster. **Fixed:** since S32 these benchmarks measured frames with nothing drawn (every
pipeline still being created; llvmpipe too), so their gates compared empty frames; they now wait
until creation is idle over 5 drawn frames.

### S15 profiler overhead (AC4)

`tools/perf/profiler_overhead.py --runs 30`, the S15 merge against its parent (both with the
benchmark focus fix), frames of 0.81 ms: idle **+0.72 %** [+0.04, +1.40] (≤ 0.5 %: over, ~6 µs a
frame), recording +1.16 % (≤ 3 %), recording + GPU timers **+4.19 %** (≤ 3.5 %: over, ~34 µs).
Software (E, 120 rounds): idle +0.40 %.

perf2 (the fixture now 1.06 ms a frame; paired rounds, 30 each, today's editor): the idle machinery
switched off (sokol's stats, the trace hub and pass recorder) -0.31 % [-0.80, +0.18]; recording
+0.42 % over idle; recording + GPU timers **+3.16 %** [+2.39, +3.93] over idle (~34 µs a frame).
No `glFlush` per commit was slower (-1.17 % for keeping it), one availability query a commit and
cached counter monitors made no measurable change. The 0.5 % bound is 5 µs at this frame, under the
measurement's ±0.5-0.7 points: AC4 now has an absolute floor (5 / 30 / 40 µs a frame, whichever
of it and the percentage is larger; `docs/specs/S15-profiling-diagnostics.md`, approved by the
user 2026-11-01), which GL meets.

### Known measurement limits

- **Fixed (perf2): Vulkan pass timers in the editor dropped the Game view's passes on light
  frames.** A light frame's commit resolved before the frame ended and the profiler only gave
  times to frames already marked; they now wait for their frame (S15 Progress). No commit was
  dropped by the timer ring (`get_render_stats` `gpu.commits`).
- **The editor draws the Scene view behind a maximised Game view**: the benches keep the Game
  view unmaximised (its tab hides the Scene view) and size it with `fit_view`.
