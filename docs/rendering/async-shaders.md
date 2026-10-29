# Asynchronous shader and pipeline creation

The first use of a shader variant or a pipeline does not stall the frame
(docs/specs/S32-async-shaders.md). The driver's work runs off the render thread, and what is not
ready yet draws a stand-in or waits.

## What happens on each backend

| Backend | Off the render thread | Mode (log line `[gpu] shader creation: <mode>`) |
|---|---|---|
| GL 3.3 core, GLES3 (desktop, Android) | compile, link, and a *warm draw* of each new program and state, on a GL context in the render context's share group | `worker` |
| WebGL2 (and GL without a worker context) | the driver's compile threads with `KHR_parallel_shader_compile`, else the status is read two frames after compiling | `parallel`, `deferred` |
| Vulkan | `vkCreateGraphicsPipelines`, through a pipeline cache saved per device in the user cache directory (`pipelinecache/`) | `thread` |
| D3D11 | `D3DCompile` and the shader objects | `thread` |
| Metal | libraries and render pipeline states, through their completion handlers | `metal` |

A *warm draw* draws the program once, with the state and texture formats of the object about to use
it, into a 1x1 target on the worker. Some drivers (Mesa's llvmpipe among them) compile the program
for that state at its first draw; the warm draw makes that compile happen on the worker.

The render thread finishes what the worker or the driver created within a per-frame budget
(**Main Thread Budget**, 2 ms by default). At least one creation finishes every frame.

## While a variant is not ready

- **Meshes, terrain and sprites** draw the nearest variant that is ready, the same one a build
  that lacks a variant would draw (`ShaderPool::getFallbackProperties`: SSAO, fog, shadows, mirror,
  IBL, normal map and so on given up, the fewest first). They swap to theirs when it is ready. With
  no ready variant at all, the object appears once its own is created.
- **Pipelines** of content (meshes, particles, UI, HTML UI) are shared between objects with the
  same state (interning), so a second object with the same state draws at once. A pipeline being
  created is not drawn for the frames it takes.
- **Passes of the frame** (post-processing, SSAO, SSR, composites, blits, atmosphere, fluid, shadow
  cache) create their pipelines at once. A skipped pass would leave the target the next pass reads
  undefined. Their shaders are still created asynchronously, and the current variant keeps drawing
  until the new one is ready.
- The scenes count as loaded (the READY signal, a game's loading screen, the editor's loading state)
  only once nothing is being created.

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
| GL Warm Draws | on | GL with a worker context only |

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
