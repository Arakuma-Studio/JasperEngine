# S06 — Job system & memory

Wave 1. Owner stream: engine core. Status: spec.

## 1. Goal

Replace the single-FIFO `ThreadPoolManager` with a work-stealing job system. It has priority
lanes and a throttled Background lane. Waits help execute instead of parking, and `parallelFor`
is legal from any thread, worker threads included. Blocking I/O moves to dedicated reader
threads behind an asset I/O service, and a small set of allocators (arena, scratch, object pool,
range allocator) reports into one memory-stats registry. The result: no nested-wait deadlocks,
asset storms that cannot starve the frame, async loading on single-threaded web, and one
CPU-thread budget instead of three competing pools (ours, Jolt's, ad-hoc `std::thread`s).

## 2. Scope

**In scope**
- `engine/core/jobs/`: scheduler, `Task<T>` handles, dependencies, `JobCounter`, `parallelFor`,
  `SerialQueue`, `JobChannel` (blocking work), `BackgroundGate` (frame protection), stats,
  profiler hooks.
- `engine/core/io/AssetIOService`: blocking-read threads, priority, cancellation, byte ranges.
- `engine/core/memory/`: `Arena`, `FrameArena`, `ScratchScope`, `ObjectPool<T>`,
  `RangeAllocator`, `MemoryStats`/`TrackedBytes`.
- `engine/core/thread/ThreadUtil`: naming, worker-count policy, OS priority/QoS, `canBlock()`.
- Migration of every `ThreadPoolManager` user and every ad-hoc `std::thread`/`std::async` in
  `engine/core` and `editor/`, listed in §3. Jolt runs on our pool through an adapter.
- Web: a single-thread mode that still runs every job, and the pthreads build (COOP/COEP).
- Lua (stats only), MCP tools, editor settings and a "Jobs & Memory" panel, tests and benchmarks.

**Out of scope**
- Fibers. Lua jobs: Lua states are single-threaded, and `LuaBinding.cpp:127-165` enforces it.
- A global `operator new` replacement, mimalloc overrides and allocation hooks in shipped code.
  The OpenEngine report also rejects them (`02-core-ecs-assets.md:193,233`).
- The ECS parallel scheduler (S07), the CPU profiler UI (S15), the tick scheduler and async
  logger (S09), the file watcher (S05) and the hang watchdog (S28). These consume this spec.
- GPU-driven anything (README do-not-copy list). `RangeAllocator` only hands out offsets.

## 3. Current state

- **Pool**: `engine/core/thread/ThreadPoolManager.{h,cpp}` (138 lines). It is a singleton with
  one `std::queue<std::function>` under a mutex and condvar. `enqueue` makes a
  `std::packaged_task` and returns a `std::future`. It has no priorities, stealing, cancellation
  or parallel-for. `Engine::systemInit` creates it with `hardware_concurrency()-1` workers
  (`Engine.cpp:1056-1058`). `Engine::setMaxResourceLoadingThreads` tears it down and rebuilds it
  while work may be in flight (`Engine.cpp:743-748`). `getQueuedResourceCount` counts *queued*
  tasks only, so running work is invisible (`Engine.cpp:751-757`). The editor uses that count as
  "loading" for its status bar and benchmark idle check (`editor/App.cpp:1037,3059`).
- **The deadlock workaround**: `MeshSystem::parallelForIndexed` (`MeshSystem.cpp:1108-1142`)
  spawns `std::async` threads. Model parsing already runs on a pool worker, and a blocking
  nested wait on the bounded pool deadlocks.
- **Pool users**:
  - `TextureDataPool.cpp:152`
  - `SoundPool.cpp:94`. `remove` must `wait()` because destroying a future doesn't cancel
    (`SoundPool.cpp:217`).
  - `MeshLodPool.cpp:129`
  - `MeshSystem.cpp:1287`
  - editor `ShaderBuilder.cpp:209`. It drops stale futures and lets the tasks run (`:453`).

  These are all whole-file read+decode jobs on one FIFO, so a texture storm delays an urgent
  sound or shader. Latent bug: `TextureDataPool.cpp` erases the same iterator twice in its
  "Invalid future" branch, which is undefined behaviour. The migration removes that code.
- **Lua**: `ThreadClassesLua.cpp:62-66` exposes `ThreadPoolManager.getInstance/initialize/
  shutdown/getQueueSize`, and `CoreClassesLua.cpp:745-746` exposes
  `Engine.setMaxResourceLoadingThreads/getQueuedResourceCount`. Both are documented in
  `docs/mcp/guide/lua-api.md:336`.
- **Second pool**: `PhysicsSystem.cpp:81-87` creates a `JPH::JobSystemThreadPool` with
  `cores-1` more threads. The engine runs about 2x the core count in compute threads. On
  single-threaded web this pool would still try to start one thread (to be confirmed by AC20).
- **Other thread owners**:
  - `RuntimeShaderCompiler.cpp:338`: one private worker.
  - `VideoStream.cpp:38`: a decoder thread per stream.
  - `Av1Decoder.cpp:210`: dav1d with up to 4 internal threads per video, on threaded web too.
  - ThorVG runs with 0 threads (`ThorVGRuntime.cpp:40`).
- **Ad-hoc editor threads**:
  - `Exporter.cpp:491`
  - `Generator.cpp:2711,2793` (`std::async`)
  - `Project.cpp:5898` (scene save), `:7934` (play startup), `:8064` (play stop)
  - `BackgroundRelease.cpp:21`
  - `TerrainMapFileWriter.cpp:99`
  - `ProjectWindow.cpp:194` (thumbnails), `:2570` (FFmpeg)
  - `HtmlUiImportQueue.cpp:80`
  - `CodeEditor.cpp:541`
  - `EntityClipboard.cpp:256` (`std::async`)
  - `GraphicUtils.cpp:348`
  - `mcp/tools/AssetTools.cpp:98,1572`
  - `ScriptTools.cpp:1642`
  - `BuildTools.cpp:604,986`

  Long-lived service threads are `McpTransportStdio.cpp:70-71`, `McpTransportHttp.cpp:455` and
  `:501` (one per client).
- **Web**: `Engine.h:7-11` defines `NO_THREAD_SUPPORT` without `__EMSCRIPTEN_PTHREADS__`, and
  then `setAsyncLoading` silently falls back to synchronous loads (`Engine.cpp:687-695`), so the
  web build hitches on every load. `EMSCRIPTEN_THREAD_SUPPORT` defaults to OFF, with
  `PTHREAD_POOL_SIZE=4` (`engine/CMakeLists.txt:208-212`).
- **Memory**: there are no allocators beyond the STL. `util/HybridArray.h` is a small-buffer
  array. No memory stats exist.
- **Apple floors**: macOS 10.15/13.0 (`engine/CMakeLists.txt:446,612`) and iOS 12/13
  (Xcode project). libc++ gates `std::pmr::memory_resource` (macOS 14/iOS 17) and
  `std::atomic::wait`/`std::counting_semaphore` (macOS 11/iOS 14) at those floors.

## 4. Reference (OpenEngine, MIT; copied code keeps its header plus a `NOTICE.md` entry)

| What | Path (under `external/OpenEngine/Engine/`) | Take |
|---|---|---|
| Pool design | `Modules/JobSystem/Include/JobSystem/WorkStealingThreadPool.h` (class doc ~l.100-150) | Lanes, Background polled last and never in local queues or steals, wake protocol with bounded spinners and a sleep backstop, shutdown drain rules. **Re-implement**: 2.4k-line `.cpp`, depends on their Logger/Types. |
| ParallelFor contract | same header l.44-95, `Source/ParallelForCore.cpp`, `Include/JobSystem/ParallelForOptions.h`, `ParallelAlgorithms.h` | Caller participates. Helpers are bare tasks that find a closed run and exit. The caller waits only for units in flight. Stop/exception propagation, a fork inherits the lane of its body, no steady-state heap use. **Adapt closely**, tests too (`Tests/ParallelAlgorithmsTests.cpp`). |
| Counter | `Include/JobSystem/JobCounter.h` header comment | Add-at-submit; decrement under the mutex (stack-lifetime rule); filtered help of the counter's own jobs; sticky failure flag. |
| Blocking work | `Include/JobSystem/JobChannel.h`, `Source/BlockingThreads.{h,cpp}` | Named channels with `MaxRunning` caps, on threads that never run compute work. A job may not wait on its own channel. |
| Graph | `Include/JobSystem/TaskDependencyGraph.h` | Dependency semantics only. |
| Frame protection | `Include/Assets/AssetDecodeGate.h`, `Source/Assets/AssetDecodeGate.cpp`, `Tests/Assets/AssetDecodeGateTests.cpp` (repo root `Tests/`) | Keep 2 workers for the frame, and keep heavy work out of the last 2 gate slots. **Copy and generalise** to all Background work. |
| I/O | `Include/Assets/AssetIOService.h`, `Source/Assets/AssetIOService.cpp`, `Tests/Assets/AssetIOServiceTests.cpp` (repo root `Tests/`) | Reader threads, priority claim gated by slot, exactly-one `onSubmitted`/`onFailure`, cancel by flag identity, byte ranges, retry window for half-written files, inline mode. **Adapt** to `FileData`. |
| Memory | `Modules/Memory/Include/Memory/{Arena,ScratchScope,ObjectPool,RangeAllocator,StatsRecord,StatsKind,StatsSnapshot,TrackedBytes,SerialUseCheck,AddressSanitizer,Residency}.h`, `Source/{Arena,ScratchScope,ObjectPool,RangeAllocator,StatsRegistry,TrackedBytes}.cpp`, `Tests/*`, `Bench/MemoryBench.cpp` | **Copy** (MIT), rename the namespace to `jasperengine::memory`, gate pmr (see D9). Skip `MimallocOverrides.cpp`, `AllocationHook*`, `AllocationSiteTag`, `ImagePins`, `ProductionAllocationHook`. |
| Platform | `Include/Platform/Thread.h` (`CanBlockCurrentThread`, `SetCurrentThreadName`), `Modules/Platform/Source/Capabilities.cpp:35-60` (worker budgets: wasm `min(cores-2,4)`, 0 without pthreads) | Adapt. |

- **Not used**: `Engine/Source/Jobs/*`. It is their C# compile-server pipeline, not a job
  system (`02-core-ecs-assets.md:231`).
- **Not vendored**: moodycamel `concurrentqueue` (BSD-2/Boost), because D2 makes it unnecessary.
  If a later benchmark revision vendors it anyway, it goes under `engine/libs/concurrentqueue/`
  with a `NOTICE.md` row naming the BSD-2-Clause option.
- **Where we improve on theirs**:
  - A wait on a worker is legal: their `TaskHandle::Wait` asserts on workers (`TaskHandle.h:164-172`).
  - Single-thread mode defers jobs to a pumped queue instead of running them at submit, so web
    loads stay async.
  - The Background lane has a progress guarantee: theirs has no aging, so Background can
    starve (`WorkStealingThreadPool.h` class doc).

## 5. Requirements

### Functional

- **R1. Lanes.** Three priorities: `High` (work the frame waits on), `Normal` (default) and
  `Background` (asset decode, LOD, cooks, editor tasks). Dequeue order on a worker is: High
  global, own local deque, Normal global, steal, Background (gated by R9).
- **R2. Work stealing.**
  - Each worker owns a local deque for High/Normal work submitted *from that worker*. The owner
    pops LIFO; thieves steal FIFO in ring order.
  - Background work never enters a local deque.
  - An idle worker spins at most 50 µs, then sleeps on a condvar with a 1 ms backstop.
- **R3. Handles.**
  - `submit(fn, opts)` returns a `Task<T>` with `isReady() / status() / get() / wait() /
    cancel()`.
  - `get()` rethrows the job's exception.
  - Destroying a handle neither cancels nor blocks.
  - Statuses are `Pending, Running, Completed, Failed, Cancelled`.
- **R4. Dependencies.**
  - `submit(fn, opts, deps)` runs only after every dep reaches a terminal status.
  - With `DepPolicy::RequireSuccess` (default), a Failed or Cancelled dep cancels the
    dependent, and that cascades. `DepPolicy::Always` runs it anyway.
  - `Task<T>::then(fn, opts)` is sugar for this.
- **R5. Waits help, never deadlock.** `Task::wait()` and `JobCounter::wait()` on any thread work
  in this order:
  1. Claim the awaited task if it is still Pending and ready, and run it inline. If it is
     blocked on deps, recurse into its Pending deps depth-first.
  2. Help-execute only jobs of the awaited counter (filtered help).
  3. Park, or on the browser main thread spin while calling
     `emscripten_main_thread_process_queued_calls`.

  A thread only ever parks on a task that is running on another thread, so any acyclic wait
  graph completes, whatever the worker count. A wait cycle (a task waiting on itself through
  deps) asserts in Debug and Development builds.
- **R6. `parallelFor(count, body, opts)`.**
  - Legal on any thread, including nested on workers and inside a Background body, with any
    nesting depth.
  - Implements the OpenEngine contract, except that the helper lane is clamped by the lane of
    the forking body: a Background body forks Background.
  - Options: `grain`, `maxHelpers`, `lane`, `stop`. The body may return `bool` (false stops).
  - The first exception is rethrown after units in flight finish.
  - No heap allocation at steady state (helpers come from a slab pool).
  - `parallelForChunked(begin, end, chunk, fn)` and `parallelSort` sit on top.
- **R7. `JobCounter`.** Holds `add(n)` / `run(fn, lane)` / `wait()` / `hasFailed()` / `reset()`,
  with the add-at-submit and decrement-under-mutex rules of the reference. It may live on the
  stack.
- **R8. Cancellation.**
  - `cancel()` on a Pending task wins atomically: it never runs, it releases its slab, and it
    cascades per R4.
  - A Running task observes `jobs::cancelRequested()`, a thread-local view of the task's
    `CancelToken`. Cancellation is cooperative.
  - `CancelToken` can be shared across a group (one load = read + decode + upload continuations).
- **R9. `BackgroundGate` (frame protection).**
  - At most `maxBackground = max(1, workers - reserve)` workers run Background bodies at once.
    `reserve` defaults to 2 on desktop and 1 on mobile and web.
  - Jobs tagged `Heavy` take at most `max(1, maxBackground - 2)` of those slots. Heavy covers
    texture cooks, mesh simplification, shader compiles and nav bakes.
  - Progress guarantee: when `workers >= 3`, the highest-index worker polls Background before
    Normal, so Background work cannot be starved indefinitely.
  - `reserve` is adjustable at runtime. The gate's counters are live, and the pool is never
    rebuilt.
- **R10. `SerialQueue`.** A strand: FIFO jobs on a lane, at most one running at a time, never
  holding a dedicated thread. It replaces single-consumer worker threads that do CPU work.
- **R11. `JobChannel`.**
  - Named channels for blocking work: waiting on processes (cmake, FFmpeg, the compiler) and on
    network or long disk writes.
  - They run on blocking threads that are created lazily up to the sum of the channel caps
    (budget 8 on desktop, 1 on threaded web, 0 on single-thread web, where jobs run inline from
    the pump) and never run compute jobs.
  - Channels: `process` (cap 2), `io-write` (1), `net` (2).
  - A job may not wait on a job of its own channel (Debug assert).
- **R12. `AssetIOService`.**
  - `submitRead({path, offset, size(0 = whole file), priority Low|Normal|High|Critical,
    cancelFlag, gateClass, onData(bytes)→continuation, onSubmitted(Task), onFailure(err)})`.
  - N reader threads (default 2 on desktop, 1 on mobile, 0 on web, where reads run in the
    continuation job) claim the highest-priority request whose gate class has a free
    `BackgroundGate` slot.
  - After the read, the decode continuation is submitted as a Background job holding that slot.
  - Exactly one of `onSubmitted`/`onFailure` fires.
  - `cancel(path, flag)` removes queued requests whose flag is that object (identity match). A
    claimed read discards its result when the flag is set.
  - Reads go through `FileData::newFile`, so resource packs and Android assets work. A
    half-written or renamed file is retried for up to 200 ms (supports S05 hot reload).
- **R13. Shutdown.** `Engine::systemShutdown` runs `AssetIOService::stop()`, then
  `JobSystem::shutdown()`. Pending `submit` tasks become Cancelled. `enqueueDetached` (must-run,
  handle-less) jobs execute in the drain. Blocking and worker threads join. A submit after
  shutdown returns a Cancelled handle and never runs.
- **R14. Allocators.** All allocators are serial-use (owner-synchronised, Debug-asserted via
  `SerialUseCheck`), ASan-poisoned and registered in `MemoryStats` by name.
  - `Arena`: chained blocks, `marker/rewind/reset`, `allocate<T>/allocateArray<T>`, and
    `ArenaAllocator<T>` (a std allocator).
  - `FrameArena`: one arena per frame in flight, reset by `Engine` at frame start.
  - `ScratchScope`: a thread-local nested stack, safe across helped jobs because help is
    stack-ordered.
  - `ObjectPool<T>`: fixed slots, intrusive freelist, double-delete and leak asserts.
  - `RangeAllocator`: TLSF-like two-level best fit over offsets, with `freeAfter(range, stamp)`
    and `reclaimThrough(stamp)` (allocation-free release).
- **R15. `MemoryStats`.**
  - Named records with `Residency {Cpu, Gpu, Mapped}` and a kind (Arena, Pool, Range, Tracked).
  - Each record holds current bytes, peak, capacity and count.
  - `TrackedBytes` lets any subsystem register a counter: texture/mesh pools, Lua heap via its
    allocator, Jolt temp allocator, RmlUi.
  - `snapshot()` is lock-free for readers (seqlock per record).
- **R16. Profiler hooks.**
  - `jobs::setProfilerHooks({onThreadStart(name), onJobBegin(name, lane, workerId),
    onJobEnd, onWaitBegin(kind), onWaitEnd, onIdleBegin, onIdleEnd})`.
  - With no hooks installed, each hook site costs one relaxed load and a branch.
  - Every job has a static `const char*` name (default `"job"`), which S15 consumes.

### Platform & backends

- **R17. Worker policy** (`thread::defaultWorkerCount()`):

  | Platform | Workers |
  |---|---|
  | Windows/Linux/macOS | `clamp(logicalCores-1, 1, 15)` |
  | Android | `clamp(sysconf(_SC_NPROCESSORS_CONF)-1, 1, 4)`. Use CONF, not `hardware_concurrency()`, which misses hot-unplugged big cores. |
  | iOS | `clamp(hw.logicalcpu-1, 1, 4)` |
  | Web with pthreads | `clamp(navigator.hardwareConcurrency-2, 1, 4)` |
  | Web without pthreads | 0 (single-thread mode) |

  Override with the project setting (R27) or env `JE_JOB_WORKERS` (`0` forces single-thread
  mode on any platform).
- **R18. Single-thread mode** (0 workers).
  - `submit` queues; nothing runs at submit.
  - `Engine` calls `jobs::pump(budget)` once per frame: all High/Normal work, then Background
    until the budget (default 4 ms, setting `threading.webPumpBudgetMs`) is spent.
  - `wait`/`parallelFor` run inline per R5/R6. Channels and I/O run inline from the pump.
  - `Engine::setAsyncLoading(true)` is honoured on web: loads complete over several frames
    instead of blocking.
- **R19. Web with pthreads.**
  - `EMSCRIPTEN_THREAD_SUPPORT=ON` links `-sPTHREAD_POOL_SIZE=8 -sPTHREAD_POOL_SIZE_STRICT=2`
    (4 workers + 1 blocking + 1 reader + video + 1 spare).
  - No thread is created after startup outside that pool.
  - The browser main thread never parks (R5).
  - The web export offers "Multithreaded (requires cross-origin isolation)". It emits
    COOP/COEP headers for the dev server and a `coi-serviceworker.js` (design ref
    `Apps/WebLibrary/coi-serviceworker.js`; write our own, its licence is unverified). At page
    load it selects the threaded build only when `crossOriginIsolated` is true, and otherwise
    the single-thread build.
- **R20. Thread identity.**
  - `thread::setCurrentName()` per platform: `SetThreadDescription` looked up dynamically on
    Windows, `pthread_setname_np` (15-char cap on Linux/Android), Apple self-only,
    `emscripten_set_thread_name`.
  - Names: `je-main`, `je-worker-N`, `je-io-N`, `je-block-N`.
  - Every remaining dedicated thread (MCP transports, VideoStream, RuntimeShaderCompiler if it
    is kept) is spawned through `thread::spawnNamed(name, fn)`.
- **R21. OS priority.**
  - Workers run at normal priority. Reader and blocking threads run below normal
    (`THREAD_PRIORITY_BELOW_NORMAL`, `setpriority(+5)` on Linux/Android).
  - Apple: workers `QOS_CLASS_USER_INITIATED`, readers/blocking `QOS_CLASS_UTILITY`.
  - No CPU affinity is set on any platform (D10).
- **R22. One CPU budget.**
  - Jolt uses `JoltJobAdapter : JPH::JobSystemWithBarrier` on our High lane
    (`GetMaxConcurrency = workers+1`), and `JobSystemThreadPool` is deleted.
  - dav1d gets `n_threads` ≤ 2 on mobile and 1 on all web builds (`Av1Decoder.cpp:210`).
  - ThorVG stays at 0.

### Performance (build-server reference machine, Linux x64, Release, 8 workers; ratios are machine-independent)

- **R23. Throughput.**
  - Empty-job submit+execute from one producer: ≥ 3 M jobs/s.
  - `parallelFor` dispatch overhead (1 unit, 0 helpers) ≤ 150 ns.
  - `submit` + `wait` round trip on an idle pool ≤ 20 µs p99.
- **R24. Scaling.**
  - `parallelFor` over 1 M units of ~1 µs: speedup ≥ 0.7 × (workers+1).
  - Nested 64×64 `parallelFor` launched from 8 concurrent worker jobs: ≥ 0.6 × (workers+1),
    with zero deadlocks over 1 000 iterations.
- **R25. Frame protection.** While 200 Heavy Background jobs of 20 ms each are queued, a
  main-thread High `parallelFor` of 8 × 1 ms units completes in ≤ 3.5 ms p99 over 500
  iterations. The same run on the old pool is recorded for comparison.
- **R26. Allocators.**
  - `Arena` 64 B allocation ≤ 5 ns average.
  - `ObjectPool` new+delete ≤ 25 ns.
  - `RangeAllocator` allocate+free with 10 k live ranges ≤ 250 ns p99.
  - Steady-state `parallelFor`, `submit` of slab-sized closures (≤ 96 B captures) and
    `JobCounter` perform 0 heap allocations after warm-up.
  - With no hooks installed, the stats and profiler hooks add ≤ 2 % to R23.

### Editor UX (human)

- **R27. Settings.** Project Settings › Player › **Threading** (Unity-like labels), stored in
  `project.yaml` and editable with undo:
  - Worker Threads (Auto/1-15)
  - Reserved Frame Workers (Auto/0-4)
  - Asset I/O Threads (Auto/0-4)
  - Web Pump Budget (ms)
  - Web: Multithreaded build

  Worker count changes apply on the next Play or launch, and the field says so. Reserve applies
  live.
- **R28. Panel.** Window › Analysis › **Jobs & Memory**, which S15 may host as a tab.
  - Per-worker busy % sparkline (last 120 frames).
  - Queued/running per lane, the gate (taken/max, Heavy taken/max), channels
    (queued/running/cap), the I/O queue by priority, and in-flight asset reads with a Cancel
    button.
  - A sortable `MemoryStats` table with filter, residency column, and current/peak/capacity.
  - The status bar's "loading" count uses R31 semantics.

### Agent UX (MCP, Lua, CLI)

- **R29. MCP tools** (in `editor/mcp/tools/CoreTools.cpp`; names avoid the existing
  `get_job` MCP-operation tool):
  - `get_job_system_stats {window_frames=60}` returns workers, single_thread, lanes{queued,
    running}, gate{max, taken, heavy_max, heavy_taken, reserve}, channels[], io{queued_by_priority,
    in_flight}, per_worker[{busy_pct, jobs}], and totals{executed, stolen, cancelled, failed}.
  - `set_job_system_config {reserved_frame_workers?, web_pump_budget_ms?}` applies live and
    undoably through the settings path.
  - `get_memory_stats {prefix?, residency?, sort?="current"}` returns records[{name, kind,
    residency, current, peak, capacity, count}] and totals by residency.

  All are reachable from `jasperengine-editor call` (S31). Documented in the MCP agent guide.
- **R30. Lua.** No job submission from Lua (Lua states are single-threaded).
  - New read-only `Jobs` table: `getWorkerCount()`, `isSingleThreaded()`,
    `getQueuedCount(lane)`, `getRunningCount(lane)`.
  - `Engine.setMaxResourceLoadingThreads(n)` keeps its name and now sets the gate's max
    Background workers (live, no pool rebuild).
  - `ThreadPoolManager` (Lua) stays one release as a deprecated shim: `getQueueSize()` returns
    the Background-lane queue, `initialize/shutdown` log a one-time deprecation warning and do
    nothing.
  - Stubs (`generate_lua_stubs.py`) and `docs/mcp/guide/lua-api.md` are updated.
- **R31. Resource-count semantics.** `Engine::getQueuedResourceCount()` returns the jobs tagged
  `Asset` that are queued or running, plus queued I/O reads. Idle detection
  (`App.cpp:3059`) therefore cannot report idle while a decode is running.

### Data & compatibility

- **R32.** New `project.yaml` keys sit under `threading:`, all defaulting to Auto. Old projects
  load unchanged. No scene or asset format change, so no S03 version bump.
- **R33.** `ThreadPoolManager.h` is deleted from `engine/core/thread/` and from
  `JasperEngine.h:215` once no C++ user remains. Exported C++ game templates that included it get
  a compile error naming `jobs/JobSystem.h` (a `#error` stub header kept one release).

## 6. Design constraints & decisions

- **D1.** Our own scheduler, about 2.5 k lines. It ports the *contracts* listed in §4, not the
  2.4 k-line `WorkStealingThreadPool.cpp`. No fibers.
- **D2. Queues.**
  - Per-worker deques are bounded rings guarded by a per-worker `std::mutex` (owner contention
    is near zero).
  - Global lanes are mutex-guarded MPMC rings with overflow to a linked spill.
  - This is TSan-clean and portable to every toolchain. Lock-free Chase-Lev is allowed later
    only if R23 fails, behind the same interface.
- **D3. No libc++-gated primitives.** Engine code avoids `std::atomic::wait/notify`,
  `std::counting_semaphore`, `std::latch/barrier`, `std::jthread/stop_token` and
  `std::pmr::memory_resource` (Apple floors, §3). Parking uses mutex+condvar.
  `jobs/Primitives.h` provides `Event`, `Semaphore`, `CancelToken`.
- **D4. Slab pool.** Task envelopes come from a per-thread 128 B slab pool. Closures over 96 B
  fall back to the heap, chosen at compile time and counted in stats.
- **D5. Instance.** One process-wide `JobSystem` is owned by `Engine` (init in `systemInit`,
  shutdown in `systemShutdown`). Free functions reach it (`jobs::submit`, `jobs::parallelFor`).
  Tests construct private instances with `JobSystem(Config)`, and every API also has a
  member form.
- **D6. Migration map.** Every row is mandatory, and done = `rg "std::async|ThreadPoolManager"
  engine/core editor` returns nothing outside the shim.

  | Site | Becomes |
  |---|---|
  | `TextureDataPool.cpp:152`, `SoundPool.cpp:94`, `MeshSystem.cpp:1287` | `AssetIOService::submitRead` + Background continuation (`Asset` tag; texture = Heavy). `std::future` maps become `Task<T>`. `SoundPool::remove` becomes `cancel()` + `wait()`. |
  | `MeshLodPool.cpp:129` | `submit` Background Heavy (drops the `NO_THREAD_SUPPORT` promise branch: single-thread mode covers it) |
  | `MeshSystem::parallelForIndexed` (`:1108-1142`) | deleted; `jobs::parallelFor` |
  | editor `ShaderBuilder.cpp:209`, `RuntimeShaderCompiler` worker (`:338`) | Background Heavy; the runtime compiler uses a `SerialQueue` unless its compiler state is proven thread-safe |
  | `Exporter.cpp:491`, `Generator.cpp:2711,2793`, `Project.cpp:7934,8064`, `ScriptTools.cpp:1642`, `BuildTools.cpp:604,986`, `AssetTools.cpp:1572`, `ProjectWindow.cpp:2570` | `process` channel |
  | `Project.cpp:5898` (scene save), `TerrainMapFileWriter.cpp:99` | `io-write` channel |
  | `AssetTools.cpp:98`, `CodeEditor.cpp:541`, `EntityClipboard.cpp:256`, `GraphicUtils.cpp:348` | Background `submit` |
  | `BackgroundRelease.cpp:21`, `HtmlUiImportQueue.cpp:80`, `ProjectWindow.cpp:194` | `SerialQueue` (Background) |
  | MCP stdio/HTTP threads, `VideoStream` | stay dedicated (long-lived service threads), via `thread::spawnNamed` |
  | `PhysicsSystem.cpp:81-87` | `JoltJobAdapter` |

  Detached threads that captured `this` get an owning `CancelToken` + `Task` held by the owner.
  This fixes the current use-after-free risk on editor shutdown.
- **D7. Main-thread affinity.** Jobs never touch sokol, Lua or ImGui. Results return to the
  main thread by polling `Task::isReady()` (the current pool pattern) or via
  `jobs::postToMain(fn)`, a queue drained by `Engine` each frame.
  `jobs::isMainThread()`/`isWorker()` assert this in Debug.
- **D8. Exceptions** propagate to `get()`/`wait()`/`parallelFor`. `enqueueDetached` logs and
  swallows them.
- **D9. pmr gating.** `Arena::resource()`/`ScratchScope::resource()` compile only under
  `JE_HAS_PMR` (`__cpp_lib_memory_resource` and not Apple below macOS 14/iOS 17). Engine code
  uses `ArenaAllocator<T>` (works everywhere). pmr is allowed in editor code only.
- **D10. No affinity pinning.** Pinning hurts thermals and big.LITTLE scheduling on mobile and
  fights the OS on desktop. QoS/priority (R21) is the only hint.
- **D11. Asset priority** maps loader intent: Critical (scene-blocking), High (visible now),
  Normal, Low (prefetch). A reader always claims the highest admissible priority, FIFO within a
  level.
- **API sketch** (`engine/core/jobs/JobSystem.h`, namespace `jasperengine::jobs`):

```cpp
enum class Lane : uint8_t { High, Normal, Background };
enum class GateClass : uint8_t { Default, Heavy };
struct SubmitOptions { Lane lane = Lane::Normal; GateClass gate = GateClass::Default;
                       const char* name = "job"; uint32_t tags = 0; CancelToken* cancel = nullptr;
                       DepPolicy deps = DepPolicy::RequireSuccess; };
struct Config { int workers = -1 /*auto*/; int reserveForFrame = -1; int blockingBudget = -1; };

template<class F> auto submit(F&& fn, const SubmitOptions& = {}) -> Task<std::invoke_result_t<F>>;
template<class F> auto submit(F&& fn, const SubmitOptions&, std::span<const TaskRef> deps) -> Task<...>;
template<class F> void enqueueDetached(F&& fn, Lane = Lane::Normal);   // never dropped
template<class Body> bool parallelFor(size_t count, Body&& body, const ParallelForOptions& = {});
void postToMain(std::function<void()> fn);
bool cancelRequested();                       // inside a running job
void pump(double budgetMs);                    // single-thread mode; no-op otherwise
Stats stats();                                 // R29 payload
class JobCounter { void add(uint32_t); template<class F> void run(F&&, Lane); void wait(); bool hasFailed() const; void reset(); };
class SerialQueue { explicit SerialQueue(const char* name, Lane); template<class F> Task<...> push(F&&); };
class JobChannel  { JobChannel(const char* name, uint32_t maxRunning); template<class F> Task<...> submit(F&&); };
```

## 7. Acceptance criteria

Unit tests live in `engine/tests/core/jobs/` and `engine/tests/core/memory/` on S02's gtest
target. Each scheduler test runs at worker counts {0, 1, 2, 8}, under TSan in CI and ASan+UBSan.
Benchmarks are the `je-bench-jobs` and `je-bench-memory` executables. `tools/perf/check_bench.py`
compares their JSON output against the thresholds and fails CI on a miss.

- **AC1. Stealing.** `JobsSteal.IdleWorkersStealFromBusyLocal`: a worker job spawns 1 000 child
  jobs into its local deque and busy-waits. ≥ 90 % of the children run on other workers, and the
  `stolen` stat is > 0.
- **AC2. Lanes.** `JobsPriority.HighBeforeNormalBeforeBackground`: on 1 worker, with the queue
  pre-loaded, the execution order respects the lanes. `JobsPriority.BackgroundProgressGuarantee`:
  under a saturating Normal flood on 3+ workers, a Background job completes within 100 ms.
- **AC3. Nested-safe.**
  - `JobsParallelFor.NestedFromWorkersNoDeadlock` passes at workers {1, 2, 8}: 16 worker jobs
    each run `parallelFor(64)`, and each unit runs `parallelFor(64)`, 1 000 iterations under a
    10 s watchdog.
  - `JobsParallelFor.InsideBackgroundForksBackground` passes.
  - Ported `ParallelAlgorithmsTests` (stop, exception, chunked, sort) pass.
- **AC4. Wait semantics.** `JobsWait.ClaimsPendingTaskInline`, `JobsWait.RecursesIntoPendingDeps`,
  `JobsWait.FilteredHelpOnlyCounterJobs` and `JobsWait.CycleAsserts` (death test) pass.
  `JobsWait.WorkerWaitOnSingleWorkerPool` completes. That test deadlocks on the old pool and is
  kept as a regression.
- **AC5. Dependencies and cancellation.**
  - `JobsDeps.RequireSuccessCascadesCancel` and `JobsDeps.AlwaysRunsAfterFailure` pass.
  - `JobsCancel.PendingNeverRuns`: 10 000 random cancel/run races leave every handle terminal,
    with exactly one of ran/cancelled.
  - `JobsCancel.RunningSeesToken` passes.
  - `JobsShutdown.PendingCancelledDetachedRun` passes.
- **AC6. Single-thread mode.** `JobsSingleThread.AllJobsRunViaPump`: with workers = 0, 5 000
  mixed-lane jobs with deps, channels, `SerialQueue` and nested `parallelFor` all complete after
  pumping. `JobsSingleThread.BackgroundRespectsBudget`: one pump with a 4 ms budget over 50 jobs
  of 1 ms each runs ≤ 6 of them.
- **AC7. Gate.** Ported `AssetDecodeGateTests`, plus `JobsGate.BackgroundNeverExceedsMax` (an
  instrumented max-concurrency counter) and `JobsGate.HeavyLeavesTwoSlots` pass.
- **AC8. I/O.**
  - Ported `AssetIOServiceTests` pass: priority order, exactly-once callbacks, cancel by flag
    identity, byte range read and short-read failure, inline mode, stop with queued requests.
  - `AssetIO.RetryHalfWrittenFile` passes.
  - `AssetIO.ReadsResourcePack` reads through `FileData`.
- **AC9. Allocators.** The ported `ArenaTests`, `ScratchScopeTests`, `ObjectPoolTests` and
  `RangeAllocatorTests` pass. `RangeAllocator.FreeAfterReclaim` and
  `MemoryStats.SnapshotConsistentUnderWriters` (TSan) pass.
- **AC10. Zero allocation.** `JobsAlloc.SteadyStateZero` passes. A counting `operator new` in the
  test binary only shows 0 allocations over 10 000 `parallelFor` + `submit`(64 B capture) +
  `JobCounter` cycles after warm-up.
- **AC11. Throughput.** The `je-bench-jobs` thresholds for R23 hold.
- **AC12. Scaling.** The `je-bench-jobs` thresholds for R24 hold.
- **AC13. Frame protection.** The `je-bench-jobs` threshold for R25 holds.
- **AC14. Allocator speed.** The `je-bench-memory` thresholds for R26 hold, including the
  ≤ 2 % hook overhead.
- **AC15. Migration completeness.** The CI grep from D6 is clean. `ThreadPoolManager.{h,cpp}`
  are removed from `engine/CMakeLists.txt:998`. The `TextureDataPool` double-erase is gone.
- **AC16. MCP.** `tools/mcp/tests/test_jobs_memory.py` covers three cases.
  - `get_job_system_stats` returns the R29 schema, and `workers` matches the setting.
  - `set_job_system_config {reserved_frame_workers: 3}` changes `gate.max` live, and undo restores it.
  - `get_memory_stats {prefix:"Texture"}` returns ≥ 1 Gpu record after loading the fixture.
- **AC17. Loading idle check.** An MCP test opens a fixture with 40 textures and polls
  `Engine.getQueuedResourceCount()`. It never reads 0 while `get_job_system_stats.lanes.Background.running > 0`.
- **AC18. Lua.** The Lua test script calls `Jobs.getWorkerCount()` (> 0 on desktop),
  `Engine.setMaxResourceLoadingThreads(1)` (then `gate.max == 1`) and
  `ThreadPoolManager.getInstance():getQueueSize()` (works and warns once).
- **AC19. Physics on our pool.** A Linux run of a scene with 500 rigidbodies shows no `Jolt`
  threads in `/proc/<pid>/task/*/comm`. Only `je-*` names appear for engine-created threads.
  The simulation matches the old pool's body positions to 1e-4 after 300 fixed steps (Jolt
  determinism with the same job graph is not guaranteed, so tolerance-based).
