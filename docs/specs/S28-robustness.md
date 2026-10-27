# S28 — Robustness & crash diagnostics

Wave 3. Owner area: `engine/core/diag/` (new: crash handler, hang watchdog, crash log ring),
`engine/platform/*` (install points), `editor/backend/*` (heartbeat), `editor/util/CrashHandler.*`
(becomes a forwarder), `tools/crash/` (new).

## 1. Goal

When the editor or a shipped game dies or freezes, the developer gets evidence they can act on:
a crash report folder with the reason, a backtrace that names module+offset (symbolicated with
the build's symbols), the last log lines, and on Windows a minidump. A main-thread hang watchdog
turns "the editor froze" into a report naming the stalled phase, the main-thread stack and what
every job worker was running. Today only the editor has a crash handler, it cannot report a stack
overflow, and exported games have none (`docs/openengine-analysis/10-platform-build-tests.md`).

## 2. Scope

In scope:
- One crash handler in the engine (`engine/core/diag/`), used by the editor and every export target
  (Windows, Linux, macOS, Android, iOS, HTML5), with per-platform backends.
- Windows minidumps from a pre-spawned writer thread; POSIX text report + OS core/tombstone parity.
- Crash report folder layout, last-log tail, session markers, the "closed unexpectedly" notice.
- Main-thread hang watchdog (editor always, games in development builds), busy-scope API, job
  occupancy (S06), runaway-Lua interruption in editor Play.
- Opt-in report hook for game developers (no default network), symbolication tool and guidance.
- Test binary with deliberate crash modes; watchdog tests with injected stalls; MCP tools.

Out of scope:
- Breakpad/Crashpad/Sentry integration or any upload endpoint (developers plug their own via the hook).
- Out-of-process crash monitors; GPU device-loss handling (separate future work).
- Hang detection of worker threads (occupancy is reported, workers are not watched).
- Watchdog on the web main thread (the browser owns that; see R24).
- Symbol stripping/splitting and publish layout: S30 owns them; S28 consumes the layout.
- Nothing from the do-not-copy list is involved.

## 3. Current state

- `editor/util/CrashHandler.{h,cpp}` (installed at `editor/App.cpp:3848`, log `<config>/editor.log`).
  Strengths to keep: paths preallocated at install (`logPath`/`exePath`), raw `write`/`WriteFile`
  instead of stdio, re-entry bounded by `fatalEntries` (second entry reports, third dies),
  `backtrace_symbols_fd` (no allocation), module+offset on Windows when no PDB, terminate handler
  that rethrows to name the exception, `EXCEPTION_CONTINUE_SEARCH` so WER still runs. Defects:
  - `std::signal`, no `SA_ONSTACK`/`sigaltstack`: a stack overflow (SIGSEGV on the guard page)
    cannot run the handler and nothing is reported.
  - `writeFormatted` calls `vsnprintf` inside the signal handler (not async-signal-safe).
  - Windows: `CaptureStackBackTrace` + `SymInitialize` run on the faulting thread inside the filter;
    `SymInitialize` allocates and loads modules, and it walks the filter's stack, not the faulting
    `ContextRecord`. No minidump. `abort()`, pure-call and invalid-parameter paths produce nothing.
  - Re-raises with `SIG_DFL`, discarding any previous handler (Android debuggerd, sanitizers).
  - No fault address, no thread name, no build ID, no module list, no log tail (the `std::ofstream`
    in `editor/Out.cpp:27` is buffered, so the last lines before a crash are often lost).
  - Report header hard-codes "JasperEngine Editor"; it is editor-only (`CMakeLists.txt:600`).
- `engine/core/util/CrashGuard.h` temporarily installs process-wide `sigaction` handlers around Lua
  callbacks (`FunctionSubscribe.h:251,294`). Its unguarded path (`crashSignalHandler`) does
  `signal(sig, SIG_DFL); raise(sig)`: a worker thread crashing while the main thread holds a guard
  bypasses the crash handler entirely.
- Games: `engine/platform/{win,linux,mac,android,apple,emscripten}` entry points install nothing;
  `Log` (`engine/core/Log.cpp`) goes only to `System::platformLog` (logcat on Android), no file.
- Hang: nothing. The editor loop idles at most 100 ms per wait (`BackendLinux.cpp:2877-2880`,
  `BackendWin.cpp:791` `MsgWaitForMultipleObjectsEx(...,100,...)`, `BackendMac.mm:49`), so a
  heartbeat per loop iteration is a valid liveness signal. Known long main-thread operations:
  project load (pumped via `App::reportLoadingProgress`, `App.cpp:2092`), native file dialogs
  (`editor/util/FileDialogs.h:40,90,114,152`), Windows live resize (`BackendWin.cpp:916`, frames
  via `liveResizeFrame`), user Lua in Play (main thread). Exports run on a thread (`Exporter.cpp:491`).
- MCP: every tool runs on the main thread (`McpServer::runOnMainThread`, `McpServer.cpp:722`), so a
  hung editor answers nothing. `run_game` detects crashes by log regex (`BuildTools.cpp:1078`
  `isFatalLine`, mirrored by `tools/smoke-test.sh:99` `fatal_lines`); our "FATAL:" line matches neither.

## 4. Reference

OpenEngine (MIT, `Copyright (c) 2026 Gamecrafters Guild Limited and contributors`):
- `Apps/Editor/Source/Diagnostics/MainThreadHangWatchdog.{h,cpp}` (90 + 489 lines) and
  `Tests/Editor/MainThreadHangWatchdogTests.cpp`. **Take**: heartbeat atomic armed by the first
  beat, 200 ms poll, re-report interval (5 s), "recovered after Xs" line, phase annotation
  (`HangWatchdogPhase`), separate shutdown window with its own threshold, env switches, occupancy
  snapshot read into a buffer sized at start. **Improve**: their stack capture is Windows-only and
  runs dbghelp inside the suspend window (their own comment names the deadlock and says the fix is
  copy-context + `RtlVirtualUnwind`); ours resumes from the watchdog thread on a deadline (D3) and
  works on Linux/macOS/Android/iOS. They have no busy scopes (only labels), so known long work
  still reports; ours raises the budget. Adapt code (logic, test cases); keep their header on
  copied files and add a `NOTICE.md` entry.
- `Apps/Editor/Source/main.cpp:53-175` `WriteEditorCrashDump`: dump type choice (Debug = full
  memory; otherwise `IndirectlyReferencedMemory|DataSegs|HandleData|ThreadInfo|UnloadedModules`),
  one dump per process (`compare_exchange`), dump path `<date>-<pid>-<reason>.dmp`. **Improve**:
  they call `MiniDumpWriteDump`, `std::filesystem` and the logger on the crashing thread, and
  their signal handler says it is "not strictly async-signal-safe" (`:300-308`); their SEH filter
  swallows the exception (no WER). We dump from a pre-spawned thread, keep WER, and stay
  async-signal-safe on POSIX.
- `Apps/Player/Source/main.cpp:40-80`: the player only logs. Not taken.

## 5. Requirements

### Functional — crash handler (all targets)
- **R1** `engine/core/diag/CrashHandler.{h,cpp}` + `CrashHandler{Posix,Win,Web}.cpp` is the only
  crash handler. `editor/util/CrashHandler.h` becomes a forwarder; `App::initializeSettings`
  calls `diag::crash::install(config)` with product "JasperEngine Editor".
- **R2** Install does all allocation up front: report root path, session folder name
  (`<UTC start yyyymmddThhmmssZ>-<pid>`), product/version/backend/platform strings, build ID of the
  main module (ELF `NT_GNU_BUILD_ID`, Mach-O `LC_UUID`, PE CodeView RSDS GUID+age, web build hash
  from S30), preformatted header bytes, the log ring (R9), and one prewarm `backtrace()` call.
- **R3** POSIX: `sigaction` with `SA_SIGINFO|SA_ONSTACK` for SIGSEGV, SIGBUS, SIGILL, SIGFPE,
  SIGABRT, SIGTRAP; a 64 KiB `sigaltstack` on the main thread and on every thread created through
  S06 `thread::spawnNamed` (`diag::crash::prepareThread()`). The previous action of each signal is
  saved and the handler **chains to it** after reporting (then `SIG_DFL` + re-raise only if the
  previous was default/ignore), so debuggerd tombstones, sanitizers and macOS ReportCrash still work.
- **R4** POSIX handler uses only async-signal-safe calls (`open/write/close/mkdir`,
  `backtrace`/`backtrace_symbols_fd` after prewarm, `_Unwind_Backtrace` on Android, `prctl`,
  `syscall(gettid)`), and own integer/hex formatters — no `*printf`, no allocation, no locks.
  Order: re-entry guard (keep the 2-entry bound) → `mkdir` session folder → `crash.txt` header,
  reason, `si_code`, fault address, PC/SP from `ucontext`, thread id + name → backtrace → module
  map (Linux/Android: raw copy of `/proc/self/maps`; Apple: `_dyld` image list, best effort, last)
  → `log-tail.txt` (R9) → stderr line `JASPERENGINE CRASH: <reason> report=<folder>` → chain.
- **R5** Windows: `SetUnhandledExceptionFilter`, `_set_purecall_handler`,
  `_set_invalid_parameter_handler`, `SIGABRT` (with `_set_abort_behavior(0, _WRITE_ABORT_MSG|
  _CALL_REPORTFAULT)`), and `SetThreadStackGuarantee(64 KiB)` on main and S06 threads. Every path
  hands `{EXCEPTION_POINTERS or RtlCaptureContext-built record, thread id}` to a **dump writer
  thread** created at install and blocked on an event; the faulting thread waits ≤ 30 s. The writer
  creates the folder, writes `crash.dmp` (`MiniDumpWriteDump`, D4), then `crash.txt` with a
  `StackWalk64` walk of the faulting `ContextRecord` (module+offset always; names when a PDB is
  found), then `log-tail.txt`. The filter returns `EXCEPTION_CONTINUE_SEARCH` (WER keeps running).
- **R6** `std::set_terminate` keeps today's rethrow-to-name logic (it is not a signal context) and
  then routes through the same report path as SIGABRT.
- **R7** `CrashGuard.h`: the unguarded branch of `crashSignalHandler` calls
  `diag::crash::handleFatalSignal(sig, info, ctx)` instead of `SIG_DFL`+`raise`; a guarded Lua
  crash (caught and recovered) appends one breadcrumb line to the log ring, no report folder.
- **R8** Session marker: install writes `<root>/sessions/<pid>.json` `{pid, started, version,
  product, session}` (normal file I/O, not in a handler); clean shutdown deletes it. On start, a
  marker whose pid is not running means the previous session ended abnormally; if its session
  folder contains `crash.txt` it crashed, if it contains `hang-*.txt` but no crash it was killed
  while unresponsive, otherwise it ended without a report (kill, OOM, power loss).
- **R9** Crash log ring: a fixed buffer (editor 256 KiB, games 64 KiB) fed by `editor/Out.cpp`
  `logMessage` and by `engine/core/Log.cpp` before `platformLog`. Writers reserve space with one
  `fetch_add` on a 64-bit cursor and `memcpy` (lock-free; a torn line at a wrap is acceptable).
  The handler writes the ring oldest→newest with raw writes. S09's ring sink, when it lands,
  feeds this same buffer; the handler-side contract stays here.
- **R10** Report folder layout (one folder per session that had something to report):
  ```
  <root>/reports/<yyyymmddThhmmssZ>-<pid>/
    crash.txt        header "JasperEngine crash report v1", key: value lines, then frames
    crash.dmp        Windows only
    log-tail.txt     ring contents
    hang-001.txt …   watchdog reports (R14)
    handled          empty marker: notice dismissed / hook delivered
  <root>/sessions/<pid>.json
  ```
  Roots: editor `AppSettings::getConfigDirectory()`; games `System::getUserDataPath()/crash`
  (Android: internal files dir, iOS: `Library/Caches/crash`, web: none, R24). `crash.txt` keys:
  `product, version, build_id, platform, backend, reason, signal|exception_code, fault_address,
  thread, pc, sp, time_since_start_ms, session`; frames as `#N <module>+0x<offset> [<symbol>]`.
- **R11** Retention, done at startup (never in a handler): editor keeps the newest 20 report
  folders and ≤ 2 GiB; games keep 5 and ≤ 50 MiB. Oldest go first; an unhandled folder newer than
  7 days is never deleted by the size cap alone in the editor.

### Functional — hang watchdog
- **R12** `engine/core/diag/HangWatchdog.{h,cpp}`: a named thread (`"Hang Watchdog"`) polls every
  250 ms. `diag::heartbeat()` is one relaxed atomic store of a steady-clock timestamp. It arms on
  the first heartbeat after `start()`. A stall is `now - lastBeat > effectiveThreshold`, where
  effectiveThreshold = max(threshold, budgets of active busy scopes) (R13). Defaults: editor 3 s,
  game development builds 5 s, shutdown window 15 s. Re-report every 5 s while stalled; one
  "recovered after X s" line.
- **R13** Busy scopes for known long operations: `diag::BusyScope s("Importing model.glb",
  120'000)` (label copied into a fixed 96-byte slot; up to 16 nested; budget 0 = unlimited).
  A stall inside a busy scope reports only past its budget and the report names every active
  scope. `diag::PhaseScope("lua:update")` labels without changing the budget (static string).
  Required call sites: project load and every `reportLoadingProgress` pump (also beats),
  native file dialogs (`FileDialogs.h`, unlimited), blocking ImGui-less alerts, main-thread scene
  save, model/texture import on the main thread, Play start/stop main-thread steps
  (`Project.cpp` play startup), shader compile on the main thread, engine shutdown (shutdown window).
- **R14** A report (`hang-NNN.txt` in the session folder + stderr line `JASPERENGINE HANG: …`)
  contains: stalled ms, threshold, phase, busy-scope stack, main-thread stack (D3), job-system
  occupancy (R15), frame number and last frame duration (S15 `frameMark` when present), play
  state, open scene name and Lua script/function when in a Lua phase (context slots updated by the
  main thread into fixed buffers). The watchdog thread never takes `Out`'s mutex; the editor
  Console/log line is posted when the main thread recovers.
- **R15** Job occupancy: S06 exposes `size_t jobs::snapshotOccupancy(std::span<ThreadOccupancy>)`,
  lock-free and allocation-free: per thread `{name, kind (worker|blocking|io), runningJobName
  (SubmitOptions::name, static), sinceNs}`. The watchdog sizes its buffer at `start()` and stops
  reading it before job-system shutdown (`beginShutdownWatch()`), as OpenEngine does. Until S06
  lands, the report says `job system: unavailable`.
- **R16** Runaway Lua (editor Play only): when the stall phase is a Lua phase and lasts longer
  than `editor.diagnostics.breakLuaAfterSeconds` (default 10, 0 = never), the watchdog calls
  `lua_sethook(L, hook, LUA_MASKCOUNT, 1)` (documented safe from another context in
  `engine/libs/lua/ldebug.c:105`); the hook clears itself and raises `luaL_error("script
  interrupted: <fn> ran for 10.0 s without returning (hang watchdog)")`, which flows through the
  existing script-error path (Console "Script crash", Play continues). Games never do this.
- **R17** Heartbeat sites: each backend loop iteration after event processing
  (`BackendLinux.cpp:2879`, `BackendWin.cpp:918`, `BackendMac.mm` loop), `EditorFrame::run`
  (covers live resize), `App::reportLoadingProgress`; games: `Engine::systemDraw`
  (`Engine.cpp:1219`). `Engine::systemPause` disarms and `systemResume` re-arms (mobile background).
- **R18** Opt-in hang dump: `editor.diagnostics.hangDumpAfterSeconds` (default 0 = off, Windows
  only) writes `hang-NNN.dmp` once per stall from the watchdog thread; the setting text names the
  loader-lock risk.

### Platform & backends
- **R19** Linux/Android main-thread stack for hang reports: `pthread_kill(main, SIGRTMIN+5)`
  (Android: same), handler on the main thread fills a preallocated frame buffer with
  `backtrace`/`_Unwind_Backtrace` and posts a semaphore; the watchdog waits ≤ 250 ms
  (`sem_timedwait`), else reports `stack: capture timed out`. `SA_RESTART` is set.
- **R20** macOS/iOS: `thread_suspend` + `thread_get_state` + frame-pointer walk with
  `mach_vm_read_overwrite` (no locks while suspended), `thread_resume`, then `dladdr` symbolization.
- **R21** Windows: `SuspendThread` + `GetThreadContext`; the walk runs on a helper thread with a
  250 ms deadline; the **watchdog thread** calls `ResumeThread` at the deadline whether or not the
  walk finished, so a walker blocked on a lock held by the main thread can never wedge the process.
  A timed-out walk is discarded and stack capture is disabled for the session (logged once).
- **R22** Android: crash report also goes to logcat (`__android_log_write`, tag `JasperEngine`,
  priority FATAL) line by line; chaining (R3) keeps the debuggerd tombstone and Play Console vitals.
- **R23** iOS/macOS: report notes that the authoritative report is the OS `.ips`
  (`~/Library/Logs/DiagnosticReports`, Xcode Organizer/TestFlight on iOS) and records LC_UUID so it
  matches the dSYM.
- **R24** Web: `engine/platform/emscripten/crash_pre.js` linked with `--pre-js` (in the link flags
  at `engine/CMakeLists.txt:229`, so custom shells keep it). It hooks `Module.onAbort`,
  `window` `error` and `unhandledrejection`, wraps `Module.print/printErr` into a 200-line ring,
  and on the first fatal event logs `JASPERENGINE CRASH: <message>` + stack + ring to
  `console.error`, calls `window.jasperengine.onCrash(report)` if defined (report = `{message,
  stack, logTail, buildHash, userAgent, time}`), and shows a DOM overlay "The game stopped
  working. [Reload]" unless the export setting **Web › Show Crash Overlay** is off. ≤ 4 KiB minified.
- **R25** Linux/macOS core parity: environment `JASPERENGINE_CRASH_CORE=1` raises `RLIMIT_CORE`
  to the hard limit at install; `crash.txt` says where cores go (`coredumpctl` hint when
  `/proc/sys/kernel/core_pattern` starts with `|`, read at install).

### Performance
- **R26** `heartbeat()` ≤ 5 ns; watchdog thread ≤ 0.1 % of one core idle; ring write ≤ 150 ns
  plus memcpy per log line; install ≤ 3 ms; memory ≤ ring + 64 KiB per thread alt stack + 64 KiB
  watchdog buffers. Disabled watchdog = no thread. No per-frame allocation anywhere.

### Editor UX (human)
- **R27** On start, an abnormal previous session (R8) shows a non-modal banner under the menu bar
  (and a Console warning), after any S03 "Recover Unsaved Changes" modal:
  `JasperEngine closed unexpectedly last time (SIGSEGV at 14:32). A crash report was saved.`
  Buttons **Open Report**, **Show in Folder**, **Copy Report Path**, **Dismiss** (writes `handled`).
  Variants: "…while it was not responding (hang report saved)" and "…no report was saved".
- **R28** After a hang recovers, a footer toast for 8 s: `The editor was not responding for 7.4 s
  (Importing model.glb). Details saved.` with **Open**. At most one toast per 60 s.
- **R29** Preferences › **Diagnostics**: Hang Watchdog (on), Report After (s) [1–60, 3], Interrupt
  Runaway Lua In Play After (s) [0–120, 10], Hang Minidump After (s) [Windows, 0], Show Crash
  Notice On Startup (on), **Open Crash Reports Folder**. Undoable through the settings path.
- **R30** Project Settings › Player › **Crash Handling**: Enabled (on), Include Log Tail (on),
  Windows Dump Type (Small | With Heap, Small), Hang Watchdog In Development Builds (on, threshold
  5 s), Web › Show Crash Overlay (on). New keys default as listed; old projects load unchanged.

### Agent UX (MCP, Lua, CLI)
- **R31** MCP tools (`editor/mcp/tools/DiagnosticsTools.cpp`, new):
  - `get_editor_health {}` — answered **off the main thread** (new `ToolDef::offMainThread`
    flag, used by this tool only): `{responsive, stalled_ms, phase, busy_scopes[], watchdog
    {enabled, threshold_ms}}`. Lets an agent tell "hung" from "slow" when other calls time out.
  - `get_crash_reports {limit=10, include_text=false}` → `[{id, kind: crash|hang|abnormal_exit,
    time, reason, product, version, folder, files[], handled}]` and `notice_open` (R27 banner),
    plus `crash.txt` text on request. `dismiss_crash_notice {id}` = the banner's Dismiss.
  - `set_hang_watchdog {enabled?, threshold_ms?, break_lua_after_s?}` (settings path, undoable).
  - `debug_crash {mode: segv|abort|terminate|stack_overflow|worker_segv|hang, hang_ms?}` —
    refused unless the editor was started with `--allow-debug-crash` or
    `JASPERENGINE_ALLOW_DEBUG_CRASH=1`.
  - `run_game` result gains `crash_report` (folder path) when the player wrote one.
- **R32** Lua (games and editor Play): `Crash.setReportHook(fn(report) -> bool)`, called at the
  next start once per unhandled report (report = `{id, kind, reason, time, version, files}`,
  file paths readable via the engine file API); `true` writes `handled`.
  `Crash.getPendingReports()`, `Crash.markHandled(id)`, `Crash.lastSessionEndedAbnormally()`,
  `Crash.setTag(key, value)` (16 fixed slots × 128 bytes, written into `crash.txt`). C++ mirror in
  `diag::crash`. The engine performs no network I/O for reports, ever.
- **R33** CLI: through S31 `jasperengine-editor call get_crash_reports`. Env (editor and dev
  games): `JASPERENGINE_HANG_WATCHDOG=0|1`, `JASPERENGINE_HANG_WATCHDOG_MS`,
  `JASPERENGINE_HANG_WATCHDOG_SHUTDOWN_MS`, `JASPERENGINE_CRASH_CORE`,
  `JASPERENGINE_CRASH_REPORT_DIR` (override root; used by tests). Development builds also honour
  `JASPERENGINE_DEBUG_CRASH=<mode>@<frame>` and web `?jasperengine_debug_crash=<mode>`.
- **R34** `isFatalLine` (`BuildTools.cpp:1079`) and `fatal_lines` (`smoke-test.sh:99`) match
  `JASPERENGINE CRASH:`; `JASPERENGINE HANG:` is a warning, not fatal.
- **R35** Docs: `docs/crash-diagnostics.md` (folder layout, symbolication per platform, privacy
  note for the hook: reports contain paths and log lines), MCP agent guide section on
  `get_editor_health`/`get_crash_reports`.

### Data & compatibility
- **R36** No scene/asset format change, no S03 version bump. New project settings and editor
  preferences default as listed. `crash.txt` is versioned by its first line; readers accept v1 and
  ignore unknown keys.

## 6. Design constraints & decisions

- **D1 Engine-owned.** One implementation in `engine/core/diag/` so games and editor share it.
  Product string and root come from the caller. Built into `jasperengine` on every target.
- **D2 Async-signal-safety is a review rule.** Everything reachable from a POSIX handler lives in
  `CrashHandlerPosix.cpp` with a header comment listing allowed calls; no `<cstdio>` include there.
- **D3 Never let diagnostics wedge the process.** Every cross-thread capture has a deadline, and
  the party that can be blocked is never the one that resumes the main thread (R19-R21).
- **D4 Dump types.** Editor: `MiniDumpWithThreadInfo|IndirectlyReferencedMemory|HandleData|
  UnloadedModules` (Debug builds add `FullMemory`). Games: `MiniDumpNormal|ThreadInfo|
  UnloadedModules` (Small) or + `IndirectlyReferencedMemory` (With Heap). One dump per process.
- **D5 Busy scopes raise budgets, they do not silence.** A wedged import still reports after 120 s.
- **D6 Upload is a hook at next start, never in the dying process.** No HTTP in the engine.
- **D7 Symbolication.** `tools/crash/symbolicate.py` (stdlib, `-I`-safe): reads `crash.txt`/
  `hang-*.txt`, finds symbols under the S30 layout `symbols/<platform>/<build_id>/`, runs
  `llvm-symbolizer` (fallback `addr2line` / `atos`), and maps web frames with the emscripten
  symbol map. Windows: prints the `cdb -z crash.dmp -y <symdir> -c "!analyze -v; q"` / Visual
  Studio steps. S30 must keep build IDs and emit `.pdb`/`.debug`/`.dSYM`/symbol maps.

```cpp
namespace jasperengine::diag::crash {
struct Config { const char* product; std::filesystem::path root; size_t ringBytes = 64 << 10;
                DumpKind dumpKind = DumpKind::Small; bool includeLogTail = true; };
void install(const Config&);             // once, main thread, as early as possible
void prepareThread();                    // alt stack / stack guarantee; S06 spawnNamed calls it
void handleFatalSignal(int, siginfo_t*, void*);   // POSIX; CrashGuard chains here
void ringWrite(std::string_view line);   // Out/Log feed
void setTag(std::string_view key, std::string_view value);
std::vector<ReportInfo> pendingReports(); void markHandled(std::string_view id);
void setReportHook(std::function<bool(const ReportInfo&)>); bool lastSessionEndedAbnormally();
}
namespace jasperengine::diag {
void heartbeat();
struct WatchdogConfig { uint32_t thresholdMs = 3000, shutdownMs = 15000; bool enabled = true; };
namespace watchdog { void start(const WatchdogConfig&); void beginShutdownWatch(); void stop();
                     HealthSnapshot health(); /* any thread */ }
class BusyScope  { public: BusyScope(std::string_view label, uint32_t budgetMs = 120'000); ~BusyScope(); };
class PhaseScope { public: explicit PhaseScope(const char* staticLabel); ~PhaseScope(); };
}
```

## 7. Acceptance criteria

Crash tests use `engine/tests/crash/CrashTestMain.cpp` → **`jasperengine-crash-test`**
(`--mode segv|abort|terminate|stack_overflow|worker_segv|fpe|pure_call|hang --out DIR`), driven by
`engine/tests/crash/test_crash.py` (CTest, stdlib) on every desktop CI leg; unit tests on S02's
gtest targets.

- **AC1 (R1-R4, R10)** Linux/macOS, each mode: process dies with the original signal (exit status
  checked), `DIR/reports/*/crash.txt` exists with every R10 key, ≥ 3 frames, a frame naming
  `CrashTestMain`'s crash function after `symbolicate.py`, non-empty `log-tail.txt` containing the
  last line logged before the crash, and stderr contains `JASPERENGINE CRASH:`.
- **AC2 (R3)** `stack_overflow` on main and on a worker thread produces a report (fails today).
- **AC3 (R5, D4)** Windows, each mode incl. `pure_call` and `abort`: `crash.dmp` opens with
  `cdb -z` and `.ecxr; k` shows the crash function (CI step); the faulting thread in the dump is
  the crashing thread; total time to exit ≤ 10 s.
- **AC4 (R3)** `CrashChain.PreviousHandlerRuns`: a test installs a sentinel `sigaction` before
  `install`; after a crash the sentinel's marker file exists. Under the ASan preset, ASan's report
  still prints after ours.
- **AC5 (R7)** `CrashGuard.WorkerCrashReachesHandler`: main thread inside a guard, worker
  segfaults → crash report exists (fails today). A guarded Lua crash writes no folder.
- **AC6 (R4, D2)** Static check in CI: `nm`/symbol scan of `CrashHandlerPosix.o` references no
  `printf`-family, `malloc`, `operator new` or `pthread_mutex_lock`.
- **AC7 (R12, R14)** `HangWatchdog.InjectedStallReports` (gtest, thresholds 200 ms): a fake main
  thread beats, then sleeps 700 ms → one report with stalled ≥ 200 ms, phase name, a stack frame
  naming the test's stall function (Linux/macOS/Windows), and a "recovered" line; beats every
  50 ms for 2 s → zero reports. `HangWatchdog.NotArmedBeforeFirstBeat` and
  `.ReReportsEvery5s` (clock injected) pass. Ported from their `MainThreadHangWatchdogTests.cpp`:
  shutdown-window report, healthy teardown silent, job system not read after `beginShutdownWatch`.
- **AC8 (R13)** `HangWatchdog.BusyScopeRaisesBudget`: stall 700 ms inside `BusyScope(…, 2000)` →
  no report; stall 2.5 s → report listing the scope label; nested scopes listed innermost-last.
- **AC9 (R15)** `HangWatchdog.ReportsJobOccupancy`: two workers run named jobs that block on a
  latch → report lists both names with durations; TSan clean.
- **AC10 (R21, D3)** Windows `HangWatchdog.WalkerDeadlineResumesMainThread`: walker forced to
  block (test hook) → main thread resumes within 300 ms, report says capture timed out, process
  exits normally.
- **AC11 (R16)** MCP `tools/mcp/tests/test_crash.py::test_runaway_lua`: Play a fixture whose
  `update` runs `while true do end`, `break_lua_after_s=2` → within 6 s the Console has the
  "script interrupted" error, `get_editor_health.responsive` is true, Play is still running.
- **AC12 (R31)** `test_crash.py::test_health_off_main`: `debug_crash {mode:"hang", hang_ms:6000}`
  with threshold 1000 → `get_editor_health` answers within 500 ms with `responsive:false`; after
  recovery `get_crash_reports` lists a `hang` entry. `debug_crash` without the flag → refused.
- **AC13 (R8, R27)** `test_crash.py::test_notice_after_crash`: editor started with
  `--allow-debug-crash`, `debug_crash segv` → restart → `get_crash_reports[0].handled == false`,
  `notice_open == true`, `dismiss_crash_notice` → `handled` file exists; restart →
  `notice_open == false`. Manual: the banner text matches R27 and its buttons work. Killing with SIGKILL → next start reports `abnormal_exit` without a report.
- **AC14 (R11)** `CrashRetention.PrunesOldest` (gtest, temp root with 25 fake folders) keeps 20.
- **AC15 (R22, R3)** Android emulator CI leg (or manual check if no emulator runner): APK built
  with `JASPERENGINE_DEBUG_CRASH=segv@10` → `adb logcat -s JasperEngine` shows the report lines,
  `adb shell ls /data/tombstones` (or `bugreport`) has a tombstone for the pid, and the report
  folder exists in the app files dir.
- **AC16 (R24)** `tools/web/browser_gate.py` on a dev web export with
  `?jasperengine_debug_crash=abort --expect "JASPERENGINE CRASH:"` passes; an `--eval` that
  defines `window.jasperengine.onCrash` records the call with non-empty `stack` and `logTail`.
- **AC17 (R32)** `CrashHook.DeliversPendingOnce`: fake report folder → hook called once, returns
  true → `handled` written; second start → not called. Lua test in `test_crash.py` does the same
  via `Crash.setReportHook`.
- **AC18 (R26)** `je-bench-diag`: heartbeat ≤ 5 ns, ring write ≤ 150 ns for 120-byte lines;
  `tools/smoke-test.sh` editor+game runs show 0 `JASPERENGINE HANG:` lines (no false positives
  under llvmpipe) and stay green.
- **AC19 (R34)** `test_build.py`: `run_game` on a crashing dev build returns `crashed:true` and a
  `crash_report` path.
- **AC20 (iOS, manual)** Simulator run with `JASPERENGINE_DEBUG_CRASH=segv@10`: `crash.txt` in
  `Library/Caches/crash/reports/*`, its `build_id` equals `dwarfdump --uuid` of the dSYM.

## 8. Dependencies & conflicts

- Depends on: S01 (C++20), S02 (gtest targets, CTest, presets, browser gate, CI legs), S06
  (`snapshotOccupancy`, `spawnNamed` calling `prepareThread`; S28 adds `snapshotOccupancy` to
  `engine/core/jobs/` itself if S06 shipped without it), S03 (session/recovery modal ordering;
  `editor.lock` pid semantics reused), S15 (`developmentBuild` export flag, `frameMark`), S30
  (symbol split layout, build hash for web), S31 (`call` CLI). S09 feeds R9's ring when it lands.
- Blocks nothing; S30 publish verification should check that build IDs in binaries match symbols.
- Merge hotspots: `engine/core/Engine.cpp` (S03, S06, S09), `engine/CMakeLists.txt` web link
  flags (S01, S02, S30), `editor/App.cpp` init/footer, `editor/backend/Backend*.{cpp,mm}` loops,
  `editor/mcp/McpServer.cpp` (off-main flag), `editor/mcp/tools/BuildTools.cpp`, `editor/Out.cpp`
  (S09), `engine/core/util/CrashGuard.h`, `tools/smoke-test.sh` (S02).

## 9. Risks & defaults

- **Biggest risk: a diagnostic that hangs or corrupts the crash it reports.** Mitigated by D2/D3,
  the writer thread on Windows, AC6 and AC10. Default: when in doubt, skip the data, keep the exit.
- Linux `backtrace()` in the hang-capture signal handler can run while the main thread is inside
  the dynamic loader; glibc's loader lock is recursive, and the 250 ms deadline bounds the rest.
  If a platform shows a deadlock, that platform falls back to "no stack" (phase + occupancy only).
- Signal number `SIGRTMIN+5` could clash with a third-party library: default kept, overridable
  with `JASPERENGINE_HANG_SIGNAL`.
- Minidumps may contain player data from memory: games default to Small dumps; the docs tell
  developers to get consent before sending any report through their hook.
- macOS/iOS signal handlers cooperate with, not replace, ReportCrash; the OS report stays the
  authoritative one there.
- Watchdog false positives on slow CI (llvmpipe): default 3 s editor threshold, busy scopes on
  every known long path; AC18 keeps the smoke runs at zero hang lines.
- Lua interruption may leave a script half-way through its update: acceptable in Play (same as a
  script error today); never enabled in shipped games.

## Deviations

Recorded by the S28 implementation (branch `feat/s28-robustness`).

- **Native backends are implemented on `feat/s28-native-crash`.** The editor now forwards to
  the engine POSIX/Windows backends, including alternate stacks/stack guarantees, saved signal
  actions, Windows writer-thread dumps, CrashGuard forwarding and bounded main-thread capture.
  Apple uses the signal-capture alternative (SIGUSR2, configurable), rather than a Mach frame
  walk. POSIX capture waits with monotonic 1 ms polling rather than `sem_timedwait`: Darwin
  has no unnamed semaphores, and a realtime-clock step must not extend the 250 ms deadline.
  A timed-out capture disables further stack captures for the process, keeping its storage
  alive for a late handler/helper. Modules are snapshotted at installation for fatal-path
  frame formatting; the bounded cache retains our image even when Android loads hundreds
  of system libraries first. Linux/Android also copy the current `/proc/self/maps` so omitted
  or later-loaded libraries remain recorded. Windows waits at most eight seconds for the writer,
  below R5's 30-second ceiling. If DbgHelp's process-wide lock is held by the faulting thread,
  the writer skips the dump/walk and writes saved PC/SP instead of waiting on that lock.
  Remaining native-platform checks requiring their hosts: Windows dump inspection (AC3),
  Windows forced-walker deadline execution (AC10), Apple builds and simulator checks (AC20).
  R18's optional hang minidump remains outside this backend task and is not exposed in UI.
- **Preferences are not undo steps** (R29, R31 `set_hang_watchdog`): Preferences > Diagnostics and
  `set_hang_watchdog` apply at once and save, like every other preference and
  `set_project_settings` (which says "not undoable"); the dialog's Cancel reverts unsaved edits.
- **Hang threshold in whole seconds** (R29): Report After is in seconds (1-60) as the spec's UI;
  `set_hang_watchdog threshold_ms` rounds to whole seconds. `JASPERENGINE_HANG_WATCHDOG_MS` keeps
  millisecond precision.
- **The heartbeat is a counter** (R12, R26): `diag::heartbeat()` is one relaxed increment (no
  clock read: a steady-clock read alone costs more than R26's 5 ns); the watchdog timestamps the
  change when it polls, so a stall is measured to within one poll period (250 ms).
- **Runaway Lua keeps Play running** (R16): today's script-error path pauses and stops Play, so an
  interrupt is logged as "Script crash: script interrupted: ..." by the error paths themselves
  (`LuaWatchdog::takeInterrupt`), not handed to the crash hook, as R16 and AC11 require.
- **`run_game` is `run_build`** (R31, R34, AC19): the tool's name in this code base; it gains
  `crash_report`.
- **Web build hash** (R2, R24): S30's build hash is used when `JASPERENGINE_BUILD_HASH` is defined;
  until S30 defines it, a hash of the engine's compile time stands in.
- **Crash Handling settings on Xcode exports** (R30): like Player > Logging, they reach a game
  through the exported CMake project; the Xcode workspace (iOS, macOS Xcode) builds with the
  defaults.
- **`HangWatchdog.*` beat test** (AC7): the "beats every 50 ms" case runs 0.9 s, not 2 s, to stay
  inside the 1 s rule of non-Slow unit tests.
- **Tool verbs**: `dismiss` and `debug` were added to the verb list of
  `test_tool_conventions.py` for the spec's `dismiss_crash_notice` and `debug_crash`.

### Native backend validation (`feat/s28-native-crash`)

All execution uses `je-remote/rb.sh`; no local builds or tests.

- Red control, before the backend/CrashGuard fix: server D,
  `WERROR=1 UNIT_SUITE=core UNIT_TEST_ARGS=--gtest_filter=CrashGuard.WorkerCrashReachesHandler
  tools/build.sh unit-tests` fails with "a worker crash during a Lua guard must reach the engine
  handler" (1 ran, 1 failed; no crash.txt).
- Server D: `ctest --test-dir build/linux/core-tests -R 'native-crash|posix-crash'
  --output-on-failure`: 2/2 pass. Fatal subprocess modes segv, abort, terminate, main/worker
  overflow, worker segv, fpe, guarded-worker segv, saved-handler sentinel and the injected
  main-thread stall all pass; reports retain registers, >=3 frames, build ID and log tail.
  The object scan finds no printf, malloc/new/delete or mutex-lock import.
- Server D, ASan+UBSan tree: `python3 -I engine/tests/crash/test_crash.py
  build/linux/core-tests-address-undefined/tests/core/jasperengine-crash-test
  --symbolicate tools/crash/symbolicate.py --expect-asan`: all 10 modes pass, explicitly checking
  that ASan's prior fatal handler prints after the engine report.
- Server D, refreshed focused gtest run: `Crash*:HangWatchdog.*` runs 29/29 successfully
  in both normal and ASan+UBSan trees, including the saved handler returning, recovered guard,
  pending hook and main-thread frame assertions. No new diagnostic allocation leak or UB is
  reported. The sanitizer process exits with the existing 56-byte `jobs::setProfilerHooks`
  leak from `GpuEnvironment::initEngine` (the common test harness), after all assertions pass.

- Server D, full normal rerun after the hook fix: 1,077 core tests and 392 editor tests pass,
  zero failures, zero skips (both process exit codes zero). This includes both
  `LoggerCrashFlushTest` cases through the editor compatibility forwarder. The initial shared-D
  timing failure did not recur in this run.
- Final refreshed server D checks after the bounded module-cache priority fix:
  `WERROR=1 tools/build.sh linux` and `tools/build.sh web` both exit zero; native CTest
  (`native-crash-reports`, `posix-crash-signal-safety`) is 2/2; ASan+UBSan native subprocess
  tests are 10/10. `LoggerCrashFlushTest.*` is 2/2 in normal and ASan+UBSan builds, with
  WERROR enabled; only the baseline 56-byte profiler-hook harness allocation remains.
- Server D MCP: `test_protocol.py` is 12/12; `test_crash.py` is 5/6, including off-main health,
  the deliberate native segfault/restart notice and abnormal-session hook delivery. The existing
  `test_runaway_lua` misses its Console interruption error. Isolated controls also fail with
  `JASPERENGINE_HANG_SIGNAL=0` (40.3 s) and the default signal (37.8 s), so the failure does not
  require native stack-capture delivery. Root cause (`feat/s28-robustness` da086c4a8, in this
  merge): the interrupt raised its error with `luaL_error(L, "...%.1f s...")`, and
  `lua_pushfstring` has no precision, so the script failed with "invalid option '%.' to
  'lua_pushfstring'" instead of "script interrupted"; the message is now formatted with
  `snprintf` first. Red on server A before the fix (the Console showed that Lua error), green
  after (`test_crash.py` 5/5); the native branch was based on d397cb284, before the fix.
- Server D smoke, the remote host's `tools/smoke-test.sh "$HOME/je/fixture-unity"`: export,
  standalone build, game and editor runs all pass; screenshots are saved and neither log has
  a crash, error or `JASPERENGINE HANG:` line. The brief's `--img` invocation cannot launch
  Docker inside the build image, so the same script runs from the remote host instead.
- Server A, final Android build: `ANDROID_PROJECT=crash-diagnostics tools/build.sh android`
  passes for all four ABIs. The final API 34 x86_64 emulator run with
  `tools/android-emulator-test.sh build/android-crash-diagnostics/app-debug.apk
  --expect-native-crash --expect 'JASPERENGINE CRASH:' --out build/android-native-module-green`
  exits zero. Artifacts include the engine report, fatal logcat marker and preserved debuggerd
  tombstone. Both reports identify fault thread 1959 (`je-main`), build ID
  `ca276c8e0a5b57b07ad161629e5a81ead4421d9f`, and the same first game frame
  (`libjasperengine-android.so+0x1fc4b4e`, `debugSegv()+14` in the tombstone).
  The strengthened game-module assertion first failed against the saved old APK, then passed
  after the bounded module cache retained our image. Final artifacts remain on A in
  `build/android-native-module-green`; the final APK SHA-256 is
  `51bafb346db8457f3f9696c7e17d306789001e7687885a76e475da4a3fb995de`.

#### Baseline gate failures owned by other streams

Per the integration owner's instruction, this branch does not change texture, scene or profiler
ownership and does not suppress their engine/editor leaks. BF2 (`fix/bf2-green-oe-port`) owns
those fixes. The full ASan+UBSan run identified:

- `Profiler.CommandLineCaptureWritesItsFile`, `JobsHooks.JobsReportBeginAndEnd` and
  `JobsParallelForLanes.*`: profiler-hook snapshots allocated by `jobs::setProfilerHooks` are
  retained; the full core run reports 280 bytes in five allocations. Even a focused CPU run
  inherits one 56-byte snapshot from common engine initialization.
- `Compat.CorpusRoundTrip` and `HotReloadServiceTests.*`: texture image buffers (default sky
  and hot reload) and `Project::createSceneRender`/scene restoration ownership leak; the full
  editor run reports 273,768,644 bytes in 24,640 allocations.
- `ToolchainFingerprint.CompareNamesEachDifferingField`: the Debug sanitizer fixture already
  has `ndebug=0`, so assigning zero does not create the third expected mismatch (two vs three).
  The test fixture remains unchanged on this branch.
- `NumericExpression.EvaluationWithinBudget` (ASan) and
  `LoadReportOverheadBench.DecodeWithReport` (normal): wall-clock limits exceeded on shared D.
  Timing acceptance is authoritative only on E; these unrelated D results are informational.

The initial full normal run's two `CrashHook.*` failures were a real report-hook mutex
reentrancy deadlock; this branch fixes it, and both refreshed normal and sanitizer focused
runs pass them. Fatal helper processes deliberately print sanitizer signal diagnostics after
our report; those are expected evidence that the saved sanitizer handler is still chained.

### Merged branch gate (`feat/s28-robustness` after merging oe-port and `feat/s28-native-crash`)

All through `je-remote/rb.sh`; merge commit b8808a966 plus the fixes after it.

- Server A: `WERROR=1 tools/build.sh linux` passes. `WERROR=1 tools/build.sh unit-tests`: 1,078 core
  and 392 editor tests, 0 failed.
- Server A, `SANITIZE=address,undefined tools/build.sh unit-tests`: core 1,078 ran, 2 failed:
  `ToolchainFingerprint.CompareNamesEachDifferingField` (the known Debug fixture value) and
  `Backends/FileWatchServiceTests.PumpBudget/Native` (wall clock: 44 ms against its 20 ms sanitizer
  limit on the loaded host; S05's file watcher). Editor 392 ran, 1 failed:
  `NumericExpression.EvaluationWithinBudget` (wall clock, known). The run's container log was
  lost when its local wrapper stopped (the results come from its gtest XML), so its leak
  summary is not recorded here; the native branch's sanitizer record above lists the known
  leaks of other streams.
- MCP (A): `test_crash.py` 6/6 (`test_runaway_lua` included; the third start of the two notice
  tests now checks the dismissed report itself, since the harness kills an editor slower than
  2 s to exit and that session is rightly reported as abnormal) and `test_protocol.py` 12/12;
  `test_tool_budget`, `test_tool_conventions`, `test_tool_coverage`, `test_docs`, `test_autosync`,
  `test_tool_groups`, `test_lua_api_json` pass.
- Web (A): `tools/build.sh web` + `web-gate` PASS; `WEB_PROJECT=crash-diagnostics` build +
  `GATE=crash-diagnostics` PASS (ready line, then `JASPERENGINE CRASH:`, `onCrash` with stack and
  log tail, the overlay).
- Android (A): `tools/build.sh android` passes; `ANDROID_PROJECT=crash-diagnostics` APK on the API 34
  emulator with `--expect-native-crash --expect 'JASPERENGINE CRASH:'`: PASS (engine report and
  debuggerd tombstone, `F/JasperEngine: JASPERENGINE CRASH: SIGSEGV (invalid memory access)`).
- Smoke (A): `tools/smoke-test.sh tools/fixtures/post-processing` PASS, no `JASPERENGINE HANG:` line.
- Server E, `je-bench-diag`: heartbeat 2.25 ns (<= 5), 120-byte ring write 11.2 ns (<= 150),
  install 0.58 ms (<= 3), idle watchdog 0.056 % of a core (<= 0.1): every budget holds.

Owner review follow-up on `feat/s28-native-crash`: concurrent faulting threads now wait up to
8 seconds for report completion before chaining (lock-free owner plus monotonic/nanosleep wait
on POSIX, the writer's completion event on Windows). A recursive fault on the reporting thread
or Windows writer immediately reaches the OS terminating action. A null-by-default internal
subprocess-test seam delays or recursively faults the first reporter before it opens its report.
POSIX stacks locate the saved fault PC and omit the handler/trampoline and duplicate PC; if that
boundary cannot be unwound, only the saved PC is retained. Symbol names are resolved offline
with `symbolicate.py`, including late-loaded libraries through the maps copy, keeping `dladdr`
and symbol-loader calls out of the fatal path. Tests check both named fault frames and absence
of handler frames. Windows dynamically resolves `GetThreadDescription` during installation;
its writer adds `thread_name`, falling back to `unknown` on older systems.

After merging oe-port again (S11, 4d5ae3093): server B `WERROR=1 tools/build.sh unit-tests` 1,121 core
and 398 editor tests, 0 failed; server D `WERROR=1 tools/build.sh linux`, MCP 43/43
(`test_crash.py` 6/6, `test_protocol.py` 12/12, budget, conventions, coverage, docs, tool groups),
`tools/build.sh web` + `web-gate` PASS, the crash-diagnostics web gate PASS, `tools/build.sh android`
passes, smoke PASS. `tools/crash/test_symbolicate.py` passes on B against a fresh
`jasperengine-crash-test`.


After merging oe-port e1ca0c71a and `feat/s28-native-crash` round 2 (0ef567c61; its last two
commits are records only), all through `je-remote/rb.sh`:

- Server D `WERROR=1 tools/build.sh unit-tests`: 1,176 core and 770 editor tests, 0 failed. Server B
  `WERROR=1 tools/build.sh linux` passes.
- Server D `SANITIZE=address,undefined tools/build.sh unit-tests`: core 1,173 ran, 1 failed
  (`Backends/FileWatchServiceTests.PumpBudget/Native`, BF2's). Core leak summary: 280 bytes in 5
  allocations, all `jobs::setProfilerHooks` from the `JobsHooks`/`JobsParallelForLanes` tests (known);
  no UBSan diagnostic. The SEGV summaries in the log are the deliberate crash subprocesses
  (`CrashGuard.WorkerCrashReachesHandler`, `LoggerCrashFlushTest`), ASan's report following ours.
  The editor binary stops on a SIGTRAP: Jolt's Debug `JPH_ASSERT(errors == None)` at
  `PhysicsSystem.cpp:616` fires in `PlacementWorldReview.OverflowingContactBuffersAreReported` and
  `PhysicsPlacementSim.OverflowingContactsAreReported`, which overflow the contact buffers on
  purpose (upstream physics-placement; the sanitizer build is Debug, so Jolt asserts are on).
  Rerun excluding those two (`--gtest_filter=-*.OverflowingContact*`): 768 ran, 2 failed,
  `TexSynthOutOfMemory.IsAFailedStatusNotATerminate` (ASan's allocator aborts instead of throwing
  `bad_alloc`) and `NumericExpression.EvaluationWithinBudget` (wall clock, known). Editor leak
  summary: 310,690,257 bytes in 30,600 allocations; no leak stack has an S28 frame. The largest roots
  are `Compat.CorpusRoundTrip` (known), `PhysicsPlacementSim.*`, and the undo, prefab-undo and
  play-change review tests.
- Server D: MCP 63/63, including `test_crash.py`, `test_protocol.py`, budget, conventions, coverage
  and docs. AC19 `test_build.py` 2/2 (the `JASPER_SLOW_BUILD=1` development build in 850 s), with
  `crash_report` an absolute path. `docs/mcp/TOOLS.md` regenerated: 203 tools.
- Server D: `tools/build.sh web` + `web-gate` PASS; the crash-diagnostics web build + gate PASS;
  `tools/build.sh android` passes; smoke PASS.
- Server C: the API 34 x86_64 emulator check on the crash-diagnostics APK passes (booted in 65 s;
  engine report, debuggerd tombstone, `F/JasperEngine: JASPERENGINE CRASH: SIGSEGV`).
- Server E, `je-bench-diag`: heartbeat 2.495 ns, ring write 11.27 ns, install 1.113 ms, idle
  watchdog 0.064 % of a core; every budget holds.

### Native review follow-up validation

- Server D red control rebuilt the test-only checkpoint `84c6c0281` (the original handlers
  plus the internal fault-injection seam), then ran the current subprocess driver with
  `--mode concurrent`. It failed because no `crash.txt` existed: exit `-11`, stderr
  `JASPERENGINE CRASH: another fatal signal while reporting`. The separate `--mode recursive`
  control terminated immediately. The gate restored the fixed sources before its next build.
- Server D fixed-source gate: `WERROR=1 tools/build.sh linux` passes (editor/install and
  standalone game). The crash CTests (`native-crash-reports`, `posix-crash-signal-safety`,
  `crash.symbolicate`) pass 3/3 in 15.77 s. All 13 POSIX subprocess modes pass, including
  complete concurrent-fault reports, immediate recursive termination, and saved-handler
  chaining after an earlier report's handler returned. The segfault cases' symbolicated stacks contain the
  fault function once; all symbolicated crash stacks omit engine handler frames.
- Windows review additionally covers recursive CRT faults: the synthetic path now terminates
  directly before invoking any saved abort callback. Its Windows-only `recursive_abort`
  subprocess installs a deliberately stalled previous abort handler; execution awaits a
  Windows host, alongside the existing Windows dump/deadline checks.
- Server D ASan+UBSan red controls also fail as intended: the original concurrent handler
  exits 77 with no report, and the original recursive handler enters ASan's saved handler
  instead of taking the OS action directly (the subprocess exits 77). After restoring the
  fixed sources and rebuilding, all 13 sanitizer subprocess modes pass, including ordinary
  segfault chaining to ASan, concurrent report completion, direct recursive termination and
  saved-handler chaining after a completed report. No unexpected leak or UB diagnostic is
  emitted by the fixed runs; the red controls' sanitizer SEGV output is deliberate evidence.
- Server D crash/protocol MCP gate: 18 passed, 0 failed, 0 skipped in 53.3 s
  (`test_crash.py` 6/6, `test_protocol.py` 12/12), including `test_runaway_lua` and the
  native segfault/restart notice. This verifies the S28 fixes merged at `3b0dd7a14` together
  with the native follow-up. All temporary red-control sources were restored afterward.
- Final source `a1555b8d1` omits optional secondary-fault stderr notes, so a full pipe cannot
  add an engine write after the bounded wait. Server D revalidation passes again: WERROR
  editor/install and standalone game, 3/3 crash CTests (15.66 s), and all 13 ASan+UBSan modes.
- Server D final Android fixture build (`ANDROID_PROJECT=crash-diagnostics tools/build.sh
  android`) passes all four ABIs: arm64-v8a, armeabi-v7a, x86 and x86_64 (40m 6s).
  The 107,656,922-byte APK SHA-256 is
  `93c668ff351fb0da56ed8267e261ba27d369b9fbbba6860c665fbdd53008a6b0`; it matches after transfer
  to A for the emulator-only gate. The initial A build attempt failed from disk exhaustion
  before compilation; the integration owner freed space and directed APK builds to B/D.
- Server A final API 34 x86_64 emulator gate exits zero: boot in 55 s, engine report,
  `F/JasperEngine: JASPERENGINE CRASH: SIGSEGV` and preserved debuggerd tombstone all found.
  The engine stack contains the game callers without reporter/trampoline duplication. Its
  first frame `libjasperengine-android.so+0x2099e4e` matches debuggerd's `debugSegv()+14`; both
  record thread 1967 (`je-main`), PC `0x7aa31bcbae4e`, SP `0x7aa3259f5368`, and build ID
  `bc4d40fddcd6771488eedfbc73fdf9361207c3b2`. The crash report is complete and parseable.
  Evidence remains on A in `build/android-native-review-green` (1.2 MB) and was fetched for
  inspection. Per the integration owner's instruction, the APK, Gradle cache and Android
  native/Java build outputs in this worktree on A were deleted after the passing run.

### Native review round 3

- Windows names are snapshots taken by `prepareThread()` (after `spawnNamed` sets the OS
  name). `GetThreadDescription`, UTF-8 conversion and `LocalFree` run only there. Native,
  trivially initialized TLS holds the fixed-size name; the fault submitter copies it to
  preallocated writer storage. Unprepared threads and unavailable APIs report `unknown`.
  The writer performs no thread-name allocation or lookup. The Windows-only
  `cached_thread_name` subprocess renames the OS thread after preparation and requires the
  cached name. A process-heap-lock fault test could not be validated here: no Windows
  runner is available, and the existing DbgHelp/minidump path independently uses the heap,
  so locking the entire process heap does not isolate this name-lookup regression.
- POSIX recursive reporter faults restore and chain the saved signal handler before OS
  termination. If that handler returns, the default fatal disposition runs immediately;
  the damaged writer never resumes. Restoring the previous disposition before calling it
  prevents its signal re-delivery from re-entering our writer. Subprocess regressions
  cover the sanitizer handler, a saved terminating handler and a saved returning handler.
- POSIX frame trimming is a pure, signal-safe helper. When the interrupted PC is absent
  (possible with Apple unwinders), it retains the available trace after the known
  capture/reporter prefix, extending that prefix through the dispatch return PC when
  present. Both capture and reporter are explicitly noinline. The saved PC remains #0;
  symbol names are still resolved offline by `symbolicate.py`, with no loader/heap calls
  in the signal path. Synthetic tests cover PC match, missing/zero PC, dispatch prefix,
  short/empty traces and ARM Thumb address normalization. Apple execution awaits its SDK
  host; the portable trim tests execute on Linux.
- Server D red control at `7e62b9d4f`: the synthetic
  `CrashFrames.MissingPcKeepsCallersAfterKnownHandlerFrames` test fails for both absent
  and zero interrupted PCs (returns 4 rather than the required caller start 2). The
  `recursive_chain` subprocess also fails because its saved-handler sentinel is absent.
  The fixed-source gate uses `1933cfa6b`; no temporary red-source edits are left in git.
