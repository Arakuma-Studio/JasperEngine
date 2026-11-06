# Job system and memory

The engine runs its parallel and background work on one job system (`engine/core/jobs/`):
a work-stealing pool of worker threads with three priority lanes, task handles with
dependencies and cancellation, fork-join counters, `parallelFor` that is legal on any thread,
strands, blocking channels and frame protection. Blocking file reads go through the asset I/O
service (`engine/core/io/AssetIOService`), and a small set of allocators
(`engine/core/memory/`) report into one memory-stats registry.

## Threads

| Thread | Count | Names | Runs |
|---|---|---|---|
| Workers | Project Settings > Player > Threading > Worker Threads (Auto: logical cores - 1, at most 15; 4 on mobile; cores - 2, at most 4 and at most the pthread pool - 3, on threaded web; 0 on web without threads) | `je-worker-N` | jobs (compute) |
| Blocking threads | created on demand, up to the channels' caps (budget 8 on desktop, 1 on threaded web) | `je-block-N` | `JobChannel` jobs: processes, network, long writes |
| Asset readers | Asset I/O Threads (Auto: 2 desktop, 1 mobile, 0 web) | `je-io-N` | file reads |
| Main | 1 | `je-main` | the frame; pumps the queues in single-thread mode |

On threaded web every thread comes out of the Emscripten pthread pool, which never grows:
`JASPERENGINE_WEB_PTHREAD_POOL_SIZE` (CMake, default 8) sets `-sPTHREAD_POOL_SIZE` and the
engine's `JE_PTHREAD_POOL_SIZE` together. The engine keeps 3 of it beside the workers (the
blocking thread, a video decoder, a spare an asset reader may take), so a worker setting or
`JE_JOB_WORKERS` above pool - 3 is capped.

`JE_JOB_WORKERS=n` overrides the worker count (`0` forces single-thread mode on any platform),
`JE_ASSET_IO_THREADS=n` the readers. Workers run at normal priority, readers and blocking
threads below it (Apple: QoS user-initiated / utility). No CPU affinity is ever set.
A worker that runs out of work spins once for at most 50 µs, then parks on a condition
variable; producers wake parked workers. Its backstop timeout starts at 1 ms and doubles with
every wake-up that finds nothing (up to 128 ms), so an idle pool costs a few wake-ups per
second and well under 1 % of a core (`JobsIdle.*`).
Jolt's physics jobs run on the workers (`JoltJobAdapter`); `-DJE_JOLT_OWN_POOL=ON` restores
Jolt's own pool for one release.

## Lanes and the Background gate

| Lane | For | Dequeue order on a worker |
|---|---|---|
| `High` | work the frame waits on | 1. High (global) |
| `Normal` | the default | 2. own local deque, 3. Normal (global), 4. steal from other workers |
| `Background` | asset decode, LOD, cooks, editor tasks | 5. Background, if the gate admits it |

The **Background gate** keeps workers for the frame: at most `max(1, workers - reserve)`
workers run Background bodies at once (Reserved Frame Workers, Auto: 2 on desktop, 1 on mobile
and web; it applies live). Jobs tagged `GateClass::Heavy` (texture decodes, mesh
simplification, shader compiles) take at most `max(1, max - 2)` of those slots, so short decodes
always find one. With 3 or more workers the highest-index worker polls Background before
Normal, so a Normal flood cannot starve Background work. `Engine.setMaxResourceLoadingThreads(n)`
sets the gate's maximum directly (live).

The High and Normal lanes are lock-free rings (32 K slots each, overflowing into a mutex
ring), so a thread feeding the workers shares no lock with them. Background lanes are
mutex-guarded rings behind the gate. `get_job_system_stats` reports the gate's peak use over
its window (`gate.peak`, `gate.heavy_peak`), and the panel shows it too.

## API