- **AC20. Web single-thread.** S02's browser gate runs an exported fixture with async loading
  on, 3D physics and 20 textures. No console errors, the centre pixel is non-black, and frame
  time stays ≤ 50 ms during the load, against the synchronous-load baseline recorded before the
  change.
- **AC21. Web threaded.** The same fixture built with `EMSCRIPTEN_THREAD_SUPPORT=ON`, served
  with COOP/COEP, logs `crossOriginIsolated=true` and `workers>=1`. With a no-COI server, the
  page falls back to the single-thread build and still passes AC20.
- **AC22. Mobile counts.** A unit test of `defaultWorkerCount()` with injected core counts gives
  the R17 table. Manual check on one Android device: `je-worker-*` threads equal the setting.
- **AC23. Editor.**
  - Manual: change Reserved Frame Workers in Project Settings › Player › Threading. The Jobs &
    Memory panel's gate max updates immediately, and Ctrl+Z reverts it.
  - Manual: start a 200-texture import. The panel shows the Background lane saturated at
    gate max while the Scene view stays interactive (≤ 33 ms frames on the reference machine).
  - `tools/smoke-test.sh` stays green.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20: `std::span`, concepts, `<bit>`).
  - S02 (gtest target, TSan/ASan presets, bench step in CI, browser gate for AC20/21).
