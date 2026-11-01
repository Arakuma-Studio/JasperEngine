# S09 — Core utilities: async multi-sink logger, RAII subscriptions, tick scheduler

Wave 1. Depends on S01 (C++20) and S02 (gtest, sanitizers, benchmarks, web gate).

## 1. Goal

Give the engine and editor three small core pieces that a lot of later work relies on:

1. A **logger** with levels, categories, source location, several sinks, background writing, backtraces on errors and a log tail that survives a crash. The ~2,300 existing `Log::*` / `Out::*` call sites keep compiling and behaving the same.
2. **RAII event subscriptions** (`Signal<Args...>` + `Subscription`). They replace string-tag bookkeeping in new C++ code and fix the re-entrancy bugs in `FunctionSubscribe`. Lua and C++-script callbacks keep their CrashGuard protection.
3. A **tick/time scheduler** with cancel handles. It supports delayed calls, repeating timers, per-frame and fixed-step callbacks, and game, unscaled and real clocks, and it respects pause. Lua gets it as `Timer`, coroutine-friendly. Lua has no timer API today.

S05 (watcher), S03 (autosave), S06 (main-thread continuations), S23 (click-to-source) and S28 (crash flush in games) are built on these pieces.

## 2. Scope

**In scope**
- The logger core, its sinks (stdout/stderr console, rotating file, in-memory store for the Console and MCP, callback/MCP stream, Android logcat, Emscripten console, Windows `OutputDebugString`, Apple `os_log`), per-sink and per-category levels, rate limiting, backtraces, crash flush wired into `editor/util/CrashHandler.*`, and Lua `Log` additions.
- `Signal`/`Subscription`, and `FunctionSubscribe` rebuilt on top of the same core with the same public API. The owner-key removal path replaces tag-substring scans.
- The `Scheduler` (engine game instance + editor instance), `TimerHandle`, `TimerGroup`, the Lua `Timer` API with coroutines, MCP tools, docs and tests.

**Out of scope**
- `std::format` as the logging API. We keep printf, because the call sites use it and `-Wformat` checks them.
- Cross-DLL "shared state redirection" (their `RedirectToSharedState`). Our state lives in the engine library (R15).
- Network/remote log sinks.
- A GET SSE stream on the MCP HTTP transport.
- Migrating `ObjectManager::destroy(delay)` (`engine/core/manager/ObjectManager.cpp:303`) or the HTML-UI anime timers onto the scheduler. They can move later.
- Nothing from the README do-not-copy list is involved.

## 3. Current state

**Engine logging.** The API is `engine/core/Log.h` / `Log.cpp`: printf-style `Log::print/verbose/debug/warn/error`. Every call goes synchronously to the virtual `System::platformLog(type, fmt, va_list)` (`engine/core/System.cpp:180`, which does `printf("(%s): ")`). Overrides:
- Android: `__android_log_vprint` with the tag `"JasperEngine"` (`engine/platform/android/JasperEngineAndroid.cpp:220`).
- Editor: formats into `Out` (`editor/Platform.cpp:106`).
- Tests: capture into a vector (`engine/tests/htmlui/harness/TestSystem.cpp:80`).

Other facts:
- `Log::debug` compiles to nothing under `NDEBUG`.
- There are no categories, no runtime level, no source location, no async path, and no Emscripten, Windows debugger or Apple sink.
- Lua `print` and `Log.*` go through `Log::print("%s")` (`engine/core/script/LuaBinding.cpp:654`, `engine/core/script/binding/CoreClassesLua.cpp:1485`).
- Usage: `Log::error` 478 sites, `Log::warn` 194, other `Log::*` 48.

**Editor logging.** `editor/Out.h` / `Out.cpp`:
- Levels are Info/Success/Warning/Error/Build (`LogType`, `editor/window/ConsoleWindow.h:14`).
- Every call, on the caller thread and synchronously:
  - takes a mutex and writes a file line with `std::endl`, i.e. an fflush per line;
  - appends a `std::string` copy to a 20,000-entry / 16 MB deque (`Out::getLogsSince`, used by MCP `read_console`, `editor/mcp/tools/ConsoleTools.cpp:84`);
  - for worker threads, also allocates a closure for `enqueueMainThreadTask` to reach the ConsoleWindow.
- The file rotates once (`.1`) at 2 MB, and only when it is opened.
- Without a window, messages go to stdout/stderr as `[ERROR] …`.
- Usage: about 450 `Out::*` sites.
- `tools/smoke-test.sh:99-101` greps `(ERROR)` and `[ERROR]`, so these markers are a contract.

**Crash handler.** `editor/util/CrashHandler.cpp` uses only async-signal-safe calls. It appends `FATAL` and a backtrace to `editor.log` (`App.cpp:3847`). Lines that were "logged" but not yet written do not exist today because writes are synchronous. They will exist once logging is async, so the crash flush is mandatory (R10). The handler is editor-only; S28 adds games.

**MCP.**
- `editor/mcp/McpServer.cpp:448` advertises the `logging` capability.
- `logging/setLevel` is a no-op (`:604`).
- `notifications/message` is never sent.

**Events.** `engine/core/util/FunctionSubscribe.h` (367 lines) is a vector of `std::function` plus a parallel vector of string tags. There are 187 declarations: `Engine::on*`, components, physics, Ease.

Defects:
- **Re-entrancy UB.** `call()` holds `auto& function = functions[i]` while the callback runs. A callback that `add`s (reallocation) or `remove`s/`clear`s (element moved or destroyed) the same event invalidates the running `std::function`. Removal during `call` also skips the next subscriber.
- **Teardown by substring scan.** Script teardown runs `removeByTagSubstring("_<addr>_")` over every event of the Engine, every component array and physics. That is 69 call sites: `Engine.cpp:830`, `Scene.cpp:74`, `PhysicsSystem.cpp:454`, `LuaBinding.cpp:721`, and generated code (`editor/Generator.cpp:781`). The cost is O(events × subscribers × strlen).
- **Leak with explicit tags.** Lua `RegisterEvent(self, ev, "m", tag)` with an explicit tag omits the table address (`LuaBinding.cpp:395`). Those subscriptions are never removed when the script is released.
- **Global state in a header.** `FunctionSubscribeGlobal::getCrashHandler()` is a header-inline static. With `JASPERENGINE_SHARED` on Windows, each DLL gets its own copy.

What works and must stay:
- **CrashGuard.** When the editor's Play installs a crash handler (`editor/Project.cpp:7871`), every callback (Lua and C++ scripts) runs under `callWithCrashGuard` (`engine/core/util/CrashGuard.h`). A crashing subscriber is removed, Play pauses and then stops. `LuaFunctionBase::call` sends Lua errors to the same handler.
- **The `REGISTER_*` macros are public API.** They are emitted into users' C++ scripts by `editor/window/dialog/ScriptCreateDialog.cpp:160`.
- **Jolt threads.** Physics events are invoked from Jolt job threads under `PhysicsSystem::getCallbackMutex3D()` (`engine/core/util/JoltPhysicsAux.h:276`).

**Timers.** There are none:
- `docs/mcp/guide/lua-api.md:536` says "No timer API is bound".
- The editor has an unbounded FIFO (`App::enqueueMainThreadTask` / `processMainThreadTasks`, `editor/App.cpp:3180,3813`, 28 users) with no delays and no cancellation.

