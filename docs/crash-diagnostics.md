# Crash and hang diagnostics

When the editor or a game dies or freezes, JasperEngine keeps evidence you can act on: a report
folder per session with the reason, the last log lines and (where the platform backend is in the
build) a backtrace and a Windows minidump; and a main-thread **hang watchdog** that turns "it
froze" into a report naming what the main thread was doing and what every job worker ran. The
design and acceptance record is `docs/specs/S28-robustness.md`.

Nothing is ever sent anywhere. A game decides what to do with its reports through a hook at the
next start (below).

## Platform status

| | Report folders, session markers, abnormal-exit detection, log tail ring | Native crash handler (crash.txt with frames, minidump) | Hang watchdog stack capture |
|---|---|---|---|
| Linux, Android | yes | alternate-stack signal handler, text report; chains to sanitizers/debuggerd | main-thread signal capture, 250 ms deadline |
| macOS, iOS | yes | alternate-stack signal handler; OS `.ips` remains authoritative | main-thread signal capture (SIGUSR2 by default), 250 ms deadline |
| Windows | yes | unhandled exceptions and CRT fatal paths; minidump/text writer started at install, WER preserved | suspended context walked on a helper; watchdog resumes within its 250 ms budget |
| Web | no files (the browser has no user folder) | the page hooks: `JASPERENGINE CRASH:` in the console, `window.jasperengine.onCrash`, the overlay | the browser owns the main thread: no watchdog |

The engine owns installation on every platform. The editor compatibility class forwards to it;
normal editor startup installs it once through `EditorDiagnostics`. Thread creation through
`thread::spawnNamed` prepares an alternate signal stack (POSIX) or stack guarantee (Windows).

Hang capture uses `SIGRTMIN+5` on Linux/Android and `SIGUSR2` on Apple. Set
`JASPERENGINE_HANG_SIGNAL` to a free signal number when another library reserves the default;
existing custom handlers are never replaced. A capture timeout disables stack capture for that
session and leaves phase and occupancy reporting active. On Windows, the faulting thread waits
at most eight seconds for the prestarted writer; a blocked DbgHelp/loader lock cannot prevent
OS exception handling. A busy DbgHelp lock yields a text report with saved PC/SP and no dump.

`JASPERENGINE_CRASH_CORE=1` raises the POSIX core limit to its hard limit at installation.
Linux reports include `/proc/self/maps` and the core pattern (or a `coredumpctl` hint). Apple
reports record the main module UUID; use the matching dSYM and the authoritative OS `.ips`.

## The report folder

```
<root>/reports/<yyyymmddThhmmssZ>-<pid>/     one folder per session that had something to report
    crash.txt        "JasperEngine crash report v1", key: value lines, then the frames
    crash.dmp        Windows only
    log-tail.txt     the last log lines (the crash ring)
    hang-001.txt …   hang watchdog reports
    session.json     a session that ended without a report (its marker, kept)
    handled          the notice was dismissed, or a game's hook took the report
<root>/sessions/<pid>.json                    this session is running
```

| | Root |
|---|---|
| Editor | the folder of `settings.yaml` and `editor.log` (keeps the newest 20 folders, at most 2 GiB; an unhandled folder younger than 7 days is not removed by the size limit alone) |
| Desktop game | `<user data>/crash` (keeps 5 folders, at most 50 MiB) |
| Android | the app's internal files directory, `crash/` |
| iOS | `Library/Caches/crash` |
| Web | none |

`JASPERENGINE_CRASH_REPORT_DIR` overrides the root (tests, CI).

`crash.txt` keys: `product, version, build_id, platform, backend, reason, signal | exception_code,
fault_address, thread, pc, sp, time_since_start_ms, session`, then frames as
`#N <module>+0x<offset> [<symbol>]`, then the module list. Readers accept v1 and ignore keys they
do not know. A hang report has `stalled_ms, threshold_ms, phase, busy_scopes, frame,
last_frame_ms, play_state, scene, lua` (in a Lua phase), `stack`, and what each job thread runs.