- **Blocks / consumed by:**
  - S07 (parallel ECS scheduler on `JobCounter`/`parallelFor`)
  - S05 (I/O retry window, watcher thread naming)
  - S09 (async logger thread via `spawnNamed`; tick scheduler may use `postToMain`)
  - S14 (texture cooks = Heavy gate class)
  - S15 (profiler hooks R16, `MemoryStats` for the VRAM panel, panel hosting)
  - S16/S20 (`RangeAllocator` for UI/particle buffers, `FrameArena`)
  - S19 (nav bakes on Background Heavy)
  - S28 (watchdog reads `jobs::isExecutingJob`, thread names)
  - S30 (web threaded/non-threaded export pair and the COI service worker; S06 defines it, S30
    ships it in the publish pipeline)
- **Merge hotspots:**
  - `engine/core/Engine.cpp` (init/shutdown/pump; S03, S09 and S28 also edit it)
  - `engine/core/subsystem/MeshSystem.cpp` (S07, S11, S26)
  - `engine/core/pool/TextureDataPool.cpp` (S04, S05, S14)
  - `PhysicsSystem.cpp` (S22)
  - `editor/Project.cpp` (S03, S24, S29)
  - `editor/Exporter.cpp` and `engine/CMakeLists.txt` web flags (S01, S02, S30)
  - `editor/mcp/tools/CoreTools.cpp` (S31)
  - `NOTICE.md`

  Land the scheduler and allocators first, behind the old API untouched. Then migrate per
  subsystem, in separate PRs, in D6 order.

