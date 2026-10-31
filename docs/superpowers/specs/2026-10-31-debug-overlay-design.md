# Debug overlay: on-device log console and stats overlay (design)

Feature `debug-overlay` (ledger #23, branch `gaps/debug-overlay`). Binding conventions:
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) "Shared conventions" (main-thread delivery; editor Play = exports).

## 1. Goals

1. A `Log` listener API in C++ (`Log::addListener`, a bounded ring buffer of entries with level, message, stack
   trace, time, frame) and in Lua (`Log.onMessageReceived`, `Log.getEntries`, `Log.clear`), Unity's
   `Application.logMessageReceived` equivalent, safe with logs coming from worker threads.
2. An on-device **log console**: engine and Lua logs, filters by level, search, collapse duplicates, stack traces,
   copy to clipboard, save to a file, a notification popup with counts, open by key / touch gesture / Lua, optional
   commands.
3. An on-device **stats overlay**: FPS and frame-time graph, frame time min/avg/max/1% low, draw calls,
   instances, triangles, memory (process and Lua), audio voices, physics bodies, entities, scenes, coroutines,
   timers, HTML UI cost; `DebugOverlay.getStats()` for scripts and agents.
4. Usable in exported builds on every platform, enabled by a Project Setting (never / development / always), the
   build type, or Lua at runtime (a hidden remote-config switch like the game's `ShowDebugConsole`), with negligible
   cost when off.
5. Robust: works when the game's UI is broken (own scene, own embedded document and font, no Lua, no project assets).

## 2. Non-goals

- Remote console (network log streaming): needs feature `http`; a later addition can be a `Log` listener.
- System share sheet for logs (needs `native-bridge`); copy + save cover it.
- Profiler timelines, GPU timers, per-system CPU breakdown (the editor profiler space).
- Android logcat capture of other processes (IngameDebugConsole option); engine logs already go to logcat.
- Replacing the editor's Console/stats (`GameWindow.cpp:547-590`); the overlay also runs in the Game view.

## 3. Users and stories

- Human tester on a phone: holds three fingers for 0.75 s, the console slides in, taps "Errors", taps the error to
  see the Lua traceback, taps "Copy" and pastes it in a chat.
- Human: presses `` ` `` in a desktop export to toggle the console, F3 to cycle the FPS/full stats.
- Developer shipping a release: Project Settings > Debug > Overlay = "never", but
  `if remoteConfig.debug then DebugOverlay.enabled = true end` lets support turn it on for one user.
- Script: `RegisterEvent(self, Log.onMessageReceived, "onLog")` with `function M:onLog(message, stackTrace, level)`
  forwards errors to analytics.
- Agent: `eval_lua return DebugOverlay.getStats()` in Play reads FPS/draw calls; `eval_lua return Log.getEntries(20,
  {LogLevel.ERROR})` reads errors with traces; `screenshot` shows the console when opened with
  `DebugOverlay.showConsole()`.

## 4. Research summary

- Unity `Application.logMessageReceived` (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Application-logMessageReceived.html):
  main thread only, `(condition, stackTrace, type)`; `logMessageReceivedThreaded` (…/Application-logMessageReceivedThreaded.html)
  any thread, handler must be thread-safe. Taken: Lua gets the main-thread form with the same argument order; C++
  gets both. `Debug.isDebugBuild` (…/Debug-isDebugBuild.html): development builds; taken as
  `Engine.isDevelopmentBuild`.
- IngameDebugConsole (https://github.com/yasirkula/UnityIngameDebugConsole), the console the game ships (prefab
  settings: popup on, start in popup mode, search bar, toggle key BackQuote, all log types, `queuedLogLimit 256`,
  `logMessageReceivedThreaded`): popup with counts, Info/Warning/Error filters, collapse, search, tap to expand
  stack trace, commands (`help`), `maxLogCount` trimming. Taken: all of these (bounded buffer instead of unlimited).
- Graphy (https://github.com/Tayx94/graphy): FPS current/avg/1% low with graph, RAM, audio; modes FULL/TEXT/BASIC/OFF;
  hotkeys. Taken: FPS graph + text, full/FPS/off modes, memory, audio voices; left: audio spectrum.
- UI technology: the engine has native UI components and the HTML/CSS UI (RmlUi, always compiled, scrolling, input
  fields, flex layout, embedded Roboto `util/DefaultFont.h:18`); immediate-mode drawing does not exist
  (`LineRenderer` is a scene object). A console needs scrolling lists, wrapping text, a search field and buttons:
  HTML UI gives them; native UI would need a scroll list and text input built from scratch; a new immediate renderer is
  a large backend-specific project. Decision 3.

## 5. Product decisions

1. **Logs are always captured** in a ring buffer (default 500 entries, project setting, 0 = off) from the first
   `Log::` call, so the console shows what happened before it opened and `Log.getEntries` works in any build.
2. **Stack traces**: Lua errors already carry `stack traceback:` (split off into `stackTrace`); `Log.warn/error`
   called from Lua capture a Lua traceback (setting `debug.log_stack_traces`: `"none"`, `"errors"`, `"warnings"`
   (default: warnings and errors), `"all"`). Native (C++) stack traces: none (portable unwinding is out of scope).
3. **HTML UI document in an engine-owned overlay Scene**, driven from C++ (no Lua), loaded from resources embedded in
   the engine binary (`engine://debug/console.rml`, `.rcss`), using the embedded Roboto font. The overlay Scene is
   not a SceneManager scene: it is updated and drawn after every scene (above the persistent and loading scenes) and
   gets input first while visible; it never appears in the Hierarchy, `Engine.getSceneLayers()` or MCP scene lists.
4. **Enable modes** (Project Settings > Debug > Overlay): `never`, `development` (default: editor Play and exports
   built with build type Debug), `always`. `DebugOverlay.enabled` (Lua/C++) overrides at runtime in any build.
   When not enabled: no overlay Scene exists, input hooks are one branch, the stats sampler is idle.
5. **Toggles**: key `` ` `` (backquote, Unity IngameDebugConsole default) toggles the console, `F3` cycles stats
   (off -> fps -> full); touch: **three fingers held 0.75 s** toggles the console (configurable: `three_finger_hold`,
   `four_finger_tap`, `none`); a draggable popup bubble with warning/error counts (shown on touch devices by default)
   opens it; `openOnError` (default off) opens it at the first error. Keys and gestures are not consumed from the
   game (they pass through), except taps on the overlay's own elements.
6. **Lua log listeners run on the main thread**: synchronously for logs made on the main thread, at the start of the
   next frame for logs from worker threads; logs made inside a listener are recorded but not re-dispatched
   (no recursion).
7. **Commands**: `DebugOverlay.addCommand(name, description, fn)` plus built-ins `help`, `clear`, `stats <off|fps|full>`,
   `timescale <x>`, `scene <name>` (load), and `lua <code>` only when `debug.overlay_allow_lua` is on (default on in
   development builds, off otherwise).

## 6. Design: runtime C++

### 6.1 Log (`engine/core/Log.{h,cpp}`)

```cpp
enum class LogLevel : uint8_t { Info = 0, Verbose = D_LOG_VERBOSE, Debug = D_LOG_DEBUG, Warn = D_LOG_WARN, Error = D_LOG_ERROR };
struct LogEntry {
    uint64_t id;            // increasing from 1
    LogLevel level;
    std::string message;    // text before "\nstack traceback:" (trimmed)
    std::string stackTrace; // from "stack traceback:" on, or a Lua traceback captured by Log.warn/error, or ""
    double time;            // Engine::getSystemTime() seconds
    uint64_t frame;         // Engine::getGameFrames() at emit
    bool mainThread;        // emitted on the Lua/main thread
    bool fromLua;           // emitted by Lua (print, Log.*, Lua errors)
};
enum class LogDelivery { MainThread, AnyThread };
using LogListenerId = uint64_t;

class JASPERENGINE_API Log {
public:
    ... existing print/verbose/debug/warn/error ...
    static void emitEntry(LogLevel, const std::string& message, const std::string& stackTrace, bool fromLua);
    static LogListenerId addListener(std::function<void(const LogEntry&)>, LogDelivery = LogDelivery::MainThread);
    static void removeListener(LogListenerId);
    static void dispatchPending();                            // Engine::runFrame start (main thread)
    static std::vector<LogEntry> getEntries(size_t maxCount = 0, uint32_t levelMask = 0xFF, uint64_t afterId = 0);
    static void clearEntries();
    static void setBufferSize(size_t entries);                // 0 = no buffer; default 500
    static size_t getCount(LogLevel);                         // since the last clear
    static FunctionSubscribe<void(const std::string&, const std::string&, int)> onMessageReceived;  // Lua event
};
```

`emit()` (`Log.cpp:14`) formats once (`vsnprintf` with a heap fallback over 4 KB), then: (1) pushes the entry into the
ring buffer under a mutex, (2) calls `AnyThread` listeners on the calling thread (C++ only), (3) on the main thread
and outside a listener: calls `MainThread` listeners and `onMessageReceived(message, stackTrace, level)`; off the main
thread: appends the entry to a pending queue (bounded 1,000; overflow counted and reported once as a warning entry),
(4) calls `System::platformLog` exactly as today (editor Console, logcat, stdout keep working). Main thread =
`LuaBinding::isLuaThread()`. A thread-local "inside a listener" flag stops recursion. `Log::debug` stays compiled out
with `NDEBUG`.

Lua bindings (`CoreClassesLua.cpp:~1520`): `Log.print/verbose/debug/warn/error(s)` become C functions that pass
`fromLua = true` and, per `log_stack_traces`, a traceback from `luaL_traceback(L, L, nullptr, 1)`; `print`
(`LuaBinding.cpp:658`) passes `fromLua = true`. `LuaBinding` errors (`Lua Error: ...` with `stack traceback:`) are split
by the generic rule. New: `Log.onMessageReceived` (event object, `RegisterEvent`; handler `(message, stackTrace, level)`;
`Engine::removeSubscriptionsByTag`, `Engine.cpp:915`, also removes from it so script subscriptions end with the script),
`Log.getEntries([maxCount [, levels]])` -> list of `{id, level, message, stackTrace, time, frame}` newest last (`levels`:
a list of `LogLevel` values), `Log.clear()`, `Log.bufferSize` (rw), `Log.count(level)`; enum `LogLevel.INFO/VERBOSE/DEBUG/
WARN/ERROR`. The `FunctionSubscribe_V_SSI` class is bound with `add/remove/has/call`.

### 6.2 Stats (`engine/core/debug/DebugStats.{h,cpp}`)

```cpp
struct DebugStatsSnapshot {
    float fps, frameMs, frameMsAvg, frameMsMin, frameMsMax, frameMs1PercentLow;   // over the last 2 s of unscaled frames
    uint32_t drawCalls, instances; uint64_t triangles;                            // Engine::getFrameStats (last drawn frame)
    uint64_t processMemoryBytes;  // 0 = unknown on this platform
    uint64_t luaMemoryBytes;      // lua_gc(L, LUA_GCCOUNT)*1024 + LUA_GCCOUNTB
    uint32_t audioVoices, audioVoicesMax;          // SoLoud getActiveVoiceCount / getMaxActiveVoiceCount
    uint32_t bodies2D, bodies3D;                   // sums over running scenes (+ persistent): b2World_GetCounters().bodyCount, JPH GetNumBodies
    uint32_t entities, scenes;                     // EntityManager::getEntityCount() (new), Engine scenes + persistent
    uint32_t coroutines, timers;                   // ScriptScheduler counts (0 until coroutines merges)
    uint32_t htmlUiDrawCalls;                      // sum of UIDocumentSystem::getRenderStats()
};
class DebugStats {
public:
    static void recordFrame(double unscaledFrameSeconds); // every runFrame (cheap: ring of 240 floats)
    static const std::vector<float>& frameHistory();     // milliseconds, oldest first
    static DebugStatsSnapshot sample();                   // computes everything now (main thread)
};
```

`System` gains `virtual uint64_t getProcessMemoryBytes()` (base 0): Linux/Android `/proc/self/statm` resident pages ×
page size; Windows `GetProcessMemoryInfo` `WorkingSetSize`; macOS/iOS `task_info(TASK_VM_INFO)` `phys_footprint`; Web
`emscripten_get_heap_size()`. `recordFrame` is called in `Engine::runFrame` with the raw clamped delta.

### 6.3 Overlay (`engine/core/debug/DebugOverlay.{h,cpp}`, resources `engine/core/debug/overlay/`)

```cpp
struct DebugOverlaySettings {
    bool enabled = false;               // resolved from the project mode and build type by the generated init
    std::string toggleKey = "`";        // key name (Input key table) or "" = none
    std::string statsKey = "F3";
    enum class Gesture { ThreeFingerHold, FourFingerTap, None } gesture = Gesture::ThreeFingerHold;
    enum class Popup { Never, TouchDevices, Always } popup = Popup::TouchDevices;
    bool openOnError = false;
    enum class Stats { Off, Fps, Full } stats = Stats::Off;    // initial stats mode
    enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight } statsCorner = Corner::TopLeft;
    bool allowLua = false;
};
class JASPERENGINE_API DebugOverlay {
public:
    static void configure(const DebugOverlaySettings&);
    static void setEnabled(bool); static bool isEnabled();
    static void showConsole(); static void hideConsole(); static void toggleConsole(); static bool isConsoleVisible();
    static void setStatsMode(DebugOverlaySettings::Stats); static DebugOverlaySettings::Stats getStatsMode();
    static void addCommand(const std::string& name, const std::string& description, std::function<void(const std::vector<std::string>&)>);
    static void removeCommand(const std::string& name);
    // engine hooks
    static bool handleKey(int key, int mods, bool down);                 // Engine::systemKeyDown/Up, before the game; returns false (never consumes)
    static void handleTouch(int pointer, float x, float y, int phase);   // Engine::systemTouch*, before the game; never consumes
    static void update();      // after the scenes update (Engine::runFrame); refreshes DOM at 4 Hz text / 10 Hz graph
    static Scene* getScene();  // the overlay Scene or nullptr
    static void shutdown();    // Engine::systemViewDestroyed / systemShutdown, editor Play stop
};
```

- The overlay Scene is created on the first show (console, popup or stats) after `enable`; it contains one UIDocument
  entity (`engine://debug/overlay.rml`), its default UI camera, nothing else. `Engine` gets
  `static void setOverlayScene(Scene*)`: `runFrame` calls `overlay->update(unscaledDelta)` after `onPostUpdate` (also
  while the game is paused by `timeScale` or the editor holds the simulation, so the console scrolls during a hold) and
  `overlay->draw()` after the scene loop, inside the composite framebuffer (`Engine.cpp:1446-1452`); the editor's
  `drawView` path (`Engine.cpp:1496`) draws it as the last layer of the Game view.
