# Asynchronous shader and pipeline creation

The first use of a shader variant or a pipeline does not stall the frame
(docs/specs/S32-async-shaders.md). The driver's work runs off the render thread, and what is not
ready yet draws a stand-in or waits.

## What happens on each backend

| Backend | Off the render thread | Mode (log line `[gpu] shader creation: <mode>`) |
|---|---|---|
| GL 3.3 core, GLES3 (desktop, Android) | compile and link (and, when turned on, a *warm draw* of each new program and state), on a GL context in the render context's share group | `worker` |
| WebGL2 (and GL without a worker context) | the driver's compile threads with `KHR_parallel_shader_compile`, else the status is read two frames after compiling | `parallel`, `deferred` |
| Vulkan | `vkCreateGraphicsPipelines`, through a pipeline cache saved per device in the user cache directory (`pipelinecache/`) | `thread` |
| D3D11 | `D3DCompile` and the shader objects | `thread` |
| Metal | libraries and render pipeline states, through their completion handlers | `metal` |

On a **software GL** (llvmpipe, softpipe, SwiftShader) there is no worker context: the mode is
`parallel` where `KHR_parallel_shader_compile` exists, else `deferred`. On llvmpipe the render
thread crashed in the driver drawing programs the worker had drawn, and once hung in a draw after
the worker had linked them; our side's synchronisation was checked (see the spec, deviation 6).
`JASPER_GL_SOFTWARE_WORKER=1` allows the worker there for driver investigations. GPU drivers keep
the worker; they are verified in the GPU campaign.

