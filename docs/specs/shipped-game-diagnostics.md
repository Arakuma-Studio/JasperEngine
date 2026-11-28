# Shipped Game Diagnostics: a gameplay journal in every report, a player's "Report a Problem", and crash grouping

Builds on S28 (crash and hang reports, landed) and S30 (publish and symbols, landed). Three work
packages: P1 the Gameplay Journal, P2 user reports ("Report a Problem"), P3 crash grouping (the
script, `get_crash_reports group`, the Crash Reports window). P1 lands first; P2 and P3 can then run
in parallel. Paths are relative to the repository root.

## 1. Goal

A crash report from a player's device says where the code died, but not what the player was doing:
today it carries 16 tags and the last log lines. A player who meets a bug that does not crash has no
way to send anything. A developer with two hundred reports has to symbolicate them one at a time
and cannot tell which crash is new in 1.4 and which one hits only Android. This spec adds a small,
fixed-size **Gameplay Journal** of structured game events that lands in every crash, hang and user
report; a **user report** a game starts from its own "Report a Problem" form, with a screenshot of
the frame the player saw, the journal, the log tail and the device; and **grouping** of report
folders by their resolved top frames with counts, versions, platforms and first and last seen.
The engine still never sends anything: the game asks the player and uploads with its own `Http`.

## 2. Scope

**In scope**
- P1: `diag::journal` (C++) and Lua `Journal`: a byte ring of pre-formatted one-line entries,
  allocated once at startup, written without heap allocation; `journal.txt` in crash folders
  (async-signal-safe on POSIX, the prestarted writer on Windows), beside hang reports, in user
  reports, and in the web page's crash report; a fixed set of engine events; Project Settings rows;
  the editor Console's `journal` category; `read_console` reads it unchanged.
- P2: `Crash.beginReport`, `Crash.createReport`, `Crash.cancelReport`, `Crash.readReportFile`;
  report folders of kind `user` (note, fields, tags, journal, log tail, device JSON, optional
  JPEG screenshot under a size cap encoded off the main thread); the S28 report hook and
  `get_crash_reports` see them; the web keeps them in memory for the session.
- P3: `tools/crash/symbolicate.py --group`; symbol lookup by build id under a symbols tree in
  either layout (S28's `<platform>/<build_id>/` or S30's release `symbols/<target>/`);
  `get_crash_reports` parameters `folder`, `kinds`, `group`, `files`; **Window > Analysis > Crash
  Reports**.

**Out of scope (decided)**
- Any network transport, account, consent dialog or upload queue in the engine: the game owns them
  (S28's rule, `engine/core/diag/CrashHandler.h:21-22`).
- A report form UI: the game builds its own (HTML UI or components); the engine gives capture,
  folder and files. Text entry on gamepads and phones is text-entry-gamepad-cursor-back-button.md.
- Attaching saves or input recordings to a user report (savegame.md, input-recorder-and-input-scripts.md):
  a game can add them through `fields` and its own upload.
- Server-side aggregation, dashboards, alerts. Grouping runs on folders a developer already has.
- Persisting the journal across sessions on disk (Craftsman's block log writes day files from a
  writer thread; a crash journal only needs the last minutes, kept in memory and dumped at death).
- Tags and the log tail in hang reports (S28 owns the hang format); P1 only adds `journal.txt`
  beside a hang report.
- Changing `crash.txt` v1: new evidence goes in new files.

## 3. Current state

All read from the code (not run) unless marked:

- **Breadcrumbs.** The crash ring is a byte ring of log lines, `ringBytes` 64 KiB in games and
  256 KiB in the editor (`CrashHandler.h:51`, `CrashHandler.cpp:596`), allocated at install
  (`CrashHandler.cpp:665-669`), written lock-free by one `fetch_add` and `memcpy`
  (`ringWriteParts`, `CrashHandler.cpp:328-358`; "a torn line at a wrap is acceptable",
  `CrashHandler.h:101`). Its only non-log writer is the guarded-callback recovery line
  `CrashGuard: recovered a guarded callback crash` (`engine/core/util/CrashGuard.h:229`). There is
  no structured game-event record: a game's only context is `Crash.setTag` (16 slots, key 32 bytes,
  value 128 bytes, a seqlock per slot: `detail/CrashState.h:23-31`, `CrashHandler.cpp:737-777`).
- **The fatal path.** POSIX writes `crash.txt` (tags at `CrashHandlerPosix.cpp:300-307`), then
  `log-tail.txt` from the ring with `open`/`write` only (`:193-201`, `:342-344`); Windows writes
  from a thread started at install (`CrashHandlerWin.cpp:73-80` for the ring). The POSIX object is
  checked for allocation, stdio and mutex imports (`tools/crash/check_posix_symbols.py`,
  docs/testing.md "Native crash diagnostics").
- **Hang reports** are built as a `std::string` on the watchdog thread and written through
  `writeSessionFile` (`HangWatchdog.cpp:258-301`, `CrashHandler.cpp:990-997`): no tags, no log tail.
- **Kinds.** `classify` knows `crash` (a `crash.txt`), `hang` (a `hang-NNN.txt`) and otherwise
  `abnormal_exit` (`CrashHandler.cpp:417-425`); `readReport` derives reason, product, version
  and time per kind (`:796-842`); `pendingReports` drops only the folder whose id equals the
  current session (`:863-870`); report ids parse as `<yyyymmddThhmmssZ>` plus anything
  (`sessionTime`, `:157-174`), so a suffixed id still sorts and dates.
- **No player-started report.** `Crash` in Lua has `setReportHook`, `getPendingReports`,
  `markHandled`, `lastSessionEndedAbnormally`, `setTag` (`CrashClassesLua.cpp:100-104`,
  stub `engine/core/script/binding/stubs/Crash.lua:5-41`); `CrashReport.kind` is
  `"crash"|"hang"|"abnormal_exit"`.