- Input: when the overlay Scene exists, `Engine::systemTouchStart/Move/End`, mouse and key handlers (`Engine.cpp:1654-1711,
  1888-1923, 2191-2213`) offer the event to its document first (the same dispatch as screen documents); the document
  consumes it only when it hits an overlay element (console panel, popup, stats panel when tapped). Keys go to the
  overlay only while its search/command field has focus. `DebugOverlay::handleKey/handleTouch` run before that for the
  toggles and never consume.
- Resources: `overlay.rml` + `overlay.rcss` embedded as C++ raw strings in `DebugOverlayResources.cpp` (the
  `htmlui/compat/ua/UserAgentStyle.cpp:42` precedent) and served under the virtual scheme `engine://` by the HTML UI
  file interface (`htmlui/RmlFile.h`). Styles reference only the embedded Roboto (`util/DefaultFont.h:18`). Layout uses a
  scale `max(1, min(screenW, screenH) / 720)` so text is >= 12 px at 720p-equivalent on any canvas setting; the panel
  honours safe-area insets (`env(safe-area-inset-*)`, feature `safe-area`; 0 until then).
- DOM driven from C++ (`UIElement` API). Console: a header row (Info/Warning/Error toggle buttons with counts, Collapse,
  Clear, Copy, Save, Stats, Size (half/full height), Close), a search field (case-insensitive substring on message and
  trace), the list (newest at the bottom, auto-scroll unless the user scrolled up; rows: colored level mark, time
  `mm:ss.mmm`, first line of the message, count badge when collapsed; tap toggles the full message + stack trace), a
  command field when commands exist or `allowLua`. The list shows at most the buffer size; DOM rows are reused
  (incremental append, rebuild only on filter/search/collapse changes).