## 9. Risks & defaults

- **Subtle scheduler races.** Default: mutex-based queues (D2), TSan on every scheduler test in
  CI, the AC5 race test, and wait-graph cycle detection in Debug.
- **Helping causes re-entrancy** (a waiter runs a job that takes a lock the waiter holds).
  Default: help is filtered to the awaited counter or task (R5), never arbitrary jobs. Code must
  not wait while holding a mutex that the awaited jobs take (documented, Debug lock-order check
  not required).
- **Background starvation, or the frame starving under a flood.** Default: gate (R9) plus the
  dedicated Background-first worker. Tune `reserve` per platform after AC13/AC23 measurements.
- **Jolt adapter behaviour differs.** Default: keep `JobSystemThreadPool` behind
  `JE_JOLT_OWN_POOL=ON` for one release as a fallback switch, then remove it.
- **Apple pmr/atomic-wait floors.** Default: D3/D9. If S01 raises the floors to macOS 14/iOS 17,
  the gates go away without API change.
- **Web pthread pool exhaustion** (dav1d, video, extra threads). Default: the
  `PTHREAD_POOL_SIZE_STRICT=2` abort makes it loud, and R22 caps dav1d.
- **Editor shaders compiling in parallel break thread-unsafe compiler globals.** Default:
  `SerialQueue` until a stress test proves parallel compiles safe.