Clocks today:
- `Engine::systemDraw` (`engine/core/Engine.cpp:1219-1303`) advances `gameTime` whenever the engine is not paused and not held. Editor Play-Pause uses `Engine::pauseGameEvents` (`Engine.cpp:365`), which only disables subscribers, so **`Engine::getGameTime()` keeps running while the editor's Play is paused**. The scheduler cannot use it as its game clock.
- `HtmlUIClock` already models the "unscaled but pause-aware" clock (`Engine.cpp:1242`).

## 4. Reference (OpenEngine, MIT, `external/OpenEngine/Engine/Modules/`)

**`Logger/`** (`Include/Logger/Logger.h`, `Source/Logger.cpp`, `LogSink.h`, `FileSink.*`, `RingBufferSink.*`, `CallbackSink.*`, `Backtrace.*`, `Tests/*`)

Take:
- the gate every entry shares (`ShouldLog` is one relaxed atomic load);
- formatting on the producer, so only owned bytes cross the queue;
- batch swap on the drain thread;
- bounded flush latency (`kMaxUnflushedSinkLatency`);
- `FlushForCrash(timeout)`, which never takes a sink lock without a deadline;
- a re-entrancy drop for logs made on the drain thread;
- synchronous Critical;
- the synchronous fallback on threadless wasm (`Logger.cpp:133`);
- rotation-disabled-after-failure (`FileSink.h:85`);
- Apple `backtrace_symbols` parsing (`Backtrace.cpp:53`);
- the shared DbgHelp mutex (`Backtrace.h`);
- test ideas (`LoggerCrashFlushTests`, `FileSinkRotationTests`, `LoggerAsyncTests`).

Improve on:
- Their queue is an unbounded `std::vector<LogMessage>` with a heap `std::string` per record. Ours is a bounded byte ring with zero allocations.
- They have no categories and no rate limiting.
- Their backtraces are symbolized on the caller. Ours capture raw addresses only and symbolize on the drain thread.
- Their `FlushForCrash` uses mutexes and futures, which are not async-signal-safe. Ours uses only atomics plus sleep from signal context.
- Their source strings use 2 KB `OwnedSourceLocation` statics. Ours are bounded copies into the record.

**`Events/Include/Events/Event.h`** (180 lines)

Take:
- the non-template, move-only handle;
- a `weak_ptr` to the subscriber state, so the handle is inert if the event died first;
- "unsubscribing does not join a running callback".

Improve on: `Invoke` copies the whole `std::function` vector on **every** call. That is an allocation per `onUpdate` per frame, which is not acceptable. Ours uses tombstones and deferred compaction with zero allocations on invoke. They have no CrashGuard and no owner keys.

**`Scheduler/`** (`Include/Scheduler/Scheduler.h`, `Source/Scheduler.cpp`, `Tests/SchedulerTests.cpp`)

Take the semantics:
- work scheduled during `ProcessDue` runs in a later call;
- FIFO for the next tick;
- `(due, seq)` ordering;
- cancelling the running task is a no-op;
- scheduling from any thread, running on the owner thread.

Improve on: they have only wall-clock time, no repeat, no game time and no pause, and they use a `std::map` plus an `unordered_map` with allocations per task. The `ChainBuilder` is replaced by Lua coroutines and `TimerGroup`.

**Licence.** We write our own implementations; the designs differ as described above. If code is lifted verbatim (Apple backtrace parsing, rotation logic, test bodies), add the OpenEngine entry to `NOTICE.md` ("Copyright (c) 2026 Gamecrafters Guild Limited and contributors", MIT) and keep their header in the copied file.

## 5. Requirements

### Functional — logger

