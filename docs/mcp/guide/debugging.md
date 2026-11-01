# Debugging on the device: the log, the console and the stats overlay

The engine has an on-device debug overlay (Unity's IngameDebugConsole and Graphy in one): a **log
console** with filters, search, collapse, stack traces, copy, save and commands; a **popup** with the
warning and error counts; and an **FPS / stats panel**. It runs in the editor's Play (Game view) and in
exported games on every platform, from a document embedded in the engine, so it works when the game's
own UI, CSS, fonts or Lua are broken. Under it is the **log** itself: every line is buffered and handed
to listeners (Unity's `Application.logMessageReceived`), in C++ and Lua.

## 1. Turning it on

Project Settings > Debug > **Overlay** (`debug.overlay`):

| Value | The overlay is enabled |
|---|---|
| `development` (default) | in the editor's Play and in exports built with the **Debug** build type (`Engine.isDevelopmentBuild`) |
| `always` | in every build |
| `never` | never, unless a script turns it on |

At runtime `DebugOverlay.enabled = true` (or `false`) overrides the setting in any build, like the
`ShowDebugConsole` switch of a remote config:

```lua
if remoteConfig.debug then DebugOverlay.enabled = true end
```

Not enabled, the overlay costs nothing: no Scene exists, the input hooks are one branch, the stats
are not sampled. The log buffer always works (`Log.getEntries` in a release build too).

## 2. Opening the console

| How | Default | Setting |
|---|---|---|
| Key | `` ` `` (backquote) toggles the console | `debug.overlay_toggle_key` (a key name: `backquote`, `f1`, `escape`...; `""` none) |
| Key | `F3` cycles the stats: off, FPS, full | `debug.overlay_stats_key` |
| Touch | three fingers held 0.75 s toggle the console | `debug.overlay_gesture`: `three_finger_hold`, `four_finger_tap`, `none` |
| Popup | a bubble at the right edge with the warning/error counts since the last open; tap opens, drag moves it | `debug.overlay_popup`: `never`, `touch_devices` (default), `always` |
| Error | the first error opens the console | `debug.overlay_open_on_error` (default false) |
| Lua | `DebugOverlay.showConsole()`, `hideConsole()`, `toggleConsole()` | |

Keys and gestures are never taken from the game: it still gets them. Only taps and clicks on the
overlay's own panels (the console, the popup) are consumed; taps go through the stats panel. Keys and
text go to the overlay only while its search or command field has the focus. While a text field of the
game has the keyboard, the toggle and stats keys type into it and do not toggle anything.

The overlay keeps its own size whatever the Canvas Scaler (`graphics.ui_scale`): 13 px text of
`min(width, height) / 720` px on desktop; on touch screens 1/160 inch from the screen's DPI (at most
`min(width, height) / 400`, which is also used when the DPI is unknown). Its panels keep clear of the safe area
(`Screen.safeAreaInsets`: notches, rounded corners, `Screen.setExtraInsets` such as a banner ad;
jasper://docs/safe-area), also in the Game view's device simulation.

## 3. The console

- **Info / Warn / Error** toggle the levels shown (verbose and debug lines count as info); each shows
  its count since the last clear.
- **Collapse** shows one row per distinct message (and trace) with a count badge.
- **Search** filters rows by a case-insensitive substring of the message or the stack trace.
- A **row** shows a level mark, the time (`mm:ss.mmm` since the app started) and the first line of the
  message (300 characters at most); **tap it** to see the whole message and its stack trace.
- **Copy** puts the shown rows (only the expanded ones when some are) on the clipboard as
  `[00:12.345] ERROR message` with the trace under it.
- **Save** writes every buffered line to `data://logs/log-YYYYMMDD-HHMMSS.txt` and logs
  `log saved to <path>`.
- **Clear** empties the log buffer and the counts (`Log.clear()`).
- **Stats** cycles the stats panel; **Full / Half** sizes the console; **Close** hides it.
- The list follows the newest line unless you scroll up; every buffered line scrolls (only the rows
  in view are made, so a buffer of 100,000 costs the same); a burst of lines updates it at 10 Hz.

The overlay keeps working while the game is frozen (`Engine.timeScale = 0`, the editor's hold, a Lua
error every frame): it updates on its own unscaled clock. It never pauses the game or changes the
time scale by itself. It sits above every scene (the loading scene too) and is unaffected by scene
loads. It is laid out at the view size, scaled so its text stays legible on any canvas setting.

## 4. Commands

The command field at the bottom runs `name args...` (quotes group words). Built-in commands:

| Command | Does |
|---|---|
| `help` | lists the commands |
| `clear` | clears the log buffer |
| `stats off\|fps\|full` | the stats panel (no argument: cycles) |
| `timescale <x>` | sets `Engine.timeScale` (no argument: prints it) |
| `scene <name>` | `SceneManager.loadScene(name)` |
| `lua <code>` | runs Lua, printing the results (`lua 1+1` prints 2); only with **Allow Lua Commands** (`debug.overlay_allow_lua`: `development` (default), `always`, `never`) |

Your own commands, from a script:

```lua
function M:init()
    DebugOverlay.addCommand("god", "god on|off: no damage", function(args)
        self.invincible = args[1] ~= "off"
        print("god mode", self.invincible)
    end)
end
```

`fn(args)` gets the words after the name. A name is `[a-z0-9_-]+`; adding it again replaces the
command (a built-in name is refused). A command belongs to the script that added it (the `self` of
the calling method, or the 4th argument `owner`) and goes when that script is released; all script
commands go when Play stops. `DebugOverlay.removeCommand(name)` removes one,
`DebugOverlay.executeCommand("stats full")` runs a line as the field does. Output goes to the log.

## 5. The stats panel

FPS mode: `60 FPS  16.7 ms` (the average of the last 2 s, yellow over 17.5 ms, red over 34 ms) and a
frame-time graph of the last 120 frames with lines at 16.7 ms (60 FPS) and 33.3 ms (30 FPS). Full mode
adds a table. Initial mode: `debug.overlay_stats`; corner: `debug.overlay_stats_corner` (`top_left`,
`top_right`, `bottom_left`, `bottom_right`). Text refreshes at 4 Hz, the graph at 10 Hz.

`DebugOverlay.getStats()` returns the same figures at any time (the overlay enabled or not):

| Field | Meaning |
|---|---|
| `fps`, `frameMs`, `frameMsAvg`, `frameMsMin`, `frameMsMax`, `frameMs1PercentLow` | over the last 2 s of unscaled frames; 1% low is the average of the slowest 1% |
| `drawCalls`, `instances`, `triangles` | the last drawn frame (the overlay's own draws excluded) |
| `processMemoryBytes` | resident memory of the process (Linux/Android RSS, Windows working set, Apple physical footprint, Web the WebAssembly heap; 0 when unknown) |
| `luaMemoryBytes` | the Lua heap |
| `audioVoices`, `audioVoicesMax` | sounds playing now, and the most that can play at once |
| `bodies2D`, `bodies3D` | physics bodies of the running scenes |
| `entities`, `scenes` | of the running scenes (`Engine.getSceneLayers()`) |
| `coroutines`, `timers` | engine coroutines (running or waiting) and pending timers (`self:startCoroutine`, `self:invoke`, `Timer.after/every`) |
| `htmlUiDrawCalls` | HTML UI draw calls of the last frame |

`DebugOverlay.statsMode` reads and sets the panel: `DebugOverlay.STATS_OFF`, `STATS_FPS`, `STATS_FULL` (another
value raises an error).

## 6. The log from scripts

```lua
Log.print("score", score)                 -- any values, tab-joined like print
Log.warn("low health")                    -- warnings and errors carry a Lua traceback
local errors = Log.getEntries(20, {LogLevel.ERROR})
for _, e in ipairs(errors) do print(e.id, e.message, e.stackTrace) end
print(Log.count(LogLevel.WARN), Log.bufferSize)
Log.clear()
```

- Levels: `LogLevel.INFO`, `VERBOSE`, `DEBUG`, `WARN`, `ERROR` (integers 0-4).
- `Log.getEntries([maxCount [, levels]])`: the buffered lines, oldest first (the newest `maxCount`),
  each `{id, level, message, stackTrace, time, frame, mainThread, fromLua}`.
- `Log.count(level)`: lines of that level since the last clear. `Log.bufferSize` (rw): the lines kept
  (Project Settings > Debug > Log Buffer Size, default 500; 0 keeps none; at most 100000).
- **Stack traces**: a Lua error's message is split at `stack traceback:` (the trace goes to
  `stackTrace`). `Log.*` lines of Lua get a traceback per **Log Stack Traces**
  (`debug.log_stack_traces`: `none`, `errors`, `warnings` (default: warnings and errors), `all`).
  C++ lines have none.

**Listening** (Unity's `Application.logMessageReceived`, same argument order):

```lua
function M:init()
    self.errors = {}
    RegisterEvent(self, Log.onMessageReceived, "onLog")
end

function M:onLog(message, stackTrace, level)
    if level == LogLevel.ERROR then self.errors[#self.errors + 1] = message .. "\n" .. stackTrace end
end
```

The handler runs on the main thread: at once for a line logged there, at the start of the next frame
for a line an engine worker thread logged (order kept). A line logged inside a handler is recorded
but not handed to handlers again; a handler that raises is logged and stays subscribed. Subscriptions
end when the script is released (and at Play stop).

## 7. C++

```cpp
#include "Log.h"
LogListenerId id = Log::addListener([](const LogEntry& entry) { /* main thread */ });
LogListenerId any = Log::addListener([](const LogEntry& entry) { /* the logging thread: thread-safe! */ },
                                     LogDelivery::AnyThread);
Log::removeListener(id);
std::vector<LogEntry> errors = Log::getEntries(20, logLevelBit(LogLevel::Error));
DebugOverlay::addCommand("spawn", "spawn <n>", [](const std::vector<std::string>& args) { /* ... */ });
DebugStatsSnapshot stats = DebugStats::sample();
```

`MainThread` listeners (the default) follow the Lua rule above; `AnyThread` listeners run on the
thread that logs, at once (Unity's `logMessageReceivedThreaded`). Listeners are never called with the
log's lock held. Logs other threads make wait in a queue of 1,000 lines for the next frame; past it
they are counted and reported once. `Log::removeListener` returns once a call of that listener
running on another thread has ended (so what it uses can go); from inside the listener itself it does
not wait for that call.

## 8. Agents (MCP)

- Settings: `get_project_settings` category `debug`, `set_project_settings` with the `debug.*` keys
  above (undoable, undo scope project). A change during Play applies to the running game at once
  (the console stays open, a stats mode picked stays unless `debug.overlay_stats` changed).
  `get_play_state` shows `debug_overlay: {enabled, console_visible, stats}` while playing.
- In Play: `eval_lua "return DebugOverlay.getStats()"` reads FPS and draw calls;
  `eval_lua "return Log.getEntries(20, {LogLevel.ERROR})"` reads errors with their traces;
  `eval_lua "DebugOverlay.showConsole()"` then `screenshot` shows the console; `send_input` with the
  key `backquote` toggles it like a player.
- `read_console` still reads the editor's Console (every line goes there too).

## 9. Costs

| State | Cost |
|---|---|
| Not enabled | per log line: one lock and one copy into the buffer (nothing with a buffer of 0); per frame: one float into the frame-time ring and a branch per input event |
| Enabled, nothing shown | the same; the overlay Scene stays loaded but is neither updated nor drawn |
| Console open | rows appended as lines come (a full rebuild only when a filter, the search or collapse changes); stats text at 4 Hz and the graph at 10 Hz |
| Memory | ~500 lines x their length (about 100 KB) plus the overlay document while shown |