- **64-bit ranges.** `FileData` uses 32-bit lengths (`FileData.h`). Default: ranges are
  limited to < 4 GiB on all backends. Widening is a separate change.
- **Open question: Lua async helpers** (e.g. a `Jobs.loadAsync` returning a promise polled from
  Lua). Default: none in S06. Revisit with S09's tick scheduler.

## Deviations

Recorded by the implementer (branch `feat/s06-jobs-memory`).

- **C++ standard and D9.** The code was written in C++17 while S01 was pending. After S01
  merged it builds as C++20, with no API change. S01's portable subset bans `std::pmr`
  entirely (`tools/check-cxx-subset.py`), so D9's gated `Arena::resource()` and
  `ScratchScope::resource()` are dropped: `ArenaAllocator<T>` serves std containers
  everywhere.
- **Test target.** The S06 suites are part of S02's `jasperengine-core-tests`
  (`engine/tests/core/{jobs,memory,io,thread}`, `tools/build.sh unit-tests`, S02's TestMain).
  The resource pack the asset I/O suite reads is written at static initialisation, before the
  engine starts. The death test sets `death_test_style=threadsafe` itself. The binary links
  the allocation counter: `--wrap=malloc` on Linux. Elsewhere, the zero-allocation tests skip.
  The benchmarks `je-bench-jobs` and `je-bench-memory` build in the same tree. The spec's suite
  names map onto parameterised suites: `Workers/JobsSteal.*`, `Workers/JobsWait*.*` and so on,
  run at {0, 1, 2, 8} workers (cases that need workers skip at 0). TSan runs are
  `SANITIZE=thread tools/build.sh unit-tests`.
- **Zero-allocation counter (AC10).** The vendored ThorVG replaces the global plain
  `operator new/delete`, so the test binary cannot define its own. It counts through
  `-Wl,--wrap=malloc,calloc,realloc` and replaces only the aligned forms. The steady-state test
  warms up with every worker blocked, so the queues' rings reach their working size first.
- **R2: High work always goes to the global High queue,** including from a worker. Only Normal
  work enters the local deques. A local High job could otherwise wait behind a long Normal
  chain on a busy worker, and it would sit below the High global queue in R1's order.
- **D4: slab sizes.** Bare jobs use 128 B envelopes (96 B of closure, as specified). Tasks need
  the result slot, the dependents list and the refcount, so `TaskState` uses a 256 B slab class.
  Larger closures fall back to the heap and are counted (`totals.heap_envelopes`).
- **D7: `postToMain`** is a queue of our own, drained by `Engine` at the start of each frame. S09
  can move it into its tick scheduler.
- **R12: callback ordering.** `onSubmitted`/`onFailure` run before the task can be seen terminal
  (not after it), so "callback ran" is guaranteed once `isReady()` is true. Exactly-once holds.
- **R27: worker and I/O thread counts apply on the next launch.** Editor Play runs in-process,
  so the editor keeps the counts it started with. Reserved Frame Workers and the web pump budget
  apply live.
- **R19: threads sized to the pthread pool.** S02's browser gate found a threaded web build
  asking more threads of its Emscripten pool than it held. That pool was 4, and the old pool
  started hardware_concurrency() - 1 threads. The pool is now a CMake value,
  `JASPERENGINE_WEB_PTHREAD_POOL_SIZE` (default 8), passed both as `-sPTHREAD_POOL_SIZE` and as
  `JE_PTHREAD_POOL_SIZE`. The worker policy caps threaded web at pool - 3 workers (and 4).
  Settings and `JE_JOB_WORKERS` are capped the same way, and asset readers only take what the
  workers, the blocking thread and a video decoder leave
  (`JobsWorkerPolicy.ThreadedWebWorkersFitThePthreadPool`). `GATE=sample-project-threads
  tools/build.sh web-gate` passes.
- **R20: web threads keep no name.** `emscripten_set_thread_name` links only with
  `-sPTHREADS_PROFILING`, and a Debug threaded build failed to link with it. The other
  platforms name every thread as specified.
- **R19: Multithreaded web export** is a Project Settings checkbox (Player > Threading > Web:
  Multithreaded, `player.threading.web_multithreaded`), not a field of the export dialog, so
  the CLI and MCP exports pick it up too. COOP/COEP are emitted as a `_headers` file (Netlify
  and Cloudflare Pages format) plus `coi-serviceworker.js`. S02's dev server serves the headers
  itself. On threaded web the asset reader count defaults to 0, not 1: reads run inside the decode
  continuation on a worker (inline mode), and the pool's reader slot stays spare. Asset I/O
  Threads can raise it.
- **D6: own-channel waits.** A job may not wait on its own channel (asserted), so:
  - the generator builds inline when it is already inside a channel job;
  - the exporter polls while inside one;
  - BuildTools and ScriptTools watchers get their own channels (`build-watch` cap 2,
    `script-build-watch` cap 1) instead of the `process` channel the table names, because they
    wait on `process` builds.
- **Shader builds count as asset jobs.** Editor shader builds carry `Tag::Asset`, so
  `getQueuedResourceCount` (the loading indicator) covers them as it did with the old pool.
- **AC17 condition.** The test polls `Engine.getQueuedResourceCount()`,
  `ResourceProgress.getActiveBuildCount()` and the Background running count from Lua. It fails
  when the count reads 0 while a build is active, or while Background bodies run across the
  read. It first waits for the editor's own Background work (the Project window's thumbnails
  of the new files) to finish, because thumbnails are Background work but not asset loads.