- **R1 Levels.** `Trace < Debug < Info < Warn < Error < Fatal`, plus `Off`. Mapping from existing calls:

  | Existing call | New level and attributes |
  |---|---|
  | `Log::print` | Info |
  | `Log::verbose` | Trace |
  | `Log::debug` | Debug (still compiled out under `NDEBUG`) |
  | `Log::warn` | Warn |
  | `Log::error` | Error |
  | `Out::info` | Info |
  | `Out::success` | Info + flag `Success` |
  | `Out::warning` | Warn |
  | `Out::error` | Error |
  | `Out::build` | Info, category `build` (Warn/Error when the text is a compiler diagnostic, keeping today's problem classification) |

- **R2 Categories.**
  - Registered statically with `JE_LOG_CATEGORY(Name, "name", defaultLevel)`, or from Lua; at most 64 in total.
  - Built-in: `engine` (default for `Log::*`), `editor` (default for `Out::*`), `build`, `lua`, `script`, `render`, `shader`, `physics`, `audio`, `asset`, `htmlui`, `io`, `mcp`.
  - Each category has a runtime minimum level.
- **R3 Gate.** A call is formatted only if `level >= effective(category)`, where effective = max(category level, lowest level any sink accepts for that category). The value is precomputed per category and stored in one `std::atomic<uint8_t>`. A filtered call does no formatting, takes no lock and allocates nothing.
- **R4 Record.** Each record carries:
  - id (process-wide, monotonic from 1, assigned at enqueue; same contract as today's `Out::lastLogId` cursors);
  - level, category, flags;
  - wall time and engine frame number;
  - thread id and name;
  - optional source (file ≤ 160 B, function ≤ 64 B, line), copied into the record;
  - formatted text;
  - optional backtrace (≤ 32 raw addresses).

  Text longer than 64 KB is truncated with a `…[truncated N bytes]` suffix.
- **R5 Source location.**
  - New macros `JE_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL(cat, fmt, ...)` capture `__FILE__/__LINE__/__func__`. Legacy `Log::*` / `Out::*` calls carry none.
  - Lua `Log.*` and `print` carry the Lua chunk name and current line (`lua_getinfo "Sl"`, level 1), computed only when the gate passes.
- **R6 Sinks.** Each sink is an object with its own minimum level, a category mask and a delivery mode: `Inline` (on the producer thread, under the sink's own lock) or `Async` (drain thread). Sinks can be added and removed at runtime.

  | Sink | Delivery | Installed by |
  |---|---|---|
  | `MemorySink` (Console + MCP store; byte arena; editor 16 MB / 20,000 entries as today, games 256 KB, can be disabled) | Inline | editor; games optionally |
  | `ConsoleSink` (stdout, Warn+ to stderr; writes to fd 1/2 at write time so the MCP stdio redirect in `editor/mcp/McpLaunch.cpp:84` still applies) | Async | editor CLI/headless, desktop games |
  | `RotatingFileSink` (size N bytes, K files `.1…K`; rotates at open **and** at runtime; disables rotation after a failed rename and keeps appending) | Async | editor (`editor.log`, 2 MB × 3), desktop games (opt-in) |
  | `CallbackSink` (`std::function<void(const LogRecord&)>`; MCP stream, tests) | Async | MCP server |
  | `AndroidLogSink` (`__android_log_write`, tag `JasperEngine`, priority by level) | Async | Android |
  | `EmscriptenConsoleSink` (`emscripten_log` with `EM_LOG_CONSOLE` + WARN/ERROR flags so devtools colour them) | Inline (web is synchronous, R14) | Web |
  | `DebuggerSink` (`OutputDebugStringA` when `IsDebuggerPresent()`) | Async | Windows |
  | `AppleLogSink` (`os_log` with subsystem = bundle id) | Async | iOS/macOS games |
  | `PlatformSink` (forwards to the virtual `System::platformLog`, so `TestSystem` and third-party overrides keep working) | Async | games whose `System` overrides `platformLog` |

- **R7 Existing output formats are preserved byte-for-byte** apart from new optional suffixes:
  - game console: `(ERROR): msg`;
  - editor stdout: `[ERROR] msg`;
  - file: `YYYY-MM-DD HH:MM:SS [ERROR] msg`.

  The file line may append ` [category]` and ` (file:line)`. The `tools/smoke-test.sh` `fatal_lines` patterns must keep matching. The session banner `=== JasperEngine Editor <ver> started at … ===` stays.
- **R8 Backtraces.**
  - Error and Fatal records from C++ capture raw frames on the calling thread: `backtrace()` / `CaptureStackBackTrace` / `_Unwind_Backtrace` on Android; none on web.
  - Records from Lua carry `NoBacktrace`, because the Lua traceback is already in the text.
  - Symbolization (`dladdr`/`backtrace_symbols` / DbgHelp under one shared DbgHelp mutex, also used by `CrashHandler.cpp`) happens in the async sinks only. Addresses in unloaded modules print as `module?+0x…`.
  - Defaults: on in the editor and in debug builds, off in shipped release games (project setting). At most 10 backtraces per second process-wide.
- **R9 Rate limiting.**
  - `JE_LOG_ONCE(...)` and `JE_LOG_EVERY(seconds, ...)` keep per-call-site static state.
  - Safety valve: more than 200 records per second from one key (format-string pointer + level; for the pass-through formats `"%s"`/`"%s\n"` the key is a hash of the first 64 bytes of the text) are suppressed. One record `suppressed N similar messages: "<first 80 chars>"` is emitted per key per second while suppression lasts.
  - Lua `print` goes through the same valve.
- **R10 Crash flush.**
  - `Logger::flushForCrash(timeout)` is async-signal-tolerant. From a signal or SEH filter it uses only atomic loads and stores plus `nanosleep`/`Sleep`, and waits until the drained sequence reaches the sequence observed at entry, or until the deadline.
  - It returns false when called on the drain thread itself or when the deadline passes.
  - `CrashHandler::report` calls it (300 ms) **before** writing the FATAL block. On false it writes `log tail incomplete: N records unwritten`.
  - The file sink issues a raw `write()` per batch, so "drained" means "in the OS".
  - The `std::terminate` handler and the Windows unhandled-exception filter do the same.
- **R11 Fatal.** Fatal does the following, then lets the caller abort if it chooses:
  1. flushes the queue (bounded, 500 ms);
  2. dispatches synchronously to all sinks on the caller thread, each sink lock taken with a deadline;
  3. flushes.
- **R12 Queue full.** The queue is a bounded byte ring: editor 4 MB, desktop games 1 MB, mobile/web 256 KB.
  - When it is full, Trace/Debug/Info records are dropped immediately.
  - Warn+ records wait up to 5 ms for space, then are dropped.
  - Drops are counted, and when space returns one `Warn` is emitted: `[log] dropped N messages (queue full)`.
  - A producer is never blocked longer than that.
- **R13 Lifecycle.**
  - The logger works before `init()`: records dispatch synchronously to a stderr fallback.
  - `Logger::shutdown()` drains and flushes. It runs at normal exit (editor `App` destructor, engine shutdown, Android `onDestroy`).
  - A record logged on the drain thread from inside a sink is written to the remaining sinks synchronously, except the sink that produced it (no recursion).

### Platform & backends

- **R14 Web without pthreads.** No drain thread; every sink is delivered inline on the caller. With `EMSCRIPTEN_THREAD_SUPPORT` (`engine/CMakeLists.txt:208`) web still uses inline delivery (console calls from a worker are proxied anyway). The scheduler and signals behave the same as on desktop.
- **R15 One copy of global state.** All global state lives behind exported non-inline functions in the engine library: logger state, category registry, signal owner index, scheduler instances, and the crash-handler hook that replaces `FunctionSubscribeGlobal`. This holds with `JASPERENGINE_SHARED` (`engine/core/Export.h`). No header-inline function-local statics hold process state.
- **R16 Thread names.** The drain thread is named `je-log`, through the platform thread-naming call, so it shows up in profilers and debuggers. It is not a job-system worker (S06 must not count it).

### Functional — signals

- **R17 `Signal<Args...>`** (`engine/core/util/Signal.h`).
  - `subscribe(fn, ownerKey = 0) -> Subscription`. `Subscription` is non-template, move-only and `[[nodiscard]]`; it unsubscribes on destruction and is inert if the signal is gone.
  - Also: `invoke(args...)`, `size()` / `empty()` (atomic count), `clear()`, `removeOwner(key)`.
  - `invoke` allocates nothing and iterates slots up to the count at invoke start.
  - Removing during an invoke tombstones the slot (the callback being invoked stays alive until it returns), and a removed subscriber that has not run yet is skipped.
  - Subscribers added during an invoke are not called in that invoke.
  - Compaction runs after the outermost invoke. Nested invokes are allowed.
- **R18 Threading.** `Signal` is externally synchronized: any thread may invoke if the owner serializes, as `PhysicsSystem` does with `getCallbackMutex3D`. `ThreadSafeSignal<Args...>` uses a mutex and a copy-on-write `shared_ptr<const Slots>` snapshot. It allocates on subscribe and unsubscribe only, never on invoke, and is meant for editor services published from background threads.
- **R19 `FunctionSubscribe` keeps its public API and Lua bindings.**
  - Kept: `add` overloads, `remove(tag)`, `removeByTagSubstring`, `call`, `callRet`, `operator()`, `operator=`, `setEnabled`, `hasSubscribers`, `clear`, and the `__call/call/add` bindings in `CoreClassesLua.cpp:935+`.
  - It is reimplemented on the R17 slot core: each slot holds `{id, ownerKey, tag, callback}`, and add-same-tag-replaces is kept.
  - It gains `subscribe(fn, ownerKey) -> Subscription` and `removeOwner(key)`.
  - Its re-entrancy behaviour becomes R17's.
- **R20 CrashGuard.** When the crash hook is set, `FunctionSubscribe` and the scheduler run every callback under `callWithCrashGuard`. A faulting subscriber is removed, a faulting timer is cancelled, and the hook gets `(tag or timer debug name, errorInfo)`. Behaviour matches `FunctionSubscribe.h` today, including `callRet` returning the default.
- **R21 Owner index.** A process-wide `SubscriptionOwnerIndex` maps `ownerKey → [(weak slot state, id)]`, so `removeOwner(key)` across all signals costs O(subscriptions of that owner).
  - Script release (`LuaBinding::removeScriptSubscriptions`, the generated `deleteCppScripts` in `Generator.cpp`) calls `releaseOwner(key)`. That removes the owner's subscriptions **and** cancels its timers.
  - Lua `RegisterEvent`/`RegisterEngineEvent` always pass `lua_topointer(self)` as the owner, with or without an explicit tag. This fixes the leak.
  - `REGISTER_*` macros pass `this`.
  - `Engine::removeSubscriptionsByTag` and `Scene::removeSubscriptionsByTag` remain as deprecated wrappers for one release. Generated code is switched to `releaseOwner`.

### Functional — scheduler

- **R22 Clocks.** `TimeDomain`:

  | Domain | Advances |
  |---|---|
  | `Game` | scaled by `timeScale`, only while game events run (not paused, not held, not in editor Pause) |
  | `Unscaled` | like Game, but ignores `timeScale` |
  | `Real` | steady wall clock, always, even while paused or loading |
  | `Frames` | simulated frames, counted under the same conditions as Game |
  | `FixedSteps` | fixed-update iterations |

  The scheduler keeps its own Game/Unscaled accumulators fed by `Engine::systemDraw`; it does not use `Engine::getGameTime()` (§3). Editor Step advances exactly one frame and one frame-delta.
- **R23 API.**
  - `after(delay, fn)`, `every(period, fn, count = ∞)`, `nextFrame(fn)`, `afterFrames(n, fn)`, `afterFixedSteps(n, fn)`, `at(domain, absTime, fn)`, and `post(fn)` (any thread, runs at the next process on the owner thread, FIFO).
  - Each returns a `TimerHandle`: 64-bit {slot, generation}, copyable, weak. It has `cancel()` and `active()`.
  - `TimerGroup` is RAII and move-only. Its destructor cancels everything it scheduled; it is the C++ equivalent of `Subscription`.
  - All schedule calls accept an optional `ownerKey` and a static `debugName`.
- **R24 Semantics.**
  - Work scheduled during `process()` runs no earlier than the next `process()` of that domain.
  - Ordering is by `(due, seq)`, so equal due times run FIFO.
  - Cancelling from any thread before a task starts guarantees it never runs. Cancelling the task that is running is a no-op for that run and stops further repeats.
  - A repeating timer fires at most once per `process()` by default. The next due time keeps its phase (`due += k·period` past now), so a stall does not produce a burst. `every(..., catchUp = true)` fires up to 8 missed periods.
- **R25 Process points** in `Engine::systemDraw` (`engine/core/Engine.cpp:1219`):
  - `post` inbox and `Real`: at frame start, always;
  - `FixedSteps`: after each `onFixedUpdate` + scene `fixedUpdate` iteration;
  - `Game`, `Unscaled`, `Frames`: after the scenes' `update`, before `onPostUpdate` (Unity's coroutine point).

  Each process call runs only the tasks that were due at its start.
- **R26 Lifetime.** The engine instance `Scheduler::game()` is cleared on Play stop and engine shutdown, before the Lua state closes. Timers bound to an owner are cancelled on `releaseOwner`, which includes S23 hot reload of that script.
- **R27 Editor scheduler.**
  - `editor::scheduler()` is processed where `processMainThreadTasks` runs today (`editor/App.cpp:2524`), with `Real` and `Frames` (editor frames).
  - `EditorHost::enqueueMainThreadTask` becomes `post()`. FIFO order, per-task exception isolation (`App.cpp:3820`), the wake callback and the `projectLoading` deferral are all kept.
  - `nextDueRealTime()` lets the idle backend loop sleep until the next timer.

### Performance (Linux x86-64 build server, Release; S02 benchmark harness)

- **R28 Logger.**
  - A filtered call costs ≤ 3 ns amortized.
  - An enabled async call with a 128-byte message and no backtrace costs p50 ≤ 1 µs and p99 ≤ 10 µs on the producer, with 4 producer threads.
  - Zero heap allocations per record when the formatted text is ≤ 1 KB (thread-local format buffer, byte ring, MemorySink arena).
  - Capturing raw backtrace addresses costs ≤ 20 µs.
  - File lines are visible (written to the OS) ≤ 100 ms after logging.
  - Drain-thread CPU stays under 2% at 10,000 records/s.
- **R29 Signals and scheduler.**
  - `Signal::invoke` and `FunctionSubscribe::call` allocate nothing, and with 1 C++ subscriber add ≤ 5 ns over a direct `std::function` call. CrashGuard cost is unchanged from today.
  - `after()` with a callable ≤ 48 bytes (`SmallFunction<void(), 48>`, with a heap fallback) costs ≤ 150 ns and allocates nothing in steady state (slot pool and binary heap reuse their capacity).
  - `process()` with nothing due costs ≤ 30 ns per domain.
  - 10,000 pending timers: inserting and processing all of them costs ≤ 2 ms.
- **R30 Release builds.** `JE_LOG_MIN_COMPILED_LEVEL` (default Debug in debug builds, Info in shipped release) removes the `JE_LOG_TRACE`/`JE_LOG_DEBUG` macro sites entirely.

### Editor UX

- **R31 Console window** (`editor/window/ConsoleWindow.*`).
  - It pulls from `MemorySink` once per frame with an id cursor. This replaces the per-message main-thread closures in `Out::logMessage`.
  - It gains:
    - a **Category** multi-select filter;
    - **Debug/Trace** rows (grey, under the existing "Log" counter) when enabled;
    - a source column `file:line` in the details pane, a click target for S23;
    - a "Collapse" key that includes the category.
  - Clear, Collapse, Clear on Play, Error Pause and search behave as today. Error Pause triggers on Error records from the play session.
- **R32 Logging preferences** (Preferences ▸ Logging, persisted in editor `settings.yaml`):
  - level per category, with a "Reset" button;
  - file log level (default Info);
  - "Capture backtrace on errors";
  - "Show source location".

  Project Settings ▸ Player ▸ Logging holds the shipped-game level (default Warn in release, Info in development builds), the file log on desktop (`<userdata>/logs/player.log`, 1 MB × 2, default off) and backtraces (default off). Preference edits are settings, not undoable scene edits.

### Agent UX (MCP, Lua, CLI)

- **R33 MCP.**
  - `read_console` gains the inputs `levels`, `categories` and `has_source`, and returns `level`, `category`, `source:{file,line}` and `frame` per entry. Every existing field and filter keeps its meaning (`type` stays).
  - New tools:
    - `set_log_level {level, category?, sink?: "console"|"file"|"memory"|"mcp"}`;
    - `get_log_config` (categories with levels, sinks with levels, drop and suppression counters);
    - `wait_for_log {since, contains?|regex?, levels?, categories?, timeout_ms ≤ 30000}`, a long poll that returns the first matching entries or `timed_out`;
    - `list_timers {domain?}` (pending count per domain, plus the 100 nearest with `debugName`/owner/due).
  - `logging/setLevel` sets the MCP sink level. A session that set a level receives `notifications/message {level, logger: category, data: text}`:
    - on stdio at any time;
    - on HTTP, on the SSE stream of any in-flight request (as progress is today, `McpServer.cpp:691`).
- **R34 Lua `Log`.** Existing `Log.print/verbose/debug/warn/error` are unchanged. New: `Log.info(text)`, `Log.setLevel(level [, category])`, `Log.getLevel([category])`, and `Log.category(name)`, which returns an object with `:trace/:debug/:info/:warn/:error`.
- **R35 Lua `Timer`** (new global):
  ```lua
  local h = Timer.after(2.0, function() ... end, { owner = self, clock = "game" })  -- "game"|"unscaled"|"real"
  Timer.every(0.5, fn, { count = 10, owner = self })
  Timer.nextFrame(fn [, opts]);  Timer.afterFrames(3, fn [, opts]);  Timer.afterFixedSteps(1, fn [, opts])
  local co = Timer.run(function()            -- a coroutine driven by the scheduler
      Timer.wait(1.0)                        -- also waitRealtime(s), waitFrames(n), waitFixedStep(),
      Timer.waitUntil(function() return self.ready end)   -- polled once per frame
  end, { owner = self })
  h:cancel(); h:isActive(); co:cancel(); Timer.cancelAll(self)
  ```
  - `Timer.wait*` outside a `Timer.run` coroutine raises `Timer.wait must be called inside Timer.run`.
  - Callbacks run with `pcallWithTraceback`. Errors go to the crash hook (Play pauses, as for events). A repeating timer or coroutine that errors is cancelled.
  - Without `owner`, a timer lives until Play stops.
  - Stubs for `generate_lua_stubs.py` and the `lua-api.md` sections replace the "No timer API" note (`docs/mcp/guide/lua-api.md:536`). The guide adds a pitfall: pass `owner = self` so timers die with the script.
- **R36 CLI.** `jasperengine-editor --log-level <level>[,cat=level...]`, and the same in exported desktop players (S30 flag plumbing). `JASPER_LOG_LEVEL` is the environment fallback.

### Data & compatibility

- **R37 Settings only.**
  - New keys: editor settings `logging.*`; project settings `player.logging.*`, with defaults equal to today's behaviour (Info everywhere, editor file on).
  - No scene or asset format change, so no version bump.
  - Old projects regenerate their C++ glue (`Generator.cpp`) on the next build, so it calls `releaseOwner`.
  - User C++ scripts using `REGISTER_*`/`UNREGISTER_*` compile unchanged.
- **R38 Format attributes.** `Log::*` and `Logger::writef` gain `JE_PRINTF_FORMAT` (`__attribute__((format(printf,…)))`, `_Printf_format_string_` on MSVC). Every warning this surfaces is fixed in this stream, because S02 enables `-Werror`.

## 6. Design constraints & decisions

**Files**
- `engine/core/log/{Logger.h,Logger.cpp,LogSink.h,LogRecord.h,LogCategory.h,Backtrace.{h,cpp}}`
- `engine/core/log/sinks/*.cpp`, one file per sink, compiled per platform
- `engine/core/util/{Signal.h,SmallFunction.h}`
- `engine/core/util/Scheduler.{h,cpp}`
- `engine/core/util/SubscriptionOwnerIndex.{h,cpp}`

`engine/core/Log.h` stays as the legacy façade. `editor/Out.*` becomes a façade: `Out::*` → `Logger`, and `getLogsSince` / `lastLogId` / `clearConsole` → `MemorySink`. `LogEntry` gains fields, and its `type` is derived from level, flags and category.

**Logger.** Formatting stays on the producer: `va_list` cannot cross threads and arguments may dangle. Only owned bytes are queued. The drain thread swaps whole batches.

```cpp
namespace jasperengine {
enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };
struct LogCategory { const char* name; uint8_t index; std::atomic<uint8_t> effective; };
JASPERENGINE_API LogCategory& logCategoryEngine();      // + one accessor per built-in (R15)
#define JE_LOG_CATEGORY(Id, name, lvl)   /* defines a function-scoped, engine-registered category */
#define JE_LOG_WARN(cat, fmt, ...) \
  do { if (::jasperengine::Logger::shouldLog(::jasperengine::LogLevel::Warn, cat)) \
       ::jasperengine::Logger::writef(::jasperengine::LogLevel::Warn, cat, JE_LOG_SRC, fmt __VA_OPT__(,) __VA_ARGS__); } while (0)
class JASPERENGINE_API Logger {
public:
  struct Config { LogLevel defaultLevel = LogLevel::Info; size_t queueBytes; bool asyncDrain = true; bool backtraceOnError; };
  static void init(const Config&);  static void shutdown();
  static bool shouldLog(LogLevel, const LogCategory&) noexcept;   // inline: one relaxed load
  static void writef(LogLevel, const LogCategory&, const LogSource*, const char* fmt, ...) JE_PRINTF_FORMAT(4, 5);
  static void vwritef(LogLevel, const LogCategory&, const LogSource*, const char* fmt, va_list);
  static void write(LogLevel, const LogCategory&, std::string_view text, const LogSource* = nullptr, uint32_t flags = 0);
  using SinkId = uint32_t;
  static SinkId addSink(std::unique_ptr<LogSink>);  static void removeSink(SinkId);
  static void setSinkLevel(SinkId, LogLevel);  static void setCategoryLevel(LogCategory&, LogLevel);
  static void flush();                                              // blocking; no-op on the drain thread
  static bool flushForCrash(std::chrono::milliseconds) noexcept;    // R10
};
class LogSink {
public:
  enum class Delivery { Inline, Async };
  virtual ~LogSink() = default;
  virtual void write(const LogRecord&) = 0;     // LogRecord: views valid for the call only
  virtual void flush() {}
  virtual Delivery delivery() const { return Delivery::Async; }
  LogLevel minLevel = LogLevel::Trace;  uint64_t categoryMask = ~0ull;
};
}
```

**Out of `System::platformLog`.** The engine stops calling `System::platformLog` directly. `PlatformSink` calls it only where it is overridden. The editor's `Platform::platformLog` override (`editor/Platform.cpp:106`) is deleted, because the editor installs its sinks directly. The Android override is replaced by `AndroidLogSink`, and the function is kept virtual for compatibility.

**Signals: one slot core, two front ends.** `Signal` (for new code) and `FunctionSubscribe` (for legacy and Lua) share `detail::SlotList<R(Args...)>`:
- slots vector, tombstone bits and invoke depth;
- `shared_ptr` state so handles can be weak;
- an atomic count for `hasSubscribers()` reads from Jolt threads.

`std::bind` with `MyPlaceholder` is replaced by lambdas.

**Name.** The type is `Signal`, not `Event`, to avoid clashing with the HTML-UI DOM `Event` types (`engine/core/object/ui/HtmlUIScriptInternal.h:37`) and Rml.

**Migration of existing sites.**
- (a) New engine/editor C++ subscribers hold `Subscription` members. Today the `REGISTER_*` macros appear only in generated or template text (`ScriptCreateDialog.cpp`, `ScriptEvents.cpp`, `EngineApiContext.cpp`, `McpInstructions.cpp`); there they gain the owner key and nothing else changes.
- (b) The tag-substring teardown in `Engine.cpp:830`, `Scene.cpp:74` and `PhysicsSystem.cpp:454` is replaced by `SubscriptionOwnerIndex::releaseOwner`.
- (c) The Lua event objects and `REGISTER_*` stay on `FunctionSubscribe`.
- No mass rewrite of the 187 declarations.

**Scheduler.**
- Owner thread: the engine main thread, or the editor main thread for its own instance.
- Storage: a chunked slot pool (stable addresses, with an atomic `{generation, cancelled}` word per slot so cross-thread `cancel` is lock-free), one binary min-heap per domain over `(due, seq, slot)`, and a mutex-protected `post`/`schedule` inbox swapped at the start of process.
- Callables: `SmallFunction<void(), 48>`.
- Lua bindings live in `engine/core/script/binding/UtilClassesLua.cpp`. Coroutines are `lua_newthread` refs held in the registry and resumed under `pcallWithTraceback` semantics (via `lua_resume` + traceback on error). They are unref'd on completion, cancel or Play stop.

```cpp
enum class TimeDomain : uint8_t { Game, Unscaled, Real, Frames, FixedSteps };
class JASPERENGINE_API Scheduler {
public:
  static Scheduler& game();
  TimerHandle post(SmallFunction<void(), 48>);                                   // any thread
  TimerHandle after(double delay, SmallFunction<void(), 48>, TimeDomain = TimeDomain::Game, TimerOpts = {});
  TimerHandle every(double period, SmallFunction<void(), 48>, TimeDomain = TimeDomain::Game, TimerOpts = {}); // opts: count, catchUp, ownerKey, debugName
  TimerHandle nextFrame(SmallFunction<void(), 48>, TimerOpts = {});
  TimerHandle afterFrames(uint32_t, SmallFunction<void(), 48>, TimerOpts = {});
  TimerHandle afterFixedSteps(uint32_t, SmallFunction<void(), 48>, TimerOpts = {});
  bool cancel(TimerHandle) noexcept;                       // any thread
  size_t cancelOwner(uint64_t ownerKey);                   // owner thread
  void advance(TimeDomain, double delta);  size_t process(TimeDomain);   // owner thread (Engine::systemDraw)
  void clear();  double nextDueRealTime() const;  SchedulerStats stats() const;
};
```

## 7. Acceptance criteria

All gtest suites live under `engine/tests/core/` (S02 framework). Benchmarks are S02 gated benchmark tests on the Linux build server.

- **AC1 (R1, R3, R30).** `LoggerTest.FilteredCallFormatsNothing`: an argument with a side effect is not evaluated through the macro, and the allocation hook counts 0 over 1M filtered calls. `LoggerBench.Filtered`: ≤ 3 ns per call.
- **AC2 (R4, R12, R28).** `LoggerBench.AsyncEnabled4Threads`: p50 ≤ 1 µs, p99 ≤ 10 µs, 0 allocations for 128-byte text. `LoggerTest.QueueFullDropsLowFirst`: with the ring filled, Info records are dropped, Warn is kept or waits ≤ 5 ms, and exactly one "dropped N" record follows.
- **AC3 (R4).** `LoggerTest.IdsMonotonicAcrossThreads`: 8 threads × 10k records; ids are unique and increasing in every sink's order. `Out::getLogsSince` called right after `Out::error` on any thread returns that entry (inline MemorySink).
- **AC4 (R6, R7).** `LoggerTest.FileFormatCompat`: file lines match `^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d \[(INFO |WARN |ERROR|OK   |BUILD)\] `. Game console lines start with `(ERROR): `. `tools/smoke-test.sh` passes unchanged.
- **AC5 (R6).** `FileSinkTest.RotatesAtRuntime`: 3 MB written with a 1 MB × 3 config gives `.1/.2/.3` and no file over 1 MB + one line. `FileSinkTest.RotationFailureKeepsAppending`: a rename forced to fail does not lose a line and does not retry per write.
- **AC6 (R8).** `LoggerTest.ErrorCarriesBacktrace`: on Linux, Windows and macOS, an Error from C++ has ≥ 3 frames and the symbolized file sink output names the test function. Lua errors have none. The 11th backtrace in one second is omitted.
- **AC7 (R9).** `LoggerTest.SafetyValve`: 10,000 identical Lua `print`s in one frame produce ≤ 200 records plus one `suppressed 9800 similar messages` record. `JE_LOG_ONCE` logs once.
- **AC8 (R10).** `LoggerCrashFlushTest`: a helper process (`je_log_crash_helper`, spawned by the test) logs 5,000 lines, then raises `SIGSEGV` (Linux/macOS) or an access violation (Windows). The log file contains line 5000 **before** the `FATAL:` block. A variant that crashes inside a sink on the drain thread writes `log tail incomplete`. Neither variant hangs (test timeout 10 s).
- **AC9 (R13, R16).** `LoggerTest.LogFromSinkNoRecursion`, `LoggerTest.PreInitFallback`, `LoggerTest.ShutdownDrainsAll`. TSan run (S02 preset) of the logger suite is clean.
- **AC10 (R14).** S02 web browser gate page: `Log::warn` appears as a console warning, `Log::error` as a console error, synchronously and in order, and `Timer.after(0.1)` fires in a WebGL2 build without pthreads.
- **AC11 (R6, Android).** Manual check on the Android export: `adb logcat -s JasperEngine` shows `W JasperEngine: [render] …` for a `JE_LOG_WARN(render…)` placed in the sample project. Recorded in the PR.
- **AC12 (R17).** `SignalTest`, with one test per behaviour:
  - `UnsubscribeSelfDuringInvoke`
  - `UnsubscribeNextDuringInvokeSkipsIt`
  - `SubscribeDuringInvokeNotCalled`
  - `ClearDuringInvoke`
  - `NestedInvoke`
  - `HandleOutlivesSignal`
  - `SignalOutlivesHandle`
  - `MoveHandle`
  - `ZeroAllocInvoke` (allocation hook)

  All pass under ASan.
- **AC13 (R19, R20).** `FunctionSubscribeCompatTest`: every public method behaves as before (same-tag replace, `callRet` first-result/default, `setEnabled`). The re-entrancy cases from AC12 pass. With a crash hook set, a subscriber that dereferences null is removed, the hook gets its tag, and later subscribers still run (POSIX + Windows).
- **AC14 (R21).** `OwnerIndexTest.ReleaseRemovesSubscriptionsAndTimers`. MCP test `tools/mcp/tests/test_core_utilities.py::test_lua_explicit_tag_released`: a Lua script with `RegisterEvent(self, ev, "onClick", "myTag")` is destroyed during Play, then the event is invoked; nothing runs and the Console has no error. This test fails on the current code.
- **AC15 (R22, R24, R25).** `SchedulerTest`, one test per behaviour:
  - `OrderingFifoEqualDue`
  - `ScheduledDuringProcessRunsNextProcess`
  - `CancelBeforeRunNeverRuns` (cancel from another thread, 10k iterations, TSan)
  - `CancelRunningStopsRepeat`
  - `RepeatNoBurstAfterStall`
  - `CatchUpCapped8`
  - `GameClockStopsWhenGameEventsPaused`
  - `UnscaledIgnoresTimeScale`
  - `RealRunsWhilePaused`
  - `FixedStepsCountIterations`
  - `ZeroAllocSmallCallable`
  - `Process10kUnder2ms`
- **AC16 (R22, R26, R35).** MCP `test_core_utilities.py`:
  - `test_timer_after_game_time` (fixed deltatime 1/60: `Timer.after(0.5)` fires on frame 30 ±0);
  - `test_timer_respects_editor_pause` (pause, wait 1 s of real time, nothing fires; Step advances one frame);
  - `test_timer_timescale_zero` (game timer frozen, `clock="unscaled"` fires);
  - `test_timer_owner_destroyed` (owner object destroyed → no callback);
  - `test_timer_play_stop_clears` (`list_timers` shows 0 after stop);
  - `test_coroutine_wait` (`Timer.run` + `wait`/`waitFrames`/`waitUntil` sequence logged in order);
  - `test_timer_error_pauses_play` (an erroring callback is reported in the Console with a traceback and Play pauses).
- **AC17 (R27).** `test_core_utilities.py::test_editor_tasks_fifo`: the existing MCP suites that depend on `enqueueMainThreadTask` (`test_busy.py`, play tests) pass unchanged.
- **AC18 (R31, R32).** Manual check described in the PR, on a desktop editor:
  1. Set the `render` category to Debug in Preferences ▸ Logging.
  2. Debug rows appear grey and filter under Category.
  3. Selecting a `JE_LOG_WARN` row shows `file:line`.
  4. Restart: the levels persist.
  5. Clear on Play and Error Pause still work.
- **AC19 (R33).** MCP tests, part of `test_core_utilities.py`:
  - `test_read_console_category_level_fields`;
  - `test_set_log_level_roundtrip` (`get_log_config` reflects it);
  - `test_wait_for_log` (returns the match within 1 frame of the log; `timed_out` after `timeout_ms`);
  - `test_mcp_logging_notifications` (stdio session: after `logging/setLevel warning`, a Lua `Log.warn` arrives as `notifications/message` with `logger: "lua"`).
- **AC20 (R35).** `tools/mcp/tests/test_docs.py` passes with the new `Timer`/`Log` sections, and the generated Lua stubs contain `Timer.after`.
- **AC21 (R38).** Builds with `-Wformat=2 -Werror` (S02 presets) on GCC, Clang and MSVC (`/analyze` not required) are clean.

## 8. Dependencies & conflicts

**Depends on:**
- S01: C++20, `__VA_OPT__`, `std::span`.
- S02: gtest target, allocation-counting hook, TSan/ASan presets, benchmark thresholds, web gate, `-Werror`.

**Blocks / is used by:**
- S03: autosave timer on the editor scheduler.
- S05: watcher events become `ThreadSafeSignal` and are delivered via `post`.
- S06: job and I/O continuations to the main thread use `Scheduler::post`. S06 must not re-invent a main-thread queue. The logger drain thread is outside the pool.
- S15: profiler shows logger and scheduler stats.
- S23: click-to-source reads `source`; Lua hot reload calls `releaseOwner`.
- S28: game crash handler and hang watchdog call `flushForCrash` and share the DbgHelp mutex.
- S30: `--log-level` in players.
- S31: CLI `call read_console` / `wait_for_log`.

**Merge hotspots:**
- `engine/core/Engine.cpp` `systemDraw` (also S06, S07, S15, S20, S22)
- `engine/core/util/FunctionSubscribe.h`
- `engine/core/script/LuaBinding.cpp` (S23)
- `editor/Generator.cpp` (S23, S30)
- `editor/Out.*`, `editor/window/ConsoleWindow.*` (S23)
- `editor/util/CrashHandler.cpp` (S28)
- `editor/mcp/McpServer.cpp`, `editor/mcp/tools/ConsoleTools.cpp`
- `editor/App.cpp` main-thread queue (S03, S25)
- `docs/mcp/guide/lua-api.md`

Land S09 early in wave 1 so the others build on it.

## 9. Risks & defaults

- **FunctionSubscribe re-core.** This is the biggest risk: 187 declarations, Lua bindings, Jolt threads and CrashGuard. Re-entrancy semantics change: removal during `call` no longer skips a neighbour. Default: land it behind `FunctionSubscribeCompatTest` plus the full MCP suite. Do not migrate declarations in the same PR.
- **Signal-context crash flush.** It only waits on atomics. If the drain thread is itself blocked, the tail is reported as lost rather than recovered. Default: accept this. Lock-free emergency dumping of the ring is not done.
- **Format attributes may surface many real mismatches.** Default: fix them in this stream. If there are more than 200, ship the attribute behind `JE_LOG_FORMAT_CHECK` for one release and file a follow-up.
- **Backtrace symbolization after a C++ script library unloads** may name the wrong symbol on Windows. Default: before unloading a script library the editor calls `Logger::flush()`, and unresolved frames print module+offset.
- **The inline MemorySink adds a lock on every enabled log call in the editor.** Default: accept it; it is ≤ 200 ns with the arena and keeps MCP read-after-log exact. Games disable it unless something reads it.
- **Lua timers without `owner`** can call into destroyed scripts. Default: Play-session lifetime, plus the documented pitfall. An auto-owner (the "currently running script") is not added because it is too implicit.
- **Open question: should `Game` timers also stop while a scene is individually paused (`pauseEngineScene`)?** Default: no. The clock is engine-global, and a per-scene scheduler can come later.

## Deviations (implementation record)

- **R32 vs R37, the shipped game's default level.** Player > Logging defaults to Info, today's
  behaviour (R37), not Warn in release builds (R32): Warn would silence every `Log::print` and Lua
  `print` of existing games, and the `Engine ready:` line `tools/smoke-test.sh` waits for. A project
  chooses Warn in Project Settings > Player > Logging (or `--log-level`, `JASPER_LOG_LEVEL`).
- **R13, a record logged from inside a sink on the drain thread** is queued with that sink
  excluded instead of written synchronously to the other sinks: writing it at once would put it
  before older records of the same batch in the sinks after the producing one (AC3's per-sink id
  order) and needs those sinks' locks while one is held. No recursion, no deadlock, the producing
  sink never sees it. A record logged from inside an Inline sink is delivered to the other Inline
  sinks at once and queued for the Async ones, the same way.
- **R6 PlatformSink.** The engine cannot tell portably whether a `System` overrides
  `platformLog`, so a new virtual `System::installLogSinks()` decides the game's sinks: the
  default installs the platform's (`installPlatformLogSinks`), an override adds a `PlatformSink`
  (the test `TestSystem` does). `platformLog` stays virtual and keeps today's default output.
- **R6 console sink in the editor.** The editor style is Inline, not Async: the editor's CLI
  commands print their own results with `std::cout` (JSON documents, reports) and redirect fds 1/2
  around quiet phases, and their log lines must stay in order with that, as they did. It also keeps
  today's stream split (errors and build output on stderr, the rest on stdout). The game style is
  Async and sends Warn and above to stderr as specified.
- **R9, build output** (category `build`) is never rate limited: a compiler's lines share long path
  prefixes and come in bursts, and `BuildTools` parses every one of them.
- **R12, drain latency.** Info and below wake the drain thread at most every 20 ms (and whenever
  the queue is a quarter full); Warn and above wake it at once. This is what keeps R28's drain CPU
  budget at a steady trickle; file lines still reach the OS well within 100 ms.
- **R23 TimerHandle** is `{Scheduler*, 64-bit id}` (id = 40-bit generation, 24-bit slot), so
  `cancel()`/`active()` need no global table and work for every scheduler instance.
- **R33 set_log_level `sink: "mcp"`** without a category sets the notification level of every
  connected MCP session (the sink's level follows the lowest session level).
- **R32 / Apple exports.** Like the other Player settings that reach a game through the exported
  CMake (VSync, window), Player > Logging is not injected into the Xcode workspace; iOS builds use
  the defaults (and `--log-level` does not exist there).
- **AC10, AC11, AC18 are automated** rather than manual checks. AC10:
  `tools/test-projects/core-utilities` built for the web (`WEB_PROJECT=core-utilities tools/build.sh
  web`) and `GATE=core-utilities tools/build.sh web-gate`, whose baseline lists the console lines
  with their levels in order (C++ and Lua warnings and errors, a Lua `Timer.after(0.1)` and a
  coroutine, no pthreads); CI runs it. AC11: the same project as an APK
  (`ANDROID_PROJECT=core-utilities tools/build.sh android`) on a headless emulator,
  `tools/android-emulator-test.sh` (KVM hosts) checking `W/JasperEngine: [render] ...` and the other
  lines with their priorities. AC18: `tools/mcp/tests/test_logging_ui.py` drives Preferences >
  Logging and the Console through MCP (`focus_window` opens Preferences pages and reports and drives
  the Console view; `preferences.logging.*` settings), screenshots in
  `build/test-results/logging-ui/`, and restarts the editor to check the levels persist. AC21 is
  verified with GCC (`-Wformat=2` over the engine and editor sources); MSVC and Clang are covered by
  CI.
- **Web builds relink when project files change.** emcc packs `project/assets` and `project/lua` at
  link time (`--preload-file`), which ninja did not track, so an edited Lua script shipped the old
  bundle until something else relinked. `engine/CMakeLists.txt` now lists those files as the
  target's `LINK_DEPENDS` (globbed with `CONFIGURE_DEPENDS`).
- **Tests** live in S02's targets: `engine/tests/core/log/`, `engine/tests/core/util/`
  (`jasperengine-core-tests`, the per-thread count of S06's
  `support/AllocationCounter.h`, which counts at `malloc` on Linux), `editor/tests/OutTest.cpp` and `editor/tests/util/CrashHandlerTest.cpp` with
  the `je_log_crash_helper` executable (`jasperengine-editor-tests`), and
  `tools/mcp/tests/test_core_utilities.py`. Benchmarks are the `*Bench` suites, with
  `JASPER_TEST_PERF_SLACK`.
- **Wall-clock budgets are enforced on the benchmark server only** (bug fix after the merge).
  `SchedulerTest.Process10kUnder2ms` and the `*Bench` suites failed ordinary unit test runs on
  loaded shared builders (10k timers in 20 ms against the 2 ms budget). Their budgets now apply
  when `JASPER_TEST_PERF_SLACK` is set: the benchmark run on server E
  (`env JASPER_TEST_PERF_SLACK=1 ... --gtest_filter=*Bench*:SchedulerTest.Process10kUnder2ms`,
  docs/testing.md). Other runs print the `[bench]` lines and check the results only.

## Upstream absorption (sync 4: debug-overlay)

Upstream's `debug-overlay` feature (`docs/superpowers/specs/2026-10-31-debug-overlay-design.md`,
`docs/mcp/guide/debugging.md`) brought its own log core in `engine/core/Log.{h,cpp}`: a `LogLevel`
(`Info, Verbose, Debug, Warn, Error` on the `D_LOG_*` values), a `LogEntry` ring buffer, `Log`
listeners callable from any thread, and a direct `System::platformLog` path. The logging plumbing is
S09's; upstream's genuinely new parts sit on top of it.

- **One `LogLevel`.** Upstream's enum is gone: `LogEntry::level`, `Log::getCount`, the overlay's
  filters and Lua's `LogLevel` are S09's `Trace < Debug < Info < Warn < Error < Fatal` (`Off`).
  Upstream's `Verbose` is `Trace`; Lua keeps `LogLevel.VERBOSE` as an alias of `LogLevel.TRACE` and
  gains `INFO`... `FATAL` with S09's values (scripts compare by name). The overlay counts and shows
  `Fatal` with the errors.
- **The buffer and the listeners are a sink.** `Log.cpp` installs `LogBufferSink` ("buffer"), an
  `Inline` sink at `Trace`, at the engine library's start (and on first use of the `Log` API). Every
  record of the process reaches it: `Log::*` (category `engine`, now `vwritef` again, so gated before
  formatting, R3), `JE_LOG_*` of every category, Lua; its `categoryMask` leaves out the editor's own
  categories (`editor`, `build`, `mcp`: `Out::*`, compiler output, the MCP server), so the buffer is
  the game's log as upstream's was (its `Log::*` never saw `Out::*`). `LogEntry` carries the record's `id`
  (S09's, R4), its `category`, its level; `message`/`stackTrace` are split at `stack traceback:` as
  upstream did. Two small, generic `LogSink` capabilities make this possible:
  - `resident()`: `Logger::shutdown()` keeps the sink, and records logged before `init()` / after
    `shutdown()` reach it (on the logging thread, with an id, next to the stderr fallback), so
    upstream's "captured from the first line" holds for the whole process;
  - `wantsAfterWrite()` / `afterWrite()`: called on the thread that logged once the Logger's lock is
    released, for the Inline sinks written under it. `write()` (under the lock) only copies the
    record into the ring and the main thread's pending queue; the listeners run from `afterWrite()`.
    This keeps upstream's guarantee that listeners are never called with a log lock held (a listener
    can join a thread that logs), and S09's per-sink id order.
  The rest is upstream's code unchanged: `AnyThread` listeners on the logging thread, `MainThread`
  ones at once on the main thread or at `Log::dispatchPending()` (frame start) in order, the 1,000-line
  pending queue with its one-line overflow report, no re-dispatch of a line logged inside a listener,
  copy-on-write listener lists, and `removeListener` that waits for running calls on other threads
  but never from inside a listener call (no hang on self or cross removal). `Log::emitEntry` hands
  the sink its message and trace as given (a thread-local keyed by the text's address), so a message
  containing `stack traceback:` is not split.
- **What the buffer sees is what every sink sees.** A line below its category's level, held back by
  the rate limit (R9) or dropped by a full queue (R12) is not buffered, not counted and not handed to
  listeners; the `suppressed N similar messages` and `[log] dropped N messages` records are. Upstream's
  tests that flood one call site log distinct texts through `"%s"` (keyed by text).
- **Lua.** `Log.print/info/verbose/debug/warn/error` are upstream's `luaLogLine` (any values,
  tab-joined, a traceback per Project Settings > Debug > Log Stack Traces) on S09's path: gated first,
  category `lua`, `LogFlagLua | LogFlagNoBacktrace`, the calling chunk and line as the source (R5),
  through `LuaBinding::logFromLua` (which takes the trace and goes through `Log::emitEntry`). R34's
  `Log.category(name)` methods and `Log.setLevel/getLevel` stay; category methods get the traceback
  per the same setting. `Log.getEntries` adds `category`.
- **Stats on S15.** The stats panel and `DebugOverlay.getStats()` read S15's frame-time ring
  (`FrameTimes`, R19) instead of upstream's own 240-float ring fed from `Engine::runFrame`;
  `FrameTimes::recordSample` is the seam the tests use. The process memory has one reader,
  `System::getProcessMemoryBytes` (`debug/ProcessMemory.cpp`), which S15's "Process RSS" monitor
  now uses too (Apple reports the physical footprint there).
- **Settings.** Project Settings > Debug is a reflection table (S08, `editor/reflect/DebugReflect.cpp`):
  the `debug:` block of `project.yaml` (only what differs from the defaults), its enum tokens
  (`DebugSettingsNames`), `jasper://reflection`'s `project_debug` schema, the S25 rows (`debug.*`
  keys unchanged, labels, help, choices and range from the table) and the window's rows. `debug` is
  a known root key of `project.yaml` (`StreamSchema`).
- **Not taken.** Upstream's direct `System::platformLog` calls (the platform log is S09's sinks);
  native backtraces in `LogEntry::stackTrace` (S09 symbolizes in the Async sinks only, R8: they stay
  in the log file). No MCP tool was added (upstream added none; `get_log_config` lists the `buffer`
  sink).
- **Tests.** Upstream's `editor/tests/LogListenerTest.cpp`, `editor/tests/DebugSettingsTest.cpp`,
  `engine/tests/htmlui/TestDebugOverlay.cpp` and `tools/mcp/tests/test_debug_overlay.py` run as
  upstream wrote them, adapted where S09 or S15 decide (floods through `"%s"`, frames through
  `FrameTimes`, the timing budgets of the off-cost test skipped in sanitizer builds).
  `engine/tests/core/log/LogBufferSinkTest.cpp` checks that the sink gets exactly what another
  Inline sink gets (ids, texts, levels), from six threads at once (each line once, `AnyThread` on its
  own thread, `MainThread` in id order), outside the Logger's lock, before init and after shutdown,
  and the rate limit's summary; it and `LogListenerTest` run under ThreadSanitizer
  (`SANITIZE=thread tools/build.sh unit-tests`).
