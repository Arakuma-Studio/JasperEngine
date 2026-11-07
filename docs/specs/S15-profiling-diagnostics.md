# S15 — Profiling & render diagnostics

Wave 2. Owner area: `engine/core/profiler/` (new), `engine/libs/sokol/` (GPU timer extension),
`engine/renders/sokol/`, `editor/window/profiler/` (new), `editor/mcp/tools/ProfilerTools.cpp` (new).

## 1. Goal

Make performance visible to humans and agents on every backend we ship. A developer opens
**Window > Analysis > Profiler** and sees, for each frame, where the CPU time went (a scope
hierarchy and a per-thread timeline that includes job workers), how long each sokol pass took on
the GPU, and how much VRAM each category and owner holds. **Window > Analysis > Frame Debugger**
lists the frame's passes in order and previews what any pass wrote. Agents get the same data over
MCP. Today we have a four-line stats overlay and an env-var log line, so perf work is guesswork,
and agents (who cannot read the overlay) are blind.

## 2. Scope

In scope:
- Per-pass GPU timings through a small sokol-gfx extension: GL 3.3 core, GLES3 (Android),
  WebGL2, D3D11, Vulkan, Metal. Any backend or device without timer support reports
  `unavailable` with a reason.
- A VRAM tracker covering every sokol image and buffer, by category and owner, with totals.
- A frame pass viewer over our existing sokol pass sequence. No render graph is introduced
  (README global decision). It shows targets, actions, draw counts, GPU time and a pass output
  preview.
- A CPU profiler: scope macros that compile to nothing when disabled, lock-free per-thread
  event buffers, a hierarchical tree, a timeline, a frame history of 600 frames, monitors
  (counters), a Lua API and a capture file format (Chrome Trace Event JSON).
- Editor panels: Profiler (modules CPU Usage, GPU Usage, Rendering, GPU Memory, Monitors) and
  Frame Debugger.
- MCP tools and resources, docs and the agent guide.
- Profiling in exported games: a **Development Build** export option, command-line capture to a
  file, and optional Tracy integration for desktop remote profiling.

Out of scope:
- A render graph, barrier views and "cull reasons". These belong to OpenEngine's RG, which is on
  the do-not-copy list; README turns "render-graph viewer" into the frame pass viewer.