**Sessions.** Each run writes `sessions/<pid>.json` and removes it on a clean exit. At the next
start, a marker whose process is gone means that session ended abnormally: with `crash.txt` it
crashed, with a hang report but no crash it was killed while not responding, with neither it
ended without a report (killed, out of memory, power loss); the last kind gets a folder with its
`session.json`, so it is listed too.

## The editor

- **The notice.** After an abnormal session the editor shows, under the menu bar (after any
  "Recover Unsaved Changes" question), `JasperEngine closed unexpectedly last time (SIGSEGV at
  14:32). A crash report was saved.`, or "…while it was not responding (hang report saved)", or
  "…no report was saved". **Open Report**, **Show in Folder**, **Copy Report Path**, **Dismiss**
  (writes `handled`). The Console gets the same line.
- **The toast.** After the editor recovers from a stall, the footer shows `The editor was not
  responding for 7.4 s (Importing model.glb). Details saved.` for 8 s, with **Open** (at most
  one per minute). The Console gets the line too.
- **Preferences › Diagnostics**: Hang Watchdog (on), Report After (s) (3, 1–60), Interrupt
  Runaway Lua In Play After (s) (10, 0 = never), Show Crash Notice On Startup (on), **Open Crash
  Reports Folder**. They apply at OK, like every preference, and are not undo steps.

## The hang watchdog

A thread named "Hang Watchdog" polls every 250 ms. The main loop beats once per iteration
(`diag::heartbeat()`, one relaxed increment); the watchdog arms on the first beat. When the
main thread has not beaten for longer than the threshold (editor 3 s, development games 5 s, the
shutdown window 15 s) it writes `hang-NNN.txt`, prints `JASPERENGINE HANG: …` to stderr, re-reports
every 5 s while the stall lasts, and prints one "recovered after X s" line when it ends.

- **Busy scopes** raise the threshold for known long work, never silence it:
  `diag::BusyScope busy("Importing model.glb", 120'000);` (budget 0 = no limit, 16 nest, the
  report lists them outermost first). The editor marks project loading, native file dialogs
  (no limit), scene saves, Play start and stop, synchronous shader compiles.
- **Phase scopes** only label: `diag::PhaseScope phase("lua");` (a string literal). Every Lua
  call is in the `lua` phase; a report then names the running function (`lua: player.lua:12
  update`).
- **Runaway Lua (editor Play only).** Past Interrupt Runaway Lua In Play After, the watchdog
  makes the function raise `script interrupted: player.lua:12 update ran for 10.0 s without
  returning (hang watchdog)`: a script crash in the Console, and Play goes on. Games never do this.
- Games run the watchdog in Development Builds only (Project Settings › Player › Crash Handling).
  Mobile backgrounding pauses it.
- Environment: `JASPERENGINE_HANG_WATCHDOG=0|1`, `JASPERENGINE_HANG_WATCHDOG_MS`,
  `JASPERENGINE_HANG_WATCHDOG_SHUTDOWN_MS`.

## Games

**Project Settings › Player › Crash Handling**: Enabled (on), Include Log Tail (on), Windows Dump
Type (Small | With Heap), Hang Watchdog In Development Builds (on) and Hang Threshold (s) (5),
Web › Show Crash Overlay (on). Old projects load with these defaults (`project.yaml`
`playerCrash`, written only when changed).

**The report hook.** At the next start a game can look at what earlier sessions left:

```lua
if Crash.lastSessionEndedAbnormally() then
    Log.warn("the last session did not end cleanly")
end
Crash.setTag("level", "forest-2")          -- a key: value line of this session's crash.txt
Crash.setReportHook(function(report)       -- once per pending report, now
    -- report = {id, kind = "crash"|"hang"|"abnormal_exit", reason, time, version, product, files}
    -- ask the player first; then upload report.files with your own code
    return true                            -- true: handled, not offered again
end)
```