- **R15: RmlUi memory** is not a separate record. Its textures go through the engine's sokol
  texture path, so they are counted under `Texture.GPU`.
- **Occupancy `sinceNs`** (for S28) is the time when the sampler first saw the current job, not
  when the job began. This keeps a clock read off the job path.
- **Benchmarks: worker count follows the machine.** `je-bench-jobs` and `je-bench-memory` run
  `min(8, hardware threads - 2)` workers (`--workers n` overrides it). The budgets assume that
  the pool and the producing thread each have a core. With 8 workers on an 8-thread machine,
  the run measures preemption, not the scheduler: R25 is 4.4 ms. One core is also left to the
  rest of a shared build server. With 7 workers on server C (8 vCPUs), one busy neighbour
  moved R25 from 2.2 ms to 4.5-12 ms in some runs. With 6 it holds.
- **D2: a lock-free ring in front of the High and Normal lanes.** D2's mutex-guarded rings
  reached 1.0-1.2 M jobs/s from one producer (R23 asks for 3 M). The limit was cache-line
  traffic between the producer and its consumers on the shared lock. Publishing alone ran at
  14 M jobs/s.

  Following D2's escape hatch, `detail::FastLane` puts a bounded lock-free ring in front of
  the High and Normal lanes, behind the same interface. The ring is Vyukov's sequenced MPMC
  ring, 32 K slots (512 KiB per lane, counted in `Jobs.Queues`). It overflows into the mutex
  ring, and FIFO order holds across the overflow. Workers take runs of up to 32 ready slots
  with one CAS, and the others steal from the taker's deque. The Background lanes keep the
  mutex rings: their gate already serialises them.

  With coalesced wakes and no shared per-job counter, R23 reaches 5.9-8.5 M jobs/s on
  server C. `JobsFastLane.*` covers FIFO across the overflow and exactly-once delivery with
  several producers and consumers, and it is clean under TSan.