A *warm draw* draws the program once, with the state and texture formats of the object about to use
it, into a 1x1 target on the worker. Drivers compile a program for its state at its first draw
(Mesa's llvmpipe; NVIDIA: about 8 ms per Lit lobe variant on an RTX 3080 Ti); the warm draw makes
that compile happen on the worker. Warm draws are **on by default on desktop GL** (verified on
NVIDIA in the GPU campaign, `docs/perf/gpu-campaign.md`), **off on GLES** until an Adreno/Mali run
verifies them (`JASPER_GL_WARM_DRAWS=1` allows them there), and never on a software GL (no worker
there). Without them a program's first draw with a new state compiles on the render thread.

The render thread finishes what the worker or the driver created within a per-frame budget
(**Main Thread Budget**, 2 ms by default). At least one creation finishes every frame.

## While a variant is not ready

- **Meshes, terrain and sprites** draw the nearest variant that is ready, the same one a build
  that lacks a variant would draw (`ShaderPool::getFallbackProperties`: SSAO, fog, shadows, mirror,
  IBL, normal map and so on given up, the fewest first). They swap to theirs when it is ready:
  the object first makes its pipelines on the new variant and warms them while it keeps drawing
  the old one, so the swap leaves no empty frame. With no ready variant at all, the object
  appears once its own is created.
- **Pipelines** of content (meshes, particles, UI, HTML UI) are shared between objects with the
  same state (interning), so a second object with the same state draws at once. A pipeline being
  created is not drawn for the frames it takes (the object waits, it is not reloaded). A cached
  shadow map slot or a reflection probe face that had such a draw is drawn again.
- **Passes of the frame** (post-processing, SSAO, SSR, composites, blits, atmosphere, fluid, shadow
  cache) create their pipelines at once. A skipped pass would leave the target the next pass reads
  undefined. Their shaders are still created asynchronously, and the current variant keeps drawing
  until the new one is ready.
- The scenes count as loaded (the READY signal, a game's loading screen, the editor's loading state)
  only once nothing is being created.

## Threads

A model may load off the main thread (the editor's thumbnail strand, a game's
`Engine.startAsyncThread`). A game's loading thread gives the main thread's result (S65): into a
scene the frames do not run yet it makes the whole entity tree; for a scene the frames run it only
reads the file into `ModelPool`, and the main thread makes the tree in a coming frame
(`MeshSystem::prepareLoadOnMainThread`), so `loadModel` has returned before the entities exist. Only
a thumbnail (`setImageDecodeMaxDimension`) bakes the nodes into one mesh. Its GPU objects are the
frames' to make (`SokolCmdQueue`, executed on the main thread), and `Engine::executeSceneOnce` keeps that thread off the frames with their semaphore.
On the main thread the scope does nothing: that thread runs the queue, so it would wait for itself
(a script's init runs before the view starts the queue), and it makes its objects at once anyway. A
loading thread needs no shared GL context on any backend (GLES3 and WebGL have none): it only queues.
It does not keep it off what the main thread does outside a frame, so two things take a lock of
their own, around their containers' operations alone:

- `ShaderPool`'s map and key sets: never while a builder runs or a GPU shader is made or destroyed.
  `get()` holds its entry for what it does next, and `remove()` leaves what someone holds, a queued
  batch making that variant included (it reads the variant's sources when it runs).
  A steady frame of 100 meshes asks for none, and a variant asked for again costs 18 ns where it
  cost 10.5 (`ShaderPoolBenchGpu.GetOfAMadeVariant`; 108 ns against 91 among 2000 variants still
  building, `ShaderPoolBench.Get`).
- sokol's resource pools (`engine/libs/sokol/PATCHES.md`): the free lists and a slot's hands, which
  `sg_alloc_*` on that thread and `sg_make_*` and `sg_destroy_*` on the main one share. A draw takes
  no lock, a steady frame allocates no slot, and a slot allocated and freed costs 18 ns where it
  cost 7.5 (`SokolPoolBenchGpu.AllocAndFree`).

`Engine::updateGpuCreation` publishes what was created off the render thread under the frames'
semaphore, since that thread asks for those states inside it.

The main thread may load a scene outside a frame next to that thread loading one, so both can make
the same new variant. `ShaderRender::createShader`'s `loading` flag is the claim (an atomic exchange,
with the look at the handle after it): one makes the variant, the other finds it being made and
asks again, as for any variant not ready. `SokolShader`'s handle is atomic, and a mutex of its own
keeps that thread's looks at the slot off the main thread's destroy. No draw takes it: the main
thread looks without it, since only it frees a slot. A variant asked for again still costs 18 ns.
Those locks never wait for another thread, so they cannot deadlock. The frames' semaphore around
the main thread's load was not an option: a script's `Engine.setScene` inside a frame already holds
it, and it is neither recursive nor owned; a load may wait for a job that needs it; and `get()` has
callers outside a load (the inspector, the MCP tools, `warmUp`).

## The shader warm-up list

**Project Settings > Graphics > Shader Warm-up**:

- **Record During Play**: each Play adds the shader variants and pipelines it used to
  `ShaderWarmup.json`, next to `project.yaml`. Commit it with the project.
- **Include in Builds** (on): exports build the list's variants as well as the scenes', and ship
  the list (`shaders/warmup.json`, or inside the shader headers of web and mobile builds).
- **Hold First Scene** (on): a game replays the list when it starts, and its scenes count as
  loaded only when the list is ready, so the loading screen covers it.
- **Clear** (and the command *Clear Shader Warm-up List*) empties the list.

The list is backend-neutral (shader keys and pipeline states by name). A list recorded on a GL
editor warms a Vulkan, Metal or D3D11 build too.

Lua:

```lua
-- a loading screen's progress while the warm-up list replays (0..1)
local progress = 1.0
if not ShaderWarmup.isDone() then
    progress = ShaderWarmup.getProgress()
end
-- variants and pipelines still being created, and whether none is
local pending = ShaderPool.getPendingShaderCount() + ShaderPool.getPendingPipelineCount()
local idle = ShaderPool.isGpuCreationIdle()
```

## Settings

**Project Settings > Graphics > Shader Creation**:

| Setting | Default | |
|---|---|---|
| Asynchronous Shader Creation | on | Off creates everything on the render thread at once, as before S32. |
| Main Thread Budget (ms) | 2 | 0.5-16 |
| GL Warm Draws | off | GL with a worker context, never on a software GL; not verified on GPU drivers |

For tests, `JASPER_ASYNC_SHADERS=sync|worker|parallel|deferred` forces a mode, and
`JASPER_ASYNC_SHADERS_DELAY_MS=<n>` holds every creation on the worker for n ms.

## Measuring it

- The editor's status bar shows "Compiling shaders N" while anything is pending. Its tooltip gives
  the mode and the render thread's share of the last frame.
- In the Profiler (and `capture_profile`), the render thread's scopes are `GPU create submit`,
  `GPU create publish` and `GPU pipeline create`. The worker's lane is "GPU Create Worker". The
  monitors are "Shaders pending" and "Pipelines pending".
- `get_render_stats` returns `shader_creation` (mode, pending counts, `last_frame_main_ms`,
  `max_main_ms`, warm-up state).