```cpp
#include "jobs/JobSystem.h"
using namespace jasperengine;

// a task with a result; get() waits (helping) and rethrows the job's exception
jobs::Task<Mesh> mesh = jobs::submit([path]{ return parse(path); },
                                     {jobs::Lane::Background, jobs::GateClass::Default, "parse-mesh"});
if (mesh.isReady()) use(mesh.get());   // poll from the main thread

// dependencies: runs once both are terminal; a failed or cancelled dependency cancels it
jobs::Task<void> upload = jobs::submit([&]{ ... }, {}, {mesh, textures});
auto next = mesh.then([](Mesh& m){ return build(m); });

// fork-join from any thread, a worker included; no heap allocation once warm
jobs::parallelFor(count, [&](size_t i){ out[i] = f(in[i]); }, {/*grain*/ 64});
jobs::parallelForChunked(0, count, 256, [&](size_t begin, size_t end){ ... });
jobs::parallelSort(values.begin(), values.end());

jobs::JobCounter counter;            // may live on the stack
for (auto& chunk : chunks) counter.run([&chunk]{ process(chunk); });
counter.wait();                      // runs this counter's own jobs, then parks

jobs::SerialQueue strand{"thumbnails", jobs::Lane::Background};   // FIFO, one at a time
strand.push([]{ ... });

jobs::system().processChannel().submit([]{ runCMake(); });   // blocking work (cap 2)
jobs::system().ioWriteChannel().submit([]{ writeScene(); }); // long writes (cap 1)

jobs::enqueueDetached([]{ ... });    // must-run, no handle (runs even during shutdown)
jobs::postToMain([]{ ... });         // runs at the start of the next frame
```

Rules:

- Jobs never touch sokol, Lua or ImGui. Return results by polling `Task::isReady()` on the main
  thread or with `jobs::postToMain`.
- Every wait (`Task::wait/get`, `JobCounter::wait`, `parallelFor`) is legal on every thread. A
  wait runs the awaited task inline when nobody started it (its pending dependencies first),
  helps only the awaited counter's jobs, and parks only on work running on another thread, so
  an acyclic wait graph always completes. A wait that closes a cycle aborts with a report in
  builds with assertions (`Config::detectWaitCycles`).
- Do not wait while holding a mutex that the awaited jobs take. Cancel first, then wait, when
  you must hold one (`cancel()` on a Pending task wins: it never runs).
- A job may not wait on a job of its own `JobChannel` (asserted): with the cap taken that is a
  deadlock. `jobs::isInChannelJob()` lets blocking code run inline instead.
- Cancellation is cooperative: a running job polls `jobs::cancelRequested()`, the view of the
  `CancelToken` passed in `SubmitOptions::cancel`; one token covers a group of jobs.
- Destroying a handle neither cancels nor blocks.
- A `parallelFor` (or a counter job) forked from a Background body is Background: forks never
  move work into a more urgent lane.

**Single-thread mode** (0 workers: the web build without pthreads): nothing runs at submit. The
Engine calls `jobs::pump(budget)` at the start of every frame: all High and Normal work, the
channel jobs, then Background work for Web Pump Budget (default 4 ms). Waits and `parallelFor`
run inline. Async loading works there too: loads complete over several frames.

**Shutdown** (`Engine::systemShutdown`): the asset readers stop first, then the job system:
Pending `submit` tasks end Cancelled, `enqueueDetached` jobs run, the threads join. A submit
afterwards returns a Cancelled handle; a detached job afterwards runs on the caller.

## Asset I/O

```cpp
AssetIOService::ReadRequest request;
request.path = path;                         // through FileData: resource packs, Android assets
request.priority = IOPriority::High;         // Low | Normal | High | Critical
request.gateClass = jobs::GateClass::Heavy;  // texture decodes
request.cancel = loadToken;                  // identity for AssetIOService::cancel
request.onFailure = [](const std::string& error){ ... };
jobs::Task<Image> image = AssetIOService::instance().submitRead(request,
    [](std::vector<uint8_t>&& bytes){ return decode(bytes); });
```

A reader claims the highest-priority read whose gate class has a free slot, reads it (a
half-written or renamed file is retried for 200 ms), then releases the decode continuation as a
Background job that holds that slot. Exactly one of `onSubmitted`/`onFailure` fires, before the
task can be seen terminal. A wait on the task performs a still-queued read itself. Byte ranges
(`offset`, `size`) are limited to 4 GiB (`FileData`).

`Engine.getQueuedResourceCount()` is the number of asset jobs queued or running (reads
included), so the editor's loading indicator and idle checks never report idle mid-decode.

## Memory

| Allocator | Use |
|---|---|
| `memory::Arena` | linear, chained blocks; `allocate<T>`, `allocateArray<T>`, `marker`/`rewind`/`reset`; `ArenaAllocator<T>` for std containers |
| `memory::frameArena()` | one arena per frame in flight, reset by the Engine at frame start |
| `memory::ScratchScope` | per-thread stack memory for a block of code; nested scopes rewind LIFO (also across helped jobs) |
| `memory::ObjectPool<T>` | fixed slots, stable addresses, O(1) create/destroy; double-destroy and leak asserts |
| `memory::RangeAllocator` | TLSF-like good fit over offsets of storage you own (GPU buffers); `freeAfter(range, stamp)` + `reclaimThrough(stamp)` |
| `memory::TrackedBytes` | a named counter any subsystem keeps current |

