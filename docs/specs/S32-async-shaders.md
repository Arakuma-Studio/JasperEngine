# S32 — Asynchronous shader and pipeline creation

Wave 3. Owner area: `engine/renders/sokol/` (new `SokolAsyncCreate`), `engine/libs/sokol/` (a
small creation extension, like S15's GPU timer), `engine/core/pool/ShaderPool`,
`engine/core/shader/` (new `ShaderWarmup`), `engine/core/subsystem/RenderSystem*` (stand-ins),
the platform GL context code, `editor/` (settings, status bar, export hook, MCP stats).

## 1. Goal

The first use of a shader variant or a pipeline must not stall the frame, on any backend. S11
measured the stall (AC12, Deviation 18): a lit variant built on a job still costs the main
thread **1.2-1.3 s** of GL compile and link in `sg_make_shader`, then **~2.6 s** of llvmpipe JIT
at its first draw. Every new variant pays this, with or without S11. Vulkan, Metal and D3D11 have
the same problem in other places: the pipeline compile (`vkCreateGraphicsPipelines`,
`newRenderPipelineStateWithDescriptor`) or the runtime HLSL/MSL compile in shader creation.
After S32, the driver work runs off the main thread on every backend. A draw whose variant is
not ready draws a documented stand-in or is skipped, and never shows garbage. A warm-up list
recorded in Play and shipped with exports gets variants ready before they are needed. S15
timers measure the main thread's share, against budgets.

## 2. Scope

In scope:
- Asynchronous creation of every `sg_shader` and `sg_pipeline` the engine makes (meshes,
  terrain, sprites, UI, HTML UI, effects, particles (S20), points, lines, sky, post-processing,
  shadows, fluid), through one engine service with a per-frame main-thread budget.
- Per backend:
  - GL 3.3 / GLES3 / WebGL2: a shared worker context where the platform has one (desktop,
    Android), `KHR_parallel_shader_compile`, and a deferred fallback. Warm draws on the worker
    context for drivers that compile at the first draw.
  - Vulkan: pipelines created on a worker thread through a `VkPipelineCache` that is persisted
    per device.
  - Metal: the completion-handler APIs for libraries and render pipeline states.
  - D3D11: shader creation (D3DCompile, `Create*Shader`) on a worker thread.
- Pipeline interning: objects with identical pipeline descriptions share one pipeline.
- Stand-ins while a variant is not ready, and readiness gates (scene ready signal, warm-up,
  variant switches).
- The shader warm-up list: recorded in Play and in development builds, exported, and replayed
  at startup.
- Editor UX (status bar, Project Settings), Lua, MCP stats, S15 scopes and monitors, docs.
- Benchmarks on E, unit tests, goldens, MCP tests.

Out of scope:
- A render graph, GPU-driven culling and the other do-not-copy items (README). The pass list
  stays as it is; S32 changes only when resources become usable.
- Shader source building (GLSL/SPIR-V/MSL/HLSL cross-compile). It is already asynchronous: the
  editor's `shader-build` job and the game's `RuntimeShaderCompiler`. S32 starts where they end,
  at the GPU object.
- Shipping driver caches. A `VkPipelineCache` blob or a GL program binary belongs to one
  device and driver, so neither ships. The warm-up list ships, and the caches build on the
  player's device.
- GL program binaries (`glProgramBinary`). Mesa and the big desktop drivers already keep their
  own disk caches, and the worker context covers the rest. See Risks.
- WebGPU (sokol has a backend, but the engine does not ship it).

## 3. Current state (verified in code at oe-port 0c22d3b91)

- **Shader source.** `ShaderPool::get` (`engine/core/pool/ShaderPool.cpp:910`) asks the editor's
  builder (`editor/shader/ShaderBuilder.cpp:150`: memory, then the disk cache, then a
  `shader-build` job, Background/Heavy) or, in a game, the precompiled `.sdat`/embedded header,
  then `RuntimeShaderCompiler` (worker thread). `Loading` leaves callers waiting, and every
  caller already treats "not created" as not ready. When the source is there,
  `shared->createShader(data)` runs **synchronously on the main thread**.
- **GPU shader.** `SokolShader::createShader` (`engine/renders/sokol/SokolShader.cpp:136`)
  fills an `sg_shader_desc` and calls `sg_make_shader`. On GL that is `glCompileShader` x2 plus
  `glLinkProgram`, with `GL_COMPILE_STATUS`/`GL_LINK_STATUS` queried immediately
  (`sokol_gfx.h` `_sg_gl_create_shader`), so it blocks. D3D11 runs `D3DCompile` on HLSL
  *source* (the engine ships source, `ShaderVariantCompiler.cpp:267`: only SPIR-V is bytecode).
  Metal runs `newLibraryWithSource:` on MSL source. On the async loading thread
  (`Engine::isAsyncThread`) the creation is queued as `MAKE_SHADER` and still runs as
  `sg_init_shader` on the main thread (`SokolCmdQueue.cpp:91`). `ShaderRender::loading` guards
  re-entry until the queued command ran (`ShaderRender.cpp:23`).
- **Pipelines.** `SokolObject::endLoad` (`SokolObject.cpp:545`) makes one pipeline per
  `PipelineType` per object, with no sharing between identical objects. `beginDraw` (`:591`,
  `:614`) refuses a pipeline that is not `VALID`. The dynamic path (`SokolPipelineCache`, used
  by HTML UI, UI effects and fluid: `RmlRenderer.cpp:2394`, `EffectRenderer.cpp:178`) creates
  pipelines on first use, mid-pass. On Vulkan every one of these is a `vkCreateGraphicsPipelines`
  with `VK_NULL_HANDLE` cache (`sokol_gfx.h` `_sg_vk_create_pipeline`). Metal calls
  `newRenderPipelineStateWithDescriptor:error:` and D3D11 builds state objects. GL pipelines
  are CPU state, but the driver compiles the program's state variant at the first draw.
- **Variants and stand-ins.** The material/shader key system (S10/S11/S12): `ShaderKey` = type
  + 64 property bits + customId. `builtin:lit#<defines>` references carry the S11 lobes.
  `getSubmeshShaderVariant` derives a mesh's variant. While a `builtin:lit` variant builds,
  `loadMesh` draws the plain variant and reloads once the new one is ready
  (`RenderSystem.cpp:5740-5748`, `needVariantReload`). `ShaderPool::getFallbackProperties`
  (`:1024`) lists layout-compatible fallbacks, nearest first, but only a game that *lacks* a
  variant uses it. A post-processing uber variant applies only once built
  (`RenderSystem.cpp:3784`). Global variant switches (post colour, shadow tier) reload meshes
  and keep the old variant until the new one is built (`postColorVariantsReady`).
- **Warm-up.** `ShaderPool::warmUp`, `warmUpAllShaders`, `ShaderVariantCollection` (Unity-like,
  `engine/core/shader/ShaderVariantCollection.h`) and their Lua bindings
  (`CoreClassesLua.cpp:990`) request variants. "Ready" means the sokol shader is `VALID`, and the
  creation itself still stalls. Export ships the scenes' recorded keys (`sceneProject.shaderKeys`,
  `Exporter::resolveShaderKeys`, `Exporter.cpp:874`) and `config.shaderAdditions`. Nothing
  records what Play actually used, and nothing warms pipelines.
- **HTML UI (S16/S17) and particles (S20).** HTML UI programs come from `ShaderPool` at renderer
  load (`RmlRenderer.cpp:2400-2410`), and their pipelines are dynamic, per `PipelineState`.
  Particle renders call `endLoad` per system and stream (`RenderSystemParticles.cpp:432,484,531`).
- **Sokol patches in tree** (`engine/libs/sokol/PATCHES.md`, `// JasperEngine:` markers): the
  ASTC block sizes, RGB565/RGBA4 (S14), the four GPU-timer hooks (S15,
  `GpuTimerGpu.EveryPassIsTimed`), the Vulkan descriptor size for lavapipe, the GL
  texture max level, and the GL depth-stencil invalidate fix. Engine-side backend code that needs
  sokol's internals lives in headers included by `engine/libs/sokol/sokol.cpp`
  (`sokol_gpu_timer_impl.h`, `jasperengine_sg_max_msaa_samples`).
- **Readiness.** `Engine::areScenesLoaded` (`Engine.cpp:1603`) waits for
  `ShaderPool::getPendingShaderCount` and `RenderSystem::isAllLoaded`. The READY signal and
  smoke screenshots follow it.
- **Measurement.** `JASPER_SHADER_TIMING=1` logs builder and create times. S15 has scopes
  ("Shader compile" around `RuntimeShaderCompiler::beginFrame`), `capture_profile`,
  `get_cpu_profile` and `get_frame_times`.

**Measured** (probe on server A, Docker image, llvmpipe LLVM 15, Mesa 23.2.1; a synthetic PBR
fragment shader; not committed):