- **Screenshots in a game** exist only for `--screenshot` runs: `RunControl` reads the composite
  framebuffer and writes a PNG with `stbi_write_png_to_func` (`engine/core/player/RunControl.cpp:114`,
  `:246-258`). Backends draw straight to the swapchain; a capture frame must draw offscreen, and a
  run that screenshots draws offscreen from its first frame "so the capture frame needs no pipeline
  the frames before did not (formats of the window and the composite can differ)"
  (`engine/core/Engine.cpp:1597-1612`). `FrameReadback` is synchronous, main thread only
  (`engine/core/render/FrameReadback.h:6-11`, `:31-41`). `stb_image_write.h` (with its JPEG
  writer) is vendored in `engine/libs/stb/`.
- **Upload path for a game.** `Http` options take a binary-safe string `body`
  (`stubs/Http.lua:10`); there is no multipart helper, so a game posts each file's bytes.
- **The web** has no report files: `crash_pre.js` builds `{message, stack, logTail, buildHash,
  product, userAgent, time}` from the last 200 console lines (`engine/platform/emscripten/crash_pre.js:8`,
  `:38`), configured by `CrashHandlerWeb.cpp:17-25`.
- **Settings.** `PlayerCrashSettings` (`editor/Project.h:861-877`) is saved as `playerCrash` only
  when it differs from the defaults (`editor/Stream.cpp:2786-2796`), exposed as `player.crash.*`
  (`editor/settings/SettingsRows.cpp:2768-2796`), and reaches compiled games as CMake defines
  (`editor/Exporter.cpp:4663-4669`) and prebuilt players through `game.json`'s `crash` object
  (`engine/core/player/PlayerConfig.cpp:146-147`, `editor/ExporterPrebuilt.cpp:682-683`).
- **The Console** has a **Category** filter menu over every category it has seen
  (`editor/window/ConsoleWindow.cpp:621-645`); `read_console` filters by `categories`.
- **Agents.** `get_crash_reports` lists only the editor's own report root (`listReports`) with
  `include_text` for the primary file (`editor/mcp/tools/DiagnosticsTools.cpp:67-102`, `:195-206`).
  It cannot read a game's folder pulled from a device, filter kinds, or group.
- **Symbolication** resolves one report (`tools/crash/symbolicate.py:1-18`, `:166-172`); its symbol
  lookup is `--binary-dir` then `<symbols>/<platform>/<build_id>/` (`:100-115`). `publish prepare`
  moves symbols to `<release>/symbols/<target>/` (`game/`, `player/`) (`editor/publish/Publishing.cpp:453`,
  `docs/shipping.md:379-385`) and records no build id in `release.json` (no `build_id` in
  `Publishing.cpp`): the two layouts do not meet, so a release's symbols are not found by build id.
- **Budgets.** `je-bench-diag` gates the heartbeat (5 ns), a 120-byte ring write (150 ns), install
  (3 ms) and the idle watchdog (`tools/perf/bench-thresholds.json:12-17`).

## 4. Reference

Ideas only. The Craftsman tree (`/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor`) is a
port of a retail Minecraft PE binary: **no code, data, assets or Minecraft-specific mechanics may
be copied**; nothing here is block- or world-specific.

| Source | Pattern studied | What we take / do differently |
|---|---|---|
| `include/mcpe015/plugin_blocklog.h`, `src/blocklog_store.h` | An append-only event log of fixed 32-byte records, a writer thread every 2 s, day files, memory and age caps | Fixed-size records and hard caps. We keep only an in-memory text ring dumped at death (no files while running, no writer thread, no per-domain record layout) |
| `include/mcpe015/discord2.h` | "Report a problem": the screenshot is the last frame *before* the form opens; a worker scales it to <= 1280 px and JPEG quality 80, lowering quality to fit 600 KB; device fields filled automatically; the draft text survives a failed send | The capture-then-form order (D6), the size cap and quality ladder (R16), automatic device fields (R18), "kept until sent" as a pending report re-offered at the next start (R20). Their transport, categories and account linking stay theirs |
| `tools/symbolize_crashes.py` | Group native stacks by the first three own frames' short names (else PCs), per version; archive unstripped libraries by build id and refuse a library whose build id differs | The signature rule (D9) and build-id-checked lookup (R27) |
| Unity User Reporting (docs) | `TakeScreenshot`, `CreateUserReport`, `SendUserReport`, device metadata | Names "user report" and "Report a Problem"; we never send |
| Unity Cloud Diagnostics (docs) | Crash and exception reports grouped as "problems" with counts and first/last seen | The group fields (R29) |
| Unity `CrashReport` (docs) | `CrashReport.reports`, `lastReport` | Already mirrored by S28's hook |

## 5. Requirements

### Functional: the Gameplay Journal (P1)

- **R1 Ring.** `diag::journal` owns one byte ring of `capacity` bytes, allocated once by
  `journal::configure(bytes)` before the crash handler installs and never resized. `bytes == 0`
  allocates nothing and every write returns after one relaxed load. Capacity: games
  `playerCrash.journalKiB` (default 32, range 0..256), editor 64 KiB fixed.
- **R2 Entry format.** One line per entry: `+<seconds since start, 3 decimals> f<frame> <kind>[ <key>=<value>]...\n`,
  for example `+412.031 f24718 level.start name=forest-2 seed=42 hard=true`. `<frame>` is
  `FrameTimes::currentFrame()`. Keys are sorted bytewise (deterministic text whatever the Lua
  table order). Values: integers as integers, other numbers `%.9g`, booleans `true`/`false`,
  strings bare when they match `[A-Za-z0-9_.:/+-]+`, else double-quoted with `\"`, `\\`, `\n`
  escapes; other Lua types are written as `<table>`, `<function>`, ... An entry is at most 256
  bytes including the prefix; longer ones end in `~`. At most 12 fields.
- **R3 Kind.** `[A-Za-z0-9_.:-]{1,48}`. Another kind is written as `invalid` with field
  `kind=<first 48 bytes, escaped>` and one warning per process (`Journal.log: kind "..." is not
  [A-Za-z0-9_.:-]{1,48}`): a shipped game never raises from a diagnostics call.
- **R4 Writers.** Any thread; one `fetch_add` reserve and `memcpy` as the crash ring; a torn
  entry at a wrap is skipped by readers (a line not starting with `+` is dropped).
  `journal::dropped()` counts entries overwritten since start.