- **RangeAllocator: TLSF good fit, not exact best fit.** It takes the most recently freed range
  of the request's bin when that range fits, else the head of the first larger non-empty bin.
  Both are constant time. When nothing larger is free, it walks the request's own bin, so a
  request the space can hold is never refused (`AFitBehindTheBinsHeadIsFoundWhenNothingLargerIsFree`).
  Walking bins for the smallest fit cost 330-440 ns p99, against R26's 250. Now it runs at
  150-180 ns p99, 105 ns mean.
- **R26 measurement methods.**
  - The range p99 comes from batches of 256 timed allocate+free pairs, with the median of 5
    runs' p99.
  - R25 frame protection is the best of ten 500-frame runs' p99. On server C, the median
    frame is 2.05-2.1 ms in every run. With neighbours busy (load 6-13), a run's p99 ranges
    from 2.3 to 14 ms; quiet runs give 2.2-2.5 ms.
  - R24 nested speedup times both sides in blocks (serial in 5 blocks of 4 runs, parallel in
    blocks of 50 of the 1 000 iterations) and takes the fastest block of each. A neighbour on
    the server slows some blocks; the scheduler's speedup is the undisturbed one.
  - "The stats add ≤ 2 % to R23" is measured where it is stable: a multi-threaded throughput
    run varies by about 10 % between runs, more than the budget. The bench first takes the
    stats' cost per job on one thread: 0 workers and the pump, as the median relative
    difference of 41 paired, alternating trials (about 12-14 ns). It divides that by the CPU
    time R23's workers have per job (workers × R23's median wall time per job). That gives
    0.9-1.4 % on server C.
  - Workers count their jobs with a plain increment instead of a locked add.
