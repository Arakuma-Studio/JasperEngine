# Release Performance and GPU Quirks: a gameplay frame sampler that ships in Release, and a table of driver workarounds that change how a frame is made, never what it shows

Builds on S15 (frame-time ring, profiler, benchmark; landed), S32 (asynchronous shader and
pipeline creation; landed) and S56 (device tiers; landed). Two work packages that can run in
parallel: P1 the Performance sampler, P2 GPU quirks. Paths are relative to the repository root.

## 1. Goal

A Release export has no profiler, and every number the project has about frame cost comes from
llvmpipe or one desktop NVIDIA card; the mobile figures in `docs/perf/gpu-campaign.md` are still
"pending (no phone here)". A shipped game cannot tell its developer how fast it runs on the
players' devices, or whether a slow frame is the main thread computing or waiting on the driver.
P1 adds a **Performance sampler** that ships in every build, costs one branch per frame when off and
a few nanoseconds when on, counts only gameplay frames, and hands the game a summary (frame-time
percentiles, main-thread CPU time from the thread clock, device tier, GPU, driver, thermal time)
for its own analytics; the engine sends nothing. P2 replaces today's scattered driver checks with a
**GPU quirk table**: named workarounds turned on by data (renderer, driver version, OS, API),
overridable from the command line, visible to Lua, MCP, logs and benchmarks, each proven by a test
to draw the same image. A quirk may change **how** a frame is made, never **what** it shows.

## 2. Scope

**In scope**
- P1: `PerfSampler` (C++) and Lua `Performance`: gameplay-only sampling with engine and game
  exclusions, a fixed-bin frame-time histogram, percentiles, main-thread CPU per frame from the
  platform's thread clock, GPU time when S15's timer runs anyway, thermal and tier context,
  `onSample` windows; `--perf-summary=<path>`; `get_frame_times` and the benchmark gain main-thread
  CPU; the Profiler's Monitors and the Game view's Stats show it.
- P2: `GpuQuirks` (C++), `engine/core/device/gpu_quirks.json`, a driver-version string per backend,
  `--gpu-quirk`, `?gpu-quirk=`, `JASPERENGINE_GPU_QUIRKS`, Lua `Device.gpuQuirks` and
  `Device.gpuDriver`, `get_device_info` and benchmark JSON fields, a lint that keeps renderer-name
  checks out of render code, and the migration of the three driver decisions S32 already makes.