- **R5 Readback.** `journal::text()` returns `journal.txt`'s content (R6) now; Lua
  `Journal.getText()` and `Journal.getEntries(n)` (the newest `n`, default 50, as
  `{time, frame, kind, text}` tables, oldest first) serve a game's own bug form. `Journal.clear()`
  empties the ring (a new run of a level, a privacy reset).
- **R6 `journal.txt`.** `JasperEngine journal v1`, then `product:`, `version:`, `session:`,
  `started:` (UTC ISO 8601 of second 0), `capacity:`, `dropped:` lines, `---`, then the entries
  oldest first. Readers ignore keys they do not know.
- **R7 Crash dump.** A crash folder gets `journal.txt` whenever the ring holds an entry: POSIX
  from the signal handler with `open`/`write` only, after `log-tail.txt`; Windows from the
  prestarted writer. The header lines are pre-formatted at install into a fixed buffer
  (`CrashState`), `dropped:` printed with `appendDecimal`. `check_posix_symbols.py` stays green.
- **R8 Hang and user reports.** A hang report writes `journal.txt` beside `hang-NNN.txt` (a
  snapshot through `writeSessionFile`, overwritten by later hangs of the session). Every user
  report (R14) contains it.
- **R9 Engine events.** With `playerCrash.journalEngineEvents` (default on) the engine writes:
  `engine.start` (`version`, `backend`, `gpu`, `tier`), `engine.scene_loaded` (`name`, `ms`),
  `engine.scene_unloaded` (`name`), `engine.pause`, `engine.resume` (`after_s`),
  `engine.focus` (`focused`), `engine.low_memory`, `engine.thermal` (`state`), `engine.tier`
  (`tier`, `source=set|reset`), `engine.guard_recovered` (`signal`), `engine.user_report`
  (`id`); in the editor also `engine.play` (`state=start|stop|pause`). Each is a rare event (at
  most a few per minute); none fires per frame.
- **R10 Web.** The web build passes the ring's address, capacity and cursor address to
  `crash_pre.js` at install (`hooks.configure`), which reads the ring from `HEAPU8` at the first
  fatal event and adds `journal` (the `journal.txt` text) to `window.jasperengine.onCrash`'s report
  and the console block after the log tail.

### Functional: user reports, "Report a Problem" (P2)

- **R11 Capture first.** `Crash.beginReport(fn)` requests a capture of the frame being drawn; `fn`
  (optional) runs on the main thread on the first frame after the capture finished or failed, so a
  game opens its form there and the form is never in the image. While a capture is pending,
  another `beginReport` replaces `fn` and keeps the capture.
- **R12 Offscreen capture frame.** The capture draws that frame through the composite
  framebuffer and reads it after commit (`Engine::readCompositeFrame`). When the frame drew a
  stand-in or skipped a draw because a shader or pipeline for the composite's format was still
  being created (S32's creation stats for the frame), the next frame is drawn offscreen and read
  instead, up to 30 frames; then the capture fails with `screenshot_unavailable: shaders still
  building`. The player sees every frame normally (the composite is presented).
- **R13 Draft.** The read RGBA is handed to a job that box-downscales to at most `maxWidth`
  (default 1280; height by aspect) and encodes JPEG with `stbi_write_jpg_to_func` at quality 80,
  then 70, 60, 50, 40 until the bytes are <= `maxBytes` (default 600 KiB); at 40 still over:
  halve the width and retry once, else no screenshot (`screenshot_too_large`). The full-size RGBA
  is freed when the job starts. The draft (JPEG bytes and its time) lives in memory until
  `createReport`, `cancelReport` or the next `beginReport`. Web (single-threaded build): the encode
  runs on the main thread in the frame after the read.
- **R14 Create.** `Crash.createReport(options)` writes a folder `reports/<session>-user<N>/`
  (`N` from 1 per session) through `io::writeFileAtomic` and returns the report table (S28's
  `CrashReport` shape, `kind = "user"`): synchronously on the main thread (<= 10 ms of file writes,
  AC14), or, when `options.done` is given, on an S06 I/O job with `done(report, err)` called on the
  main thread afterwards. Options: `note` (string, <= 4000 bytes, required non-empty),
  `category` (`[a-z0-9_]{1,32}`, default `"other"`), `fields` (<= 16 string/number/boolean pairs),
  `screenshot` (default true: the draft's JPEG when one exists), `logTail` (default the project's
  Include Log Tail), `maxWidth`, `maxBytes` (applied by `beginReport`; ignored here when the draft
  exists). Files:
  - `user.txt`: `JasperEngine user report v1`, `product`, `version`, `build_id`, `platform`,
    `backend`, `session`, `time`, `category`, the tags (`Crash.setTag`), `field.<key>: <value>`
    lines, then `note:` and the note verbatim;
  - `device.json`: R18; `journal.txt` (R6); `log-tail.txt` (the crash ring's contents) unless
    `logTail = false`; `screenshot.jpg` when present; `perf.json` when the Performance sampler of
    release-performance-and-gpu-quirks.md has a summary.
- **R15 Kind `user`.** `classify` returns `user` for a folder holding `user.txt`; `readReport`
  takes `time` and `product`/`version` from it and `reason` = `"user report (<category>): <first
  line of the note, at most 80 bytes>"`. A user folder never raises the editor's startup notice
  and never changes `lastSessionEndedAbnormally`.
- **R16 Limits.** At most 3 unhandled user folders: a 4th `createReport` returns `nil,
  "too_many_pending: upload a report or Crash.markHandled(id) first"` and writes nothing. User
  folders count in the root's retention (5 folders / 50 MiB in games).
- **R17 Pending and the hook.** `pendingReports` excludes the current session's folder **and**
  folders whose id starts with `<current session>-`; at the next start an unhandled user report is
  pending and offered to `Crash.setReportHook` with `kind = "user"`. This is the "draft kept until
  sent" rule: a game marks it handled only after its upload succeeded.
- **R18 Device JSON.** `{product, version, build_id, platform, backend, os_version, android_api
  (Android), gpu, gpu_driver, device_tier, tier_reason, gpu_quirks, memory_mb, cores, screen:
  {width, height, dpi_scale}, thermal_state, low_memory_warnings, uptime_s, session}`, from
  `Device`, `System` (`getOperatingSystemVersion`, `getAndroidApiLevel`, `System.h:321-331`) and the
  engine. `gpu_driver`/`gpu_quirks` come from release-performance-and-gpu-quirks.md (empty until it
  lands).
- **R19 Reading files.** `Crash.readReportFile(id, name)` returns a report file's bytes (at most
  8 MiB; `nil, "not_found"` otherwise) for the game's `Http` body, on every platform.
- **R20 Web.** No folders: user reports live in memory for the session (same 3-report limit),
  `files` lists their names, `readReportFile` serves them, `markHandled` frees one. They do not
  survive a reload (documented); a web game uploads at once.

### Functional: grouping (P3)

- **R21 Signature.** For a crash: `crash` + the signal or exception name (`SIGSEGV`,
  `EXCEPTION_ACCESS_VIOLATION`, `abort`, `terminate`) + the first 3 *own* frames. Own frames are
  frames whose module is the game's executable or the engine library (the report's main module and
  `libjasperengine*`), after dropping frames resolved to the skip list (`abort`, `raise`,
  `__pthread_kill*`, `std::terminate`, `__cxa_*`, `jasperengine::diag::*`,
  `jasperengine::CrashGuard*`). A frame is its short function name when resolved (namespaces kept,
  arguments and template arguments dropped, `(anonymous namespace)` kept), else
  `<module>+0x<offset>`. With no own frame, the first 3 frames of any module. For a hang: `hang` +
  phase + the innermost busy scope, and the stack's first 3 own frames when present. A user report:
  `user` + category. An abnormal exit: `abnormal_exit`.