- **Background gate peaks.** `get_job_system_stats` reports `gate.peak` and `gate.heavy_peak`,
  the most slots taken at once over the window, sampled each frame. The panel shows them too.
  Without them, a short load storm cannot be shown to saturate the gate: sampling the running
  count every frame misses it.
- **TSan on GCC 11** needs `setarch -R` (high ASLR entropy on current kernels). In Docker it
  also needs `--security-opt seccomp=unconfined`. Timed condvar waits go through
  `jobs/detail/TimedWait.h` because that TSan does not intercept `pthread_cond_clockwait`.
- **MCP tool budget.** The three R29 tools take 900 characters after trimming. The branch
  base was already at 100.6 K, over the ~100 K target, so the total is 101.5 K. The overage
  predates S06. `focus_window` also gains `jobs_memory`.

- **R2: the 1 ms backstop grows while idle** (fix/bf1-bugs). With a fixed 1 ms backstop
  every worker woke 1000 times a second, and each wake-up spun another 50 µs: 8 idle workers
  used 45 % of a core (61 % was seen in the core test process). A worker now spins only when it
  runs out of work, parks again without spinning when a wake-up finds nothing, and doubles its
  backstop on each such wake-up, from 1 ms up to 128 ms; running a job resets both. Producers
  wake parked workers (the propagation read of the sleeper count and the gate release are
  sequentially consistent with a sleeper's count), so the backstop only bounds a missed wake.
  `JobsIdle.IdleWorkersUseUnderOnePercentOfACore` measures the workers' own CPU clocks;
  `JobsIdle.AParkedPoolWakesForNewWorkAtOnce` checks that a long-parked pool still starts new
  work at once.
- **R22: `JoltJobAdapter` joins its envelopes** (fix/bf1-bugs). Jolt jobs go through a
  `JobCounter` of the adapter instead of `enqueueDetached`, and the adapter's destructor waits
  on it, because an envelope still queued after a barrier's waiter ran its job frees that job
  through the adapter. A `JobCounter` built for the process system resolves it at each call.

### Acceptance evidence (server C, 8 vCPU VM, after merging oe-port with S01, S02 and S03)

- **AC1-AC10, AC22.**
  - `WERROR=1 tools/build.sh unit-tests`: `jasperengine-core-tests` passes 608 of 608 (the
    S06 suites with S02's and S03's), and `jasperengine-editor-tests` passes 247 of 247.
  - `SANITIZE=thread UNIT_SUITE=core tools/build.sh unit-tests` (GPU suites filtered out):
    606 of 606, no race report.
  - The unity builds (`UNITY=1`) of the engine, the editor and the core tests compile.
- **AC11-AC14.** `je-bench-jobs`, `je-bench-memory` and `check_bench.py` (6 workers on the
  8-vCPU server, runs started at load < 1): all budgets hold in three consecutive runs.
  - R23: 7.7-8.1 M jobs/s, dispatch 26 ns, round trip p99 1-2.7 µs.
  - R24: 0.91-1.02 flat, 0.78-0.79 nested, 0 deadlocks.
  - R25: 2.13-2.27 ms p99.
  - R26: arena 2.9 ns, pool 4.7-4.9 ns, range p99 164-175 ns, stats 1.1-1.7 %.
- **AC15.** `rg "std::async|ThreadPoolManager" engine/core editor` finds only the Lua shim.
  S03's Auto Save backup writes, which arrived in a detached `std::thread`, run on the
  io-write channel.
- **AC16-AC18.** `tools/mcp/tests/test_jobs_memory.py` passes.
- **AC19.**
  - `JobsJolt.AdapterMatchesJoltsOwnPool`: 500 boxes stepped 300 times with `JoltJobAdapter`
    and with Jolt's `JobSystemThreadPool` end with identical positions (difference 0 m;
    tolerance 1e-4). The stack topples, so the scene is not at rest.
  - `test_physics_runs_on_the_job_workers` (200 dynamic bodies in Play) finds no Jolt thread
    and no thread created like the main one.
- **AC23.** `tools/mcp/tests/test_jobs_memory_ui.py`, driven by xdotool in the harness's Xvfb:
  - Edit > Project Settings > Player > Threading > Reserved Frame Workers = 3, then OK. The
    panel's gate max goes from 5 to 4 at once. Ctrl+Z in the panel brings it back to 5.
  - Ctrl+Z in the Jobs & Memory panel now undoes in the project history, where the setting
    lives. Before, it undid the selected scene's edits.
  - 200 textures (512² noise PNGs) load at once. The gate peaks at 5 taken, with Heavy at 3 of
    3, its maximum (texture decodes are Heavy, so R9 gives them max - 2).
  - Editor frames on llvmpipe (no GPU on the server): idle median 90 ms, p95 98 ms; during
    the storm median 116 ms, p95 126 ms. The 33 ms budget is for the reference machine. The
    software renderer's idle frame is already slower, so the test allows the storm 1.5× the
    idle p95.
  - Screenshots are written to `$JE_EVIDENCE_DIR`.
  - The smoke test passes.
- **AC20, AC21 (S02's browser gate).**
  - `tools/build.sh web`, then `tools/build.sh web-gate`: PASS (ready, centre pixel, no
    console errors).
  - `WEB_THREADS=1 tools/build.sh web`, then `GATE=sample-project-threads tools/build.sh
    web-gate`: PASS (crossOriginIsolated, ready, centre pixel, no console errors).
  - Before the fix, the threaded gate failed in two ways. Its link lacked `-pthread` (the S02
    merge kept the compile flag only), so `pthread_create` returned EOPNOTSUPP. Its Debug link
    failed on `emscripten_set_thread_name`.
  - Not run: an exported game behind a server without cross-origin isolation (the loader's
    fallback to the single-thread player). That needs S30's publish pipeline.
- **Builds.** `WERROR=1 tools/build.sh linux`, `tools/build.sh android` and the smoke test
  pass.