| Case | Link on main | First draw on main |
|---|---|---|
| everything on the main context | 29-42 ms (blocking; `GL_KHR_parallel_shader_compile` is exposed and `GL_COMPLETION_STATUS_KHR` is already true after the link call) | 743-795 ms |
| link on a shared worker context | 0 (worker 22-49 ms) | 682-814 ms |
| link + a warm draw on the worker, Mesa disk cache off | 0 | 704 ms |
| link + a warm draw on the worker, Mesa disk cache on (Mesa's default) | 0 | **5-8 ms** (heavier shader: 2060 ms → **14 ms**) |

The main thread's worst frame while the worker compiled was 1-7 ms. So on Mesa,
`KHR_parallel_shader_compile` is present but does nothing (the GLSL linker runs on the calling
thread), and the draw-time JIT is per context. A shared context with a warm draw moves both the
link and the JIT off the main thread. The JIT result reaches the main context through Mesa's
shader cache.

## 4. Reference (OpenEngine, MIT; read-only, never executed)

- `Engine/Modules/Rendering/Source/Vulkan/VulkanPipelineCache.{h,cpp}`: a `VkPipelineCache` with
  disk persistence behind a header (magic, version, size, checksum), validated on load, with
  creation statistics. We take the idea, write our own (smaller) file format, and add the
  device's `pipelineCacheUUID`/vendor/device ids to the header, so a cache from another
  driver is ignored rather than handed to it.
- `Engine/Modules/Rendering/Source/Vulkan/VulkanPipelineCompileGate.h`: creations that share
  their shader stages run one after another, so the later ones hit the pipeline cache. We get
  the same effect from interning plus a single creation worker per backend.
- `Engine/Source/Engine/Rendering/ProjectMaterialPrewarmService.cpp`,
  `MaterialPrewarmVariants.h`: a prewarm service that walks the project's materials. Ours is
  driven by what Play used (a recorded list), because our variants depend on the scene
  (lights, fog, shadows tier), not only on the material.
- OpenEngine is Vulkan-first (bindless, render graph) and has no GL/WebGL path, so it does not
  cover our GL, Metal or D3D11 design. No code is copied. If any is, it keeps its header and
  gets a `NOTICE.md` entry.

## 5. Requirements

### Functional

- **R1. Asynchronous shaders.** Every engine shader creation (`SokolShader::createShader`, also
  from the async loading thread) returns at once with a handle in the *pending* state. The GPU
  object is built off the main thread (§6.2) and published on the main thread.
  `ShaderRender::isPending()` reports it. `isCreated()` is false and `isFailed()` false while
  pending, and `ShaderPool::getShaderState` reports `Loading`. `ShaderPool::get` never submits
  a pending variant again.
- **R2. Asynchronous pipelines.** Pipelines are created asynchronously where their creation
  compiles (Vulkan, Metal) and synchronously where it is state only (GL, D3D11). A pending
  pipeline is never applied: `beginDraw` returns false, as for a failed one today.
- **R3. Pipeline interning.** Pipelines made through `SokolObject::endLoad` (and its no-cull
  editor variants) come from a table keyed by the whole description: shader, layout, primitive
  and index type, depth, stencil, colours, blend, cull, winding, sample count and
  alpha-to-coverage, but not the label. They are reference counted. A second object with the
  same description gets the existing pipeline: ready at once if it is ready.
- **R4. GL warm draws.** With a worker context, the first use of a GL program with a pipeline
  state and a set of bound texture formats is drawn once on the worker context. The draw goes
  into a 1x1 target of the pipeline's formats and sample count, with the object's own textures
  and samplers and the pipeline's vertex formats. Until that warm draw is done the object's
  `beginDraw` returns false (R2 semantics). Warm draws are deduplicated by (program, pipeline
  description, texture signature).
- **R5. Main-thread budget.** One `SokolAsyncCreate::update()` per frame publishes finished
  creations until the frame's budget (default 2 ms) is spent. It always publishes at least one,
  so the queue makes progress. Submitting costs only a copy of the description.
- **R6. Lifetime safety.** Destroying a pending shader or pipeline is deferred until its
  creation ends, so a worker never writes to a freed slot. Shutdown cancels what has not started
  and waits for what runs, as do context loss on Android and the editor's project switch.
- **R7. Failures.** A failed asynchronous creation ends in `FAILED`, as a synchronous one does.
  The compile and link log is in the console (GL logs come from the kept shader objects), and
  `ShaderPool` marks the variant failed. Nothing crashes, retries stay as today
  (`isShaderBuildFailed`), and custom shaders still fall back to the built-in one.
- **R8. Stand-ins.** While a variant is pending, the draw uses the nearest *ready* variant and
  swaps when the exact one is ready (`needVariantReload`):
  - MESH, DEPTH and GBUFFER submeshes take the first ready entry of `getFallbackProperties`
    (layout-compatible by construction). A pending candidate is never requested. A
    `builtin:lit#...` variant also falls back to its plain variant (the S11 stand-in), and a
    fork to the same fork's fallback properties.
  - A shader type that switches variants at runtime keeps drawing the current one until the
    new one is ready. That is already the rule for post-processing, post colour, the shadow tier,
    points, lines and sky. It now also waits for the new variant's pipelines
    (`ObjectRender::isReady`).
  - Otherwise the draw is skipped. A pass whose output another pass reads is not consumed
    until it has drawn once: no uninitialised target is ever sampled.
  - There is no "dedicated placeholder material". A mesh's draw binds uniform blocks,
    textures and attributes chosen by its key, so a placeholder must share them, and that is
    exactly what `getFallbackProperties` returns (§6.4).
- **R9. Readiness.** Pending shaders, pipelines and warm draws count as not ready in
  `ShaderPool::getPendingShaderCount`, `Engine::areScenesLoaded` (so the READY signal, the smoke
  test and the editor's loading state), `ShaderVariantCollection::warmUp` and
  `ShaderPool::warmUp`. New: `ShaderPool::getPendingPipelineCount()`.
- **R10. Warm-up list.** A `ShaderWarmup` list holds shader variants (key text plus the fork's
  custom shader name) and pipeline records (shader key plus a backend-neutral pipeline
  description).
  - Recording: in the editor during Play when **Record During Play** is on, and in a
    development build with `--record-shader-warmup <file>`. Each variant and pipeline that
    becomes ready is added; the list merges and is deduplicated.
  - Storage: `ProjectSettings/ShaderWarmup.json`, versioned, sorted (stable diffs).
- **R11. Export.** With **Include in Builds** on (default), the export adds the list's
  variants to the variants it builds (`resolveShaderKeys`). `buildAndSaveShaders` writes
  `shaders/warmup.json` with the entries whose variants it built for the target backend. It is
  the same step on S30's prebuilt path.
- **R12. Runtime warm-up.** At startup, a game loads `shader://warmup.json` and warms it
  progressively (the R5 budget, plus a request cap per frame). Warmed pipelines are held in the
  intern table for the session, so the objects that need them later find them ready. With
  **Hold First Scene** on (default), `areScenesLoaded` is false until the warm-up is done, so the
  loading screen covers it. Lua: `ShaderWarmup.isDone()`, `getProgress()`, `warmUp()`.
- **R13. Vulkan pipeline cache.** One `VkPipelineCache` lives for the device. It is loaded from
  and saved to `<user cache>/jasperengine/pipelines/<vendor>-<device>-<uuid>.vkpc` (header:
  magic, version, vendor/device id, `pipelineCacheUUID`, size, checksum). A mismatched or
  corrupt file is ignored and replaced. The cache is saved at shutdown and, at most once a
  minute, after new pipelines. The write is atomic (S03).

### Platform & backends

- **R14. GL family modes** (§6.3), chosen once at setup:
  - `worker`: the platform supplies a context in the main context's share group (Linux GLX,
    Windows WGL, macOS NSOpenGL/CGL, Android EGL). Shaders are built and warm draws run there.
  - `parallel`: no worker context, and `KHR_parallel_shader_compile` is present (WebGL2 in
    Chrome and Firefox). Compile and link are issued on the main context and published when
    `GL_COMPLETION_STATUS_KHR` is true.
  - `deferred`: neither. Compile and link are issued, and the status is read 2 frames later
    (browsers and threaded drivers compile meanwhile), at most one per frame.
  - `sync`: async off.
  - The mode is in the stats and the log line `[gpu] shader creation: <mode>`.
  - `JASPER_ASYNC_SHADERS=worker|parallel|deferred|sync` forces a mode for tests (falling back
    when it is not available, and saying so).
- **R15. Vulkan.** Shader modules and layouts are created on the main thread: it is cheap and
  touches sokol's delete queue on failure. Pipelines are created on one worker thread
  (`_sg_vk_create_pipeline`, through the R13 cache).
- **R16. Metal.** MSL libraries are compiled with `newLibraryWithSource:options:completionHandler:`
  (bytecode libraries are created directly). Render pipeline states use
  `newRenderPipelineStateWithDescriptor:completionHandler:`. Results are registered in sokol's
  id pool on the main thread only. iOS and macOS are both covered.
- **R17. D3D11.** `D3DCompile` and `Create{Vertex,Pixel}Shader` run on a worker thread
  (`ID3D11Device` is free-threaded; `d3dcompiler_47.dll` is loaded on the main thread at setup).
  Pipelines (input layout and state objects) stay synchronous.
- **R18. Web.** WebGL2 has no shared contexts and no threads can touch it. It uses `parallel`
  (the extension enabled through `emscripten_webgl_enable_extension`) or `deferred`, in the
  single- and multi-threaded web builds alike.
- **R19. Sokol patches** are few and marked `// JasperEngine: async creation`. They are
  documented in `PATCHES.md` and exercised by the `AsyncCreateGpu` creation and lifetime tests.
  Everything else lives in `engine/libs/sokol/sokol_async.h` / `sokol_async_impl.h` (§6.1).
- **R20. Async off** (`asyncCreation = false`) gives today's synchronous behaviour exactly,
  pipeline interning included.

### Performance (budgets; measured with S15 timers on E, median of 5 runs)

- **R21. Main-thread creation work.** The per-frame total of the S15 scopes `GPU create submit`
  + `GPU create publish` + `GPU pipeline create` stays at **≤ 4 ms in every frame** of the
  variant-toggle scenario (§7 AC13), on GL `worker` and on Vulkan. Before S32 it is the full
  1.2 s link (GL). E is about 10% slower than the old E, and the budget includes that.
- **R22. Frame-time spike.** In the same scenario, the worst frame is **≤ max(2× idle, idle + 4 ms)**
  (idle: the idle median frame) **with the driver shader cache warm**, on GL `worker` and on
  Vulkan (approved by the user 2026-11-01). Before S32 the worst GL frame is about 3.9 s on A
  against a 190 ms idle frame. Measured over every frame of the toggles (§7 AC13), the frames that
  ran an MCP tool apart (the agent's edit and the bench's own calls: the editor's cost of the edit,
  not the creation's).
  - Why the bound has the absolute term and the warm cache (GPU campaign perf2; it was ≤ 2× the
    idle median, set on E's 190 ms frame): on the RTX 3080 Ti, with no edit at all, the same
    recording's worst frame is already 2.0× (GL) and 2.2× (Vulkan) the idle median, so at a GPU's
    ~1 ms frame 2× is the machine's own frame-time noise; R21 allows 4 ms of creation work in a
    frame, which a 2 ms bound forbids. And NVIDIA recompiles every program on the context that
    first draws it (driver debug output: "Program/shader state performance warning: ... being
    recompiled based on GL state", ~0.75 s per stage, fragment then vertex), whichever context
    linked it and whatever the warm draw did; no creation mode avoids it (worker, parallel, warm
    draws on and off, matched draw state, program binaries: measured). A variant the driver cache
    (Mesa's and NVIDIA's disk caches, on by default) already has does not recompile; the first
    run's variants are R24's warm-up list's to build during loading.
- **R23. Idle cost.** `SokolAsyncCreate::update()` with nothing pending: **≤ 2 µs** (micro-bench
  `AsyncCreateBench.IdleUpdate`). With async off nothing runs. The pipeline intern lookup in
  `endLoad`: ≤ 1 µs per pipeline (`AsyncCreateBench.InternLookup`).
- **R24. Warm-up.** Replaying the fixture's recorded list (the warm-up fixture, §6.7) holds no
  frame above the R22 bound, and finishes within 1.5× the time of the same variants created
  synchronously (the worker overlaps, the budget spreads).
- **GPU campaign (later, real GPUs):**
  - R21/R22 on desktop NVIDIA/AMD/Intel (GL `worker` and `parallel`, Vulkan), on Android
    Adreno/Mali (GL `worker`) and on Chrome/Firefox WebGL2 (`parallel`);
  - Metal on Apple silicon and D3D11 on Windows;
  - the warm-draw benefit on hardware drivers.

### Editor UX (human)

- **R25. Status bar.** On the left: "Compiling shaders N", with the warm-up's "Warming up
  shaders N/M" while it runs. The tooltip gives the mode, pending shaders, pipelines and warm
  draws, and the last frame's main-thread ms. It is hidden when idle.
- **R26. Project Settings > Graphics > Shader Creation:**
  - **Asynchronous Shader Creation** (bool, on);
  - **Main Thread Budget (ms)** (0.5-16, 2);
  - **GL Warm Draws** (bool, on);
  - **Shader Warm-up**: **Record During Play** (bool, off), **Include in Builds** (on), **Hold
    First Scene** (on); a read-only entry count; a **Clear** button.

  Every row is searchable (S25) and undoable like the other project settings. A command
  "Clear Shader Warm-up List" is in the S25 CommandCatalog.
- **R27.** The material Inspector's existing "Compiling..." status (S11) stays until the GPU
  object is ready, not only until the source is built.

### Agent UX (MCP tools, Lua API, CLI)

- **R28. MCP.** `get_render_stats` (and `jasper://stats`) gain `shader_creation`: `mode`,
  `pending_shaders`, `pending_pipelines`, `pending_warm_draws`, `created_shaders`,
  `created_pipelines`, `interned_pipelines`, `last_frame_main_ms`, `max_main_ms`, and `warmup`
  {`entries`, `ready`, `recording`}.
  - No new tool: the settings are `set_project_settings` rows (`shader_creation.*`), and clearing
    the list is `run_command`.
  - `docs/mcp/guide/shaders.md` gets the section; the agent guide gets at most one short line,
    within its 16,384-byte limit.
- **R29. Lua.**
  - `ShaderPool.getPendingPipelineCount()` and `ShaderPool.isGpuCreationIdle()`;
  - `ShaderWarmup` (`isDone`, `getProgress`, `warmUp`, `getEntryCount`);
  - the existing warm-up calls keep their names and now mean "GPU-ready".
- **R30. CLI.** `--record-shader-warmup <file>` (development builds and the editor's play
  runner). `benchmark` output gains `shader_creation` (max main ms, max frame ms).
- **R31. S15.**
  - Scopes `GPU create submit`, `GPU create publish`, `GPU pipeline create` (main) and
    `GPU create worker` (the worker thread, named "GPU Create Worker").
  - Monitors "Shaders pending" and "Pipelines pending".

### Data & compatibility

- **R32.** Project YAML gets a `shaderCreation:` block (S08 `StructDesc`, every field
  `OmitIfDefault`): older projects load with the defaults and re-save unchanged.
  `ShaderWarmup.json` has `"version": 1`. An unknown version is ignored with a warning, and a
  malformed entry is skipped (counted).
- **R33.** The scene and shader formats, `.sdat` and the shader keys are unchanged. Exports
  without a `warmup.json` warm nothing.

## 6. Design constraints & decisions

### 6.1 Layers

```
ShaderPool / ShaderRender / ObjectRender / ShaderWarmup        (engine/core)
        │ createShader, endLoad, beginDraw, isReady
SokolShader / SokolObject / SokolPipelineCache (interning)     (engine/renders/sokol)
        │
SokolAsyncCreate  — queue, worker thread, budget, stats, warm-draw jobs, settings
        │ sgj_* C API
sokol_async.h / sokol_async_impl.h  — prepare / create / publish per backend, GL issue/poll,
        │                              warm draw, VkPipelineCache, Metal completion handlers
sokol_gfx.h  — 3 small patches (marked)
```

`sokol_async_impl.h` is included by `sokol.cpp` after `sokol_gfx.h` (as `sokol_gpu_timer_impl.h`
is), so it can call sokol's private backend functions in the same translation unit. The creation
is split as sokol already splits it internally:
- `sgj_shader_prepare` (main): defaults, validation, `_sg_shader_common_init`. The slot stays
  `ALLOC`.
- `sgj_shader_create` (thread allowed by the backend): `_sg_create_shader`.
- `sgj_shader_publish` (main): the slot state, sokol's stats and the `init_shader` trace hook
  (S15's pass recorder sees it).
- `sgj_pipeline_*` are the same for pipelines.

Main-thread sokol code never reads an `ALLOC` slot's backend fields. The worker writes only
the slot it was given. The slot is never freed while a job holds it (R6).

```c
// sokol_async.h (C API, sgj_ prefix like the GPU timer's)
typedef enum sgj_async_mode { SGJ_ASYNC_SYNC, SGJ_ASYNC_GL_WORKER, SGJ_ASYNC_GL_PARALLEL,
    SGJ_ASYNC_GL_DEFERRED, SGJ_ASYNC_THREAD /* vulkan, d3d11 */, SGJ_ASYNC_METAL } sgj_async_mode;
bool sgj_shader_prepare(sg_shader shd, const sg_shader_desc* desc_def);
sg_resource_state sgj_shader_create(sg_shader shd, const sg_shader_desc* desc_def);
void sgj_shader_publish(sg_shader shd, const sg_shader_desc* desc_def, sg_resource_state state);
// the GL main-context path
bool sgj_gl_shader_issue(sg_shader shd, const sg_shader_desc* desc_def);   // compile+link calls only
bool sgj_gl_shader_complete(sg_shader shd);                               // KHR completion (non-blocking)
// same trio for pipelines; sgj_gl_warm_draw(...) on the worker context; sgj_vk_pipeline_cache_*;
// sgj_mtl_shader_begin/_ready, sgj_mtl_pipeline_begin/_ready
```

The patches in `sokol_gfx.h`:
1. **GL** `_sg_gl_create_shader`: a program that sokol_async already issued
   (`shd->gl.prog` set) is adopted. Its compile and link are skipped, while the link-status
   check and the uniform resolution run as before.
2. **Vulkan**: `vkCreate{Graphics,Compute}Pipelines` take `_sgj_vk_pipeline_cache` (declared
   in the patch, `VK_NULL_HANDLE` unless sokol_async set it) instead of `VK_NULL_HANDLE`.
3. **Metal**:
   - `_sg_mtl_create_shader_func` takes a library sokol_async compiled ahead, when there is one.
   - `_sg_mtl_create_pipeline` either takes a render pipeline state sokol_async created ahead,
     or, in the capture phase, hands its descriptor to sokol_async and returns early.

### 6.2 SokolAsyncCreate

```cpp
// engine/renders/sokol/SokolAsyncCreate.h
struct ShaderCreationSettings {        // S08-reflected (editor) and set by the exported main
    bool asyncCreation = true;
    float mainThreadBudgetMs = 2.0f;
    bool glWarmDraws = true;     // desktop GL (GPU campaign); GLES and software GL never
};
struct AsyncCreateStats { const char* mode; size_t pendingShaders, pendingPipelines, pendingWarmDraws;
    uint64_t createdShaders, createdPipelines; size_t internedPipelines; double lastFrameMainMs, maxMainMs; };
class SokolAsyncCreate {
public:
    static void setup();            // SokolSystem::setup: mode, worker thread, worker context
    static void shutdown();         // before sg_shutdown
    static sg_shader makeShader(const sg_shader_desc& desc);              // pending handle
    static void initShader(sg_shader shd, const sg_shader_desc& desc);    // SokolCmdQueue MAKE_SHADER
    static sg_pipeline makePipeline(const sg_pipeline_desc& desc);
    static void initPipeline(sg_pipeline pip, const sg_pipeline_desc& desc);
    static void destroyShader(sg_shader shd);
    static void destroyPipeline(sg_pipeline pip);
    static bool isPending(sg_shader shd);
    static bool isPending(sg_pipeline pip);
    static uint64_t requestWarmDraw(sg_pipeline pip, const sg_bindings& bindings);  // GL worker; 0 = none needed
    static bool isWarm(uint64_t warmKey);
    static void update();           // once a frame, main thread, before the scenes draw
    static bool waitIdle(double timeoutMs);   // tests, benchmark start, shutdown
    static bool isIdle();
    static AsyncCreateStats getStats();
    static void setSettings(const ShaderCreationSettings& settings);
};
```

- **Descriptions are deep-copied** at submit: sources, bytecode, entry names, attribute,
  uniform and pair names, labels. The job owns them, so callers may free theirs
  (`ShaderRender::cleanupShader` keeps releasing the source).
- **One worker thread** ("GPU Create Worker", `std::thread`, not an S06 job). GL needs a
  thread with a current context for its lifetime. Vulkan and D3D11 creations then run one after
  another, which suits driver caches (OpenEngine's compile gate, §4). Jobs that share a shader
  run in submission order. The thread sleeps on a condition variable.
- **Queue order**: shaders first, then pipelines whose shader is ready, then warm draws. A
  pipeline submitted for a pending shader waits in the queue.
- **Publish** happens in `update()` in completion order, within the budget (R5), measured with
  the steady clock (also without the profiler) and scoped for S15.
- **Platform worker context**: `GpuWorkerContextFactory` (`engine/core/render/GpuWorkerContext.h`),
  set by each platform before `systemViewLoaded`. It is called on the main thread with the main
  context current, and returns an object with `makeCurrent()`/`release()` for the worker
  thread, or null (no worker: `parallel`/`deferred`). The editor backends (Linux/Windows/macOS),
  the game platforms (Linux/Windows/macOS GL/Android) and the test `HeadlessContext` implement
  it.

### 6.3 GL family

- **worker**: the worker thread makes its context current once.
  - Shader: `sgj_shader_create` (sokol's own `_sg_gl_create_shader`, link status and uniform
    locations included), then `glFinish`. The program is shared, and the finish makes it
    complete before the main context uses it.
  - Warm draw: the job carries what the main thread resolved at request time, plain GL names
    and values, so the worker never reads sokol's pools:
    - the program;
    - the pipeline's attributes (size, type, normalized, stride, offset, divisor);
    - primitive type;
    - depth, stencil, blend, colour mask, cull and winding state;
    - colour/depth formats and sample count;
    - per texture unit, the texture's target, pixel format and sample count and the sampler's
      state. The worker binds its own 1x1 texture and sampler of those (cached), never the
      render thread's objects, which that thread may respecify or delete meanwhile.
  - The worker binds them to its own VAO, a dummy vertex buffer and a cached 1x1 FBO per
    format set. It sets the state, draws 3 vertices (1 instance) and calls `glFinish`.
  - The key is (program, pipeline description hash, per-unit texture internal format and
    target, sampler state hash), and a done key is remembered.
  - A program deleted meanwhile only makes that warm draw useless: GL errors on the worker
    context are read and dropped.
  - The driver makes a program's variant for a state at the draw, into a list the contexts
    share. So a warm draw of a program and the render thread's first draw of it with another
    warmed state never overlap (`SokolAsyncCreate::beginFirstDraw`): that first draw waits
    while the worker draws the program, and a program first drawn in a frame is not warmed
    until the next `update()`.
  - **On by default on desktop GL since the GPU campaign (off on GLES until a device run), and
    never on a software GL** (`sgj_async_caps.gl_software`: llvmpipe,
    softpipe, SwiftShader; `JASPER_GL_WARM_DRAWS=1` overrides for driver investigations). On
    llvmpipe the render thread crashed in the driver drawing programs the worker had drawn
    (`AtmosphereGpu.ScreenCloudsHideFarTerrain`, deterministic). Isolated on D: with the
    worker's `glDrawArraysInstanced` skipped (all else the same) it passes; it still crashes
    with the worker's own textures, with no overlap of the two contexts on a program, and with
    only programs the render thread never drew warmed. So the render thread reuses what the
    worker's draw made in the driver, which llvmpipe does not support. Without warm draws a
    program's first draw with a new state compiles on the render thread where the driver
    compiles at draw time (llvmpipe: measured in §7 AC13). Warm draws stay for the GPU campaign
    (§8).
- **parallel**: `sgj_gl_shader_issue` on the main context (the compile and link calls only,
  shader objects kept for the log). On a hardware GL the mode first sets
  `glMaxShaderCompilerThreadsKHR(0xFFFFFFFF)` (the extension leaves the initial count to the driver;
  NVIDIA's is none, so without it the link blocks and the status is always complete; GPU campaign).
  Not on a software GL: llvmpipe keeps its at-once completion, the CI reference. `update()` polls `GL_COMPLETION_STATUS_KHR`, which does not
  block. When it is true, `sgj_shader_create` adopts the program (patch 1) and checks the link
  status (no wait now). On failure the shader logs are read. There are no warm draws.
- **deferred**: as parallel, but the program counts as "complete" 2 frames after the issue. At
  most one deferred program is finished per frame, so an unexpected wait does not pile up.
- **Mode choice (`Auto`)**: `worker` when the factory gives a context (never on a software GL,
  deviation 6), else `parallel` when the extension is listed, else `deferred`. `worker` is first because it moves what
  `KHR_parallel_shader_compile` moves *and* what it leaves on the calling thread (Mesa's GLSL
  linker; measured, §3), and only it can do warm draws. The worker also issues the queued
  programs together when the extension is present, so a driver with compile threads overlaps
  them.

### 6.4 Stand-ins and readiness (R8, R9)

- `RenderSystem::loadMesh` generalises the S11 stand-in (`builtinStandIn` becomes `standIn`).
  For the main, depth and G-buffer shader of a submesh, a pending variant takes the first
  candidate of `getFallbackProperties(type, properties)` with `ShaderPool::isShaderReady` (the
  same customId first, then 0 for a `builtin:lit` reference). Then the S11 plain-Lit rule
  applies. `mesh.needVariantReload` makes the exact variant replace it once ready.
- A loaded object switches variants without an empty frame: once the new variant's shaders are
  built (`meshVariantShadersReady`), each of its objects (colour, depth, G-buffer, extra pass)
  makes its pipelines on the new shader ahead (`ObjectRender::prewarmWith`: its own description,
  created where the backend creates pipelines) and warms them with its bindings. The reload
  waits for those, and its pipelines are then the same interned ones, ready at once. A stand-in
  that dropped a vertex attribute (Nmp, Tan, Uv1, Uv2) has another layout, so its exact
  variant's pipelines are new and that object can miss frames until they are made.
- A draw whose pipeline (or shader) is still being created or warmed draws nothing and waits:
  `ObjectRender::isDrawWaiting()` tells it from a failed or dead pipeline, and only the latter
  reloads the object. Passes whose result is kept count the waiting draws
  (`ObjectRender::getWaitingDrawCount`): a cached shadow slot is drawn again the next frame, a
  reflection probe face is captured again.
- `ObjectRender::isReady()` is true when every pipeline of the object is `VALID` and warm.
  - The sites that swap a pass at runtime (uber/post chain, bloom programs, SSAO,
    atmosphere/fog, HTML UI composite) build the new object next to the current one and swap
    when it is ready.
  - A pass that consumes another (uber reading bloom) switches its variant only once its inputs
    have drawn.
  - The audit list is in the Progress section.
- HTML UI warms the geometry, gradient and primitive pipelines for every `BlendMode` at the
  document's target formats when its renderer loads, and the document counts as loaded after
  that. Rarer states (stencil clip levels, filters) are created on first use and skipped
  until ready.

### 6.5 Interning (R3)

- `SokolPipelineCache::acquire(const sg_pipeline_desc&)` / `release(sg_pipeline)`. The hash
  and equality are computed field by field (never raw struct bytes: padding).
- An entry whose count reaches 0 is destroyed by the next `collect()` round but one (once a
  frame, from `SokolAsyncCreate::update`), unless it is acquired again meanwhile or the warm-up
  holds it: an object loaded again in the same frame keeps its pipeline.
- `releaseShader` destroys every entry on that shader, as it does for the dynamic entries
  today. An object that keeps such a handle sees an invalid pipeline and does not draw until it
  reloads (today's rule, `SokolObject.cpp:625`).
- The live-pipeline counter (`ObjectRender::getLivePipelineCount`) keeps counting acquisitions.

### 6.6 Warm-up list format (R10)

```json
{ "version": 1, "engine": "<JASPERENGINE version>",
  "shaders": [ {"key": "0x10000001f", "custom": ""}, {"key": "...", "custom": "shaders/myMesh"} ],
  "pipelines": [ {"shader": 0, "primitive": "triangles", "index": "uint16", "cull": "back",
                  "winding": "ccw", "samples": 1, "colors": [{"format": "rgba8", "blend": {...}, "mask": "rgba"}],
                  "depth": {"format": "depth_stencil", "compare": "less_equal", "write": true, "bias": [0,0,0]},
                  "stencil": null, "layout": {"buffers": [{"stride": 48}], "attrs": [{"buffer": 0, "offset": 0, "format": "float3"}]} } ] }
```

- `pipelines[].shader` indexes `shaders`.
- Tokens are sokol's enum names. `default` formats stay `default` (the swapchain's, resolved
  at creation), so a list is portable between machines.
- Entries recorded on GL replay on Vulkan/Metal/D3D11 with both windings for an
  offscreen-format target, because GL flips the winding of offscreen pipelines
  (`SokolObject::makePipelineDesc`).

### 6.7 Fixtures

- `tools/fixtures/async-shaders/` holds a small project with two scenes:
  - `Variants`: 12 lit spheres. A script toggles a different lobe or feature on a sphere every
    60 frames, so every toggle is a never-built variant.
  - `Warmup`: the same materials, with a recorded `ProjectSettings/ShaderWarmup.json`.
- The fixture drives the E benchmark (`tools/perf/async_shaders_bench.py`) and the MCP tests.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R5, R6) | Core GPU tests, GL (Headless worker context) and Vulkan, `AsyncCreate.*`: `ShaderPendingThenReady` (handle pending at once; ready after `waitIdle`; `ShaderPool` state Loading→Finished); `DestroyWhilePending` (no crash, slot freed after, ASan clean); `ShutdownWithJobsInFlight`; `BudgetPublishesAtLeastOne` (0.01 ms budget still publishes one per update). |
| AC2 (R7) | `AsyncCreate.FailedCompileIsFailed` on each GL mode and Vulkan: a shader with a GLSL error ends FAILED, the log names the error line, `ShaderPool::isShaderBuildFailed` is true, and a custom fork falls back to the built-in variant (existing fallback test re-run with async on). |
| AC3 (R14) | `AsyncCreate.GlModes`: `JASPER_ASYNC_SHADERS=worker`, `parallel`, `deferred`, `sync` each render the `RenderGpu.LitCube` scene identical to its existing golden once idle. The mode reported equals the forced one (llvmpipe exposes the extension; Headless supplies a worker). |
| AC4 (R4) | `AsyncCreate.WarmDrawRunsOnWorker`: an object on GL worker is not drawn until its warm key is done; the warm draw ran on the worker thread (counter); the main context's first draw then succeeds. Timing (the JIT moved) is checked by AC13, not by a unit test. |
| AC5 (R3) | `PipelineIntern.SameDescShares`, `DifferentStateDiffers` (every field toggled once changes the hash/equality), `RefCountDestroysAtZero`, `ReleaseShaderDestroysEntries`; `RenderGpu.*` goldens unchanged with interning on (whole GPU suite). |
| AC6 (R8) | `RenderGpu.StandInWhilePending`: a mesh whose variant (normal map) is held pending (test hook) draws its ready fallback (no normal map), matching golden `async_standin.png`; after release the frame equals the exact variant's golden `async_exact.png`; never an empty frame. MCP `test_async_shaders.py::test_lobe_toggle_stand_in`: toggling clear coat on in the editor keeps every screenshot non-empty and the final one equal (±1%) to a synchronous run's. |
| AC7 (R8 passes) | MCP `test_async_shaders.py::test_post_variant_switch`: enabling bloom and a LUT on Vulkan and GL with a forced slow creation (`JASPER_ASYNC_SHADERS_DELAY_MS=300`) — every frame's screenshot has no NaN/garbage (mean luminance within 10% of the previous frame until the switch), the switch happens once, final frame equals the synchronous run's (±1%). |
| AC8 (R9) | `test_async_shaders.py::test_ready_waits`: with `JASPER_ASYNC_SHADERS_DELAY_MS=500` the READY signal comes after `pending_*` reach 0, and the first screenshot after READY equals the final one. Smoke test passes on GL, GLES3 and Vulkan. |
| AC9 (R10, R11) | `ShaderWarmup.RoundTrip` (save/load equal, unknown version ignored, malformed entry skipped and counted); MCP `test_async_shaders.py::test_record_and_export`: Record During Play on, play the Variants scene 400 frames, stop → the list has the toggled variants and their pipelines; export (source, linux) writes `shaders/warmup.json` with those keys and the `.sdat`s; with Include in Builds off the file is absent. |
| AC10 (R12) | `ShaderWarmup.ReplayHoldsPipelines` (GPU test): replay a list → the pipelines exist in the intern table; an object created after with that state is ready at its first `endLoad`. Smoke variant `SMOKE_GAME_RUNS=warmup` on the exported Warmup scene: the game log has `[gpu] warm-up done N/N`, and the first frame after READY has no pending creation. |
| AC11 (R13) | `VkPipelineCache.SaveLoadValidates` (Vulkan GPU test): a saved cache reloads; a header with another UUID, a truncated file and a bad checksum are each ignored and replaced; the save is atomic (S03 `AtomicFile`). |
| AC12 (R19) | `AsyncCreateGpu.AShaderIsPendingUntilItsCreationIsPublished` and `APipelineWaitsForItsShaderAndIsShared` exercise patched creation; `PATCHES.md` lists the patches. The Metal and D3D11 paths compile in `tools/build.sh` cross targets where present and in the Xcode project check (`xcode_sync_*.py --check`); **Metal and D3D11 are runtime-unverified** until the GPU campaign. |
| AC13 (R21, R22) | E bench `tools/perf/async_shaders_bench.py` (fixture Variants scene, editor headless with MCP, 5 runs, GL worker and Vulkan): per frame the sum of the R31 main-thread scopes from `capture_profile` + `get_cpu_profile` over 600 frames (10 toggles); the median-of-runs of the per-run max ≤ 4 ms; worst frame ≤ max(2× idle median, idle median + 4 ms) with the driver cache warm (R22). The same script with `--sync` records the pre-S32 numbers (red control). Thresholds in `tools/perf/bench-thresholds.json`. **R22's method (perf2):** the captures record windows with gaps, so the worst frame is taken over every frame from the first toggle to the last (`get_frame_times` read every 0.25 s, its 600-frame ring), leaving out the frames flagged `tool` (`FrameTimes::FLAG_HOST_WORK`: an MCP tool ran in them, the edit or the bench's own calls); `r22_worst_frame_ratio` is that over the idle median, `edit_worst_frame_ms` / `tool_worst_frame_ms` the edit's frames apart, `worker_worst_frame_ms` the frames the worker created in. Controls: `--control` (the same edits between variants built before), `--idle` (the same recording with no edit: the editor's own noise, R22's floor), `--driver-cache <dir>` (the drivers' disk caches on, runs after the first warm). |
| AC14 (R23) | `AsyncCreateBench.IdleUpdate` ≤ 2 µs, `AsyncCreateBench.InternLookup` ≤ 1 µs (E, median of 5). |
| AC15 (R24) | The bench's `--warmup` run: replaying the Warmup list holds no frame above the R22 bound and ends within 1.5× the synchronous creation time of the same list. |
| AC16 (R25-R27) | Editor checklist `docs/specs/checklists/S32.md`, run on an isolated editor with screenshots: status bar shows "Compiling shaders N" during a toggle and hides after; Project Settings rows present, searchable ("warm-up"), undoable; Clear empties the list; Inspector "Compiling..." until GPU-ready. `test_async_shaders.py::test_settings_rows` sets each row over `set_project_settings` and reads it back. |
| AC17 (R28, R29, R31) | MCP: `get_render_stats.shader_creation` fields present with types; `test_protocol.py`, `test_tool_budget.py`, `test_docs.py` (guide ≤ 16,384 bytes, core listing < 100k, `docs/mcp/guide/shaders.md` section linked); TOOLS.md regenerated. Lua: `test_async_shaders.py::test_lua_api` calls the R29 functions in Play. `capture_profile` shows the four scopes and the worker lane "GPU Create Worker". |
| AC18 (R18) | Web gate (`tools/build.sh web-gate`): the sample page renders as before; the log line names `parallel` (Chrome headless) or `deferred`. |
| AC19 (Android) | Android emulator check (A): the emulator smoke renders the sample scene; log names `worker` (EGL shared context) or the fallback and why. |
| AC20 (R20, R32, R33) | `ShaderCreationSettings` reflection round trip; a pre-S32 project loads and re-saves byte-identical; `asyncCreation: false` renders every `RenderGpu.*` golden identical and logs `sync`. |
| AC21 (gate) | Linux WERROR GL and Vulkan, web, android builds; core and editor unit tests also under ASan+UBSan; MCP `MCP_SUITE=all -j 2` subset (async_shaders, materials_pbr, profiler, protocol, budget, docs, conventions, groups, html_ui_render); htmlui-tests; smoke; `xcode_sync_*.py --check`. |

## 8. Dependencies & conflicts

- **Depends on (landed):**
  - S06 (jobs: the editor's `shader-build` job feeds R1, and `jobs::Tag::Asset` readiness);
  - S15 (scopes, monitors, `capture_profile`, `get_cpu_profile`, `get_render_stats`);
  - S08 (`StructDesc` for `ShaderCreationSettings`);
  - S25 (SettingsRegistry rows, CommandCatalog command);
  - S31 (MCP conventions);
  - S03 (`AtomicFile` for the pipeline cache and the list);
  - S10/S11/S12 (variant keys, `builtin:lit`, `getFallbackProperties`);
  - S16/S17 (HTML UI renderer);
  - S20 (particle renders).
- **S30 (shipping, in parallel).** Export hooks, kept small:
  1. `Exporter::resolveShaderKeys` adds the list's keys when Include in Builds is on.
  2. `Exporter::buildAndSaveShaders` writes `shaders/warmup.json` (new private helper
     `writeShaderWarmup`). S30's prebuilt path reuses `buildAndSaveShaders` unchanged, so it
     ships the file with no S30 change. Its `writeContentManifest` lists `shaders/` as a whole.
  3. The generated `main` calls `Engine::setShaderCreationSettings(...)` only when it differs
     from the defaults (as the threading settings do). S30's `writeGameConfig` should carry the
     same three values for the prebuilt player (`shaderCreation` in `game.json`). Until it
     does, a prebuilt player runs with the defaults, which is correct, just not customised.
     Noted for the integrator.
- **Merge hotspots:**
  - `engine/renders/sokol/SokolShader.cpp`, `SokolObject.cpp`, `SokolPipelineCache.*`,
    `SokolCmdQueue.cpp`, `SokolSystem.cpp`;
  - `engine/core/subsystem/RenderSystem.cpp` (`loadMesh` stand-ins; pass swap sites);
  - `engine/core/pool/ShaderPool.cpp`, `engine/core/Engine.cpp` (frame loop, readiness);
  - `engine/libs/sokol/sokol_gfx.h` + `PATCHES.md` (anyone updating sokol);
  - the platform files (`engine/platform/*/`, `editor/backend/Backend*.cpp`);
  - `editor/Exporter.cpp` (S30);
  - `editor/settings/SettingsRows.cpp`, `editor/Generator.cpp`;
  - `editor/mcp/tools/ProfilerTools.cpp`, `editor/mcp/McpInstructions.cpp` (guide bytes);
  - `docs/mcp/TOOLS.md` (regenerated, never hand-merged).

## 9. Risks & defaults

- **Shared GL contexts on some Android drivers** have had bugs (old Mali/PowerVR). Default: if
  the worker context fails to create or to make current, log why and fall back to
  `parallel`/`deferred`. A creation the worker reports as failed while the same source compiles
  on the main context marks the worker unreliable for the session. Every later program then
  goes through the main-context path (logged once).
- **Warm draws depend on the driver's cache sharing.** On Mesa they work through the disk cache
  (the default). With `MESA_SHADER_CACHE_DISABLE` the main context JITs again, and the stall
  returns at the first draw only. On hardware drivers the program's compiled state is shared in
  the share group. Default: warm draws on, measured in the GPU campaign. Off (setting), objects
  draw as soon as their program is linked.
- **One frame of pop-in** for content whose pipeline is new: a mesh appears one or more frames
  late instead of stalling the frame. Interning and the warm-up list make this rare, and the
  readiness gates hide it behind loading. This is the documented trade-off (Unity's async
  shader compilation and Unreal's PSO precache skip draws the same way).
- **Lavapipe** may still JIT at the first draw (a software driver). Vulkan's contract is that
  `vkCreate*Pipelines` compiles fully, so R22 on Vulkan is measured on lavapipe and repeated in
  the GPU campaign. If lavapipe misses R22 only because of a draw-time JIT outside our
  scopes, R21 still holds, and the deviation is recorded with numbers.
- **`deferred` can still wait** in the finishing frame on a driver that compiles nothing in the
  background. It is bounded to one program per frame, and the warm-up list moves it to load.
- **Program binaries** (`GL_ARB_get_program_binary`) are not used. The driver caches cover
  restarts, and binaries are device-specific (they cannot ship). This may be revisited after the
  GPU campaign.
- **Metal and D3D11 cannot run here.** Both paths stay close to sokol's own code (the
  same functions, split), they compile where a compiler is available, and they are listed as
  runtime-unverified.

## Deviations

1. **Passes create their pipelines at once** (§6.4 refined). The spec asked the sites that swap a
   pass at runtime to build the new object next to the current one and swap once it is ready.
   Instead, every pass object of the frame is marked `ObjectRender::setImmediatePipelines(true)`:
   post-processing (uber, bloom, the effect chain), SSAO, SSR, the scene colour copy, the
   composites and blits, the atmosphere, fluid, the shadow cache restore and atlas view, the 2D
   shadow casters, the probe filter and the HTML UI composite. Such an object's pipelines are
   created on the render thread (an interned one still being created is waited for) and drawn
   without a warm draw.
   - Why: a pass is consumed by the next one (the uber pass samples bloom, the composite samples
     the scene). Skipping one for a frame leaves an undefined target, which Vulkan may show as
     garbage, so readiness would have to be threaded through every consumer.
   - Their shaders stay asynchronous, and the existing "keep the current variant until the new
     one is built" logic (uber variants, post colour, the shadow tier) covers them.
   - The cost is one pipeline compile on the render thread the first time a pass's state appears
     on a device: on Vulkan and Metal only, since GL pipelines are state. The VkPipelineCache
     saved per device and the warm-up list make it a cache hit afterwards.
   - Content (meshes, particles, UI, HTML UI elements, points, lines, sky) stays fully
     asynchronous, as R2 says.
2. **HTML UI warms nothing at renderer load** (§6.4). Its pipelines depend on the layer target in
   use when it draws, which the renderer does not know when it loads. The global readiness gate
   (`Engine::areScenesLoaded` waits for `isGpuCreationIdle`) holds the READY signal and the
   loading state until its first frame's pipelines and warm draws are done. The warm-up list
   records HTML UI pipelines like any other.
3. **The warm-up list lives at `<project>/ShaderWarmup.json`**, not `ProjectSettings/`. Projects
   have no `ProjectSettings/` directory, and the list belongs next to `project.yaml` under version
   control.
4. **No checked-in benchmark fixture** (§6.7). `tools/perf/async_shaders_bench.py` builds its
   scene over MCP in a new project (twelve spheres; each toggle turns on a lobe set no sphere had,
   a never-built variant). It records each toggle with `capture_profile` and reads every frame's
   creation scopes with `get_cpu_profile`. The MCP tests make their scenes the same way.
5. **The editor does not replay the warm-up list** (R25's "Warming up shaders N/M"). The editor
   records the list. Games replay it (`ShaderWarmup::beginShipped` at view load), so the progress
   is the game's (`ShaderWarmup.getProgress()` in Lua). The status bar shows "Compiling shaders N".
6. **No worker context on a software GL; GL warm draws off by default** (R4, R5, §6.3).
   - Software GL (`sgj_async_caps.gl_software`: the renderer string names llvmpipe, softpipe,
     SwiftShader or "Software Rasterizer"): the mode is `parallel` (llvmpipe lists
     `KHR_parallel_shader_compile`) or `deferred`, never `worker`, even when
     `JASPER_ASYNC_SHADERS=worker` asks (a warning). `JASPER_GL_SOFTWARE_WORKER=1` allows the
     worker there for driver investigations.
   - Evidence on llvmpipe (D, Mesa in the linux image):
     - with warm draws on, the render thread crashed in `swrast_dri.so` (a read at 0x135) in a
       mesh draw of `AtmosphereGpu.ScreenCloudsHideFarTerrain`, every run, under ASan too;
     - with only the worker's `glDrawArraysInstanced` skipped, everything else equal, it passed
       (twice); it still crashed with the worker binding its own 1x1 textures and samplers, with
       no overlap of the two contexts on a program (`beginFirstDraw` gating every draw), and with
       only programs the render thread had never drawn warmed;
     - with warm draws off, the full core suite hung once in a render-thread draw
       (`EveryGlModeDrawsTheLitCube`, all llvmpipe threads idle, the main thread in a futex
       inside the driver), and a repeat run of the Async, Atmosphere and Render GPU suites hung
       again in its first iteration (killed at 40 min);
     - the §3 probe program (compile, link and draw on worker contexts sharing with the
       main one) never exited on B, C or D (found still running 25 h later and stopped);
     - without the worker context (`parallel`), the same repeat ran 3 iterations with no hang or
       crash, and the full core suite ran to the end (2344 tests).
   - Our synchronisation was checked for that hang: the worker calls `glFinish` after every
     program (stronger than a fence) and the render thread binds a program only after its
     creation is published; the worker binds no object of the render thread's (no textures,
     buffers or framebuffers; warm draws use its own) and deletes none; each context is current
     on one thread only (the worker's from its creation thread until `release()`); the worker
     never touches sokol's GL state cache (`_sg_gl_create_shader` reads the bound program with
     `glGetIntegerv` and restores it).
   - Warm draws need the worker, so they do not run on a software GL either. **GPU campaign
     (NVIDIA RTX 3080 Ti, driver 595.99, GLX):** the worker context and warm draws run without a
     crash or hang over every bench run, and a warm draw takes the driver's first-draw compile of
     a new variant (about 8 ms) off the render thread, so `shader_creation.gl_warm_draws` now
     defaults to **on** for desktop GL. GLES (Android EGL) keeps them off until an Adreno/Mali run
     (`JASPER_GL_WARM_DRAWS=1` allows them there); see Progress. So on llvmpipe the GLSL link and a program's first-draw compile stay on the render
     thread (spread by `parallel`'s completion polling); R21/R22 are measured on E with that
     cost included (Progress).
7. **A variant switch makes the new pipelines first** (R8, §6.4, added): the loaded object makes
   its pipelines on the new variant ahead (`ObjectRender::prewarmWith`) and loads again only
   once they are created (Vulkan, Metal) and warmed (GL with warm draws). Without it Vulkan
   showed an empty frame per switch (the stand-in rule covers shaders, not pipelines). Released
   interned pipelines live one more frame (`SokolPipelineCache::collect`) so the reload finds
   them.

## Progress (implementation notes, keep updated)

- **The first frame of a many-variant scene, measured (2026-11-02; `tools/fixtures/pbr-extended-export`,
  24 mesh variants, the smoke's editor stage).** The stall is the first draws' compile, in `SokolObject::draw` on
  the render thread, all in one frame's opaque draw loop (reports at frame 2, the same call site).
  | | First open (main thread stall) | With the driver's disk cache warm |
  |---|---|---|
  | llvmpipe (Mesa 25.2.8), `parallel`, host load 2-9 | 7.0-8.0 s (8 runs) | no hang (9 opens) |
  | the same, host load 15-30 | 7.8-9.5 s (4 runs) | |
  | llvmpipe, Mesa cache off | 7.0 s (link, `sgj_gl_issue`, frame 0), then 22.8 s (JIT) | |
  | RTX 3080 Ti GL, `worker`, driver cache on, empty | no hang (worst frame 1.1 s) | no hang (worst frame 170 ms) |
  | RTX 3080 Ti GL, `worker`, driver cache off | three stalls of 4.8-7.0 s | |

  So it is the work, not the load (the JIT runs on the render thread, whatever the other cores do). This is
  deviation 6 as designed, not a missed path: a software GL has no worker context, because llvmpipe crashed
  reusing what a worker's draw had made. On NVIDIA the worker's warm draw reaches the render thread through the
  driver's disk cache, on by default; with it off the recompile at the first draw (Progress, perf2) lands on
  the render thread, in one frame for every program of the scene. The smoke takes the llvmpipe case
  (`tools/ci/smoke_jit_hangs.py`, `docs/testing.md`). Not measured: Mesa's hardware drivers and Vulkan's editor here.

- **GPU campaign perf2 (2026-11-01; RTX 3080 Ti, driver 595.99.02; branch
  `feat/perf2-gpu-budgets`).** R22 re-measured over every frame (§7 AC13's perf2 method), medians of 5:

  | RTX 3080 Ti | R21 max_main_ms | R22 worst / idle (tool frames apart) | worst frames |
  |---|---|---|---|
  | GL `worker`, drivers' caches off (every run cold) | 0.016 | 676x | 755-828 ms (NVIDIA's recompile) |
  | GL `worker`, driver cache on (run 1 cold, 2-5 warm) | 0.016 | **1.69x** (second batch 2.06x) | runs 2-5: 1.9-2.8 ms |
  | the same with the compile-start delay off (before) | 0.014 | 9.06x | 9.3-10.8 ms every toggle |
  | GL, no edit at all (`--idle`, the noise floor) | 0 | 2.02x | 1.4-2.8 ms |
  | Vulkan `thread` | 0.24 | 3.7x | 2.8-4.4 ms |
  | Vulkan with the compile-start delay (not kept) | 0.22 | 3.0x, a second batch 3.7x | 2.5-3.8 ms |
  | Vulkan `--control` / `--idle` | 0 | 1.5x / 2.2x | |

  - **The campaign's GL R22 (16.4x, worst 18.7 ms) was wrong**: its capture windows missed NVIDIA's
    draw-time recompiles, 2 of ~0.75 s per new variant (fragment, then vertex; the driver's debug
    output names them). They happen on whichever context first draws a program; warm draws (on or
    off: the same), a warm draw state matched cap for cap (sRGB, multisample, seamless cubemaps,
    texture attachments), program binaries, `parallel` (one ~0.75 s stall at the first draw) and
    `__GL_THREADED_OPTIMIZATIONS` do not avoid them; `sync` pays them inside its 2.1 s link. With
    the driver's disk cache (on by default) a variant compiled once does not recompile.
  - **Fixed: the ~10 ms worker stall.** A compile or warm draw the GL worker starts within ~50 ms
    of its request held the render thread's next GL calls for 8-10 ms; started 60 ms after the
    request it holds nothing (5 and 20 ms only moved it). `SokolAsyncCreate` waits until 60 ms
    after the request (`waitCompileStart`; GL `worker` on a hardware GL; on Vulkan the same wait
    changed nothing measurable, so it is off there; `JASPER_GPU_COMPILE_DELAY_MS` overrides):
    9.06x -> 1.69x on GL.
  - **The edit's own cost is apart:** frames that ran an MCP tool carry `FLAG_HOST_WORK`
    (`get_frame_times` `tool`), 2.0-2.8 ms on GL and 3.7 ms on Vulkan; the old control's 8.7x
    was the edit (no creation).
  - **Vulkan's 3.7x:** not creation work on the render thread (R21 0.22 ms; no pipeline
    creation and no creation scope in the worst frames). The mean frame is the same with the worker
    creating (idle 0.28 / 0.47 / 0.05 ms for Scene Views / Editor UI / the editor's render, the same
    within 0.01 ms in the 2,600 frames the worker created in); a few frames of each run take 2.5-3.8
    ms of main-thread work in the editor's own Vulkan render and the scene views while a pipeline
    compiles on the worker (the driver; a niced worker thread changes nothing), against a 2.2x noise
    floor with no edit. Under R22 as approved (idle + 4 ms) it is met.
  - Remaining: on GL with the cache warm, the control's first toggle still holds one ~9 ms frame in
    the editor's own GL render while the worker loads a program from the driver cache (the delay
    and warm draws off do not change it).

- **State at e0746f700+ (merged oe-port a57c7844d):** everything in §5 is implemented; the
  deviations above record what changed. Evidence so far:
  - builds: linux GL WERROR (D, B), Vulkan WERROR (B), web (B), android (A);
  - unit tests, GL (D, llvmpipe, parallel mode): full core suite 2344 ran, 2 failed, both fixed
    since (AStandIn: the test delay now holds parallel creations; WaterWavesGpu waited for no
    creation); editor 1379/1379;
  - unit tests, Vulkan (B, lavapipe): the S32 filter (Async, ObjectRenderShader, Render,
    Atmosphere, Shadow, Probe suites) 65/65, editor 13/13; before the waiting-draw fix every
    RenderGpu test failed with PIPELINE_POOL_EXHAUSTED (the red);
  - red-before-green: AtmosphereGpu.LightShafts (cached shadow slots), RenderGpu.LitCube on
    Vulkan (reload storm), AsyncCreateGpu.ThePipelineCacheIsSavedAndValidated (shutdown wrote
    over the file it validated), WaterWavesGpu.MatchesTheCpuTwin, MCP lobe toggle (6 empty
    frames);
  - xcode_sync_*.py --check: all pass after the S21 merge.
- **E benches** (`tools/perf/async_shaders_bench.py --runs 5`, an idle E, Mesa's disk cache off,
  each lobe toggle recorded from before the change until the sphere draws its new variant;
  medians of 5 runs; idle frame 114 ms on GL, 55 ms on Vulkan):

  | E (software drivers) | R21 max_main_ms (<= 4) | R22 worst_frame_ratio (<= 2) |
  |---|---|---|
  | GL llvmpipe, `parallel` (no worker, deviation 6) | 385.7 FAIL | 10.0 FAIL |
  | GL llvmpipe, `sync` (red control) | 375.5 | 12.9 |
  | Vulkan lavapipe, `thread` | **0.105 ok** | 16.0 FAIL |
  | Vulkan lavapipe, `sync` (red control) | 298.4 | 21.3 |

  - Vulkan: the pipeline compile leaves the render thread (0.1 ms against 298 ms).
  - GL on llvmpipe: `KHR_parallel_shader_compile` is listed, but the link still runs at
    `glLinkProgram` on the calling thread (the "GPU create submit" scope, 386 ms); the worker
    that would move it is off there (deviation 6).
  - R22 fails on both software drivers for the same reason: the worst frame (1.1 s GL, 0.83 s
    Vulkan) is the first draw with the new variant ("Draw" on GL, "Editor Render", the
    submission, on Vulkan): llvmpipe and lavapipe compile the shader for the draw state at
    draw time, which no creation API moves. Warm draws were meant for that on GL (deviation 6).
  - So R21 is met on Vulkan only and R22 nowhere on E. The GPU campaign measures both on GPU
    drivers (§8); the budgets in `tools/perf/bench-thresholds.json` stay as the spec sets them.
- **Gate at the merged commit (oe-port b620283cd, 0ece0d497 and test-only commits after):**
  - builds: linux GL WERROR and Vulkan WERROR (G), web + browser gate PASS (A), android (A);
  - units: GL full suite 2346 ran, 1 failed (AtomicFileBench.CacheWriteNearPlainWrite, an I/O
    timing bench on the loaded G; passes re-run), editor 1381/1381; Vulkan S32 suites 66 ran,
    1 failed (WaterWavesGpu: its one-off probe pass now creates its pipeline at once; 9/9 after);
  - ASan+UBSan, S32 suites: 67 core + 15 editor tests, no report (G, before the BF7 merge);
  - smoke GL, GLES3, Vulkan (G host) and the Android emulator render gate (lit3d, ui2d,
    htmlui with goldens, A): pass, at the commit before the BF7 merge (BF7 touched none of S32's
    files); xcode_sync_*.py --check: all pass after it;
  - full MCP (MCP_SUITE=all, JASPER_SLOW_BUILD=1, G): 1041 passed, 9 failed, 2 skipped. 7 are
    test_pose_runtime.py, which fails the same way without S32 (S30's runs); 2 are
    test_reflection.py's resave/codegen of the 2D scene's soft particle points (now transparent
    once loaded: goldens updated, the file re-run on its own). test_async_shaders.py: 5/5.
- **For the GPU campaign (§8):** GL worker mode and warm draws on real drivers (Linux Mesa
  radeonsi/iris, NVIDIA, Windows WGL, macOS, Android EGL); R21/R22 on GPU drivers; Metal and
  D3D11 creation at runtime (they compile here only by reading: not built on the servers).
- **GPU campaign (2026-10-31; RTX 3080 Ti, driver 595.99.02, GLX on a headless Xorg and Vulkan;
  docs/perf/gpu-campaign.md).** `tools/perf/async_shaders_bench.py --runs 5` (10 toggles; the
  drivers' disk caches off; the editor focused and its vsync off, so the idle frame is the real
  1.1 ms and not a 16.7 ms pacing period), medians of 5:

  | RTX 3080 Ti | R21 max_main_ms (<= 4) | R22 worst_frame_ratio (<= 2) | worst frame |
  |---|---|---|---|
  | GL `worker`, warm draws on (the new default) | **0.014 ok** | 16.4 FAIL | 11-18.7 ms |
  | GL `worker`, warm draws off | 0.015 ok | 15.7 FAIL | 12-18.5 ms |
  | GL `parallel` (after the compile-thread fix) | 8.83 FAIL | 10.6 FAIL | 11-25 ms |
  | GL `sync` (red control) | 2192 | 1477 | 2.2 s |
  | GL `worker`, the same edits between two built variants (no creation: control) | 0.011 | 8.7 | 9.8-10.7 ms |
  | Vulkan `thread` | **0.227 ok** | 3.9 FAIL | 3.7 ms (one run 10.2) |
  | Vulkan `sync` (red control) | 799 | 838 | 0.8 s |

  - The worker context and warm draws work on NVIDIA: no crash or hang in any run; the mode is
    `worker` (R5's Auto). A Lit lobe variant's cold compile and link is **2.1 s** on this driver
    (llvmpipe: 0.39 s); on the worker the render thread pays 0.01 ms for it.
  - **Fixed: `parallel` did nothing on NVIDIA.** The extension leaves the initial compiler thread
    count to the driver and NVIDIA's is none, so `glLinkProgram` blocked for the 2.1 s and
    `COMPLETION_STATUS` was always true (2187 ms in "GPU create submit"). The mode now calls
    `glMaxShaderCompilerThreadsKHR(0xFFFFFFFF)` on a hardware GL: 2187 -> 8.8 ms (the GLSL front end stays on the
    calling thread, so R21 still fails in this fallback mode). Only in `parallel`: with a worker
    context the setting reaches its share group, the worker's links return before the program is
    built and the render thread's first draw waited the 2.1 s (measured, then excluded).
  - R22 is not met. The worst frames come from the driver, not from our creation work: while the
    worker's NVIDIA GLSL front end runs (about 8 ms per new variant) the render thread's GL calls in
    `drawCameras` wait (isolated with temporary scopes: not in any mesh draw, pipeline apply or
    load); a first draw with a new state compiles another ~8 ms on the render thread when it was not
    warmed. Over 4-toggle probes warm draws took the worst frame from 18 to 10 ms; over the full
    runs the max per run is 11-18.7 ms either way (a toggle's front-end wait and a frame's
    first draw can fall in one frame). The control without any creation (the same edits between
    built variants) already has 10 ms frames (8.7x): against a 1.1 ms GPU idle frame the 2x
    bound is 2.2 ms, under what one edit costs the editor. At a 60 Hz pace every worst frame on
    both backends fits in the 16.7 ms period (none is dropped); the uncapped ratio stays the
    reported R22.
  - Defaults chosen on real drivers: GL `Auto` = `worker` (unchanged); **warm draws on for desktop
    GL** (`shader_creation.gl_warm_draws`, was off: safe on NVIDIA, and they remove the
    render-thread first-draw compile when it does not coincide with the front-end wait), off on
    GLES until an Adreno/Mali run (`JASPER_GL_WARM_DRAWS=1`), never on a software GL; `parallel`
    with the driver's compile threads.
  - Pending: Metal and D3D11 at runtime, AMD/Intel (Mesa radeonsi/iris), Android EGL (Adreno,
    Mali) and the WebGL2 browsers (no such devices here).
- **Older notes:**

- Spec written at oe-port 0c22d3b91 (branch `feat/s32-async-shaders`). The GL probe numbers in
  §3 come from a throwaway program run on A in the Docker image.
- **Done** (commits up to 2e97e889b; the first build compiled the sokol extension, SokolAsyncCreate,
  SokolObject and SokolPipelineCache clean under WERROR; later edits await the next build, since
  every server's slots were busy):
  - the sokol extension (`sokol_async.h`, `sokol_async_impl.h`) and 11 `JasperEngine: async
    creation` markers; `PATCHES.md`;
  - `SokolAsyncCreate` (modes, worker, budget, deferred destroys, Vulkan cache, warm draws, waits,
    `makePipelineNow`), interning, recorder, immediate pipelines;
  - worker contexts: `GlxWorkerContext.h` (editor Linux, player Linux, test HeadlessContext),
    `WglWorkerContext.h` (editor and player Windows), MacViewGL, Android EGL; `XInitThreads` in
    WindowLinux;
  - stand-ins in `loadMesh` (main, depth, G-buffer); readiness (`areScenesLoaded`, editor redraws
    while pending, `GpuEnvironment::runFrames`);
  - `ShaderWarmup` (format, record, replay, Lua) and `SokolPipelineRecord`; export hooks in
    `resolveShaderKeys` and `buildAndSaveShaders` (file or header entry "warmup");
  - settings (`ShaderCreationProjectSettings`, reflected in `editor/reflect/ShaderCreationReflect.cpp`,
    project.yaml `shaderCreation`, rows `shader_creation.*`, `ShaderCreationSettingsCmd`, Project
    Settings drawing, generated main); command `assets.clearShaderWarmup`;
  - status bar; `get_render_stats.shader_creation`; Lua; docs (`docs/rendering/async-shaders.md`,
    guide section, one guide line; the guide is 5 bytes smaller than before);
  - unit tests `engine/tests/core/render/AsyncCreateTest.cpp`, MCP `test_async_shaders.py`, bench
    `tools/perf/async_shaders_bench.py` with thresholds.
- **Builds so far:**
  - linux GL WERROR (D, at 348ada8f0): pass;
  - web (B): pass;
  - Vulkan WERROR (B) caught an unused GL-only helper, fixed in the next commit.
- Merged oe-port 1608d1588 (S29) at 57648a5e4. Conflicts in StreamSchema (keys: both
  `shaderCreation` and `versionControl`), DefaultCommands and StatusBar were resolved keeping
  both sides.
- **Next:** build GL + Vulkan WERROR, unit tests (write the golden `async_standin_fogged`), MCP
  suite, web, android, ASan+UBSan, smoke, xcode checks, E benches (worker, Vulkan, the sync red
  control), merge oe-port, TOOLS.md, re-gate.
### Metal on macOS (BF4, 2026-10-31)

The Metal path built and ran natively (macOS 26.2, Apple silicon, CommandLineTools clang 17):
the core GPU suites run on the default Metal device, `[gpu] shader creation: metal`, and
`AsyncCreateGpu` passes 7 of 7. One defect was found and fixed: `SokolObject::prewarmWith` made
the right variant's pipelines on the layout of the stand-in the object had loaded on (another set
of vertex inputs), and Metal failed them (`METAL_CREATE_RPS_FAILED`, "Vertex attribute a_color(3)
is missing from the vertex descriptor", `SplineGpu.RoadOnTerrain` after `RenderGpu`); a variant
with other vertex inputs is now left to its own load. R21/R22 timing on Apple GPUs is still
unmeasured.
In the macOS editor the creations were never published: BackendMac's own loop did not call
`SystemRender::updateGpuCreation()` (EditorFrame::run does, on the other backends), so a shader
created off the render thread stayed pending and the Scene view's Lines never loaded. The Mac loop
now does EditorFrame::run's per-frame work (BF4, 0413db955).

Screenshots wait for every creation, not only the GPU's (BF4, 2026-10-31). The editor's
`captureView` and the player's `--screenshot` (`RunControl`) waited on
`SystemRender::isGpuCreationIdle`, which a variant still compiling is not part of: a variant on
the runtime compiler's worker (or queued for a later frame on the main thread), and one the
editor's ShaderBuilder is building, left the wait idle and the captured frame showed its stand-in.
They now wait on `ShaderPool::isCreationIdle`: no runtime compile pending
(`RuntimeShaderCompiler::getPendingCount`, which also counts a variant a compile settled this
frame that no draw has taken yet), no ShaderBuilder build running (the builder's count, given
with `ShaderPool::setShaderBuilder`), and the GPU creation idle. Test:
`AsyncCreateGpu.AScreenshotWaitsForAVariantCompilingAtRuntime` (red with the GPU-only wait: the
frame without the variant was captured). `run_build`'s timed window grab
(`screenshot_after_seconds`, `screenshots_at`) cannot see the player's creations and does not
wait; `test_html_ui_effects_export.py::test_desktop_player_draws_the_effects` caught the effects'
stand-ins that way with H at load 60 and now uses the player's own capture, which waits (BF12's
`after_ready` + `fixed_fps` + `quit_after`, merged from oe-port in place of BF4's `screenshot_frame` one).