- Replaying a frame up to draw N (Unity's per-draw stepping). We list draws but do not re-render
  partial passes.
- GPU timing of the editor's own ImGui rendering. It uses `editor/backend/renderer/RendererGL.cpp`
  and `RendererVulkan.cpp`, not sokol, so we report only its CPU scope.
- RenderDoc, Nsight and RGP integration, and per-VUID validation stats (OpenEngine's
  `get_gpu_tooling` and `rgp_capture`).
- Pipeline-statistics and occlusion queries.
- Our own remote-profiling network protocol. Remote profiling is Tracy (desktop/Android) or
  capture files (all targets). See §6.9.

## 3. Current state

- **Frame counters.** `Engine::FrameStats {drawCalls, instances, triangles}` is at
  `engine/core/Engine.h:226`. `Engine::addDrawStats` fills it and `Engine::beginFrameStats`
  rotates it (`engine/core/Engine.cpp:615-628`). `Engine::drawView` saves and restores it so an
  editor view does not count (`Engine.cpp:1362/1386`). Readers are the Game-view overlay
  (`editor/window/GameWindow.cpp:548` `drawStats`), the status bar (`editor/App.cpp:1070`) and
  the `benchmark` CLI (`editor/App.cpp:3099`). MCP does not expose it; the only related MCP field
  is the `show_stats` toggle (`editor/mcp/tools/ViewTools.cpp:1070`).
- **Pass log.** `JASPERENGINE_FRAME_STATS` calls `sg_enable_stats()`
  (`engine/renders/sokol/SokolSystem.cpp:73-77`). `SokolSystem::recordPass` keeps a per-frame
  pass list with label, size, samples and actions (`SokolSystem.cpp:109-128`), and
  `getFrameStatsLine` logs it (`SokolSystem.cpp:130`) through `SystemRender::logFrameStats`.
  Docs and tests depend on that output (`docs/mcp/guide/post-processing.md`, `docs/shaders/README.md`).
  It only covers passes begun through `SokolCamera`'s `beginPass` (`SokolCamera.cpp:51-57`). It
  misses the queued passes in `SokolCmdQueue.cpp:107/111` and the clear pass in
  `SokolFramebuffer.cpp:394`.
- **CPU timing.** `engine/core/render/RenderBackendTime.h` is a thread-local, test-only total of
  time spent in sokol. It has no scopes, tree, threads or history.
- **GPU timing: none.** sokol-gfx has no timestamp queries. `docs/openengine-analysis/01-rendering.md:285`
  notes that dynamic resolution must fall back to CPU time because of this.
- **VRAM: none.** Live counts come from `sg_query_stats().total` and have no byte sizes.
- **Threads.** `engine/core/thread/ThreadPoolManager.{h,cpp}` is the generic pool, which S06
  replaces with work stealing. Other threads: `RuntimeShaderCompiler.cpp:75/420` workers, the
  `VideoStream.cpp:38` decoder, the async loader thread (`Engine::isAsyncThread`) and
  `MeshSystem::parallelForIndexed`. None is named or instrumented.
- **Our sokol copy and its patches.** sokol lives at `engine/libs/sokol/` (single TU
  `sokol.cpp`, `CMakeLists.txt`). Existing patches are inline and marked `// JasperEngine:`
  (`sokol_gfx.h:7017` descriptor size, `sokol_gfx.h:11702` GL invalidate fix). Backend-internal
  helpers live in `sokol.cpp` next to `SOKOL_IMPL` and read `_sg` directly; see
  `jasperengine_sg_max_msaa_samples` (`sokol.cpp`), which reads `_sg.vk.dev_props` and
  `_sg.d3d11.dev`. `SOKOL_TRACE_HOOKS` is defined unconditionally in `sokol.cpp`, so
  `sg_install_trace_hooks` (`sokol_gfx.h:24876`) is available everywhere. The hooks
  (`sg_trace_hooks`, `sokol_gfx.h:4010`) cover make/init/destroy/uninit of images and buffers,
  `begin_pass`, `draw`/`draw_ex`, `apply_pipeline`, `end_pass`, `commit` and debug groups.
  **Placement matters.** The `begin_pass` hook fires *before* the backend begins the pass
  (`sokol_gfx.h:25501`, backend call at `:25533`), and `end_pass` fires before the backend ends
  it (`:25763`). The `commit` hook fires *after* `_sg_commit()` has submitted (`:25773-25781`).
  Trace hooks are therefore fine for bookkeeping, but they cannot place GPU queries.
- **Commits.** A frame commits once (`Engine::systemDraw` → `SystemRender::commit`,
  `Engine.cpp:1318`). Each editor view commits again (`Engine::drawView`, `Engine.cpp:1388`). The
  constant `SystemRender::FRAME_STATS_FRAME` (`SystemRender.h:46`) already tags "the game frame"
  as opposed to a view id. `SG_NUM_INFLIGHT_FRAMES` is 2 (`sokol_gfx.h:2040`).
- **Vulkan device.** We create the Vulkan device ourselves in
  `engine/platform/common/VulkanContext.cpp`, with API 1.3 (`:108`) and a
  `VkPhysicalDeviceVulkan12Features` chain (`:219`). `hostQueryReset` is not enabled today.
- **Builds.** Exports use `config.buildType` (default `"Release"`, `editor/Exporter.h:59`;
  `editor/Exporter.cpp:887`). `JASPERENGINE_HTMLUI_DEBUGGER` (`engine/CMakeLists.txt:723-731`)
  already shows the "on for the editor and Debug, off for release games" option pattern.

## 4. Reference (OpenEngine, MIT, read-only)

Base: `/var/lib/orquester/workspaces/jaspersito/JasperEngine-2/external/OpenEngine`.

| File | Take | Improve / leave |
|---|---|---|
| `Engine/Modules/Rendering/Include/Rendering/Core/QueryPool.h` | The `TimestampSemantics` idea: `PipelinePoint` (Vulkan) vs `EncoderSpan` (Metal stage-boundary samples are upper bounds and not additive); frame-slot readback; never serve stale results after a slot reset | Their interface is Vulkan/DX12-shaped with occlusion and pipeline stats. Ours is a C API inside sokol's TU with four backends plus GL |
| `Engine/Include/Core/CpuProfiler.h`, `Engine/Source/Core/CpuProfiler.cpp` | Compile-time collapse to no-ops; `TreeRow {name,total,self,calls,depth}`; 600-frame history; frozen history when disabled; "first frame after arming is empty" | Theirs takes a **mutex on every scope exit** and builds a tree for the main thread only (workers appear only as "off-main" flat rows). Ours uses per-thread lock-free rings and a timeline for every thread |
| `Engine/Include/Core/DebugMetrics.h` | Named monitors (Quantity/Memory/TimeMs/Percent), latest/peak/avg, 600-sample ring | They lock per push. Ours keeps per-frame values on the main thread, plus atomics for off-thread producers |
| `Apps/Editor/Source/Panels/CpuProfilerPanel.cpp` (760 lines) | Frame chart with scrub, sortable scope tree, freeze on disable | Rebuild in ImGui (their UI toolkit is do-not-copy) |
| `Apps/Editor/Source/Panels/VisualProfilerPanel.cpp` (952 lines) | Frame-budget presets `{30,60,90,120,144,240}` fps and the budget line (`:36-39`), per-pass CPU/GPU bars | ImGui; per-pass data comes from our pass recorder |
| `Apps/Editor/Source/Panels/MonitorsPanel.cpp` (1657 lines) | Labelled time-series plots, pick-a-monitor | Much smaller: ImGui `PlotLines` plus a table |
| `Apps/Editor/Source/Panels/VramPanel.cpp` (575 lines) | Name/type/details/size table, sort by size, `FormatBytes`, a cheap signature so it rebuilds only when the set changes (`:470-480`) | Their "render-graph resources" source does not apply. We add category and owner grouping and a driver budget |
| `Apps/Editor/Source/DebugServer/FramePhaseReport.cpp`, `mcp/src/tools/profiling.ts` | The agent contracts: a ring of the newest N frames attributed after the fact; summaries `{median,p95,p99,max}`, `minFrameMs`, spike interval; tool descriptions that state measurement caveats (armed overhead, single-sample frames, semantics) | Fewer, broader tools (our `tools/mcp/tool_budget.py` charges every description) |

Licence: MIT. Code adapted closely (for example the panel layout logic) keeps their copyright
header and gets a `NOTICE.md` entry. Tracy (BSD-3-Clause) gets its own `NOTICE.md` entry.

## 5. Requirements

### Functional — GPU timings

- **R1** The sokol extension `sg_jasper_gpu_timer` (C API, `engine/libs/sokol/sokol_gpu_timer.h`)
  measures every render pass begun through `sg_begin_pass`. That includes passes from
  `SokolCamera`, `SokolCmdQueue`, `SokolFramebuffer`, RmlUi and tests. A pass result is
  `{label, commit index, view tag, gpu_ns, valid}`. Results arrive asynchronously and are never
  blocking: available ≤ 4 commits later, or dropped with a counted reason.
- **R2** Per commit the extension reports `frame_gpu_ns`. On timestamp backends this is the last
  pass end minus the first pass begin. On the GL family it is the sum of pass times, labelled
  `sum_of_passes`. On Metal without per-pass counters it is command-buffer
  `GPUEndTime - GPUStartTime`.
- **R3** Each result carries `semantics`: `pipeline_point` (Vulkan, D3D11), `elapsed` (GL family)
  or `encoder_span` (Metal). It also carries `availability`: `available` or `unavailable` with one
  of `backend`, `extension_missing`, `device_unsupported`, `disjoint`, `overflow` or `disabled`.
- **R4** A disjoint event (GL `GL_GPU_DISJOINT_EXT`, D3D11 `Disjoint=TRUE`) invalidates the whole
  commit's results; nothing partial is reported.
- **R5** GPU timing runs only while the profiler is recording or a GPU consumer (S13 dynamic
  resolution) requests it. When off, no query objects are issued.

### Functional — VRAM

- **R6** Every sokol image and buffer is tracked from creation to destruction through trace hooks
  (`make_*`/`init_*` and `destroy_*`/`uninit_*`, deduplicated by id). This includes queued
  creation through `SokolCmdQueue` (`sg_alloc_*` + `sg_init_*`).
- **R7** An entry holds id, kind (Texture, RenderTarget, DepthTarget, Buffer), category, owner,
  sokol label, size, format, mips, slices, samples, usage, and **estimated bytes**. Bytes are the
  sum over mips and slices of `sg_query_surface_pitch`, times the sample count, plus the resolve
  image where separate. Buffers use `desc.size`.
- **R8** Categories (fixed enum): Textures, Render Targets, Shadow Maps, Post FX, Meshes (VB/IB),
  Uniform/Streaming, UI (RmlUi/HTML UI), Particles, Terrain, Editor, Other. Owner is an interned
  string: an asset path for loaded textures, or a subsystem name ("SSAO", "Bloom", "CSM atlas").
  Code pushes both with `JE_GPU_ALLOC_SCOPE(category, owner)`. `SokolCmdQueue` captures the scope
  at enqueue time and re-applies it at execution. Without a scope, the category comes from usage
  and the owner from the sokol label.
- **R9** Totals by category, by owner and overall, plus the peak since reset. Where the driver
  reports device memory, show it alongside the estimate: Vulkan `VK_EXT_memory_budget`, Metal
  `MTLDevice.currentAllocatedSize`, D3D11 `IDXGIAdapter3::QueryVideoMemoryInfo`, GL
  `GL_NVX_gpu_memory_info`/`GL_ATI_meminfo`. Otherwise it reads "n/a".

### Functional — frame pass viewer

- **R10** A pass recorder (trace hooks: `begin_pass`, `apply_pipeline`, `draw`, `draw_ex`,
  `end_pass`, `commit`) produces each commit's ordered pass list. Per pass it records: index,
  label, view tag (game frame or editor view id), attachments (label, size, format, samples, mip
  and slice per colour, resolve and depth), load and store actions, draws, instances, elements
  (triangles where the pipeline is triangles), pipelines applied, bindings applied and uniform
  bytes. GPU time and CPU time (begin→end on the CPU) are joined in when available. It replaces
  `SokolSystem::recordPass`. `getFrameStatsLine` output stays **byte-identical**.
- **R11** Draw list (on request, capture only): for each draw, the pipeline label, shader label,
  element count, instances and base element.
- **R12** Pass output preview. When a preview is requested for (view tag, pass index, label,
  attachment), the next matching pass gets one extra blit pass right after its `sg_end_pass`, in
  our wrapper and not in a hook. The blit copies the attachment into a persistent RGBA8 preview
  target of ≤1024 px on the longest side. Modes are Color, Alpha, Depth (linearised with the
  camera near/far when known, else min-max normalised) and per-channel R/G/B. Colour attachments
  that are MSAA use their resolve image; MSAA depth without a resolve image and swapchain passes
  report `preview_unavailable` with a reason. Cubemap and array attachments preview the bound
  face/slice and mip.

### Functional — CPU profiler

- **R13** Macros live in `engine/core/profiler/Profile.h`:
  - `JE_PROFILE_SCOPE("literal")` and `JE_PROFILE_FUNCTION()`.
  - `JE_PROFILE_SCOPE_DYN(std::string_view)`, which interns the name and is meant for Lua and
    asset names.
  - `JE_PROFILE_THREAD("name")`, `JE_PROFILE_FRAME()` (Engine only) and
    `JE_PROFILE_COUNTER("name", value)`.
  - `JE_GPU_ALLOC_SCOPE(cat, owner)`.

  With `JASPERENGINE_PROFILER=0` each macro expands to `((void)0)` and evaluates no argument.
- **R14** Events go to a per-thread SPSC ring: 16-byte `{u64 ns, u32 nameId|kind, u32 aux}` and
  65,536 events per thread by default. The main thread drains them at `JE_PROFILE_FRAME()`.
  Overflow drops events (it never blocks) and sets a per-frame `dropped` count. Scopes still open
  at the frame boundary are split, so each frame's tree is complete.
- **R15** Per frame the profiler builds a tree per thread (name, total, self, calls, depth;
  children merged by name at the same level) and a timeline (raw intervals per thread). History is
  600 frames of summaries (per-category ms, totals, monitors). The full detail of the newest 300
  frames is kept within a cap of 64 MiB (configurable). Older detail is evicted first.
- **R16** Standard top-level scopes are added at these sites:
  - `Engine::systemDraw`: Frame, Input, FixedUpdate, Physics2D/3D, Update, Lua, Animation,
    UI Update, Draw, Commit, Present/Wait.
  - `RenderSystem` per pass family: Shadows, Depth prepass, GBuffer, Opaque, Transparent, SSAO,
    SSR, Bloom, PostFX, HTML UI.
  - Asset loading, shader compile and `SokolCmdQueue` execute.
  - Every job (S06), named by the job's label.

  These map to chart categories Rendering, Scripts, Physics, Animation, UI, Loading, Jobs, Wait
  and Other.
- **R17** Every engine thread registers itself: the main thread, the async loader, every S06
  worker ("Job Worker #n", "Job Background #n"), the runtime shader compiler, the video decoder,
  the S09 logger thread and the audio callback. Unregistered threads that emit scopes get
  "Thread <os id>".
- **R18** Monitors are a registry of named per-frame series (latest, avg, min, max, 600 samples).
  Built-in monitors:
  - FPS, CPU frame ms, GPU frame ms.
  - Draw calls, instances, triangles (from `Engine::FrameStats`).
  - Passes, pipelines applied, uniform bytes, image and buffer upload bytes (from sokol
    `sg_frame_stats`).
  - VRAM estimated total, process RSS, Lua heap KiB.
  - Job queue depth and worker utilisation % (S06), HTML UI draws (`HtmlUIRenderStats`).
- **R19** The frame-time ring (cpuMs, gpuMs, fps, draws) is **always recorded** in the editor and
  in development builds, even with the profiler off. It is one 32-byte struct per frame.
- **R20** Capture files use Chrome Trace Event JSON (`.jetrace.json`): `traceEvents` (`X` events
  per scope, `C` counters, `M` thread names, GPU passes as a "GPU" pseudo-thread) and a `jasper`
  metadata object `{version:1, backend, device, frames[], passes[], vram}`. Perfetto and
  `chrome://tracing` open it unchanged, and the editor loads it back into the Profiler.

### Platform & backends

- **R21** GL family: one `GL_TIME_ELAPSED` query per pass (sokol passes never nest), read with
  `GL_QUERY_RESULT_AVAILABLE` only and never blocking.
  - GL 3.3 core: core `ARB_timer_query`.
  - GLES3: `EXT_disjoint_timer_query`, entry points through `eglGetProcAddress` (Android uses
    `SOKOL_EXTERNAL_GL_LOADER`).
  - WebGL2: `EXT_disjoint_timer_query_webgl2`, enabled through
    `emscripten_webgl_enable_extension`; check `GL_GPU_DISJOINT_EXT` every commit.

  A missing extension gives `unavailable: extension_missing`.
- **R22** D3D11: per commit, one `D3D11_QUERY_TIMESTAMP_DISJOINT` (begun at the commit's first
  pass, ended in the pre-commit hook) and two `D3D11_QUERY_TIMESTAMP` per pass, on
  `_sg.d3d11.ctx`. Results are read with `D3D11_ASYNC_GETDATA_DONOTFLUSH`.
- **R23** Vulkan:
  - One `VK_QUERY_TYPE_TIMESTAMP` pool per ring slot (8 slots × 512 queries).
  - `vkCmdWriteTimestamp2` on `_sg.vk.frame.cmd_buf`, after `vkCmdBeginRendering` (begin, stage
    `TOP_OF_PIPE`) and before `vkCmdEndRendering` (end, `ALL_COMMANDS`).
  - Read with `VK_QUERY_RESULT_64_BIT` without WAIT. A slot is reset with host `vkResetQueryPool`
    after reading.
  - `VulkanContext.cpp` enables `hostQueryReset` when supported. Ticks are scaled by
    `timestampPeriod` and masked by the queue family's `timestampValidBits`. Zero valid bits or no
    `hostQueryReset` gives `device_unsupported`.
- **R24** Metal (macOS, iOS):
  - When `[device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]` and the
    timestamp counter set exist, each render pass descriptor gets a `sampleBufferAttachments[0]`
    (`startOfVertex`/`endOfFragment` indices) in a ring of `MTLCounterSampleBuffer`s.
  - Samples are resolved after command-buffer completion (`addCompletedHandler`) and converted to
    ns with `sampleTimestamps:gpuTimestamp:`. Semantics: `encoder_span`. One sokol pass is one
    render encoder, so the span is per pass, but spans may overlap and are not additive.
  - Without counter support, only the command buffer's `GPUStartTime/GPUEndTime` is available
    (frame level), and passes are `device_unsupported`.
  - Compute passes (we have none in-game) are `unavailable` on Metal.
- **R25** Any other backend (`GraphicBackend::WGPU`) or compile path returns `unavailable: backend`
  and compiles to stubs. No backend may fail to build or crash because of the extension.

### Performance (budgets; measured on the remote build servers)

- **R26** Compiled out (`JASPERENGINE_PROFILER=0`, release games): no profiler symbols in the
  binary and no trace hooks installed. The GPU timer hook sites cost one null-pointer test each.
- **R27** Compiled in, runtime disabled (the editor and development builds by default):
  - ≤ 2 ns per scope (one relaxed atomic load + branch).
  - VRAM tracker ≤ 1 µs per create/destroy.
  - Pass recorder ≤ 0.2 µs per draw and ≤ 1 µs per pass.
  - Frame-time ring ≤ 1 µs per frame.
  - Total ≤ 0.5% of CPU frame time on the `benchmark` fixture.
- **R28** Enabled, recording:
  - ≤ 50 ns per scope pair on desktop x86-64/ARM64; ≤ 150 ns on Android and WebAssembly.
  - Main-thread drain and tree build ≤ 0.3 ms per frame at 5,000 events.
  - Total CPU overhead ≤ 3% of frame time on the `benchmark` fixture with the default
    instrumentation.
  - GPU timers: ≤ 2 queries per pass, ≤ 5 µs CPU per commit + 1 µs per pass, no GPU stalls (never
    read a result that is not available).
- **R29** Memory: 1 MiB ring per registered thread (configurable 256 KiB–8 MiB) and history ≤ 64 MiB
  by default. The VRAM tracker uses ≤ 128 B per resource.
- **R30** Pass preview costs one extra blit pass only on frames where a preview is requested,
  refreshed at ≤ 10 Hz while the Frame Debugger is open and live.

### Editor UX (human)

- **R31** **Window > Analysis > Profiler** (Unity naming):
  - **Toolbar:** Record (toggle), Clear, Deep Profile Lua (toggle: an automatic scope per Lua
    callback), frame budget dropdown (30/60/90/120/144/240 fps; draws the budget line), Load,
    Save (`.jetrace.json`) and Target (Editor / Play Mode / attached capture). A frame-index scrub
    and "pause on frame > X ms" sit on the toolbar too.
  - **Module list (left):** CPU Usage (stacked area chart by category), GPU Usage (stacked by pass
    family, or "unavailable: <reason>"), Rendering (draws, tris, passes, pipelines), GPU Memory
    (stacked by category) and Monitors (user-chosen series). Clicking any chart selects that frame
    in every module.
  - **Detail pane (bottom):**
    - CPU: Hierarchy (sortable Total ms / Self ms / Calls / % columns, search, thread selector)
      and Timeline (one lane per thread, zoom with the wheel, pan with drag, hover tooltip with
      name/duration/depth, click selects the scope in Hierarchy).
    - GPU: a table of passes with GPU ms, CPU ms, draws and a semantics note ("Metal: encoder
      spans overlap; do not sum").
    - GPU Memory: a category summary with bars plus an allocation table (name, owner, category,
      kind, size, format, bytes), sortable, grouped by owner or category, filterable.
- **R32** **Window > Analysis > Frame Debugger**:
  - Toolbar: Enable (live) / Freeze (holds the last captured frame), and a view selector (Game,
    Scene view N).
  - Left side: the ordered pass tree (pass → draws when the draw list is on), repeats folded
    ("Shadow 2048×2048 ×4").
  - Right side: the selected pass's details (targets, actions, counts, timings) and the preview
    image with mode buttons (Color/Alpha/Depth/R/G/B), plus the attachment selector.
- **R33** Clicking the status bar FPS indicator (`editor/App.cpp:1070`) opens the Profiler. The
  Game-view Stats overlay gains "CPU x ms  GPU y ms" lines when the data is available.
- **R34** Panel state (open modules, budget, columns) persists in editor settings, not the
  project. The profiler edits no project data, so it has no undo entries.

### Agent UX (MCP tools, Lua API, CLI)

- **R35** MCP tools (all `readOnly`, `allowedWhilePlaying`, registered by
  `registerProfilerTools` in `editor/mcp/tools/McpTools.h`). Names and params are fixed in §6.6.
  Each description states its caveats: the first frame after arming is empty, armed timings
  include overhead, a frame is a single sample, and the meaning of each semantics value.
- **R36** Resources: `jasper://stats` (a view onto `get_render_stats`) and
  `jasper://profiler/capture/{id}` (`.jetrace.json` text).
- **R37** Lua `Profiler` table (in `engine/core/script/binding/`, stubs regenerated through
  `generate_lua_stubs.py`, documented in `docs/mcp/guide/lua-api.md`):
  - `Profiler.beginSample(name)`, `Profiler.endSample()` and `Profiler.sample(name, fn)`.
  - `Profiler.counter(name, value)` and `Profiler.enabled` (rw).
  - `Profiler.frameStats()`, returning `{drawCalls, instances, triangles, cpuMs, gpuMs?, fps}`.

  In builds without the profiler these exist and are no-ops; `frameStats()` still works.
  Unbalanced `endSample` logs once and is ignored.
- **R38** CLI: development builds and the editor accept `--profile` (start recording),
  `--profile-capture=<path>` and `--profile-frames=N` (default 300). After N frames they write the
  capture and log `Profiler capture written: <path>`. S31's
  `jasperengine-editor call get_render_stats` works without extra code.

### Data & compatibility

- **R39** No scene or asset format change. The export config gains `developmentBuild: bool`
  (default false; old project settings load unchanged), which drives `JASPERENGINE_PROFILER=1` and
  `JASPERENGINE_DEVELOPMENT_BUILD=1` for Release exports. Debug exports always have both.
- **R40** `JASPERENGINE_FRAME_STATS` / `_ACTIONS` keep working with unchanged output, now fed by
  the new pass recorder.
- **R41** The `.jetrace.json` `jasper.version` is 1. Loading a newer major version is refused
  with a message; unknown fields are ignored.

## 6. Design constraints & decisions

**6.1 Where the GPU timer lives.** Backend code goes in a new
`engine/libs/sokol/sokol_gpu_timer_impl.h`, included by `sokol.cpp` right after `sokol_gfx.h`
(same TU, so `_sg` internals are visible, like `jasperengine_sg_max_msaa_samples`). We do not
use trace hooks for queries, because of the placement problem in §3. Instead `sokol_gfx.h` gets
exactly **four** hook calls, each marked `// JasperEngine: gpu timer hook`:
1. `sg_begin_pass`, right after `_sg_begin_pass(&pass_def, &atts_ptrs)` (`:25533`).
2. `sg_end_pass`, right before `_sg_end_pass(&atts_ptrs)` (`:25767`).
3. `sg_commit`, right before `_sg_commit()` (`:25777`).
4. Metal only: `_sg_mtl_begin_render_pass`, right before `renderCommandEncoderWithDescriptor:`
   (`:16234`), passing `pass_desc`.

Each call is `if (_sg_jasper_hooks.fn) _sg_jasper_hooks.fn(...)`. The table is filled by
`sg_jasper_gpu_timer_setup()` from `SokolSystem::setup`.

**6.2 Public C API** (`sokol_gpu_timer.h`):
```c
typedef enum { SGJ_TIMER_AVAILABLE, SGJ_TIMER_UNAVAILABLE } sgj_timer_state;
typedef enum { SGJ_SEM_PIPELINE_POINT, SGJ_SEM_ELAPSED, SGJ_SEM_ENCODER_SPAN } sgj_timer_semantics;
typedef struct { const char* label; uint32_t pass_index; double gpu_ms; bool valid; } sgj_pass_time;
typedef struct { uint64_t commit_index; uint32_t view_tag; double frame_gpu_ms; bool frame_is_sum;
                 sgj_timer_semantics semantics; uint32_t num_passes; const sgj_pass_time* passes;
                 uint32_t dropped_passes; } sgj_commit_times;
void sgj_gpu_timer_setup(void);  void sgj_gpu_timer_shutdown(void);
void sgj_gpu_timer_set_enabled(bool on);  void sgj_gpu_timer_set_view_tag(uint32_t tag);
sgj_timer_state sgj_gpu_timer_state(const char** reason_out);
// pops the oldest resolved commit; false when none is ready (non-blocking)
bool sgj_gpu_timer_pop(sgj_commit_times* out);
```
Labels are copied into the timer's own ring storage at begin_pass, because `sg_pass.label`
pointers are not guaranteed to outlive the frame.

**6.3 Engine-side C++ API** (`engine/core/profiler/`):
```cpp
namespace jasperengine::profiler {
  bool isEnabled(); void setEnabled(bool);          // runtime switch (atomic)
  void frameMark();                                 // JE_PROFILE_FRAME
  struct ScopeSite { const char* name; const char* file; uint32_t line; std::atomic<uint32_t> id; };
  class Scope { /* ctor: if(isEnabled()) push begin; dtor: push end */ };
  uint32_t internName(std::string_view);            // JE_PROFILE_SCOPE_DYN, Lua
  void setThreadName(const char*);                  // JE_PROFILE_THREAD
  void counter(const char* name, double v);         // monitors
  const FrameHistory& history();                    // main thread only
  CaptureId startCapture(uint32_t frames); bool saveCapture(CaptureId, const std::string& path);
}
namespace jasperengine::gpumem { enum class Category : uint8_t { Textures, RenderTargets, ShadowMaps,
  PostFX, Meshes, Uniforms, UI, Particles, Terrain, Editor, Other };
  struct Scope { Scope(Category, const char* owner); ~Scope(); };    // JE_GPU_ALLOC_SCOPE
  Report report(); }                                                  // totals + entries
```
- `RenderStats` (`engine/core/render/RenderStats.h`) brings together `Engine::FrameStats`, the
  sokol frame stats, the pipeline cache counters (`SystemRender::getPipelineCache*`), GPU commit
  times and the pass list, per view tag.
- `SystemRender` exposes `getRenderStats(viewTag)`, `getPassList(viewTag)`,
  `requestPassPreview(...)` and `getVramReport()`. The editor and MCP read only `SystemRender`,
  never sokol.
- `RenderBackendTime` stays as is (tests use it).

**6.4 Trace hook ownership.** One `SokolTraceHub` (`engine/renders/sokol/SokolTraceHub.cpp`)
installs the hooks once and chains to the previous table (`sg_install_trace_hooks` returns it).
The pass recorder and the VRAM tracker both subscribe through it. In release games
(`JASPERENGINE_PROFILER=0`) it installs only the pass-recorder subset, and only when
`JASPERENGINE_FRAME_STATS` is set.

**6.5 View tags.** `Engine::systemDraw` sets tag `FRAME_STATS_FRAME` and `Engine::drawView`
sets `viewId`, both before drawing. The commit record keeps the tag. The Profiler's GPU module
shows the game frame by default with a view selector. CPU frames are editor frames (one
`JE_PROFILE_FRAME` per main-loop iteration), and each one contains 1..n commits.

**6.6 MCP tools** (`editor/mcp/tools/ProfilerTools.cpp`):

| Tool | Params | Returns |
|---|---|---|
| `get_render_stats` | `view?: "game"\|"scene"\|<id>` (default game) | draws, instances, triangles, passes, pipelines/bindings applied, uniform bytes, upload bytes, alive resources, pipeline cache {misses, applies, size}, `cpuMs`, `fps`, `gpu {available, reason?, frameMs?, semantics}` |
| `get_frame_times` | `window?≤600`, `minFrameMs?`, `frames?`, `monitors?: string[]` | `summary {cpuMs,gpuMs,fps: {median,p95,p99,max}}`, spike list with `spikeIntervalFrames`, newest N frames, requested monitor series |
| `profiler_capture` | `frames?=120 (≤600)`, `gpu?=true`, `deepLua?=false`, `savePath?` | replies after the frames are recorded: `captureId`, overhead note, per-category ms summary, worst frame index, `path` when saved |
| `get_cpu_profile` | `captureId?` (default live last frame), `frame?: int\|"worst"\|"median"`, `thread?` (default main), `view?: "tree"\|"flat"\|"threads"`, `filter?`, `maxRows?=64` | tree rows `{name,depth,totalMs,selfMs,calls}`, or flat rows with per-thread split, or per-thread busy % |
| `get_gpu_passes` | `view?`, `captureId?`, `includeDraws?=false`, `maxDraws?=200` | ordered passes `{index,label,targets[],actions,draws,instances,triangles,pipelines,cpuMs,gpuMs?,valid}`, `semantics`, `availability` |
| `capture_pass_output` | `view?`, `pass: int\|label`, `attachment?: "color0".."color3"\|"depth"`, `mode?`, `max_width?=512` | PNG image + pass header, or `preview_unavailable` with reason |
| `get_vram_report` | `groupBy?: "category"\|"owner"`, `top?=30`, `filter?` | totals, per-group bytes and counts, top allocations, `estimated:true`, driver `{usedBytes,budgetBytes}` or null, peak |

`profiler_capture` arms GPU timing and recording, then restores the previous state. The combined
character budget for all seven tools (name + description + schema, `tools/mcp/tool_budget.py`)
is ≤ 7,000. Document the workflows in a new `docs/mcp/guide/profiling.md`: read stats, capture,
find the worst frame, drill down, check passes, check VRAM.

**6.7 Pass preview path.** Our pass-ending call sites (`SokolCamera::endRenderPass`,
`SokolCmdQueue` END_PASS, the clear pass in `SokolFramebuffer`) call
`SokolPassPreview::afterEndPass()`. Only when a request matches does it begin the blit pass,
using the existing fullscreen-copy shader extended with the visualisation modes (built for every
backend by our shader pipeline). The preview target is a `Framebuffer` the editor shows through
`Backend::getImGuiTexture` (`editor/Backend.h:45`). MCP readback reuses the screenshot capture
path (`ViewTools.cpp` `w3::captureView`) on that target.

**6.8 Build flags.**
- `option(JASPERENGINE_PROFILER ...)` defaults ON when `JASPERENGINE_SHARED` (editor), for Debug
  builds, or for `JASPERENGINE_DEVELOPMENT_BUILD`; OFF otherwise. It follows the
  `JASPERENGINE_HTMLUI_DEBUGGER` pattern in `engine/CMakeLists.txt`.
- The Export window gets a **Development Build** checkbox (Unity naming). S30's prebuilt players
  ship a `development` flavour per platform.

**6.9 Tracy: yes, optional.**
- Vendor only the Tracy client (`engine/libs/tracy`, pinned v0.11.x, BSD-3), compiled only with
  `JASPERENGINE_TRACY=ON` (default OFF; desktop and Android only, refused at configure time for
  Emscripten/iOS).
- With Tracy on, the `JE_PROFILE_*` macros emit a Tracy zone too (`ZoneScopedN` / `ZoneTransientN`
  for DYN), `JE_PROFILE_FRAME` emits `FrameMark`, and counters become `TracyPlot`. Resolved GPU
  pass times are sent as plots `GPU/<pass>`; there are no Tracy GPU zones.
- Our in-editor profiler stays the primary tool. Tracy is how you profile a running development
  game remotely. We build no other remote protocol in S15. Default decision: capture files cover
  mobile and web.

**6.10 Threading.**
- Rings are written only by their owner thread. The main thread drains them with
  acquire/release indices.
- Thread registration is a lock-protected list touched only on thread start/exit.
- The VRAM tracker and pass recorder run on the thread that calls sokol. That is the main thread:
  `SokolCmdQueue` executes queued commands there.
- Timestamps come from `std::chrono::steady_clock` (wasm: `emscripten_get_now`). Precision is
  stored in the capture metadata.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R13, R26) | `ProfilerCompileOut.MacrosEvaluateNothing`: a TU built with `-DJASPERENGINE_PROFILER=0` passes a side-effecting expression to every macro; a counter stays 0. CI step: `nm` on a Release export of the fixture finds no `jasperengine::profiler::` symbols. |
| AC2 (R14, R15) | `Profiler.NestedScopesBuildTree`: 3 levels across 2 frames; totals and self times are consistent (self = total − Σchildren ±1 µs); calls merge by name. `Profiler.ScopeSplitAtFrameBoundary`. `Profiler.RingOverflowDropsAndCounts`: 100k events into a 1k ring → no block, `dropped>0`. |
| AC3 (R17) | `Profiler.WorkerThreadsAppear`: S06 `ParallelFor` over 8 jobs with scopes → ≥2 thread lanes named "Job Worker #n" with the job scopes; TSan job in S02 CI is clean. |
| AC4 (R27, R28) | `jasperengine-editor benchmark` on the fixture, 3 runs each with the profiler compiled in + disabled, enabled, and enabled + GPU timers, against a `JASPERENGINE_PROFILER=0` build: median CPU frame time overhead ≤0.5% / ≤3% / ≤3.5% of the frame **or** ≤ 5 / 30 / 40 µs a frame, whichever is larger (the absolute floor approved by the user 2026-11-01: on a GPU's ~1 ms frame 0.5 % is 5 µs, under the measurement's resolution). Micro-bench `ProfilerBench.ScopePair` ≤50 ns (x86-64 server). Thresholds asserted by `tools/perf/profiler_overhead.py`. |
| AC5 (R1–R4, R21–R23) | `test_profiler.py::test_gpu_passes_glcore` and `::test_gpu_passes_vulkan` (smoke backends, llvmpipe/lavapipe): `get_gpu_passes` after 10 frames → every pass `valid`, `gpuMs>0`, the frame sum within 0..`frameMs`·1.05. On a forced-unsupported device (`JASPERENGINE_GPU_TIMER=off`) → `availability.state=="unavailable"`, reason `disabled`, and no crash. |
| AC6 (R22, R24, R21-web) | Manual check, recorded in the PR: on a Windows D3D11 box, a macOS Apple-silicon Metal box, Android GLES3 and Chrome WebGL2, open Profiler → GPU Usage, see per-pass ms (Metal reads "encoder spans"). Firefox WebGL2 shows "unavailable: extension_missing". Screenshots attached. |
| AC7 (R25) | Every CI platform build compiles the extension. `GpuTimerGpu.EveryPassIsTimed` checks the hooks through valid per-pass timings and their frame sum. |
| AC8 (R6–R9) | `VramTracker.CreateDestroyBalances`: make 10 images (RGBA8 256², mips, MSAA 4 RT, BC1 if supported) + 5 buffers → bytes match the closed-form sums; destroy → 0. `VramTracker.QueuedCreationKeepsOwner`: creation through `SokolCmdQueue` under `JE_GPU_ALLOC_SCOPE(PostFX,"Bloom")` → owner "Bloom". MCP `test_vram_report`: fixture scene → `Other`+untagged ≤5% of bytes, and Shadow Maps contains the CSM atlas. |
| AC9 (R10, R40) | Golden-log test: `JASPERENGINE_FRAME_STATS=1` on the post-processing fixture produces a line byte-identical to the pre-change baseline (stored in `engine/tests/data/framestats_baseline.txt`). `get_gpu_passes` on the same fixture lists the same labels in the same order, plus passes queued through `SokolCmdQueue`. |
| AC10 (R12) | `test_profiler.py::test_capture_pass_output`: Scene with a red unlit quad; `capture_pass_output` of the opaque pass color0 → centre pixel ≈ (255,0,0) ±3; depth mode → non-uniform image; swapchain pass → `preview_unavailable`. |
| AC11 (R19, R35) | `test_frame_times`: play 120 frames with a script that sleeps 50 ms every 30th frame → `get_frame_times(minFrameMs=40)` lists ≥3 spikes with `spikeIntervalFrames==30`. |
| AC12 (R20, R38, R41) | `test_profiler_capture`: `profiler_capture(frames=60, savePath=…)` → the file parses as JSON with `traceEvents` (≥1 `X` event per frame on main) and `jasper.version==1`; Load in the Profiler restores 60 frames (UI test via the editor-UI MCP harness). The CLI `--profile-capture` on an exported development build of the fixture writes the file and logs the line (smoke-test step). |
| AC13 (R37) | Lua test script: `Profiler.sample("work", fn)` appears under Lua in `get_cpu_profile` with calls==1. In a non-profiler build `Profiler.frameStats().drawCalls>0` and `beginSample` is a no-op. Unbalanced `endSample` logs once. |
| AC14 (R31–R33) | Manual check script in `docs/specs/checklists/S15.md` (created by the implementer): open each module; select a frame from the chart and check that Hierarchy and Timeline follow it; set the 60 fps budget and see the line at 16.7 ms; open the Frame Debugger, select the bloom pass, switch the preview modes; click the status-bar FPS indicator and the Profiler opens. |
| AC15 (R35) | `tool_budget.py --json` shows the 7 tools ≤7,000 chars in total. `test_docs.py` passes with `profiling.md` linked from the guide index. |
| AC16 (R39) | Project from before S15 opens; the export config shows Development Build off. `test_build.py::test_development_build_flag` checks that the configure line has `-DJASPERENGINE_DEVELOPMENT_BUILD=ON` when checked. |
| AC17 (R9) | On Vulkan (lavapipe exposes `VK_EXT_memory_budget`), `get_vram_report.driver.usedBytes>0`. On glcore llvmpipe `driver==null` and the panel shows "n/a". |

## 8. Dependencies & conflicts

- **Depends on:** S01 (C++20: `std::atomic_ref`, `<bit>`, designated initializers), S02 (gtest,
  TSan, perf-check CI step), S06 (worker threads call `JE_PROFILE_THREAD`; each job runs inside a
  scope named by its label; queue-depth and utilisation counters). S06 may land first using
  `Profile.h` from S15's phase 1, which is header-only stubs plus the runtime.
- **Consumers / coordinated:**
  - S13 dynamic resolution reads `sgj` frame GPU ms (falling back to CPU ms when unavailable).
  - S09 names its logger thread and adds Lua/log scopes.
  - S16 publishes UI draw counters as monitors.
  - S14 changes compressed-format byte math (sokol pitch functions already handle blocks).
  - S28's hang watchdog may use `frameMark` timestamps.
  - S30 needs a development flavour of the prebuilt players.
  - S31's CLI exposes the tools.
  - S25 adds the command palette entries "Open Profiler" and "Open Frame Debugger".
- **Merge hotspots:**
  - `engine/libs/sokol/sokol_gfx.h` (any sokol update) and `sokol.cpp`.
  - `engine/renders/sokol/SokolSystem.cpp`, `SokolCamera.cpp`, `SokolCmdQueue.cpp`,
    `SokolFramebuffer.cpp`.
  - `engine/core/Engine.cpp` (`systemDraw` and `drawView`, which S06/S07/S09 also touch).
  - `engine/core/subsystem/RenderSystem.cpp`: scopes and alloc scopes, shared with S10–S13 and S20.
    Keep each insertion to a single line.
  - `engine/platform/common/VulkanContext.cpp` (feature chain).
  - `editor/App.cpp` (menu, status bar) and `editor/mcp/tools/McpTools.h`.
  - `editor/Exporter.{h,cpp}` (S30) and `engine/CMakeLists.txt` (S02/S14).

**Phasing (each phase shippable):**
- P1: CPU profiler core, macros, monitors, the frame-time ring, `get_render_stats`,
  `get_frame_times`, the Lua API.
- P2: trace hub, pass recorder, VRAM tracker, Frame Debugger (no preview), `get_gpu_passes`,
  `get_vram_report`.
- P3: GPU timer extension (Vulkan/GL first, then D3D11 and Metal), Profiler GPU module, pass
  preview.
- P4: captures and files, the CLI, Development Build export, Tracy.

## 9. Risks & defaults

- **Metal counter sampling on older iOS GPUs and the simulator** may be unsupported. *Default:*
  frame-level command-buffer GPU time only, with passes shown as `device_unsupported`.
- **WebGL timer queries** are missing on Firefox and Safari, and quantised on Chrome. *Default:*
  unavailable with a reason, and the CPU profiler still works. Never poll synchronously.
- **Emscripten GL entry points** for `GL_TIME_ELAPSED_EXT`/`GL_GPU_DISJOINT_EXT` may not pass
  through `glGetIntegerv`/`glBeginQuery` cleanly. *Default:* a 30-line `EM_JS` shim that calls
  `ext.*` directly when the plain GL path fails the self-test at setup.
- **Vulkan without `hostQueryReset`** (rare Android drivers). *Default:* `device_unsupported`;
  `vkCmdResetQueryPool` sequencing is not attempted.
- **sokol updates drop the hooks.** *Default:* the marker unit test (AC7) fails the build. Hooks
  are 4 one-line calls, re-applied by hand.
- **Estimated VRAM differs from driver numbers** (padding, tiling, compression). *Default:* label
  the column "Estimated" and show the driver total next to it where one exists. We do not try to
  model driver padding.
- **Owner coverage** depends on `JE_GPU_ALLOC_SCOPE` reaching all creation sites. *Default:* fall
  back to category from usage and owner from label; AC8 caps untagged bytes at 5%.
- **Editor double counting.** Editor views commit separately and editor ImGui is not timed.
  *Default:* the GPU module defaults to the game frame, with a view selector, and labels ImGui
  as "CPU only".
- **Profiler overhead skews results.** *Default:* MCP descriptions and the panel show "Recording:
  +x% overhead (estimated)" from the measured per-scope cost × event count.
- **Prebuilt development players (S30)** double the per-platform binaries. *Default:* ship them.
  If S30 cannot, Development Build requires a compiler-based export until it can.

## Deviations

Recorded by the implementation (the branch `feat/s15-profiling`); each is the closest correct
alternative to what the text above asks.

- **MCP parameter names are snake_case** (S31 R20): `min_frame_ms`, `capture_id`, `deep_lua`,
  `save_path`, `include_draws`, `max_draws`, `max_rows`, `group_by`, `max_width`. Result keys are
  snake_case too (`gpu_ms`, `frame_ms`, `spike_interval_frames`). The seven tools are in S31's
  `profiling` group (4,523 of its 7,000 characters, `tools/mcp/budget.json`); `focus_window`'s
  `profiler` and `frame_debugger` windows stay in core (+177).
- **An eighth MCP entry point.** §6.6 lists seven tools; `capture_pass_output` is one of them, and
  `focus_window` gained `profiler` and `frame_debugger` (the UI tests open the panels with it).
- **The view parameter's default** is the engine's frame (the running game while playing, else
  the Scene view), not "game": in edit mode the Game view draws only while its tab is shown, so a
  default of "game" would often find nothing. `"game"` and `"scene"` name the views explicitly.
- **The sokol extension's API** grew what the engine needs: `sgj_gpu_timer_setup` takes a desc
  (`disabled`, for `JASPERENGINE_GPU_TIMER=off`), results carry `frame_valid` and `pass_reason`,
  and the TU exports `sgj_gpu_timer_get_stats`, `sgj_commit_index`, `sgj_pass_valid`,
  `sgj_pixel_format_name`, `sgj_query_device_memory`, `sgj_device_name` and
  `sgj_gpu_timer_set_vulkan_host_query_reset` (whoever creates the VkDevice says whether it enabled
  `hostQueryReset`; both device creators, `VulkanContext` and the editor's renderer, enable it when
  supported). The four hooks are where §6.1 puts them; as a consequence the GL query of a pass
  starts after the backend's begin (its load clears are before it) and ends before the MSAA
  resolve of the backend's end, and the Vulkan timestamps sit inside the dynamic rendering
  instance. The hooks are installed while a consumer holds the timer and removed otherwise (R26:
  one null-pointer test per site), not filled at setup.
- **Metal** samples four points per render encoder (start and end of the vertex and fragment
  stages; begin = the first that ran, end = the last), because an encoder that only clears runs no
  vertex stage. The frame time on Metal is always the command buffer's `GPUEndTime -
  GPUStartTime` (encoder spans overlap; their first-to-last would not be the frame either).
- **Threads.** S06 has one worker kind (lanes, not separate background threads), so the lanes are
  "Job Worker #n", "Job Blocking #n" (S06's blocking channel threads) and "Asset Reader #n"; there
  is no "Job Background #n". The audio callback runs inside SoLoud's/miniaudio's own thread with no
  engine code, so it has no scopes and no lane. Thread names come from `thread::setCurrentName`
  (every engine thread names itself there) and the job system's `onThreadStart` hook.
- **Frames.** A CPU frame is one rendered iteration of the editor loop (`EditorFrame::run`) or one
  `Engine::systemDraw` in a game; `cpuMs` is its start to the end of its CPU work (before
  presenting and waiting), `frameMs` its start to the next frame's start.
- **AC4's compiled-out baseline** is the editor built from the commit before S15: the editor
  itself cannot be built without the profiler (its panels and tools use it), and release games
  have no benchmark mode. `tools/perf/profiler_overhead.py` measures the editor with the profiler
  idle, recording (`benchmark --profile`) and recording with GPU timers (`--profile
  --gpu-passes`) against it, interleaving the runs.
- **AC8's tests** are `VramTrackerGpu.CreateDestroyBalances` and `VramTrackerGpu.QueuedCreationKeepsOwner`
  (a suite that needs a device ends in `Gpu`, docs/testing.md). "Other+untagged ≤ 5%" is read as
  the bytes the tracker could not classify (category Other): resources made outside a
  `JE_GPU_ALLOC_SCOPE` still get a category from their usage and an owner from their label;
  `get_vram_report` reports `untagged_bytes` separately.
- **AC9's golden log** compares the line after its `Frame stats (frame N): ` prefix (N depends on
  how long loading took): `tools/ci/check_profiler_smoke.py --frame-stats
  engine/tests/data/framestats_baseline.txt build/smoke/game.log build/smoke/game-actions.log` after
  `SMOKE_GAME_ENV=JASPERENGINE_FRAME_STATS=1 SMOKE_GAME_RUNS="actions
  JASPERENGINE_FRAME_STATS_ACTIONS=1" tools/smoke-test.sh tools/fixtures/post-processing`.
  The pass recorder records every pass; the frame stats line keeps listing the passes SokolCamera
  begins, as before, from SokolCamera's own pass descriptions.
- **The pass preview shader** (`PASS_PREVIEW`, `shaderlib/pass_preview.frag`) is built on demand
  by the editor and is not exported (games have no Frame Debugger). Depth is linearised with a
  near/far the Frame Debugger is given (Linear), else mapped through Levels;
  `capture_pass_output` normalises depth to the exact range it read back on the CPU (the min-max
  of R12). Its blit is timed by the GPU timer but left out of pass lists.
- **The Xcode workspace** builds without the profiler (no `JASPERENGINE_PROFILER` definition, the
  macros compile out): Apple development builds go through CMake.
- **Profiler clock.** Scopes read the CPU's counter (`rdtsc` on x86-64, `cntvct_el0` on arm64,
  steady_clock elsewhere), converted to steady_clock nanoseconds when the main thread drains the
  rings, with a factor refined every frame: a steady_clock read is a vDSO call costing ~30 ns on
  the virtual build servers, which put a recording scope pair over R27's 50 ns.