- Popup: 48 px bubble at the right edge, draggable vertically, shows `warnings/errors` counts since the last open, tap
  opens the console.
- Stats panel: FPS mode = one line "60 FPS  16.7 ms" + a 120-bar frame-time graph (bars as `div` heights, lines at
  16.7 ms and 33.3 ms); Full mode adds the snapshot fields of 6.2 in a two-column table.
- Copy: the filtered list (or the expanded entry) as text (`[time] LEVEL message\nstack`) through
  `System::setClipboardText`. Save: `FileUtils` (feature `script-data`; until it merges, `File`) writes
  `data://logs/log-YYYYMMDD-HHMMSS.txt` and logs "log saved to <absolute path>".
- Commands: the field splits on spaces (quotes group); unknown command prints "unknown command 'x' (type help)";
  outputs go through `Log::print`. `lua <code>` runs `luaL_loadbuffer` + `pcallWithTraceback`, printing results with
  `tostring`.
- Failure: if the embedded document fails to load, one `Log::error("Debug overlay: ...")` and the overlay disables itself.

### 6.4 Lua API (`DebugOverlay` static class, `Engine` addition)

| Name | Kind | Semantics |
|---|---|---|
| `DebugOverlay.enabled` | rw bool | master switch (initial value from the project setting/build type) |
| `DebugOverlay.showConsole()`, `hideConsole()`, `toggleConsole()` | fn | no-op (with a one-time warning) while not enabled |
| `DebugOverlay.consoleVisible` | ro bool | |
| `DebugOverlay.statsMode` | rw | `DebugOverlay.STATS_OFF`, `STATS_FPS`, `STATS_FULL` |
| `DebugOverlay.getStats()` | fn | table with the fields of 6.2 (camelCase names as in the struct); works while not enabled |
| `DebugOverlay.addCommand(name, description, fn)` | fn | `fn(args)` gets a list of strings; name `[a-z0-9_-]+`, re-adding replaces; commands added by a script are removed when the script is released |
| `DebugOverlay.removeCommand(name)` | fn | |
| `Engine.isDevelopmentBuild` | ro bool | true in editor Play and Debug-type exports |

