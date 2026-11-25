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
  monitors. The **Animation** band holds the Animators and the Animation Players' clip pass, also
  when they run under Update or FixedUpdate. Their monitors: `Animators`, `Animators sampled`,
  `Animators culled`, `Anim Prepare ms`, `Anim Wave ms`, `Anim Finalize ms` and `Clip memory`.

**Window > Analysis > Frame Debugger**: **Enable** lists the passes of the chosen view's frame in
order (repeats folded: "shadow 2048x2048 x4"), **Freeze** holds the last one, **Draw list** adds
each pass's draws. The selected pass shows its targets with their load and store actions, its
counts and times, and a preview of one attachment (Color, Alpha, Depth, R, G, B; depth linear
with a near/far, or through Levels).

The Game view's **Stats** overlay shows the frame's CPU time, and its GPU time while something
times the GPU. Next to **Draw calls** it shows **SetPass calls** (Unity's name): the pipelines the
frame applied. A draw whose pipeline is the one its pass applied last applies none, and a draw
whose buffers, textures and samplers are the ones applied last binds nothing either: every
backend skips them, which matters most on D3D11 (every state set again) and Vulkan (a pipeline
bind and a descriptor set each). The opaque queue of a 3D pass is culled first, then sorted by
sort priority, distance band (one per doubling of the distance), render state and distance, so
draws of one material and mesh run together within a band. SetPass calls well below draw calls
mean that sharing works; close to them, the scene's meshes use too many different materials or
shaders. Lua reads the count from `Profiler.frameStats().setPassCalls` and
`DebugOverlay.getStats().setPassCalls`; agents read `pipelines_applied` and `bindings_applied` of
`get_render_stats`.

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
has the `Profiler` table (`docs/mcp/guide/lua-api.md` 2.14): `Profiler.gpuTiming = true` turns GPU
timing on (the timer `get_gpu_passes` arms; false lets it go), and `Profiler.frameStats()` then
has `gpuMs` and `gpuPasses`, `{name, ms}` per pass of the newest timed frame. Where the backend
has no GPU timer, and in a release export, `gpuTiming` stays false and those fields are absent.

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

## Performance in shipped games

A Release export has no profiler, but every build has the **Performance sampler**
(`engine/core/profiler/PerfSampler.h`, Lua `Performance`): it summarizes the frames of gameplay so a
game can tell its developer how fast it runs on players' devices. The engine sends nothing: the game
asks the player before it sends a summary anywhere. A summary holds no personal data (frame times,
the GPU, its driver, the OS family, the tier).

```lua
Performance.setSampling(true)                    -- gameplay starts
Performance.setSampling(false, "menu")           -- the main menu: excluded, named
Performance.onSample(function(s)                 -- every 5 minutes of gameplay
    MyAnalytics.send("perf", {p50 = s.fps.p50, p10 = s.fps.p10, cpu = s.mainThreadCpuMs})
end, 300)
```

Off (the default) it costs one branch per frame. On, each frame of the frame-time ring goes into a
pending second, which once a wall second is **committed** to the window or **discarded**. A second is
discarded when the app was paused or hidden in it (`paused`), within 2 s after a resume, a focus
regain or the window becoming visible (`resume`), within 1 s after a scene load or while scenes are
loading (`loading`), when a frame in it took 1 s or more (`stall`), or when the game turned sampling
off (`game`, and its reason in `excluded.reasons`; at most 8 names, then `other`). Editor frames that
ran an MCP tool are skipped. A pause menu over gameplay (`Engine.timeScale = 0`) is the game's call:
`Performance.setSampling(false, "pause_menu")`.

The summary (`Performance.getSummary{histogram = true}`, `onSample`'s argument): `seconds`, `frames`,
`fps {avg, p50, p10}` (p10 is the rate 90 % of frames reach: 1000 / `frameMs.p90`), `frameMs {p50,
p90, p99, max}`, `cpuMs {avg, p90}`, `mainThreadCpuMs`, `gpuMs {avg, frames}` (only while something
times the GPU: the sampler never starts the timer), `hitches` (frames of 250 ms to 1 s), `excluded`,
`thermal` (seconds per thermal state), `tier`, `tierChanges`, `gpu`, `driver`, `quirks`, `backend`,
`platform`, `version`, `buildId`. Frame times go into 201 fixed bins (0.5 ms below 50 ms, centred on
multiples of 0.5 ms; 2 ms to 250 ms; one above), so a percentile below 50 ms is within 0.25 ms.

**Main-thread CPU** is what the frame thread computed, from its thread clock read once a second:
`cpuMs` counts the frame's waits on the driver, locks and I/O, `mainThreadCpuMs` does not. A
`mainThreadCpuMs` far below `cpuMs` means the frame waits more than it computes.

| Platform | Thread clock |
|---|---|
| Linux, Android, macOS, iOS | `CLOCK_THREAD_CPUTIME_ID`, exact over committed seconds |
| Windows on x64 | `QueryThreadCycleTime` over the time-stamp counter's rate (measured over 2 ms at the first read), exact over committed seconds |
| Windows without an invariant time-stamp counter, Windows on ARM | `GetThreadTimes` (user + kernel), advanced at the scheduler tick (about 15.6 ms) |
| the web | none: `mainThreadCpuMs` is nil, `mainThreadCpu = "unavailable"` |

"Main thread" is the frame thread (the one that runs `Engine::runFrame`), which on Android and iOS is
not the platform's UI thread.

**On a device without code:** every player takes `--perf-summary=<path>`: sampling starts at the
frame that logs `Engine ready:` (unless the game called `Performance.setSampling`), and the summary is
written as JSON (snake_case keys) at quit and at each window end. The web prints it instead
(`?perf-summary=1`): `JASPERENGINE PERF: <json>` on the console.

In the editor the sampler works in Play (it resets at Play start; `onSample` is cleared at Stop); the
Game view's Stats show **Main Thread CPU** (the last full second) and, while the sampler runs,
**Sampling** (`63 s · p50 58 fps · p10 41 fps`); the Profiler's Monitors have `Main thread CPU ms`;
`get_frame_times` adds `main_thread_cpu_ms` and the sampler's `performance`; the benchmark's
`result.json` adds `mainThreadCpuMs {mean}` and `device {gpu, driver, tier, quirks}`.

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