- **S13 dynamic resolution** (merged after this spec was written) is the GPU consumer of R5: while
  it runs it holds the GPU timer (in every build, the profiler compiled in or not) and its GPU law
  is fed the game frame's GPU time (`FrameTimes::latestGpuMs`, the newest of the last 8 frames,
  a few frames late); `fake_gpu_ms` still takes precedence, and without a timer (WebGPU, Firefox,
  a device without timestamps) it uses its frame law as before.
- **AC4 measured** on the benchmark server E (4 dedicated vCPU, llvmpipe under Xvfb,
  `vblank_mode=0`, `tools/fixtures/post-processing`, 600 measured frames a run) against oe-port
  f93897204 with the same benchmark fix (below), with `tools/perf/profiler_overhead.py`: shuffled
  rounds, each configuration's overhead paired with the same round's baseline, mean and 95 %
  confidence interval. Recording (+1.03 %) and recording with GPU timers (+0.66 %) are well within
  3 % and 3.5 % (9 rounds). The idle profiler, after taking the idle costs out (a pass's
  attachments are queried only while a reader wants them; allocation scopes intern their owner
  only when something is made; the monitors use handles and skip the driver's memory query): 60
  rounds +0.32 % [+0.06, +0.57], 60 more +0.48 % [+0.19, +0.76], pooled 120 rounds +0.40 %
  [+0.21, +0.59]. The mean is within 0.5 %; the interval's top is not, on a device whose frame
  is software rasterisation on the same four cores (a round's own spread is about 1 %). What is
  left on the idle path per frame is the scopes' relaxed load and branch, sokol's counters and the
  pass recorder's per-pass record. The idle figure is to be measured again on the real-GPU
  benchmark machine in the GPU performance campaign, where the CPU does not also rasterise.
- **`jasperengine-editor benchmark` is no longer throttled when unfocused**: an X server without a
  window manager focuses no window, and the editor's unfocused frame-period sleep held every
  benchmark at 16.3 ms (60 fps) whatever its work.
- **AC12's smoke run**: `SMOKE_EXPORT_ARGS=--development-build SMOKE_READY_SETTLE=60
  SMOKE_GAME_ARGS="--profile-capture=/src/build/smoke/capture.jetrace.json --profile-frames=30"
  tools/smoke-test.sh tools/fixtures/post-processing`, then `tools/ci/check_profiler_smoke.py
  --capture build/smoke/capture.jetrace.json build/smoke/game.log` (the path is the container's;
  llvmpipe draws the recorded frames slowly, so the game needs the longer settle).
- **`profiler_capture` is named `capture_profile`**: S31's convention (a new tool's name is
  verb_noun, `tools/mcp/tests/test_tool_conventions.py`) landed before S15.
- **AC4 re-measured on the real-GPU machine (GPU campaign, RTX 3080 Ti, GL, docs/perf/gpu-campaign.md).**
  `tools/perf/profiler_overhead.py --runs 30` on the post-processing fixture, the editor of the S15
  merge (a4b621ebb) against its first parent (9f26ea019), both with the benchmark focus fix
  (c3acfbd56), so S15 is the only difference; frames of 0.81 ms: idle **+0.72 %** [+0.04, +1.40]
  (≤ 0.5 %: over, about 6 µs a frame), recording +1.16 % [+0.56, +1.77] (≤ 3 %), recording with GPU
  timers **+4.19 %** [+3.59, +4.79] (≤ 3.5 %: over, about 34 µs a frame). A budget relative to the
  frame is 100x tighter at a GPU's 0.8 ms than at llvmpipe's 100 ms; what the idle path still pays
  per frame is listed above (scope loads and branches, sokol's counters, the pass recorder's
  per-pass record). Open: these microseconds were not optimised in the campaign. Measured against
  the pre-S15 commit f93897204 instead, today's editor is +25.9 %: everything merged since, not the
  profiler.
- **Vulkan pass timers in the editor (GPU campaign):** on light frames (under ~0.5 ms, 1,000+ fps
  uncapped, three commits a frame) captures carried none of the Game view's passes while heavy frames
  were timed every run. **Fixed in perf2:** not the timer ring (no commit dropped: `get_render_stats`
  `gpu.commits` now counts resolved and dropped commits by reason) but the profiler: a light
  frame's commit resolves before the frame ends, and `profiler::recordGpuFrame` only looked for
  frames already marked, so those times reached no frame (all-zero pass medians in captures, while
  `get_gpu_passes` live had them). Times of the frame in progress now wait for it
  (`Profiler.GpuTimesOfTheFrameInProgressReachIt`); the probe capture went from every pass 0 to
  CloudsScreen 0.29 ms, camera 0.24 ms.
- **AC4 in perf2 (RTX 3080 Ti, GL, the post-processing fixture now 1.06 ms a frame,
  `profiler_overhead.py`-style paired rounds, 30 each):**
  - the idle profiler's machinery switched off in today's editor (sokol's stats, the trace hub and
    pass recorder): -0.31 % [-0.80, +0.18], under the resolution; recording against idle +0.42 %
    [-0.12, +0.95]; recording with GPU timers against idle **+3.16 %** [+2.39, +3.93] (~34 µs a
    frame, two GL queries a pass and their read-back);
  - tried: no `glFlush` per commit (the swap flushes): **slower**, -1.17 % [-2.00, -0.34] for the
    flush, so it stays; one availability query a commit instead of one a pass (queries complete in
    order) and counters that look their monitor up once: no measurable change (+0.13 % [-0.46,
    +0.73]), kept as they do less;
  - **revision, approved by the user 2026-11-01 (AC4 above):** the relative bounds were set on
    llvmpipe's ~100 ms frame; on a GPU's ~1 ms frame 0.5 % is 5 µs, under the confidence interval
    of 30 rounds (±0.5-0.7 points). AC4 is now ≤ 0.5 % / 3 % / 3.5 % of the frame **or** ≤ 5 / 30 /
    40 µs a frame, whichever is larger (`tools/perf/profiler_overhead.py` checks it); GL meets it
    (idle within noise, recording ~4 µs, GPU timers ~34 µs).