Allocators are serial-use (one user at a time, asserted in builds with assertions) and poison
the memory they hold under AddressSanitizer. Each registers a record in `memory::snapshot()`
with its kind (Arena, Pool, Range, Tracked), residency (Cpu, Gpu, Mapped), current bytes,
peak, capacity and count. Readers never lock: each record is a seqlock. Tracked by default:
`Texture.GPU`, `Texture.RenderTargets`, `Mesh.GPUBuffers`, `Lua.Heap`,
`Physics.JoltTempAllocator`, `Jobs.Slabs`, `Jobs.Queues`, the scratch arenas, `Engine.FrameArena.*`
and `Render.PipelineDescs` (the base pipeline descriptions render objects share: one per distinct
shader and vertex layout, however many objects draw with it).
For std containers, use `ArenaAllocator<T>`. `std::pmr` is outside the portable C++20 subset
(S01).

Components hold what they use: a mesh's submeshes, external buffers and bone palette, a sprite's
and a Points' frames, a Sprite Animation's frames and a tilemap's tiles and rects are arrays that
grow when an index past their end is written (`GrowArray`, `StableArray`; a submesh never moves
while its mesh lives), with no capacity; `ARRAY_INDEX_LIMIT` (65,536) only refuses absurd indices.
A mesh without a skin has no bone palette. The Systems window and `get_ecs_stats` (`element_bytes`,
`heap_bytes` per type) show what each component type takes.

After a scene load, a Play Stop and a scene close, the engine gives the heap's free pages back to the
system (`memory::requestHeapTrim`: one Background-lane job; glibc Linux `malloc_trim`, macOS and
iOS `malloc_zone_pressure_relief`, Android 9+ `mallopt(M_PURGE)`; nothing on the web, where linear
memory cannot shrink, nor on Windows).

## Tools

- **Editor:** Window > Analysis > Jobs & Memory: per-worker busy time, lanes, the gate,
  channels, asset reads (with Cancel), and the memory records (sortable, filterable).
  Project Settings > Player > Threading holds the settings (`project.yaml` `threading:`);
  changes are undoable and the reserve applies at once.
- **MCP:** `get_job_system_stats {window_frames}`, `set_job_system_config
  {reserved_frame_workers, web_pump_budget_ms}` (undo with scope `project`), `get_memory_stats
  {prefix, residency, sort}`, and the `player.threading.*` keys of `set_project_settings`.
- **Lua:** the read-only `Jobs` table (see `docs/mcp/guide/lua-api.md`).
- **Profilers (S15):** `jobs::setProfilerHooks({onThreadStart, onJobBegin, onJobEnd,
  onWaitBegin, onWaitEnd, onIdleBegin, onIdleEnd})`; every job has a static name
  (`SubmitOptions::name`). `jobs::snapshotOccupancy` reports what each thread runs (S28).

## Tests and benchmarks

The suites are part of `jasperengine-core-tests` (`tools/build.sh unit-tests`, filter with
`UNIT_TEST_ARGS=--gtest_filter='Jobs*:Workers/*'`). The benchmarks build next to them:

```bash
cmake -S engine -B build/linux/core-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON
cmake --build build/linux/core-tests --target je-bench-jobs je-bench-memory
cd build/linux/core-tests/tests/core
./je-bench-jobs --json jobs.json && ./je-bench-memory --json memory.json
python3 ../../../../../tools/perf/check_bench.py jobs.json memory.json
```

Run these inside the Linux image (`tools/build.sh exec linux ...`).

The scheduler suites run at worker counts {0, 1, 2, 8}. For TSan, configure with
`-DCMAKE_CXX_FLAGS=-fsanitize=thread` (and the C and linker flags) in its own build directory.
GCC 11's TSan aborts with "unexpected memory mapping" on kernels with high ASLR entropy: run the
binary under `setarch $(uname -m) -R`. Inside Docker, that needs
`--security-opt seccomp=unconfined`. Timed condition-variable waits go through
`jobs/detail/TimedWait.h`, because GCC 11's TSan does not intercept the clock wait libstdc++
uses.
`tools/perf/bench-thresholds.json` holds the budgets `check_bench.py` enforces.
`JE_TEST_TRACE_ALLOCATIONS=1` prints the stack of each heap allocation inside an
`AllocationCountScope` (the zero-allocation tests).