C++ scripts use the C++ class directly.

## 7. Editor UX, MCP, exported builds, docs

- Project Settings window (`editor/window/dialog/ProjectSettingsWindow.cpp`), new category "Debug": Overlay
  (never/development/always), Toggle key, Stats key, Gesture, Popup (never/touch devices/always), Open on error, Initial
  stats, Stats corner, Allow Lua commands (development builds only / always / never), Log buffer size, Log stack traces.
  Stored in `project.yaml` under `debug:` (`Stream` read/write next to the loading settings), undo like other project
  settings.
- MCP: `get_project_settings` category `debug` (keys `debug.overlay`, `debug.overlay_toggle_key`,
  `debug.overlay_stats_key`, `debug.overlay_gesture`, `debug.overlay_popup`, `debug.overlay_open_on_error`,
  `debug.overlay_stats`, `debug.overlay_stats_corner`, `debug.overlay_allow_lua`, `debug.log_buffer_size`,
  `debug.log_stack_traces`) and `set_project_settings` validation; `get_play_state` adds
  `debug_overlay: {enabled, console_visible, stats}`; `read_console` unchanged. No new tool.
- Editor Play: `Project` calls `DebugOverlay::configure(settings, development = true)` at Play start and
  `DebugOverlay::shutdown()` at stop; the editor's own Game view stats stay; key/gesture work in the Game view (send_input
  can drive them).
