# Crash and hang diagnostics (agents)

The `diagnostics` tool group (`tool_groups {enable: ["diagnostics"]}`; every tool is callable by
name either way). The full description is `docs/crash-diagnostics.md` in the repository.

## Hung or slow?

Every tool but one runs on the editor's main thread, so a hung editor answers nothing.
`get_editor_health` is answered on the transport's thread:

- `responsive: false` and a growing `stalled_ms`: the main thread is stuck. `phase` and
  `busy_scopes` say on what (`lua` = a script; `Loading project`, `Compiling shader` ...).
  Wait for it, or stop Play / the editor; do not pile up more calls.
- `responsive: true` while other calls are slow: it is busy, not hung (a long job, a build).
- `watchdog.enabled: false`: nothing is measured (Preferences > Diagnostics > Hang Watchdog,
  or `set_hang_watchdog {enabled: true}`).

A busy scope raises the threshold for known long work (loading: 120 s) but never hides a hang:
past its budget the stall is reported.

## Reports

`get_crash_reports {limit, include_text}`: the editor's report folders, newest first, each
`{id, kind: crash|hang|abnormal_exit, time, reason, files, handled, this_session}`, and
`notice_open` (the startup notice "JasperEngine closed unexpectedly last time"). `include_text`
adds crash.txt or the newest hang report. A hang of this session is listed as soon as it is
written. `dismiss_crash_notice {id?}` is the notice's Dismiss (marks the report handled).

## Runaway Lua in Play

A Lua function that does not return in Play stalls the editor. After
`break_lua_after_s` (default 10, `set_hang_watchdog {break_lua_after_s}`), it raises
`script interrupted: <file>:<line> <function> ran for N s without returning (hang watchdog)`:
a "Script crash" error in the Console (read_console), and Play goes on. Fix the loop.

## A callback that fails

An event callback, a timer or a coroutine that fails, by a crash (a fault), a Lua error or a C++
exception, is one case:

| Where | Lua error | C++ exception | Fault |
|---|---|---|---|
| event callback, timer, coroutine, editor Play | removed; Console "Script crash in scene ..."; Play pauses, then stops | the same, `Error: C++ exception <type>: <what>` | the same, `Error: SIGSEGV (...)` |
| the same, exported game | logged, kept | logged (once a second per callback), kept | the game's crash report |
| `init`, `start`, `onEnable`... and physics messages | logged, the script goes on | logged | as above |
| Web | as above | the game stops (no exception catching) | the game stops |

Read the Console after Play (read_console): the `Location` line names the callback (its tag).

## Testing the diagnostics

`debug_crash {mode: segv|abort|terminate|stack_overflow|worker_segv|hang, hang_ms}` is refused
unless the editor was started with `--allow-debug-crash` (or JASPERENGINE_ALLOW_DEBUG_CRASH=1).
It replies first, then acts. `hang` stalls the main thread for hang_ms: `get_editor_health` then
shows `responsive: false`, and `get_crash_reports` lists the hang afterwards.

## Players

`run_build` reports `crashed` and, when the player printed `JASPERENGINE CRASH: ... report=<folder>`,
`crash_report` (the folder). A game's own reports stay on the player's machine; Lua
`Crash.setReportHook` gets them at the game's next start (jasper://docs/lua-api, Crash).