C++: `diag::crash::setReportHook`, `pendingReports`, `markHandled`, `lastSessionEndedAbnormally`,
`setTag` (`engine/core/diag/CrashHandler.h`).

**Privacy.** Reports hold file paths, log lines and (Windows With Heap) memory of the game. Get
the player's consent before your hook sends one anywhere.

**Development builds** also take `JASPERENGINE_DEBUG_CRASH=<mode>[@<frame>]` (segv, abort,
terminate, stack_overflow, worker_segv, fpe, pure_call, hang; without a frame: 30 frames after
every scene loaded) and on the web `?jasperengine_debug_crash=<mode>[@<frame>]`, to check the
whole path once.

## The web

`engine/platform/emscripten/crash_pre.js` is linked with `--pre-js`, so custom page shells keep
it. The first fatal event (an abort, an uncaught error, an unhandled rejection) logs
`JASPERENGINE CRASH: <message>`, the stack and the last 200 console lines with `console.error`,
calls `window.jasperengine.onCrash(report)` when the page defines it (`report = {message, stack,
logTail, buildHash, product, userAgent, time}`), and shows "The game stopped working.
[Reload]" unless Show Crash Overlay is off.

## Symbolication

```sh
python3 -I tools/crash/symbolicate.py <report folder or crash.txt> --binary-dir <the build> [--symbols <symbols dir>]
```

POSIX fatal handlers keep raw module offsets and the saved fault PC. They skip the reporting
handler and signal trampoline by locating the interrupted PC in the captured unwind. If that
boundary cannot be found, only the saved PC is retained. Symbol names are resolved **after the
crash**, so the fatal path never calls `dladdr` or a symbol loader. The raw maps copy also places
frames from libraries loaded after installation.

Another faulting thread waits up to eight seconds for the first report to finish before chaining
to its previous handler. A recursive fault on the reporting thread (or Windows writer) terminates
immediately; a damaged reporter cannot wait for itself. Windows obtains the faulting thread's
name with dynamically resolved `GetThreadDescription`, or writes `unknown` when unavailable.

It names each frame with `llvm-symbolizer` (else `addr2line`, `atos` on macOS) against the
module (from `--binary-dir`, the S30 layout `symbols/<platform>/<build_id>/`, or the path the
report lists), maps web frames with the emscripten symbol map (`--symbol-map`), and for a Windows
report prints the steps: `cdb -z crash.dmp -y <symbols> -c "!analyze -v; .ecxr; k; q"`, or open the
`.dmp` in Visual Studio with the symbol path set. Keep the symbols of every build you ship
(`build_id` matches the binary: ELF build id, Mach-O UUID as `dwarfdump --uuid` prints it, the
PDB GUID and age).

## Agents (MCP)

The `diagnostics` tool group: `get_editor_health` (answered off the main thread, so it replies
while the editor is hung), `get_crash_reports`, `dismiss_crash_notice`, `set_hang_watchdog`,
`debug_crash` (refused unless the editor started with `--allow-debug-crash` or
`JASPERENGINE_ALLOW_DEBUG_CRASH=1`). `run_build` adds `crash_report` when the player printed a
`JASPERENGINE CRASH:` line. See `jasper://docs/crash-diagnostics`.

## Budgets

`je-bench-diag` (built with the core tests) measures what the diagnostics cost a running
program, gated by `tools/perf/check_bench.py` against `tools/perf/bench-thresholds.json`: the
heartbeat (≤ 5 ns), a 120-byte crash-ring write (≤ 150 ns), the install (≤ 3 ms) and the idle
watchdog thread (≤ 0.1 % of one core). Run it on a quiet machine:

```bash
tools/build.sh exec linux bash -c 'cmake -S engine -B build/linux/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON \
    && cmake --build build/linux/bench --target je-bench-diag \
    && build/linux/bench/tests/core/je-bench-diag --json build/linux/bench/diag.json \
    && python3 tools/perf/check_bench.py build/linux/bench/diag.json'
```