- Exported builds: `Generator` (`Generator.cpp:2378-2402`, next to the loading settings) emits
  `DebugOverlay::configure({...})` with `enabled = mode == always || (mode == development && JASPERENGINE_DEVELOPMENT_BUILD)`;
  the export CMake defines `JASPERENGINE_DEVELOPMENT_BUILD` when the export build type is `Debug`
  (`Exporter.cpp:1164`) and `Log::setBufferSize`. Same code on Linux, Windows, macOS, Android, iOS, Web.
- Docs: new guide `docs/mcp/guide/debugging.md` (overlay, settings, gestures, commands, Log listeners, stats fields,
  costs), lua-api.md 2.3 Log rewritten (levels, listeners, getEntries), short `McpInstructions.cpp` line ("on-device
  console: DebugOverlay.showConsole(); stats: DebugOverlay.getStats()"), unity-glossary rows
  (Application.logMessageReceived -> `Log.onMessageReceived`, Debug.isDebugBuild -> `Engine.isDevelopmentBuild`,
  IngameDebugConsole/Graphy -> DebugOverlay), README feature list line.
- Stubs: generated from bindings (`DebugOverlay`, `LogLevel`, `Log` additions); the `getStats` table typed in a fragment
  `stubs/DebugOverlay.lua` (`---@class DebugStats`).

## 8. Behaviour rules and edge cases

- Order in the frame: `Log::dispatchPending()` at the very start of `runFrame` (before `applyPendingLoad`);
  `DebugOverlay::update()` after `onPostUpdate`/`ObjectManager::update`; overlay drawn last.
- Scene loads, persistent scene, loading scene: the overlay is unaffected (not a SceneManager scene) and stays on top.
- App pause: nothing updates; on resume the console shows the logs queued meanwhile.
- `timeScale = 0` and editor hold: the overlay keeps updating (UI clock) so it stays usable on a frozen game.
- Lua errors every frame: the buffer stays bounded; collapse mode shows one row with a count; the popup count saturates
  at "999+".
- A listener that errors (Lua) is logged (that log is not re-dispatched) and stays subscribed.
- Listeners added during dispatch get the next entries; removed ones stop at once.
- Very long messages: stored whole; rows show the first 300 characters of the first line, the expanded view the whole
  text (wrapped).
- Worker-thread logs: their `frame` is the frame at emit; delivery to Lua is the next frame start, order preserved.
- Thread safety: buffer and pending queue under one mutex; listeners never called under it.
- The overlay never changes `Engine.timeScale` or pauses the game by itself.
- Disabled at runtime while visible: hides and destroys the overlay Scene at the next frame.
- Play stop: overlay destroyed; buffer kept (editor) but `Log.clear()` is not implied.

## 9. Performance

- Off (not enabled): per log line one mutex lock + one string copy into the ring (or nothing when size is 0); per frame
  one float write (`recordFrame`) and two branches in input handlers. Target: < 0.01 ms per frame, < 5 us per log line.
- Console visible: incremental DOM appends; full rebuild of 500 rows < 5 ms; stats text 4 Hz, graph 10 Hz; target
  < 0.5 ms per frame on a mid Android phone with the console open and idle.
- Memory: ~500 entries × average 200 B ≈ 100 KB plus the overlay document (created only when shown).

## 10. Tests

gtest `editor/tests/LogListenerTest.cpp`:
1. Ring buffer: size 3, four logs -> the last three, ids increasing; `getEntries(2, Warn|Error mask)`.
2. Split: `Log::error("Lua Error: x\nstack traceback:\n\tfoo.lua:1")` -> message `Lua Error: x`, trace from
   `stack traceback:`.
3. Main-thread listener called synchronously with the entry; listener logging inside -> recorded, not re-dispatched.
4. Worker thread (std::thread) logs 100 lines -> `AnyThread` listener gets them on that thread; `MainThread` listener gets
   them only in `dispatchPending()`, in order.
5. Lua: `RegisterEvent(self, Log.onMessageReceived, "onLog")`, `Log.warn("w")` -> `onLog("w", <traceback>, LogLevel.WARN)`
   with `log_stack_traces = "warnings"`; script released -> no more calls.
6. `Log.getEntries`, `Log.clear`, `Log.count`, `Log.bufferSize = 0`.
gtest `editor/tests/DebugOverlayTest.cpp` (GPU test environment as other HTML UI tests):
7. `configure(enabled=false)`: no overlay Scene after 10 frames with keys/touches sent.
8. Enabled: backquote key -> console visible, document loaded from `engine://`; second press hides.
9. Three touches held 0.75 s (fake touch events over frames with `setFixedDeltatime`) -> toggles; two touches do not.
10. Filters/search/collapse change the row count as expected for a known log set; row tap expands the trace.
11. Copy puts the expected text in `System::getClipboardText()`; Save writes the file under a temp user data dir.
12. Commands: `addCommand("god", ...)` then dispatch "god 1 two" -> fn gets `{"1","two"}`; `lua 1+1` prints 2 only with
    `allowLua`.
13. Stats: `DebugStats::sample()` fields non-negative; `frameHistory` length 240 after 300 frames; `getStats()` from Lua.
14. Overlay above a scene stack with a loading scene: draw order (overlay last), and its document receives a tap before
    a game document under it; a tap outside the panel reaches the game.
MCP `tools/mcp/tests/test_debug_overlay.py`: `set_project_settings debug.overlay=always`; play; `eval_lua
DebugOverlay.showConsole()`; `screenshot` non-empty and `get_play_state.debug_overlay.console_visible == true`;
`send_input` key backquote hides it; `eval_lua return DebugOverlay.getStats().fps > 0`; `eval_lua` a `Log.error` then
`Log.getEntries(1)[1].message`.
Export check: Linux Debug export with `development` shows the console on backquote (manual smoke); Release export with
`never` -> `DebugOverlay.enabled == false`, no overlay Scene.

## 11. Acceptance criteria

- [ ] `Log` C++ listener API, ring buffer, thread rules; Lua `Log.onMessageReceived/getEntries/clear/count/bufferSize`, `LogLevel`.
- [ ] Console with filters, search, collapse, traces, copy, save, popup, commands; stats overlay FPS/full; `getStats()`.
- [ ] Key, gesture, popup and Lua toggles; enable modes resolved per build type; runtime override.
- [ ] Works in editor Play (Game view) and exported builds on all platforms (Linux verified; others compile).
- [ ] Off-cost targets of 9 met (measured in a test with 10,000 log lines and 1,000 frames).
- [ ] Project Settings category, MCP keys, `get_play_state` field, docs/guide/glossary/README, stubs green.

## 12. Integration points

`engine/core/Log.{h,cpp}`; `engine/core/System.{h,cpp}` (`getProcessMemoryBytes`) and `engine/platform/*/System*`;
`engine/core/Engine.{h,cpp}:915-950` (subscriptions), `:1336-1490` (dispatchPending, recordFrame, overlay update/draw),
`:1496-1530` (drawView), `:1580-1592` (pause), `:1654-1711, 1888-1923, 2191-2213` (input); `engine/core/script/LuaBinding.cpp:658-675`
(print); `engine/core/script/binding/CoreClassesLua.cpp` (Log, Engine, new DebugOverlay binding); `engine/core/htmlui/RmlFile.h`
(`engine://`); `engine/core/htmlui/compat/ua/UserAgentStyle.cpp:42` (embedding precedent); `engine/core/util/DefaultFont.h`;
`engine/core/subsystem/AudioSystem.h:37`; `engine/core/subsystem/PhysicsSystem.h:256-268`; `engine/core/ecs/EntityManager.h:113`;
`engine/core/subsystem/UIDocumentSystem.h:123`; `editor/Project.cpp` (Play start/stop), `editor/Generator.cpp:2378-2402`,
`editor/Exporter.cpp:1164`, `editor/Stream.cpp` (project settings), `editor/window/dialog/ProjectSettingsWindow.cpp`,
`editor/mcp/tools/ProjectTools.cpp:2255-2292`, `editor/mcp/tools/PlayTools.cpp:3393`, `editor/window/GameWindow.cpp:547-590`.

## 13. Cross-feature dependencies

- **script-data**: Save uses `FileUtils` (`data://logs/`), fallback `File` until it merges.
- **coroutines**: coroutine/timer counts in stats (0 before it merges).
- **persistence**: the overlay sits above the persistent scene; counts include it.
- **safe-area**: the panel uses `env(safe-area-inset-*)` once that feature resolves them.
- **ui-multitouch / input**: gesture detection reads raw touches in `Engine::systemTouch*` before routing; no
  dependency, but both features must keep those entry points calling `DebugOverlay::handleTouch` first.
- **http** (future): a remote log sink is a `Log` listener.
- Risk: HTML UI robustness is shared with the game's documents (same RmlUi); mitigated by an embedded document with no
  project CSS/fonts/scripts and a self-disable on load failure.

## Implementation notes (2026-10-31, branch `gaps/debug-overlay`)

Implemented as specified, with these decisions where the spec was open, ambiguous or did not fit
the code:

1. **Log**: `LogLevel`, `LogEntry`, `LogDelivery`, `addListener/removeListener`, `dispatchPending`
   (first thing of `runFrame`, under the draw semaphore), `getEntries`, `clearEntries`,
   `setBufferSize`, `getCount` as specified. Added: `getLastId`, `getFirstBufferedId` (the console
   follows trims and clears with them), `getPendingCount`, `setStackTraces`/`wantsStackTrace`
   (`LogStackTraces`), `setMainThread`/`isMainThread`. The main thread is the one
   `LuaBinding::createLuaState` runs on (until then the thread that loaded the engine), not
   `LuaBinding::isLuaThread` directly, so logs before the Lua state exists are judged correctly.
   `onMessageReceived` is declared with a forward-declared `FunctionSubscribe` (FunctionSubscribe.h
   includes Log.h). The platform log is called **before** the listeners (spec: after), so the
   editor's Console shows a line before what a listener logs about it. A line with a split-off
   `stack traceback:` is marked `fromLua` (Lua errors are logged by C++ code). `time` is seconds
   since the engine was loaded (a steady clock of Log.cpp), `frame` reads `Engine::gameFrames`, made
   atomic since worker threads read it. Worker-thread overflow (1,000 lines) is reported as one
   warning when the queue has room again.
2. **Lua `Log.*` take any values** (tab-joined `tostring`, like `print`) instead of one string; the
   lua-api pitfall and the agent guide say so. `Log.getEntries` entries also carry `mainThread`
   and `fromLua`. `FunctionSubscribe_V_SSI` binds `add/remove/has/call` (`has` added to
   `FunctionSubscribe`). `Log.onMessageReceived` subscriptions and the scripts' console commands
   are dropped by `Engine::removeSubscriptionsByTag` (script release) and
   `Engine::clearAllSubscriptions` (Play stop), and `LuaBinding::cleanup` drops both before the
   state closes (a line logged during shutdown would otherwise call into a closed state); a
   command added with no owner is tagged "lua" so it goes with the state too. A Lua `Log.warn` /
   `Log.error` line reaches the editor's Console and the platform log with its traceback under the
   message (as Lua errors already did); `test_play.py`'s console regex filter was adjusted to it.
3. **Commands' owner**: `DebugOverlay.addCommand(name, description, fn [, owner])`. The owner
   defaults to the `self` local of the calling Lua function (a script method), so commands added
   in `init` go with their script without passing anything; an explicit table can be given.
   Built-in names cannot be replaced. Added `DebugOverlay.executeCommand(line)` (IngameDebugConsole's
   `ExecuteCommand`, also handy for agents) and C++ `hasCommand`, `getCommands`,
   `removeCommandsByTag`, `removeOwnedCommands`. The command field is always shown (the built-in
   commands always exist); `lua` is listed by `help` only when allowed.
4. **The overlay Scene**: a plain `Scene` owned by `DebugOverlay`, made the first time something
   of the overlay shows (console, popup or stats) once the view is loaded, kept (document hidden,
   neither updated nor drawn) while enabled and nothing shows, destroyed when disabled, on view
   loss (`viewDestroyed`, made again when shown) and at shutdown / Play stop. `Engine` reaches it
   through `DebugOverlay::getScene()` instead of a `setOverlayScene` setter. Its document loads
   synchronously (`UIDocumentSystem::loadDocumentNow`) so a load failure turns the overlay off at
   once (one error). The overlay's draw calls are excluded from `Engine::getFrameStats` (the game's).