- **R22 Unresolved frames** make a signature that is only stable within one build: such groups also
  key on `build_id`, and the output says `unresolved` so the reader adds symbols.
- **R23 Script.** `python3 -I tools/crash/symbolicate.py --group <dir> [--symbols <dir>]...
  [--binary-dir <dir>]... [--symbol-map <file>] [--by version|platform] [--kinds k,...] [--since
  <ISO date>] [--json] [--top N]`. `<dir>` is searched to depth 3 for report folders (a folder
  holding `crash.txt`, `hang-*.txt` or `user.txt`). Output per group, most reports first: count,
  signature, kind, `versions {v: n}`, `platforms {p: n}`, `first_seen`, `last_seen`, up to 3 example
  folder paths, and the resolved top frames of the newest report. `--by` splits groups by that
  field. Exit 0 when at least one report was read, 1 when none, 2 on a bad argument. The
  single-report form is unchanged.
- **R24 Symbols by build id.** `--symbols` accepts S28's layout and S30's release layout (and any
  tree): the script indexes candidate files (`.debug`, executables, `.so`, `.dSYM` DWARF) by build
  id (ELF note read by `readelf -n` or a pure-Python ELF note parser; Mach-O UUID by `dwarfdump
  --uuid`), and a module is resolved only by a file whose build id equals the report's `build_id`
  (for the main module) or the module list's id where present. A mismatched file is never used.
- **R25 Shared rules.** The signature rules live in one table of test cases,
  `tools/crash/fixtures/group/` (report folders plus `expected.json`), which both the Python tests
  and the C++ grouping (R31) must pass.

### Platform & backends

- **R26** The journal, user reports and grouping work on Windows, Linux, macOS, Android, iOS and
  HTML5, on all five backends: the capture uses `FrameReadback` (glcore, GLES3/WebGL2, Vulkan,
  D3D11, Metal). User reports need no compute, SSBO or any banned feature.
- **R27** Android and iOS write user folders under S28's roots (`crash/` in the app's files
  directory, `Library/Caches/crash`); desktop under `<user data>/crash`. `JASPERENGINE_CRASH_REPORT_DIR`
  still overrides.

### Performance (Release; mobile and web first)

- **R28 Journal off.** `journalKiB = 0`: 0 bytes allocated; `diag::journal::log` <= 5 ns (one
  relaxed load and a branch); `Journal.log` costs only the Lua call.
- **R29 Journal on.** `diag::journal::log` with a kind and 4 fields (two strings, two numbers),
  <= 250 ns on the benchmark server (`je-bench-diag`, new `journal_log_4f_ns`); Lua
  `Journal.log(kind, {4 fields})` <= 1.5 us there; **zero heap allocations** in both
  (`AllocationCountScope`). Memory = capacity + 1 KiB of header buffer.
- **R30 Crash path.** Writing `journal.txt` adds no allocation, lock or stdio call to the POSIX
  handler and at most 2 `write` calls plus the ring's bytes.
- **R31 Capture.** Main-thread cost of a capture beyond the GPU wait of the synchronous readback:
  <= 2 ms at 1920x1080 on the benchmark server; the readback's own wait is reported in the log line
  `User report: captured 1920x1080 in N ms`. The encode job at 1920x1080 -> 1280x720: <= 60 ms on
  the benchmark server; web main thread <= 80 ms (one hitch, user-initiated). Nothing is drawn
  offscreen unless a capture is pending (a disabled feature costs nothing).
- **R32 Grouping.** The script groups 1000 unsymbolicated report folders in <= 10 s on the
  benchmark server; with symbols it runs one symbolizer process per (module, build id).
  `get_crash_reports group: true` over 100 folders answers in <= 200 ms.

### Editor UX (human)

