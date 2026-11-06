# Profiling and render diagnostics

JasperEngine measures where a frame's time goes on the CPU and the GPU, which passes the
renderer runs and what each one draws, and how much GPU memory each part of a game holds. The
design and acceptance record is `docs/specs/S15-profiling-diagnostics.md`; agents use the tools of
`docs/mcp/guide/profiling.md`.

## In the editor

**Window > Analysis > Profiler** (Ctrl+7, or a click on the FPS in the status bar):

- **Record** starts recording the frames the editor draws (in Play mode every frame). While it
  records, the views keep drawing and GPU timing runs.
- **Clear** empties the history; **Deep Profile Lua** adds a scope per Lua callback.
- The **budget** (30, 60, 90, 120, 144 or 240 fps) draws a line on the charts.
- **Load** and **Save** read and write `.jetrace.json` captures; **Target** switches between the
  live recording and loaded or captured ones.
- The frame **scrub** (arrows, slider, Current) selects a frame; **Pause on frame > X ms** stops
  recording on the first frame over X ms and selects it.
- The modules: **CPU Usage** (stacked by category: Rendering, Scripts, Physics, Animation, UI,
  Loading, Jobs, Wait, Other), **GPU Usage** (stacked by pass family, or "unavailable: reason"),
  **Rendering** (draw calls, passes, pipelines), **GPU Memory** (stacked by category) and
  **Monitors** (the series you check in its detail pane). A click on any chart selects that frame
  in every module; a right click hides a module.
- The detail pane follows the module: the CPU **Hierarchy** (Total ms, Self ms, Calls, Total %,
  sortable; search; one thread at a time) and **Timeline** (a lane per thread, job workers
  included; the wheel zooms, a drag pans, a click selects the scope in the Hierarchy); the GPU
  pass table with GPU ms, CPU ms and draws; the render counters; the VRAM categories and the
  allocation table (by category or owner, sortable, filterable, with the driver's figure); the
  monitors.

**Window > Analysis > Frame Debugger**: **Enable** lists the passes of the chosen view's frame in
order (repeats folded: "shadow 2048x2048 x4"), **Freeze** holds the last one, **Draw list** adds
each pass's draws. The selected pass shows its targets with their load and store actions, its
counts and times, and a preview of one attachment (Color, Alpha, Depth, R, G, B; depth linear
with a near/far, or through Levels).

The Game view's **Stats** overlay shows the frame's CPU time, and its GPU time while something
times the GPU.

The panels change no project data; their layout is kept in the editor settings.

## In code

```cpp
#include "profiler/Profile.h"

void Enemy::think(){
    JE_PROFILE_FUNCTION();
    {
        JE_PROFILE_SCOPE("Pathfind");
        ...
    }
    JE_PROFILE_COUNTER("Enemies alive", alive);
}
```

`JE_PROFILE_SCOPE("literal")`, `JE_PROFILE_FUNCTION()`, `JE_PROFILE_SCOPE_DYN(name)` (a run-time
name), `JE_PROFILE_THREAD(name)`, `JE_PROFILE_COUNTER(name, value)` and
`JE_GPU_ALLOC_SCOPE(Category, owner)` (what the GPU resources made in the scope are for: Textures,
RenderTargets, ShadowMaps, PostFX, Meshes, Uniforms, UI, Particles, Terrain, Editor, Other). Lua
has the `Profiler` table (`docs/mcp/guide/lua-api.md` 2.14).

A scope costs one relaxed atomic load and a branch while nothing records, and two reads of the
CPU's counter (rdtsc, cntvct_el0) and two writes into the thread's own ring while it records; job workers name themselves
("Job Worker #3") and every job is a scope named by its label.

## Builds

| Build | Profiler |
|---|---|
| the editor | always |
| a Debug export | always |
| a Release export with **Development Build** (Build Settings, or `player.development_build`) | always |
| a Release export | none: the macros compile to nothing, no trace hook is installed |

`JASPERENGINE_PROFILER` (CMake) follows those defaults; `JASPERENGINE_TRACY=ON` (desktop and
Android) also sends the zones, frame marks and plots (GPU pass times as `GPU/<pass>`) to a
[Tracy](https://github.com/wolfpld/tracy) 0.11 server.

A development build and the editor take `--profile` (record from the start),
`--profile-capture=<path>` and `--profile-frames=N` (default 300): the capture starts once every
scene is loaded (logging `Profiler capture started: N frames for <path>`), runs with GPU timing,
and logs `Profiler capture written: <path>`. Mobile and
web development builds write their captures to the path given, for Load in the Profiler.

`JASPERENGINE_FRAME_STATS=1` (and `=all`, `JASPERENGINE_FRAME_STATS_ACTIONS=1`) still logs the
frame's sokol statistics and its camera pass list, unchanged, in every build.

## Measuring GPU passes from the command line

The per-pass GPU times of a scene, for budgets:

```sh
jasperengine-editor benchmark --project <dir> --scene <scene> --warmup 120 --frames 300 \
    --gpu-passes --out result.json [--profile-capture result.jetrace.json]
```

`result.json` gets `cpuMs {median, p95, mean}` and `gpu {available, reason, backend, device,
semantics, frameIsSum, frames, frameMs {median, p95, mean}, passes [{label, perFrame, medianMs,
p95Ms, meanMs}]}`: each pass label's time summed per frame (a label used by several passes, the
four cascades of a shadow map, counts them together; `perFrame` says how many). Run it on an idle
machine and compare medians.

Every run also writes `renderCpuMs {median, p95, mean}` (the scenes' draw and the commit: the
render submission) and `render {mainDrawCalls, shadowDrawCalls, depthDrawCalls, otherDrawCalls,
mainTriangles, shadowTriangles, instancedDraws, batchedInstances, visible, culled, uniformBytes,
uniformBytesKept, bufferUploadBytes, pipelinesApplied, bindingsApplied, aliveBuffers, aliveImages,
alivePipelines}`, the game frame's counters averaged over the measured frames (with `--play`, the
frame the Game view shows). `drawBatch {offered, instancedDraws, batchedInstances, splitPipeline, splitElements, splitBindings,
splitUniforms}` is GPU Instancing in the last measured frame: the draws that could batch, and why one
did not join the batch before it. `--gpu-instancing on|off` overrides the project's GPU Instancing for
the run. The mesh benchmark gate (spec S55) runs both:

```sh
python3 tools/fixtures/mesh-bench/generate.py /tmp/mesh-bench
python3 tools/perf/mesh_bench.py --editor <jasperengine-editor> --project /tmp/mesh-bench --json mesh-bench.json --check
```

## GPU timing per backend

| Backend | Queries | Semantics |
|---|---|---|
| OpenGL 3.3 core | `GL_TIME_ELAPSED` per pass | `elapsed`: the frame is the sum of its passes |
| OpenGL ES 3 (Android) | `EXT_disjoint_timer_query` | `elapsed`; a disjoint event drops the frame |
| WebGL2 | `EXT_disjoint_timer_query_webgl2` (Chrome; Firefox and Safari: unavailable) | `elapsed` |
| Direct3D 11 | a `TIMESTAMP_DISJOINT` per commit, two `TIMESTAMP`s per pass | `pipeline_point` |
| Vulkan | timestamp pools, `vkCmdWriteTimestamp2`, host reset (`hostQueryReset`) | `pipeline_point` |
| Metal | stage-boundary counter samples per render encoder (Apple GPUs) | `encoder_span`: overlapping, never sum |

Results are never waited for: a frame's times arrive two to four frames later, or the frame is
dropped with a counted reason. With nothing timing (the Profiler not recording, no agent asking,
no dynamic resolution running) no query is issued. Running dynamic resolution (S13) keeps the
timer on in every build and steers by the game frame's GPU time (its GPU law); where the timer is
unavailable it uses its frame law. `JASPERENGINE_GPU_TIMER=off` forces "unavailable: disabled".

## The capture format

`.jetrace.json` is Chrome Trace Event JSON: `traceEvents` (`X` per scope with `args.frame` and
`args.depth`, `C` per monitor, `M` thread names, the GPU passes as a `GPU` lane) and a `jasper`
object `{version: 1, backend, device, clock, semantics, threads, frames, passes, vram}`. Perfetto
and chrome://tracing open it unchanged. A newer `jasper.version` is refused; unknown fields are
ignored.