5. **Layout scale**: one overlay px is `max(1, min(viewW, viewH) / 720)` view pixels on desktop and
   `/ 400` on touch devices (spec: 720 for all), so buttons stay finger-sized on phones; text is
   13 overlay px. The document is laid out in the game's canvas (the context of the overlay's
   Scene is the canvas, which the composite maps onto the view) with every length of the sheet in
   `rem`, and the body's font size set to 13 overlay px in canvas px: the overlay looks the same
   whatever the canvas setting, and its text rasterizes at the view's resolution.
6. **Input**: `Engine::getInputScenes` puts the overlay Scene on top of the input snapshot for
   pointer events and, for keys, text and IME, only while the console's search or command field
   has the focus; `canSendDocumentEvents/canSendUIEvents` never treat it as covered by the
   loading scene. Gamepads do not reach the overlay. The **stats panel lets taps through**
   (`pointer-events: none`; spec: consumed when tapped) so an FPS counter over a pause button
   never eats the game's taps. `handleTouch` gets screen pixels (before the canvas transform, so
   fingers outside the canvas count); a hold or tap is cancelled when a finger slides more than
   max(24 px, 4% of the shorter screen side). The four-finger tap needs all four down within
   0.5 s of the first and all up within 0.5 s.
7. **Toggle keys** are key names (`InputControlNames`): defaults `backquote` and `f3` (the spec's
   "`" and "F3" parse to the same keys). Keys toggle on a non-repeat key down.