**Out of scope (decided)**
- Network transport, consent, sessions, cohorts, A/B experiments: the game owns analytics. The
  engine never sends a sample (S28's rule for reports applies here too).
- Self-adjusting budgets. The sampler only reports; S32's 2 ms main-thread creation budget
  (`engine/core/render/GpuCreation.h:21-22`), S56's upload budget and the thermal response stay
  fixed and rule-based. A frame budget that tunes itself from measurements oscillates and hides
  regressions.
- Per-thread CPU of job workers, GPU counters (busy %, bandwidth), power draw: not portable.
- Changing what a tier does: quality reductions stay S56 tier overrides. A workaround that changes
  the image (for example drawing cutout foliage opaque to keep a tile GPU's hidden-surface removal)
  is a quality setting or a tier cell, not a quirk.
- Inventing driver bugs. The initial table holds only the three decisions JasperEngine makes today
  (D8). New rules need a reproduced issue and a test (R27).
- Live quirk changes: quirks are resolved once per process.

## 3. Current state

All read from the code (not run):

- **Frame times in every build.** `FrameTimes` keeps a 600-frame ring of 32-byte samples
  (`frameMs`, `cpuMs`, `gpuMs`, `fps`, `drawCalls`, `flags`) "recorded in every build whether the
  profiler is on, compiled in or not" (`engine/core/profiler/FrameTimes.h:7-11`, `:21-30`).
  `beginFrame` reads `steady_clock` once and completes the previous sample (`FrameTimes.cpp:41-67`);
  `cpuMs` is wall time from frame start to `endFrameWork` (`:88-92`), called from
  `Engine::runFrame` (`engine/core/Engine.cpp:2423-2427`, `:2830-2831`). `FLAG_HOST_WORK` marks
  editor frames that ran an MCP tool (`FrameTimes.h:37-39`).
- **What a game can read.** `Profiler.frameStats()` (last frame: draws, cpuMs, fps, gpuMs while
  timed) works in Release (`stubs/Profiler.lua:5-13`, `:44-46`); `DebugOverlay.getStats()` gives
  2-second fps, min/max and "1% low" (`engine/core/debug/DebugStats.h:15-41`). Nothing aggregates
  over minutes, nothing separates gameplay from menus, loading or the app in the background.
- **No profiler in Release** (`docs/profiling.md`, Builds table: "a Release export | none").
- **Thread CPU time only in tests.** `CLOCK_THREAD_CPUTIME_ID` and `GetThreadTimes` appear only in
  `engine/tests/core/log/LoggerBenchTest.cpp:168`, `animation/AnimationPlayerTest.cpp:1262-1267`,
  `animation/ClipAudioPlayersTest.cpp:263-274` and `jobs/JobSystemTest.cpp:402`. So "CPU ms"
  everywhere includes waits on the driver, locks and I/O inside the frame's work.
- **Pause and focus.** `Engine::applyPause` broadcasts `onApplicationPause` and pauses the hang
  watchdog (`Engine.cpp:3027-3056`); `systemFocus` and `systemVisibilityChanged` fire
  `onFocus`/`onVisibilityChanged` (`:3058-3087`); `Engine::onSceneLoaded` exists
  (`Engine.h:1138`); `Engine::areScenesLoaded(textures)` says whether loading is done
  (`Engine.h:650-654`).
- **Benchmark.** `jasperengine-editor benchmark` writes `cpuMs {median, p95, mean}` from
  `FrameTimes.cpuMs` (`editor/App.cpp:5238-5239`) and a `gpu` block with backend and device
  (`docs/profiling.md`); no thread CPU, no tier, no quirks.
- **The GPU database holds only tiers.** `gpu_tiers.json` has `version, generator, source, tiers,
  vendors, rules` (641 rules) and nothing else (read with `json.load`); `GpuTierDatabase` matches
  device ids, names, families and vendors (`engine/core/device/GpuTierDatabase.h:141-204`).
  `DeviceSpecs` has renderer, backend, platform, memory, cores and **no driver version**
  (`engine/core/device/Device.h:20-26`). `sgj_api_version` returns `GL_VERSION` on GL (which carries
  the driver's version) and only `apiVersion` on Vulkan; D3D11 and Metal return nothing
  (`engine/libs/sokol/sokol_gpu_timer_impl.h:1097-1110`).
- **Driver decisions today, scattered.** All in `engine/renders/sokol/SokolAsyncCreate.cpp`, each
  with its own environment override and invisible to `get_device_info`, logs or benchmarks:
  1. a software GL gets no worker context: "on llvmpipe the render thread crashed drawing programs
     the worker had drawn ... and once hung in a draw" (S32 deviation 6; `:257-266`, `:672-678`;
     `JASPER_GL_SOFTWARE_WORKER=1` allows it);
  2. GLES gets no warm draws until an Adreno/Mali run verifies them (`:268-281`;
     `JASPER_GL_WARM_DRAWS=1`), also in `GpuCreation.h:23-28`;
  3. the driver's compile threads are not enabled on a software GL, where "a lazily made shader can
     finish after the frame a test or an export check reads" (`:706-714`).
  "Software GL" is `caps.gl_software`, set by substring checks of `GL_RENDERER` for `llvmpipe`,
  `softpipe`, `SwiftShader`, `Software Rasterizer` in our sokol extension
  (`engine/libs/sokol/sokol_async_impl.h:103-104`, ledger `engine/libs/sokol/PATCHES.md`).
- **No vendor literals elsewhere.** A grep of string literals naming a GPU vendor or software
  renderer in `engine/renders`, `engine/core/render`, `engine/shaderlib` finds only comments
  (`SystemRender.h:170`); the NVIDIA nested-pass crash is prevented for every GPU by an invariant
  (`engine/renders/sokol/SokolSystem.h:191-196`), which is the right shape. One platform-level
  driver workaround exists: on Linux, `dlopen("libGLX_nvidia.so.0", RTLD_NOLOAD | RTLD_NODELETE)`
  keeps NVIDIA's ICD mapped so `XCloseDisplay` does not jump into unloaded code at exit
  (`engine/platform/common/VulkanContext.cpp:19-26`); it is a no-op on other drivers, runs before
  any device exists and changes nothing about frames, so it stays as it is (not a quirk).
- **Order at startup.** `Engine::systemViewLoaded` calls `SystemRender::setup()` and then
  `Device::detect()` (`Engine.cpp:2156-2163`); `SokolAsyncCreate::setup()` runs inside the render
  setup (`engine/renders/sokol/SokolSystem.cpp:378`), so the creation mode is chosen **before**
  detection reads the GPU (inferred from the call order).
- **Overrides.** Players parse `--device-tier` (`engine/core/player/LaunchOptions.cpp:228-235`); the
  web page's URL query becomes the launch options (`engine/platform/emscripten/main.cpp:42-49`).
  `System` has `getOperatingSystemVersion`, `getAndroidApiLevel`, `getGpuRendererName`
  (`engine/core/System.h:321-331`).
- **Lua `Device`** is a luabridge class (`engine/core/script/binding/CoreClassesLua.cpp:4097-4117`)
  whose stub `tools/lua_api` generates.

## 4. Reference

Ideas only. The Craftsman tree (`/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor`) is a
port of a retail Minecraft PE binary: **no code, data, assets or Minecraft-specific mechanics may
be copied**; nothing here depends on blocks, worlds or game modes.

| Source | Pattern studied | What we take / do differently |
|---|---|---|
| `include/mcpe015/perf_sample.h` | Frame interval measured only while playing (no menus, loading, an ad in front, the background gap); 200-bin 1 ms histogram; p10 fps from the p90 frame; a 5-minute window handed to the app layer; per frame one clock, one subtraction, one increment | Gameplay-only windows, a fixed histogram, p10 fps, a window callback (R1-R9). Our bins are finer below 50 ms (D2); the game, not the engine, defines "playing" beyond engine states; modes are the game's business |
| `include/mcpe015/adreno_perf.h`, `src/adreno_perf.cpp` | Thread CPU time (`CLOCK_THREAD_CPUTIME_ID`) next to frame time to tell a driver wait from compute; fixes gated by renderer and cohort, forceable with a system property, telemetry reporting what is active | Main-thread CPU (R5), forced overrides (R21) and "report what is active" (R23, R24). No cohorts or experiments; no Adreno-specific code |
| `include/mcpe015/perf2.h` | Cutout foliage drawn opaque on PowerVR/Mali because `discard` defeats hidden-surface removal; a stable 30 fps cap | The counter-example of a quirk: it changes the image, so here it would be a quality setting or tier cell (Scope). Fixed caps over adaptive ones (Scope) |
| Unity `FrameTimingManager` (docs) | `cpuFrameTime`, `cpuMainThreadFrameTime`, `gpuFrameTime` in Release | Field naming (`mainThreadCpuMs`) |
| Unity Adaptive Performance (docs) | Thermal state, "bottleneck" CPU/GPU | Thermal time in the summary; no automatic scaler |
| Chromium `gpu_driver_bug_list.json`, ANGLE feature overrides (docs, BSD) | Rows with OS, vendor, device, driver-version range, the workaround names and a bug link; `--enable/--disable-<feature>` overrides | Data-driven rules, version ranges, a reason per row, named overrides (D5-D7). No rows copied |

## 5. Requirements

### Functional: the Performance sampler (P1)

- **R1 Off by default.** Sampling starts with `Performance.setSampling(true)` (or `--perf-summary`,
  R12) and stops with `setSampling(false[, reason])`. Off, the only cost is one branch in
  `FrameTimes::beginFrame`.
- **R2 Frames.** While sampling, each completed `FrameTimes` sample adds `frameMs` and `cpuMs` to a
  pending second (fixed histograms of the R4 bins, so a second of any frame rate is committed or
  discarded whole). Editor frames with `FLAG_HOST_WORK` are skipped one by one.
- **R3 Seconds.** Once per wall second (measured from the `steady_clock` value `beginFrame` already
  read) the pending second is **committed** to the window or **discarded**. Discarded when any of:
  the app was paused or hidden during it; it lies within 2 s after a resume, a focus regain or the
  window becoming visible; it lies within 1 s after `Engine::onSceneLoaded`; `Engine::areScenesLoaded(false)`
  is false at the tick (loading); a frame in it took >= 1000 ms (a stall, a breakpoint, the
  background); the game turned sampling off during it. Each discarded second adds 1 s to
  `excluded.<cause>` (`paused`, `resume`, `loading`, `stall`, `game`), and to
  `excluded.reasons[reason]` for the game's reason (at most 8 reason names; more go to `other`).
- **R4 Histogram.** 201 `uint32` bins: 0.5 ms wide over [0, 50), 2 ms wide over [50, 250), one
  bin >= 250 ms. Percentiles interpolate linearly inside a bin (error <= 0.25 ms below 50 ms).
  Frames of 250..1000 ms also count in `hitches`.
- **R5 Main-thread CPU.** At each tick the frame thread (the thread that runs `Engine::runFrame`)
  reads its CPU time: `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` on Linux, Android, macOS, iOS;
  `GetThreadTimes` (user + kernel) on Windows. A committed second adds its delta;
  `mainThreadCpuMs` = committed CPU / committed frames. Windows' thread times advance at the
  scheduler tick (about 15.6 ms), so its average is documented as +-0.3 ms per frame at 60 fps over
  a 60 s window or longer. Web: `mainThreadCpuMs = nil`, `mainThreadCpu = "unavailable"`.
- **R6 GPU time.** Frames whose `gpuMs >= 0` (S15's timer already running: the Profiler, an agent,
  dynamic resolution) add to `gpuMs {avg, frames}`; the sampler never starts the GPU timer.
- **R7 Context.** The window records seconds per `ThermalState`, the tier in effect at the end and
  how often it changed, and copies `gpu`, `driver` and `quirks` (P2), `backend`, `platform`,
  `version` and `build_id` into the summary.
- **R8 Summary.** `Performance.getSummary(options)` returns the current window without resetting:
  ```
  {seconds, frames, fps = {avg, p50, p10}, frameMs = {p50, p90, p99, max},
   cpuMs = {avg, p90}, mainThreadCpuMs, mainThreadCpu = "thread_clock"|"unavailable",
   gpuMs = {avg, frames}?, hitches, excluded = {paused, resume, loading, stall, game, reasons = {...}},
   thermal = {nominal, fair, serious, critical}, tier, tierChanges, gpu, driver, quirks,
   backend, platform, version, buildId, histogram? = {edgesMs = {...}, counts = {...}}}
  ```
  `fps.avg` = frames / seconds; `fps.p50` = 1000 / `frameMs.p50`; `fps.p10` = 1000 / `frameMs.p90`
  (the fps 90 % of frames reach). `options.histogram = true` adds the bins. With no committed
  second the call returns `{seconds = 0, frames = 0, excluded = {...}}`.
- **R9 Windows.** `Performance.onSample(fn, seconds)` (seconds default 300, range 30..3600) calls
  `fn(summary)` on the main thread each time the window reaches `seconds` of committed gameplay,
  then starts a new window. `Performance.onSample(nil)` removes it. `Performance.reset()` starts a
  new window now.
- **R10 Game time and pauses.** `Engine.timeScale = 0` (a pause menu drawn over gameplay) is not
  excluded by the engine; the game calls `setSampling(false, "pause_menu")` when it wants that.
- **R11 Editor Play.** The sampler works in Play (frames are the editor's, `FrameTimes` external
  frames) and resets at Play start; `onSample` is cleared at Stop (`tools/check_event_clears.py`).
- **R12 `--perf-summary=<path>`.** A launch option of every build (web: `?perf-summary=1`, which
  prints instead). It turns sampling on at the frame that logs `Engine ready:` unless the game
  already called `setSampling`, writes the summary as JSON (snake_case keys) through
  `io::writeFileAtomic` at quit and at each window end, and on the web prints
  `JASPERENGINE PERF: <json>` with `console.log`.

### Functional: GPU quirks (P2)

- **R13 Quirks are levers in C++.** `enum class GpuQuirk` lists what a quirk can do; each has a
  name, a one-line description and exactly one place in engine code that reads it. Data cannot
  invent a quirk; it only says where one applies. The initial catalogue: `gl_no_worker_context`,
  `gl_no_compile_threads`, `gl_no_warm_draws` (D8). A test-only `test_tint` exists only in test
  builds (AC23).
- **R14 Rules are data.** `engine/core/device/gpu_quirks.json` (hand-written, embedded into
  `GpuQuirksData.cpp` by `tools/device/convert_forge_gpu_data.py --embed`, verified by `--check`):
  `{"version": 1, "rules": [{"quirk", "reason", "issue", "check", "match": {...}}]}`. `reason` is
  at least 20 characters and says what goes wrong without the quirk; `issue` cites the spec,
  deviation or issue; `check` names the test that proves the quirk draws the same image (R27).
- **R15 Match keys** (all present keys must match; lists match any element): `api` (`glcore`,
  `gles3`, `metal`, `d3d11`, `vulkan`), `platform` (`android`, `apple`, `pc`, `web`, as the tier
  database), `vendor` (vendor names of `gpu_tiers.json`'s `vendors`), `renderer` (family patterns,
  the tier database's syntax, against the normalized renderer), `driver` (a family pattern against
  the normalized driver string), `driver_at_least` / `driver_below` (dotted numbers compared
  component-wise with the first `\d+(\.\d+)+` in the driver string; no number: no match),
  `os_at_least` / `os_below` (the Android API level on Android, the OS version elsewhere).
- **R16 Driver string.** `SystemRender::getDriverVersion()`: GL and GLES the `GL_VERSION` text;
  Vulkan `VkPhysicalDeviceDriverProperties.driverName + " " + driverInfo` where Vulkan 1.2 is
  available, else `driverVersion` decoded (NVIDIA 10.8.8.6 bits, others `VK_VERSION_*`); D3D11 the
  user-mode driver version from `IDXGIAdapter::CheckInterfaceSupport` as `a.b.c.d`; Metal the OS
  version; WebGL `""`. Sokol changes go in our extension files and `engine/libs/sokol/PATCHES.md`.
- **R17 Resolve once, early.** `GpuQuirks::resolve()` runs in `SystemRender::setup()` after the
  device exists and **before** `SokolAsyncCreate::setup()`, from the renderer (`System::getGpuRendererName`
  on the web, else `SystemRender::getDeviceName`), the driver string, the backend, the platform and
  the OS version; then forced overrides apply (R21). The set is fixed for the process. One log
  line: `GPU quirks: gl_no_worker_context (rule: software GL ...), gl_no_warm_draws (forced on)` or
  `GPU quirks: none`.
- **R18 Consumers.** The three S32 decisions read `GpuQuirks::active(GpuQuirk::...)` instead of
  `caps.gl_software`, `SOKOL_GLES3` and the two environment variables. `caps.gl_software` stays a
  sokol fact for `SokolAsyncCreate::isSoftwareGl()` and stats.
- **R19 Behaviour preserved.** With no override, every device makes the same choices as before:
  the software-GL rule matches exactly `llvmpipe`, `softpipe`, `swiftshader`, `software rasterizer`
  on `glcore`/`gles3` (sokol's four substrings, normalized); the warm-draw rule matches `gles3` on
  every platform.
- **R20 How, never what.** A quirk may change: object creation and its thread, contexts, upload
  method and timing, synchronization, buffer and texture usage flags, pass and draw submission
  order where the result is order-independent. It may not change: shader arithmetic or precision,
  formats of different precision, sampling, blending, culling, LOD, what is drawn, quality settings.
  Docs and review enforce the list; R27's tests enforce the result.
- **R21 Overrides.** `--gpu-quirk=<name>:on|off` (repeatable, or comma-separated) on every player;
  `?gpu-quirk=<name>:off` on the web; `JASPERENGINE_GPU_QUIRKS=<name>:on|off,...` for the editor,
  tests and players (the command line wins). `JASPER_GL_SOFTWARE_WORKER=1` and
  `JASPER_GL_WARM_DRAWS=1` remain aliases of `gl_no_worker_context:off` and `gl_no_warm_draws:off`
  with one deprecation line. An unknown name fails a player's parse (exit 2) with
  `--gpu-quirk: unknown quirk "x" (known: gl_no_worker_context, gl_no_compile_threads, gl_no_warm_draws)`;
  in the environment it is a warning naming the known quirks.
- **R22 Lua.** `Device.gpuQuirks` (read-only array of active names), `Device.gpuDriver` (read-only
  string). No setter: quirks are not a game's graphics option.
- **R23 Reports.** `get_device_info` adds `driver` and `quirks: [{name, reason, source:
  "rule"|"forced_on"}]` plus `quirks_forced_off: [names]`; the benchmark JSON adds `device: {gpu,
  driver, tier, quirks}`; the Performance summary carries `driver` and `quirks` (R7); a user report's
  `device.json` carries them (shipped-game-diagnostics.md R18).
- **R24 Lint.** `tools/check_gpu_vendor_branches.py` fails on a string literal in `engine/renders`,
  `engine/core/render` or `engine/shaderlib` containing a vendor or renderer token of
  `gpu_tiers.json` (`adreno`, `mali`, `powervr`, `nvidia`, `geforce`, `radeon`, `intel`,
  `llvmpipe`, `swiftshader`, ...), comments excluded, with an allowlist file for log-only strings.
  Not scanned: `engine/libs` (vendored code keeps its own ledger) and `engine/platform` (gamepad
  names and the loader workaround above are not drawing decisions). It passes on today's tree.

### Platform & backends

- **R25** The sampler works on all six targets and all five backends (it reads `FrameTimes`, which
  every build records). Thread CPU per R5; the web reports `unavailable`.
- **R26** Quirk rules can target any backend and platform; the initial three are GL-only levers.
  The web resolves quirks from the unmasked WebGL renderer, as tier detection does.

### Equivalence (a quirk changes how, never what)

- **R27** Every rule's `check` names a test that renders a scene with the quirk **forced on** and
  compares it with the same scene without it, on a renderer where both paths are safe, after S32
  creation is idle: identical within the golden's tolerance. Where the device class the rule
  targets is not in CI (a phone GPU), forcing the lever on in CI still proves it draws the same;
  the rule's `issue` records the device run that justified it. A rule whose `check` names no
  existing test fails `GpuQuirks.EmbeddedTableIsValid` (AC17); the named test itself runs in CI.

### Performance (Release; mobile and web first)

- **R28 Off.** Sampling off: <= 2 ns per frame (one predictable branch), no clock read, no memory
  beyond the sampler's static state (about 3 KiB: histogram, pending second, counters), no
  allocation.
- **R29 On.** Per frame <= 15 ns on the benchmark server (a store and two increments), measured by
  `PerfSamplerBench.FrameCost`; per tick (1 Hz) <= 50 us including the thread-clock read and
  `areScenesLoaded(false)`; zero allocations except building the Lua summary table in
  `getSummary`/`onSample` (once per call). Mobile target <= 4x those figures (inferred; recorded on
  the first device run).
- **R30 Quirk cost.** `GpuQuirks::active` is an array lookup; resolution <= 1 ms at startup.

### Editor UX (human)

- **R31 Game view Stats** gains **Main Thread CPU** (ms, the last full second) and, while the sampler
  runs, **Sampling** `63 s · p50 58 fps · p10 41 fps`. The Profiler's **Monitors** module gains the
  series `Main thread CPU ms` (one point per second; the editor reads the thread clock once a second
  while the Profiler records or an agent asked for frame times).
- **R32 Project Settings > Quality > Device Tiers**: the row above the table that shows this
  machine's tier also shows `GPU Quirks: none` or the active names, each with its reason in the
  tooltip. Read-only; no undo needed.
- **R33 Docs.** `docs/profiling.md` gains "Performance in shipped games" (the sampler, exclusions,
  the summary, thread CPU per platform, privacy note: the game asks before it sends);
  `docs/device-tiers.md` gains "GPU quirks" (rules, match keys, overrides, the how-not-what rule,
  how to add a rule with its check); `docs/rendering/async-shaders.md` replaces the two environment
  variables with the quirk names.

### Agent UX (MCP, Lua, CLI)

- **R34 `get_frame_times`** (group `profiling`) adds `main_thread_cpu_ms` (average per frame over
  the window's seconds; `null` on the web) and `performance` (the sampler's current summary in
  snake_case while Play sampled; omitted otherwise). No new parameter. When `main_thread_cpu_ms` is
  far below `cpu_ms` (< 60 %), `next` says `The frame thread waits more than it computes (driver,
  locks, I/O): see get_gpu_passes and the Timeline's Wait band.`
- **R35 `get_device_info`** (group `device`) adds `driver`, `quirks`, `quirks_forced_off` (R23);
  its description adds two words; the group stays within its 1,500 characters (measured by
  `tool_budget.py`).
- **R36 Benchmark CLI** adds `mainThreadCpuMs {mean}` and `device {gpu, driver, tier, quirks}`
  (R23) to `result.json`, and `--gpu-quirk name:on|off` (repeatable) for the run.
- **R37 Lua stubs.** `engine/core/script/binding/stubs/Performance.lua` (new, hand-written: Lua C
  API in `ProfilerClassesLua.cpp` beside `Profiler`); `Device.gpuQuirks` and `Device.gpuDriver`
  come from the luabridge binding through `tools/lua_api`. `docs/mcp/guide/lua-api.md` gets the
  `Performance` table; `docs/mcp/guide/profiling.md` gets one line: "game frame rate on a device ->
  run the player with --perf-summary=<file> and read it"; `docs/mcp/guide/device.md` one line on
  quirks.
- **R38 Eval.** `tools/mcp/eval/cases.py` gets a case where the agent must say whether a slow Play
  scene is CPU-bound or waiting, failing without R34's `next`.

### Data & compatibility

- **R39** No project, scene or asset format changes. `gpu_quirks.json` is new and embedded;
  `gpu_tiers.json` is unchanged. `result.json` and `get_device_info` only add keys. `.jetrace.json`
  is unchanged.
- **R40** `ShaderCreationSettings.glWarmDraws` keeps its meaning (the project's wish); the quirk
  removes warm draws where it applies, as `SOKOL_GLES3` did.

## 6. Design constraints & decisions

- **D1 Ride on `FrameTimes`.** The sampler takes the sample `beginFrame` just completed, so it adds
  no clock read per frame; the only new clock is the thread clock at 1 Hz. Per-second commit makes
  every exclusion a whole second and keeps the per-frame path to a store.
- **D2 Bins.** 1 ms bins (Craftsman) are 12 % of a 120 Hz frame; 0.5 ms up to 50 ms keeps p50 and
  p90 within 0.25 ms where it matters and 2 ms bins to 250 ms keep hitches visible in 804 bytes.
- **D3 Thread CPU at 1 Hz.** Linux and Android serve `CLOCK_THREAD_CPUTIME_ID` with a system call
  (no vDSO), and Windows' resolution is the scheduler tick: a per-frame read would cost or lie.
  Averages over committed seconds are exact on POSIX and good enough on Windows (R5).
- **D4 API sketch (C++).**
  ```cpp
  namespace jasperengine {
      struct PerfSummary { /* R8 fields */ };
      class JASPERENGINE_API PerfSampler {
      public:
          static void setSampling(bool on, std::string_view reason = {});
          static bool isSampling();
          static PerfSummary summary(bool histogram = false);
          static void reset();
          static void setWindowSeconds(int seconds);                // onSample's window
          static FunctionSubscribe<void(const PerfSummary&)> onWindow; // cleared at Stop
          static double mainThreadCpuMsLastSecond();                // Stats, Monitors, MCP
          // FrameTimes::beginFrame, behind one branch
          static void onFrame(const FrameTimeSample& sample, std::chrono::steady_clock::time_point now);
          static void tickForTest(double seconds);                  // tests: a second elapsed
      };
  }
  ```
  Files `engine/core/profiler/PerfSampler.{h,cpp}`, thread clock in
  `engine/core/profiler/ThreadCpuClock.{h,cpp}` (one function, per platform).
- **D5 Lua sketch.**
  ```lua
  Performance.setSampling(true)                    -- gameplay starts
  Performance.setSampling(false, "menu")           -- the main menu: excluded, named
  Performance.onSample(function(s)                 -- every 5 minutes of gameplay
      MyAnalytics.send("perf", {p50 = s.fps.p50, p10 = s.fps.p10, cpu = s.mainThreadCpuMs,
                                tier = tostring(s.tier), gpu = s.gpu, hot = s.thermal.serious})
  end, 300)
  local s = Performance.getSummary{histogram = true}
  Log.print(Device.gpuDriver .. " quirks: " .. table.concat(Device.gpuQuirks, ","))
  ```
- **D6 Quirks vs tiers.** Tiers (S56) answer "how much quality can this device afford" and may
  change the image within the project's settings; quirks answer "how must this driver be fed" and
  must not change it. Separate files, separate code, separate reports.
- **D7 Catalogue in C++, rules in JSON.** A data row cannot make the engine do something no test
  covers; adding a lever is a code change with its test, adding a device to a lever is a data change
  with its `issue`.
- **D8 The initial table** (migrated, not invented):
  | Quirk | Match | Reason | Check |
  |---|---|---|---|
  | `gl_no_worker_context` | `api` glcore, gles3; `renderer` llvmpipe, softpipe, swiftshader, software rasterizer | On llvmpipe the render thread crashed drawing programs a worker context had drawn (warm draws) and once hung in a draw after the worker linked its programs (S32 deviation 6) | `AsyncCreateGpu.EveryGlModeDrawsTheLitCube` (the LitCube golden in every GL mode) and `test_async_shaders.py::test_forced_gpu_quirks_draw_the_same_image` |
  | `gl_no_compile_threads` | same renderers | With the driver's compile threads, a lazily made shader can finish after the frame a test or an export check reads; llvmpipe compiles at once without them (S32, `docs/perf/gpu-campaign.md`) | `GpuQuirksGpu.CompileThreadsLeverDrawsTheLitCube` and the NVIDIA lane test |
  | `gl_no_warm_draws` | `api` gles3 | Warm draws on Adreno and Mali drivers are unverified; no device run yet (S32) | the NVIDIA lane test (forced on) |
- **D9 Overrides syntax.** `name:on|off` avoids a second `=` inside a URL query value.
- **D10 No runtime toggling.** S32 picks its creation mode at setup; letting a quirk flip mid-run
  would recreate contexts. A restart is the cost of an override.

## 7. Acceptance criteria

Unit tests: `engine/tests/core/profiler/PerfSamplerTest.cpp` (new: suites `PerfSampler`,
`PerfSamplerBench`), `engine/tests/core/device/GpuQuirkTest.cpp` (new: `GpuQuirks`),
`engine/tests/core/render/GpuQuirksGpuTest.cpp` (new: `GpuQuirksGpu`), tests in the existing
`engine/tests/core/render/AsyncCreateTest.cpp` (`AsyncCreateGpu`) and
`engine/tests/core/player/LaunchOptionsTest.cpp` (`LaunchOptions`); editor test
`editor/tests/mcp/ProfilingHintsTest.cpp` (new: `ProfilingHints`). MCP tests extend
`tools/mcp/tests/test_profiler.py`, `test_device.py` and `test_async_shaders.py` (the last is on the
NVIDIA lane, `tools/mcp/gpu_lane.json` class `gpu`). The web gate extends a gate of
`tools/web/gates/` (`core-utilities.json`). "Red" is the run on the tree without the change.

| AC | Verifies | Check |
|---|---|---|
| AC1 | R2, R4, R8 | `PerfSampler.PercentilesFromFixedBins`: 600 synthetic frames (`FrameTimes::recordSample`) of 16 ms with 60 of 33 ms, `tickForTest` per second: `frameMs.p50` in [15.75, 16.25], `p90` in [32.75, 33.25], `fps.p10` ~ 30.3, `frames == 600`. Red: no `PerfSampler` |
| AC2 | R3 | `PerfSampler.ExcludesPausedResumeLoadingAndStallSeconds`: a scripted timeline (pause 3 s, resume, 2 s gap, a scene load, a 1.2 s frame) commits only the expected seconds and fills `excluded.paused/resume/loading/stall` with the expected seconds |
| AC3 | R3, R10 | `PerfSampler.GameReasonsNameExcludedTime`: `setSampling(false, "menu")` 4 s, then on: `excluded.reasons.menu == 4`; a 9th reason lands in `other` |
| AC4 | R2 | `PerfSampler.SkipsHostWorkFrames`: frames flagged `FLAG_HOST_WORK` are not counted |
| AC5 | R5 | `PerfSampler.ThreadCpuSeparatesWaitingFromWork`: frames that sleep 20 ms and spin 2 ms: `mainThreadCpuMs` < 6 while `cpuMs.avg` > 20 (Linux/macOS exact; Windows bound 8); on the web build the field is nil (web gate AC14) |
| AC6 | R9, R11 | `PerfSampler.WindowCallbackFiresAndRestarts`: window 30 s, 31 committed seconds: `onWindow` called once with `seconds == 30`, the next summary has 1 s; `test_profiler.py::test_performance_in_play` checks Stop clears `onSample` (a second Play does not call the old function) |
| AC7 | R1, R28 | `PerfSampler.OffAllocatesNothingAndReadsNoClock`: sampling off, 10,000 frames: `AllocationCountScope` 0, thread-clock read counter 0 (test seam) |
| AC8 | R29 | `PerfSamplerBench.FrameCost` and `.TickCost` print per-frame (on, off) and per-tick costs, enforced through `perfSlack()` (<= 15 ns, <= 2 ns, <= 50 us) |
| AC9 | R8, R34 | `test_profiler.py::test_performance_in_play`: Play; `eval_lua` `Performance.setSampling(true)`; `wait` 4 s of game time; `get_frame_times` returns `main_thread_cpu_ms` > 0 and `performance.seconds >= 2`; `eval_lua` `Performance.getSummary().fps.p50 > 0` |
| AC10 | R34 | `ProfilingHints.WaitingWhenThreadCpuIsUnder60Percent` (new, `editor/tests/mcp/ProfilingHintsTest.cpp`): the hint function behind `get_frame_times` returns R34's `next` for `cpu_ms` 30 / `main_thread_cpu_ms` 10, nothing for 30 / 25 and nothing when thread CPU is null (web). Red: no hint |
| AC11 | R12 | `test_profiler.py::test_player_perf_summary`: `build_project` the CI project, `run_build` with `args: ["--perf-summary=<tmp>/perf.json"]` and `seconds: 6`: the file parses, `seconds >= 3`, keys snake_case, `main_thread_cpu` = `thread_clock` on Linux |
| AC12 | R31 | `test_profiler.py::test_stats_overlay_rows`: Play with the Stats overlay on (`configure_game_view show_stats: true`): `find_ui_item` finds "Main Thread CPU"; with sampling on, "Sampling"; screenshot kept as evidence |
| AC13 | R36 | `test_profiler.py::test_benchmark_device_and_main_thread_cpu`: `jasperengine-editor benchmark` on the CI project: `result.json` has `mainThreadCpuMs.mean > 0` and `device.quirks` equal to `get_device_info`'s names |
| AC14 | R5, R25 | Web gate `core-utilities.json`: the fixture's Lua samples 3 s and logs `[gate] perf mainThreadCpu=unavailable p50>0`; `expect` lists that line |
| AC15 | R15, R19 | `GpuQuirks.SoftwareGlRuleMatchesExactlySokolsRenderers`: renderer strings `llvmpipe (LLVM 15.0.7, 256 bits)`, `softpipe`, `Google SwiftShader`, `Apple Software Rasterizer` on glcore activate `gl_no_worker_context` and `gl_no_compile_threads`; `llvmpipe` on vulkan, `NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2` on glcore and `Adreno (TM) 650` on gles3 do not (the last activates only `gl_no_warm_draws`) |
| AC16 | R15, R16 | `GpuQuirks.DriverAndOsRangesCompareNumbers`: a test table with `driver_below: "502.0"` matches `OpenGL ES 3.2 V@415.0 (GIT@...)`, not `V@0615.0`; a string without a dotted number never matches a range; `os_at_least: "29"` reads the Android API level |
| AC17 | R13, R14, R27 | `GpuQuirks.EmbeddedTableIsValid`: every rule names a known quirk, has a reason >= 20 characters, an `issue`, and a `check` naming a test that exists (the test list is generated at build time from gtest and the MCP files); `convert_forge_gpu_data.py --check` passes. Red: a planted rule without `check` fails |
| AC18 | R21 | `LaunchOptions.GpuQuirkFlagParses` (`--gpu-quirk=gl_no_warm_draws:off,gl_no_worker_context:on`), `LaunchOptions.GpuQuirkRejectsUnknownNames` (exit 2 with the R21 text), `GpuQuirks.ForcedOverridesWinOverRules`, `GpuQuirks.LegacyEnvironmentAliases` |
| AC19 | R17, R18, R19 | `AsyncCreateGpu.SoftwareGlDecisionsComeFromQuirks`: on llvmpipe the quirks are active and the mode is not `GlWorker`; with `JASPERENGINE_GPU_QUIRKS=gl_no_compile_threads:off` the compile threads are enabled (test seam) while the worker stays off. The existing `AsyncCreateGpu.EveryGlModeDrawsTheLitCube` and `WarmDrawsNeverRunOnASoftwareGl` pass unchanged |
| AC20 | R27 | `GpuQuirksGpu.CompileThreadsLeverDrawsTheLitCube`: on llvmpipe in parallel mode, `gl_no_compile_threads` forced off and on: both frames match the LitCube golden after `waitIdle` (glcore; Vulkan: the lever does not apply and the test asserts that) |
| AC21 | R27, D8 | `test_async_shaders.py::test_forced_gpu_quirks_draw_the_same_image` (NVIDIA lane): two editors on one fixture scene, one with `JASPERENGINE_GPU_QUIRKS=gl_no_worker_context:on,gl_no_compile_threads:on,gl_no_warm_draws:on`: after creation is idle, Scene view screenshots are identical (max channel difference 0); `get_device_info.quirks` lists all three with `source: "forced_on"` in the second |
| AC22 | R22, R23, R35 | `test_device.py::test_device_info_quirks`: on llvmpipe `get_device_info` has `driver` (non-empty on GL) and `quirks` with the two software-GL names and their reasons; `eval_lua` `#Device.gpuQuirks == 2`; `tool_budget.py --check` passes |
| AC23 | R27 | `GpuQuirksGpu.EquivalenceCheckCatchesAVisibleChange` (red control of the method): the test-only `test_tint` lever, which changes the clear colour, fails the same comparison AC20 uses |
| AC24 | R24 | `python3 tools/check_gpu_vendor_branches.py` passes on the tree; `--self-test` plants `strstr(renderer, "Mali")` in a temp copy of a render file and fails |
| AC25 | R38 | The R38 eval case fails without R34's `next` and passes with it (`tools/mcp/eval/README.md`, Orquester accounts) |

## 8. Dependencies & conflicts

- **Depends on:** S15 (`FrameTimes`, Monitors, benchmark), S32 (the three decisions, creation
  stats), S56 (`Device`, tier database syntax, Device Tiers header row), S30 (launch options).
- **Sibling specs:** shipped-game-diagnostics.md puts this summary in `perf.json` and `driver`,
  `quirks` in user reports' `device.json`; either may land first. build-and-run-on-device.md runs a
  player on a phone and should pass `--perf-summary` and pull the file: that is how the mobile rows
  of `docs/perf/gpu-campaign.md` get real numbers and how a future mobile quirk gets its `issue`.
  build-profiles.md may turn `--perf-summary` on for a QA profile. dev-console-for-agents.md may
  expose `Performance.getSummary` to an agent attached to a running game.
  culling-group-and-simulation-distance.md lets a game act on a slow summary (its own decision; the
  engine does not adapt). project-test-runner.md tests can assert summary fields in Play.
- **Merge hotspots:** `engine/core/profiler/FrameTimes.{h,cpp}`, `engine/core/Engine.cpp`
  (pause, focus, scene-loaded hooks), `engine/core/debug/DebugStats.{h,cpp}`,
  `engine/core/script/binding/ProfilerClassesLua.cpp`, `CoreClassesLua.cpp` (Device),
  `engine/renders/sokol/SokolAsyncCreate.cpp`, `engine/renders/sokol/SokolSystem.cpp`,
  `engine/libs/sokol/sokol_gpu_timer_impl.h` + `PATCHES.md`, `engine/core/render/SystemRender.h`,
  `engine/core/device/*`, `tools/device/convert_forge_gpu_data.py`,
  `engine/core/player/LaunchOptions.{h,cpp}`, `editor/App.cpp` (benchmark),
  `editor/mcp/tools/` (profiling and device tools), `docs/profiling.md`, `docs/device-tiers.md`,
  `docs/rendering/async-shaders.md`, `docs/mcp/TOOLS.md` (regenerated).

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| `areScenesLoaded(false)` at 1 Hz is too slow on a big scene | Measured by AC8; over 50 us, the engine keeps a loading counter updated at load start and end and the sampler reads that instead (recorded in Deviations) |
| A game never turns sampling on | Then it costs a branch and reports nothing; the docs show the three-line setup |
| Windows thread times are coarse | Averaged at 1 Hz; documented error (R5) |
| iOS/Android frame thread is not the platform's main thread | "Main thread" in the API means the frame thread; documented |
| A device class needs a quirk CI cannot render | Forced-on equivalence in CI plus the device run named in `issue` (R27) |
| The NVIDIA lane is unavailable | AC21 runs where the lane runs; AC20 and AC19 keep llvmpipe coverage in every CI run |
| Someone adds an image-changing quirk | R20's list in review, R27's test fails on a visible change (AC23 proves the method can fail) |
| Players' analytics expose device names | The game asks for consent; the summary holds no personal data (GPU, OS, tier) |
| `get_frame_times` result grows | Two keys; no schema change; budget measured |
| Old environment variables in scripts and docs | Kept as aliases with a deprecation line (R21) |

## Deviations

1. **Bins (R4).** The 0.5 ms bins below 50 ms are centred on multiples of 0.5 ms (bin 0 is [0, 0.25),
   bin 99 is [49.25, 50)), so a percentile there is within 0.25 ms as R4 states; edges at 50 and
   250 ms stay. Interpolation takes the rank's middle (`rank + 0.5`), so AC1's p90 sits at the edge
   of the 33 ms bin: `fps.p10` is 30.5 against the ideal 30.3 (AC1 checks it within 0.3).
2. **Driver ranges (R15).** `driver_at_least` / `driver_below` read the first dotted number after
   `GL_VERSION`'s leading API version (`OpenGL ES 3.2 V@415.0` reads `415.0`, `4.5 (Core Profile)
   Mesa 23.2.1` reads `23.2.1`); a string without that prefix is read whole, as R15 says. The first
   dotted number of a GL_VERSION is always the API's, so a rule could never have keyed on the driver.
3. **AC17's test list** is read when the test runs, not generated at build time: gtest's registry of
   the binary, the unit-test sources (`TEST(Suite, Name)` / `TEST_F`) for a test another backend's
   build leaves out, and the function names of the MCP test file. `convert_forge_gpu_data.py --check`
   validates the rules' shape; the test compares the embedded table with the file, as
   `GpuTier.EmbeddedDatabaseIsTheCheckedInFile` does.
4. **`check`** is a list (or one string): D8 names two tests for two of the rules.
5. **AC11** runs the player with `after_ready` and `quit_after` (and `seconds` as a cap): `run_build`
   ends a run past its seconds with SIGTERM, which a player does not turn into a quit, so the
   at-quit write needs the player to quit by itself. `--perf-summary` makes a run wait for the ready
   line but does not make it a counted run (`RunControl::isActive`).
6. **The quit write (R12)** is skipped when a finished window was already written and nothing has
   been committed since, so the file keeps the last full window instead of an empty one.
7. **Off with a reason (R3).** `setSampling(false, reason)` keeps the sampler's clock running (even
   before the first gameplay second) so the excluded seconds count under the reason; `setSampling(
   false)` after sampling does too, as `excluded.game`. `reset()` keeps sampling as it is (R9 "starts a
   new window now"); `PerfSampler::resetSession()` (new) turns it off and forgets the window, the
   reasons and the game's choice, at Play start and Stop (`Engine::resetApplicationState`).
8. **A second resumed before its first frame** is the resume's, not the pause's, and a sampling
   turned on before a second's first frame makes it gameplay whole; a mid-second change leaves the
   second to the pause or the game. This keeps AC2's and AC3's timelines exact.
9. **The thread-clock watch (R31).** `PerfSampler::watchThreadCpu()` keeps the 1 Hz thread-clock
   read alive for 5 s without sampling; the Stats overlay, the Profiler's Monitors (while it records)
   and `get_frame_times` renew it. `get_frame_times`' `main_thread_cpu_ms` averages the newest
   measured seconds covering its window (a ring of 16), `null` until one second was measured.
10. **AC12's rows** are reported to `find_ui_item` as items without input (`ItemAdd` and the test
   engine's item info): the Stats overlay is drawn text, which `find_ui_item` does not see.
11. **Compile threads both ways.** `sgj_gl_disable_compile_threads()` (our sokol extension) turns the
   driver's compile threads off again on a context where an earlier setup turned them on, so AC19 and
   AC20 can force `gl_no_compile_threads` off and back on in one process without leaving the
   threads on for the tests after them.
12. **test_tint** stays in `enum class GpuQuirk` in every build (one header for every unit), but only
   builds with `JASPERENGINE_TESTS` know its name and read it (`JASPERENGINE_GPU_QUIRK_TEST_HOOKS` on
   `GpuQuirks.cpp` and `SokolCamera.cpp`, where it changes a pass's clear colour).
13. **`get_device_info`'s description** adds "driver, GPU quirks" (three words).
14. **AC25** (the eval run) was not run by the implementer: it needs lent Orquester accounts; the
   case is in `tools/mcp/eval/cases.py` (`bad_frame_waits_not_computes`).
15. **AC8's off frame.** `PerfSamplerBench.FrameCost` runs its off loop with a compiler fence
   (`std::atomic_signal_fence`) where it had a `volatile` counter: the counter's store-to-load chain
   alone took 2.37-2.62 ns a frame on box A (a 2.4 GHz Haswell), over R28's 2 ns whatever the branch
   cost. The fence still makes each frame read the flag; the off frame measures 0.41-0.46 ns there
   (six runs, `JASPER_TEST_PERF_SLACK=1`), the on frame 12.4-13.6 ns.
16. **AC21 on llvmpipe.** The CI runs the test on llvmpipe, where the rules already apply
   `gl_no_worker_context` and `gl_no_compile_threads` and `gl_no_warm_draws` acts only with a worker
   context, so both editors make their frames the same way. Two CI runs (44955597126, 44971786109)
   still differed by 1 between the two editors, which box B never reproduced (9 runs, and 3 of a
   control with two rule editors). There the test accepts test_batching's same-image tolerance
   (every channel within 1); where the rules do not apply them (the GPU lane) it keeps AC21's 0.
17. **The Windows thread clock (R5, D3)** is `QueryThreadCycleTime` on x64, divided by the time-stamp
   counter's rate that the first read measures over 2 ms, where the CPU reports an invariant counter;
   `GetThreadTimes` serves elsewhere. `GetThreadTimes` does not only round to the scheduler tick: it
   charges a whole tick to the thread the tick finds running, so AC5's frames (2 ms of work after
   20 ms of sleep) read 14.06 ms a frame on a CI runner (run 45035957551), not R5's +-0.3 ms.
   `ThreadCpuClock.ResolvesAFramesWork` holds the clock to a 3 ms spin, and AC5's limit is 6 ms on
   every platform.
18. **Pending second (R2).** R2 held up to 512 frames of a second in a list and sent the rest
   straight to the window, so past 512 frames a second (menus and loading screens with vsync off)
   a second later discarded as paused, loading or excluded still added its frames, and `fpsAvg`
   counted frames of seconds `seconds` left out. The pending second is now a pair of histograms of
   the R4 bins (1.6 KB), committed or discarded whole at any rate
   (`PerfSamplerTest.FastSecondsAreExcludedWhole`); the per-frame cost is two counter increments.