- **R33 Project Settings > Player > Crash Handling** gets **Gameplay Journal (KiB)** (0..256, 32;
  tooltip "Recent game events (Journal.log) kept in memory and saved with crash, hang and user
  reports. 0 turns the journal off.") and **Journal Engine Events** (on). They follow the other rows:
  revert-to-default marker, applied with the window's OK, one undo step in scope `project` as every
  Project Settings edit (inferred from the window's pending-copy pattern,
  `ProjectSettingsWindow.cpp`).
- **R34 Console.** In the editor every journal entry is also logged at Info in a new log category
  `journal` (message = the entry without its `+time`, which the Console shows itself), so the
  existing **Category** menu shows only the journal with one click; no new tab.
- **R35 Window > Analysis > Crash Reports** (catalog id `window.crashReports`, after Build Report):
  a toolbar with **Folder** (the editor's reports, or **Open Folder...** for a game's reports root
  pulled from a device, remembered per project in the editor settings), **Group Similar** (toggle),
  **Kinds** (Crash, Hang, Abnormal Exit, User), a search field and **Refresh**; a table (Kind,
  Reason or Signature, Version, Platform, Count, First Seen, Last Seen, Handled); a detail pane
  with tabs **Report** (crash.txt, the hang report or user.txt), **Journal**, **Log Tail**,
  **Device** and **Screenshot** (fit, click opens the file); buttons **Show in Folder**, **Copy
  Path**, **Mark Handled**, **Copy Symbolicate Command** (the R23 command for the selection, with
  the project's last `--symbols` folder). The window changes no project data (no undo); Mark
  Handled writes `handled` as the notice's Dismiss does.
- **R36 Docs.** `docs/crash-diagnostics.md` gains "The Gameplay Journal", "Report a Problem" and
  "Grouping reports" sections, the new files in the folder diagram, and the kind `user`.

### Agent UX (MCP, Lua, CLI)

- **R37 `get_crash_reports`** (group `diagnostics`) gains: `folder` (absolute path of a reports
  root or a folder of report folders; default the editor's), `kinds` (array of `crash`, `hang`,
  `abnormal_exit`, `user`), `group` (boolean: R21 groups instead of reports, fields `signature,
  kind, count, versions, platforms, first_seen, last_seen, report_ids, unresolved`), and `files`
  (array of file names to inline as text, e.g. `["journal.txt", "user.txt"]`, each at most 64 KiB;
  `include_text` keeps its meaning). An unknown folder fails with `No report folder at <path>: give
  a reports root (it holds <yyyymmddThhmmssZ>-<pid> folders).` When `group` finds unresolved
  groups, `next` says `Names need symbols: python3 -I tools/crash/symbolicate.py --group <folder>
  --symbols <dir>`. Description: one sentence added; the group stays within its 15,000 characters.
- **R38** No new MCP tool. Agents drive the Lua API through `eval_lua` in Play (`Journal.log`,
  `Crash.beginReport`, `Crash.createReport`) and read results with `read_console categories:
  ["journal"]` and `get_crash_reports kinds: ["user"] files: [...]`. `focus_window`'s `window`
  enum gains `crash_reports`.
- **R39 Lua stubs.** `engine/core/script/binding/stubs/Journal.lua` (new, hand-written: Lua C API)
  and `Crash.lua` (new functions, `kind` adds `"user"`, `CrashReport.category`). `docs/mcp/guide/lua-api.md`
  gets the `Journal` table and the new `Crash` functions; `docs/mcp/guide/crash-diagnostics.md`
  gets one line per trigger: "a game's player hits a bug -> Crash.beginReport then createReport",
  "many reports -> get_crash_reports group: true".
- **R40 Eval.** `tools/mcp/eval/cases.py` gets a case where the agent must find which crash is new
  in a version from a folder of reports (needs `group` and `versions`), failing without R37's text.

### Data & compatibility

- **R41 Settings.** `PlayerCrashSettings` adds `int journalKiB = 32` and `bool journalEngineEvents =
  true`, saved as `playerCrash.journalKiB` / `journalEngineEvents` only when the block differs from
  the defaults (existing rule); keys `player.crash.journal_kib`, `player.crash.journal_engine_events`;
  CMake `JASPERENGINE_CRASH_JOURNAL_BYTES`, `JASPERENGINE_CRASH_JOURNAL_ENGINE_EVENTS`; `game.json`
  `crash.journalBytes`, `crash.journalEngineEvents`. Absent keys take the defaults: old projects
  load with a 32 KiB journal; no version bump, no migration (new optional fields).
- **R42 Reports.** `crash.txt` v1 and the hang report are unchanged; new files are additive. An older
  engine reading a folder holding only `user.txt` classifies it `abnormal_exit` (documented). The
  `.jetrace`, `project.yaml` and scene formats are untouched.
- **R43 Writes.** Every user-report file goes through `io::writeFileAtomic` (`Durability::Cache`, as
  S28's markers). The fatal-path `journal.txt` is written exactly as `log-tail.txt` is (raw
  `open`/`write` in a dying process, where an atomic rename buys nothing). Run
  `python3 tools/check_raw_writes.py`; if it flags the new write, add an allowlist entry with that
  reason.

## 6. Design constraints & decisions

- **D1 A text ring, not records.** Entries are formatted once at write time into the ring, so the
  signal handler copies bytes and never formats. Fixed binary records (Craftsman's 32 bytes) would
  need a formatter in the handler or a post-mortem decoder; text keeps the handler trivial and the
  file readable. The cost is paid by the writer (<= 250 ns, R29).
- **D2 Separate from the crash ring.** The journal does not share the log ring: a log storm (a
  script error every frame) would push game events out of a shared ring in seconds.
- **D3 Configured before install, not lazy.** `Engine` calls `journal::configure` from the player
  config before `diag::crash::install`, which copies the ring pointer and capacity into `CrashState`
  (constant afterwards, so the handler needs no atomic publication). With crash handling disabled
  the journal still works for user reports and `Journal.getText`.
- **D4 Formatting without allocation.** `journal::log` formats into a 256-byte stack buffer: a
  sorted array of at most 12 `(key, value)` views on the stack (insertion sort), numbers through
  `std::to_chars`. The Lua binding iterates the table with `lua_next`, takes `lua_tolstring` views
  of string keys and values (no copies), and rejects non-string keys silently.
- **D5 API sketch (C++).**
  ```cpp
  namespace jasperengine::diag::journal {
      struct Field { std::string_view key; std::variant<std::string_view, int64_t, double, bool> value; };
      JASPERENGINE_API void configure(size_t bytes);           // once, before crash::install
      JASPERENGINE_API bool enabled();
      JASPERENGINE_API void log(std::string_view kind, std::span<const Field> fields = {});
      JASPERENGINE_API std::string text();                     // journal.txt now
      JASPERENGINE_API std::vector<std::string> entries(size_t newest);
      JASPERENGINE_API void clear();
      JASPERENGINE_API uint64_t dropped();
  }
  ```
  File `engine/core/diag/Journal.{h,cpp}`; the crash backends read it through `detail/CrashState.h`
  (`journal`, `journalCapacity`, `journalCursor` pointers, `journalHeader[512]`).
- **D6 Capture order.** `beginReport` does not open anything: the game opens its form in `fn`, one
  or more frames after the shot. Taking the frame *before* the button press is impossible without
  keeping every frame (memory and a copy per frame on mobile); the frame of the press is what the
  player saw when they decided to report, so it is the right image.
- **D7 Offscreen only while capturing.** No standing composite copy per frame (a full-screen blit
  per frame on mobile is exactly the cost the invariants forbid). R12's retry handles the pipeline
  formats issue that `Engine.cpp:1603-1606` describes.
- **D8 Lua API sketch.**
  ```lua
  Journal.log("level.start", {name = "forest-2", seed = 42})   -- fields optional
  Journal.enabled             -- read-only: false when Gameplay Journal (KiB) is 0
  Journal.getEntries(20)      -- {{time = 412.031, frame = 24718, kind = "level.start", text = "..."}, ...}
  Journal.getText()           -- journal.txt
  Journal.clear()

  -- the pause menu's "Report a Problem" button
  Crash.beginReport(function(ok, err)        -- ok: a screenshot is in the draft
      openReportForm()                        -- the game's own form
  end)
  -- the form's Send
  local report, err = Crash.createReport{note = text, category = "gameplay", fields = {level = "forest-2"}}
  if report then
      Http.post(url, {body = Crash.readReportFile(report.id, "user.txt"), callback = function(response)
          if response.ok then Crash.markHandled(report.id) end   -- else it is offered again next start
      end})
  end
  Crash.cancelReport()        -- the form's Cancel: drops the draft screenshot
  ```
- **D9 Signature details.** Short names drop return types, argument lists and template arguments
  (`std::vector<int>::push_back(int const&)` -> `std::vector::push_back`), keep namespaces and
  `operator` names; lambdas become `<lambda>` with their enclosing function. Three frames, not
  one: one frame (`memcpy`) merges unrelated crashes; five split one bug across call sites.
- **D10 C++ grouping for MCP and the window.** `editor/util/CrashGroups.{h,cpp}` implements R21 for
  frames as written in the report (names where the report has them, else module+offset) without
  running a symbolizer: the editor must not depend on Python or LLVM tools being installed. The
  script adds symbolization. R25's fixtures keep the two in step.
- **D11 Ids.** `<session>-user<N>` keeps S28's id parsing (`sessionTime` reads the prefix) and sorts
  user reports beside their session.
- **D12 No editor test hook.** No `debug_crash` mode is added for user reports: `eval_lua` reaches
  the real API, so tests exercise what games call.

## 7. Acceptance criteria

Unit tests: `engine/tests/core/diag/JournalTest.cpp` (new file, suite `Journal`, `JournalBench`),
`engine/tests/core/diag/CrashReportsTest.cpp` (suites `CrashReports`, `CrashHang`, `CrashUser`,
`CrashUserBench`), `engine/tests/core/diag/CrashUserGpuTest.cpp` (new, suite `CrashUserGpu`),
`engine/tests/crash/CrashTestMain.cpp` (new `--journal` mode) with CTest; editor tests in
`editor/tests/util/CrashGroupsTest.cpp`; Python `tools/crash/test_symbolicate.py`; MCP tests extend
`tools/mcp/tests/test_crash.py` (`CI = True`); the web gate extends
`tools/web/gates/crash-diagnostics.json`. "Red" is the run on the tree without the change.

| AC | Verifies | Check |
|---|---|---|
| AC1 | R2, R3 | `Journal.FormatsKindAndSortedFields`: fields given in reverse order come out sorted; ints, `%.9g` floats, booleans, bare and quoted strings with escapes match a literal; a 300-byte entry ends in `~` at 256 bytes; kind `bad kind!` becomes `invalid kind="bad kind!"` with one warning (`LogCapture`). Red: no `diag::journal` |
| AC2 | R1, R4, R5, R6 | `Journal.RingKeepsTheNewestWholeEntries`: 4 KiB ring, 1000 entries: `text()` starts with the header, holds only whole entries, the last is entry 999, `dropped()` equals the overwritten count; `entries(5)` returns the 5 newest oldest first; `clear()` empties |
| AC3 | R1, R28 | `Journal.ZeroSizeAllocatesNothing`: `configure(0)`, 1000 `log` calls: `AllocationCountScope` counts 0, `text()` is empty, `enabled()` false |
| AC4 | R29 | `Journal.LogDoesNotAllocate` (`ThreadAllocationCount` 0 over 10,000 4-field logs) and `je-bench-diag` `journal_log_4f_ns` <= 250, gated by `tools/perf/check_bench.py` with a new row in `bench-thresholds.json` |
| AC5 | R7, R30 | CTest `native-crash-journal`: `jasperengine-crash-test --journal segv` logs 3 entries then faults; the folder holds `journal.txt` whose header and 3 entries match; `check_posix_symbols.py` passes on the POSIX object. Windows CTest the same through the writer. Red: no `journal.txt` |
| AC6 | R8 | `CrashHang.WatchdogWritesTheJournalBesideTheHangReport`: a journal entry, then a debug hang: the session folder holds `hang-001.txt` and `journal.txt` with the entry |
| AC7 | R9 | `test_crash.py::test_journal_engine_events`: Play a scene, pause and resume Play: `read_console categories: ["journal"]` returns `engine.play`, `engine.scene_loaded`; with `player.crash.journal_engine_events` false (set_project_settings) a second Play logs none |
| AC8 | R10 | Web gate `crash-diagnostics.json`: the fixture's Lua logs `gate.ready`; `eval_expect` adds `"journal":true` where `init_script` checks `report.journal.indexOf('gate.ready') >= 0`. Red: the field is undefined |
| AC9 | R34, R38 | `test_crash.py::test_journal_in_console`: `eval_lua` `Journal.log("test.kind", {a = 1})` in Play: `read_console categories: ["journal"]` returns `test.kind a=1`; `Journal.getEntries(1)[1].kind == "test.kind"` |
| AC10 | R11, R12, R13 | `test_crash.py::test_user_report_screenshot`: Play a scene with a red full-screen sprite; `eval_lua` `Crash.beginReport(function(ok) _G.shot = ok end)`; `wait` until `_G.shot == true`; `Crash.createReport{note = "x"}`; `get_crash_reports kinds: ["user"]` lists it; `screenshot.jpg` starts with `FF D8`, decodes to width <= 1280, its centre pixel is red within 24, size <= 600 KiB. Red: `Crash.beginReport` is nil |
| AC11 | R12 | `CrashUserGpu.CaptureRetriesWhilePipelinesArePending` (`JASPER_REQUIRE_GPU()`, a test seam that holds one pipeline pending): the capture reads the first frame without stand-ins; with creation never finishing it fails after 30 frames with `screenshot_unavailable` and the callback gets `false` |
| AC12 | R13 | `CrashUser.EncodeLadderFitsTheCap`: a noise image 1920x1080 with `maxBytes = 100 KiB` ends <= 100 KiB at a quality from the ladder or at half width; with `maxBytes = 1 KiB` the draft has no screenshot and reason `screenshot_too_large` |
| AC13 | R14, R15, R18 | `CrashUser.CreateWritesAUserFolder`: installed handler in a `TempDir` root: `createReport{note, category, fields}` writes `<session>-user1/` with `user.txt` (keys of R14, tags, `field.*`, the note), `device.json` (every R18 key), `journal.txt`, `log-tail.txt`; `readReport` gives kind `user` and reason `user report (gameplay): first line` |
| AC14 | R14, R31 | `CrashUserBench.CreateReportWrites` prints the write time; `perfSlack()` enforces <= 10 ms; capture main-thread cost and encode time are printed and enforced (<= 2 ms, <= 60 ms) the same way |
| AC15 | R16, R17 | `CrashUser.FourthPendingReportIsRefused` and `CrashUser.UnsentReportIsOfferedNextStart` (jasperengine-crash-test, two runs on one root): run 1 creates 1 user report and exits cleanly; run 2's hook receives it with `kind = "user"`; current-session user folders are not in `pendingReports` during run 1. Red: run 1's own report is offered in run 1 |
| AC16 | R19, R20 | `test_crash.py::test_read_report_file`: `Crash.readReportFile(id, "user.txt")` returns bytes equal to the file; unknown name `nil, "not_found"`. Web gate: a second gate file is not added; `crash-diagnostics.json`'s fixture also creates a user report and the eval checks `Crash.readReportFile` length > 0 |
| AC17 | R21, R22, R25 | `tools/crash/test_symbolicate.py` runs `--group --json` over `tools/crash/fixtures/group/` (8 folders: two crashes of one bug at different offsets in two versions with names, one unrelated crash, one unresolved pair from one build, a hang, a user report, an abnormal exit) and compares with `expected.json`; `CrashGroups.MatchesTheSharedFixtures` (editor test) compares the C++ groups with the same file |
| AC18 | R24 | `test_symbolicate.py`: a symbols tree in S30's layout holding the test binary and a decoy binary of another build id under the same name: the group's frames resolve from the matching one; with only the decoy, frames stay unresolved and `unresolved` is true |
| AC19 | R23, R32 | `test_symbolicate.py` generates 1000 copies of the fixture folders and asserts `--group` finishes and prints the expected counts; the time is printed and enforced with `JASPER_TEST_PERF_SLACK` (<= 10 s) |
| AC20 | R37 | `test_crash.py::test_crash_reports_folder_group`: `get_crash_reports folder: <fixtures copy> group: true` returns the expected groups and `next` names the symbolicate command; `kinds: ["user"]` filters; `files: ["journal.txt"]` inlines it; a missing folder fails with the R37 text |
| AC21 | R35 | `test_crash.py::test_crash_reports_window`: `focus_window window: "crash_reports"`; `find_ui_item` finds **Group Similar**, **Open Folder...**, **Mark Handled**; `screenshot` of the editor saved as evidence; Mark Handled on a fixture report writes `handled` |
| AC22 | R33, R41 | `StreamProjectCrashTest.JournalSettingsDefaultsAreNotWritten` and `...JournalSettingsRoundTrip` (extend `editor/tests/stream/StreamProjectCrashTest.cpp`); `PlayerConfig` round-trip of `journalBytes` in `engine/tests/core/player/`; an old `project.yaml` without the keys loads with 32 and true |
| AC23 | R37, R40 | `uv run ... python tools/mcp/tool_budget.py --check tools/mcp/budget.json` passes; the eval case of R40 fails on the tree without R37 and passes with it (`tools/mcp/eval/README.md`, Orquester accounts) |
| AC24 | R26 | Android emulator: `ANDROID_PROJECT=crash-diagnostics` with `--expect-native-crash`: the pulled report folder holds `journal.txt` with the fixture's `gate.ready`; recorded in the PR (manual, exact command in docs/testing.md "Native crash diagnostics") |

## 8. Dependencies & conflicts

- **Depends on:** S28 (report folders, handler, hook; landed), S30 (symbols layout; landed), S06
  (I/O jobs for writes), S32 (creation stats per frame for R12), S56 (`Device` fields).
- **Sibling specs:** release-performance-and-gpu-quirks.md supplies `perf.json`, `gpu_driver` and
  `gpu_quirks` in `device.json` (R14, R18); either may land first, the fields are empty until it
  does. build-and-run-on-device.md pulls a device's report root, which `get_crash_reports folder`
  and the Crash Reports window then read. dev-console-for-agents.md may expose `Journal.getText`
  to an agent attached to a running game. text-entry-gamepad-cursor-back-button.md gives games the
  text entry their report form needs. build-profiles.md may set `journalKiB` per profile.
  savegame.md and input-recorder-and-input-scripts.md produce files a game may attach itself.
  project-test-runner.md tests can assert journal entries through `Journal.getEntries`.
- **Merge hotspots:** `engine/core/diag/CrashHandler.{h,cpp}`, `CrashHandlerPosix.cpp`,
  `CrashHandlerWin.cpp`, `detail/CrashState.h`, `engine/core/script/binding/CrashClassesLua.cpp`,
  `stubs/Crash.lua`, `editor/mcp/tools/DiagnosticsTools.cpp`, `editor/Project.h`,
  `editor/Stream.cpp`, `editor/settings/SettingsRows.cpp`, `editor/Exporter.cpp`,
  `editor/ExporterPrebuilt.cpp`, `engine/core/player/PlayerConfig.cpp`,
  `editor/window/dialog/ProjectSettingsWindow.cpp`, `editor/App.cpp` (Analysis menu),
  `engine/platform/emscripten/crash_pre.js`, `tools/crash/symbolicate.py`,
  `tools/perf/bench-thresholds.json`, `docs/crash-diagnostics.md`, `docs/mcp/TOOLS.md`
  (regenerated).

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| A game writes personal data into the journal or the note | Documented in "Privacy" beside S28's text; the engine never sends; `Journal.clear()` exists; nothing in the engine's own events names a player |
| 32 KiB by default changes old projects' memory | 32 KiB is half the crash ring; documented; 0 turns it off |
| A game logs a journal entry every frame | It works (<= 250 ns) and simply shortens the history; the docs say "events, not frames" |
| The capture frame's offscreen pass creates new pipelines on mobile (a hitch) | Accepted for a user-initiated action; R12 retries; R31 measures |
| `stbi_write_jpg` quality on large flat areas | Quality ladder and halving (R13); JPEG is what Craftsman shipped at the same caps |
| C++ and Python signatures drift | One fixture set both must pass (R25, AC17) |
| A symbols tree with thousands of files is slow to index | Index once per run, keyed by path and mtime; `--binary-dir` remains the fast path |
| The web loses user reports on reload | Documented (R20); a web game uploads at once |
| Older editors show user folders as abnormal exits | Documented (R42); no format change to existing files |
| Windows writer timing with the extra file | The writer already has an 8 s budget (`docs/crash-diagnostics.md`); the journal is at most 256 KiB of memcpy-speed writes |

## Deviations

- **D5 `Field`** has a third member, `verbatim`: Lua's other types are written as `<table>`,
  `<function>` (R2) without quotes, which a plain string value would get.
- **R1** A ring smaller than four whole entries (1028 bytes) is rounded up to that, so Gameplay
  Journal (KiB) = 1 allocates 1028 bytes. `configure` replaces the ring until `crash::install`
  took it (the unit tests set a ring per test); afterwards it is ignored.
- **R9** The editor's `engine.play` also writes `state=resume` (AC7 pauses and resumes; a journal
  with a pause and no resume reads wrong).
- **R34** The Console's message drops the frame too (`test.kind a=1`, as AC9 reads it): the Console
  records each line's frame itself.
  An exported game has no version the engine knows (`crash.txt` writes `version: unknown` too),
  so its `engine.start` says `version=unknown`.
- **R11/R13** `maxWidth` and `maxBytes` are the second argument of `Crash.beginReport(fn,
  {maxWidth, maxBytes})`: the capture is encoded before `createReport` runs, so `createReport`
  takes no such options. A capture that fails runs `fn(false, err)` on the frame after, as a
  successful one does.
- **R14** No `perf.json`: the Performance sampler of release-performance-and-gpu-quirks.md is not
  in this tree; that spec adds the file to `UserReportContent` when it lands. `gpu_driver` is `""`
  and `gpu_quirks` `[]` in `device.json` until then (R18 says so).
- **R20** User reports also stay in memory on a native game whose crash handler is off (no report
  root): the same API works, nothing is written.
- **R8** `journal.txt` is written beside a hang report only when the journal holds an entry (as R7
  for crashes).
- **AC10** The red frame is the scene's background colour (a 2D scene with nothing in it) rather
  than a sprite; the MCP harness has no JPEG decoder, so the centre is compared with
  `diff_images` against a red PNG of the JPEG's size. The decode and centre pixel are also checked
  in `CrashUserGpu` (stb_image).
- **AC22** The new Stream tests are in the file's existing suite, `StreamProjectCrash`.
- **R21/R23** Folders holding only `session.json` (an abnormal exit) count as report folders when
  grouping, as `classify` lists them. A signature joins its parts with `" | "`; an unresolved
  group's ends in `"| build <id>"` (R22). A hang with no own frame falls back to its stack's first
  3 frames of any module, as a crash does.
- **R23/R37** `first_seen` and `last_seen` are the reports' times as `readReport` gives them (the
  file time of `crash.txt` or the hang report, `user.txt`'s `time`, else the folder name's).
  `get_crash_reports folder` also takes a crash root that holds `reports/`. The `next` command and
  the window's Copy Symbolicate Command assume `tools/crash/symbolicate.py` relative to the
  repository (the editor knows no checkout path).
- **R35** The Crash Reports window has a **Symbols Folder...** button (the folder Copy
  Symbolicate Command passes as `--symbols`); its folders are saved per project in
  `.jasperengine/user/build.yaml` (the editor's per-project user settings). Mark Handled on a
  group marks every report of it. A binary file (crash.dmp) in a text tab shows a placeholder.
- **R29 on box A.** `diag::journal::log` took 296-370 ns on box A (a 2.4 GHz Haswell VM on kvm-clock).
  It now formats with its write position in a register and inline short copies, finds the ring
  offset without a 64-bit division, counts an entry as it is reserved (an entry still being copied
  at a crash counts as dropped), and stamps entries from `CLOCK_MONOTONIC_COARSE` where that clock
  ticks at 1 ms or finer (Linux at HZ=1000: a stamp then lags by less than the millisecond R2 prints;
  coarser kernels, most Android ones, and the other systems keep the precise clock). Six runs
  each: 190-213 ns in `JournalBench.LogWithFourFields`, 199-221 ns for `je-bench-diag`'s
  `journal_log_4f_ns`. `JournalBench.LuaLogWithFourFields` measures R29's Lua figure, a new 4-field
  table at each call: 1.01-1.14 us, and 0 allocations in the binding for a table built once.
- **R31 on box A.** `CrashUserBench` times the capture's main-thread share beyond the readback, R31's
  figure: the stand-in readback (a copy of the frame) is timed on its own and printed, as the
  capture's log line reports a real one; 0.04-0.08 ms beyond an 8-12 ms copy. The encode's downscale
  divided per pixel (66 ms on box A); it now finds its column spans once and divides each mean
  through an exact reciprocal, with the same bytes: 36-40 ms (six runs).