8. **Clocks**: the overlay runs on its own unscaled clock (the frame time, `fixedDeltatime`
   applied), which also advances while the editor holds the game (the HTML UI clock stops then).
   Gestures, the 4 Hz text and 10 Hz graph refreshes use it.
9. **Copy / Save**: Copy takes the shown rows, or only the expanded ones when some are. Save writes
   every buffered line (not only the filtered ones: a saved log is for sharing everything) with
   `jasperengine::FileUtils::writeText` (feature script-data, merged) to
   `data://logs/log-YYYYMMDD-HHMMSS.txt` and logs `log saved to <system path>`.
10. **Popup** counts warnings and errors since the console was last opened (reset by a clear),
    saturating at "999+"; dragged vertically (RmlUi `drag`), a drag does not open the console.
    `openOnError` opens the console at the first error after the overlay was enabled, once.
11. **Stats**: `recordFrame` gets the raw frame time clamped to 1 s (a stall is one long frame,
    not the scale of the graph). `processMemoryBytes` is implemented once in the base `System`
    (`engine/core/debug/ProcessMemory.cpp`, platform `#if`s: Linux/Android `/proc/self/statm`,
    Windows `K32GetProcessMemoryInfo` working set, Apple `TASK_VM_INFO` `phys_footprint`, Web
    `emscripten_get_heap_size`) instead of in each platform's System subclass, so the editor and
    every export get it. Entities and bodies are summed over `Engine::getSceneLayers()`.
    `AudioSystem::getVoiceCounts`, `EntityRegistry::getEntityCount` added.
12. **Cross-feature hooks** (record what to wire when they merge):
    - **coroutines**: `DebugStats::setTaskCounters(&ScriptScheduler::coroutineCount, &ScriptScheduler::timerCount)`
      (both `size_t()` statics in the coroutines spec) at engine start, e.g. in `Engine::systemInit`;
      until then `coroutines` and `timers` are 0.
    - **safe-area**: `DebugOverlay::setSafeAreaInsetsProvider(fn)` with a function filling the
      canvas-px insets from `Screen::getSafeAreaInsets()`; the overlay moves its body's edges in by
      them (RCSS of an `.rml` document has no `env()`), re-applied when they change. Until then 0.
    - **persistence** (merged): the persistent scene is a running scene layer, so the counts include
      it; the overlay is drawn after it and after the loading scene.
13. **Editor**: Project Settings > Debug page (Overlay and Log sections) with reset arrows; OK
    applies one `DebugSettingsCmd` to the project history (undoable, like Input). The settings
    live in `editor/DebugSettings.{h,cpp}` (struct, value names, `toOverlaySettings`), used by
    Stream, Generator, Project, the dialog and MCP. MCP `debug.*` keys are undoable (undo scope
    project). Play start calls `Project::applyDebugSettingsForPlay` (development build, buffer
    size, stack traces, `DebugOverlay::configure`); Play stop calls `DebugOverlay::shutdown()`.
    The Game view in Play is drawn by `Engine::systemDraw`, so the overlay shows there; the Scene
    view and the edit-mode Game preview (`drawView`) never show it (it exists only in Play).
14. **Exports**: `Generator::debugInitSource` emits `Engine::setDevelopmentBuild(true)` under
    `#if defined(JASPERENGINE_DEVELOPMENT_BUILD) || defined(DEBUG)`, the log settings and
    `DebugOverlay::configure` (development mode = `Engine::isDevelopmentBuild()` at run time).
    `engine/CMakeLists.txt` defines `JASPERENGINE_DEVELOPMENT_BUILD` for the Debug configuration of
    standalone builds (`$<CONFIG:Debug>`: single- and multi-config generators, Android's debug
    variant); the Xcode project's Debug configurations define `DEBUG=1`.
15. **Tests**: the GPU tests (spec 7-11, 13, 14) are in `engine/tests/htmlui/TestDebugOverlay.cpp`
    (the HTML UI GPU harness lives there, not in editor/tests); the log, Lua, commands, stats and
    off-cost tests (spec 1-6, 12, 13, 9) in `editor/tests/LogListenerTest.cpp`; settings in
    `editor/tests/DebugSettingsTest.cpp`. The off-cost test measures the overlay's per-frame hooks
    (frame ring, update check, input hooks) rather than a whole `runFrame`.
