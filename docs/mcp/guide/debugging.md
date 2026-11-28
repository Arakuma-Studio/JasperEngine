# Debugging on the device: the log, the console and the stats overlay

The engine has an on-device debug overlay (Unity's IngameDebugConsole and Graphy in one): a **log
console** with filters, search, collapse, stack traces, copy, save and commands; a **popup** with the
warning and error counts; and an **FPS / stats panel**. It runs in the editor's Play (Game view) and in
exported games on every platform, from a document embedded in the engine, so it works when the game's
own UI, CSS, fonts or Lua are broken. Under it is the **log** itself: the engine log (C++ `JE_LOG_*` and
`Log::*` of every category, Lua; not the editor's own lines) keeps its newest lines in a buffer and hands every line to
listeners (Unity's `Application.logMessageReceived`), in C++ and Lua. The buffer is one of the log's
outputs (a sink, like the Console and the log file): a line the log's level or rate limit holds back
reaches none of them.

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

- **Info / Warn / Error** toggle the levels shown (trace and debug lines count as info, fatal as
  error); each shows its count since the last clear. A line of a category other than engine, editor,
  lua or build shows it first: `[render] 3 draws dropped`.
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

The command field at the bottom runs `name args...`. Quotes group words; inside them `\"` and `\\`
escape. **Tab** completes the word at the cursor (command names, then the arguments); a Tab that changes
nothing, or a second Tab within a second, prints the candidates as one line. **Up/Down** walk the last
32 lines run. On touch screens up to 8 suggestion chips under the field follow the typing; tap one.
A usage error prints in the warning colour with the usage; an unknown name suggests the nearest one
(`unknown command 'gve' (did you mean 'give'? type help)`).

| Command | Does |
|---|---|
| `help [name]` | one line per command (aliases, `(cheat)`); `help give` describes one; `help a*` filters |
| `clear` | clears the log buffer |
| `stats [off\|fps\|full]` | the stats panel (no argument: cycles) |
| `timescale [x]` | sets `Engine.timeScale`, 0 to 100 (no argument: prints it); a cheat |
| `scene <name...>` | `SceneManager.loadScene(name)`; a cheat |
| `lua <code...>` | runs Lua, printing the results (`lua 1+1` prints 2); only with **Allow Lua Commands** (`debug.overlay_allow_lua`: `development` (default), `always`, `never`); a cheat |

Your own commands, from a script, with typed arguments:

```lua
function M:init()
    DebugOverlay.addCommand{
        name = "give", description = "adds an item", aliases = {"g"}, cheat = true,
        args = {
            {name = "item", type = "enum", choices = {"gold", "gem", "key"}},
            {name = "count", type = "int", optional = true, default = 1, min = 1, max = 999},
        },
        run = function(args, call)
            self.inventory[args.item] = (self.inventory[args.item] or 0) + args.count
            call:print(("%d %s"):format(args.count, args.item))
            return self.inventory[args.item]
        end,
    }
end
```

Argument types: `string` (a word), `int` (decimal or `0x` hex), `float`, `bool` (`true/false/on/off/
yes/no/1/0`), `enum` (`choices`, a unique prefix works), `vector3` (`1,0,-2`), `gameobject` (a name,
a `/`-path or `#id`, as `GameObject.find`; passed as the GameObject) and `rest` (the rest of the line,
last only). `run(args, call)` gets each argument by name and by position (`args.count`, `args[2]`);
a bad word never reaches it: the caller gets the error and the usage
(`give: count must be a whole number from 1 to 999 (got "lots"). Usage: give <item> [count=1]`).
`call:print(...)` adds a line to the result (logged as `[give] 10 gold`); lines `print` logs during
the call are lines too. `call:fail(message)` ends with status `failed`; a Lua error gives `error`.
What `run` returns is the result's `value`. `complete = function(argIndex, prefix, words)` returns
candidates for an argument (enum, bool and gameobject complete themselves).

The old form `DebugOverlay.addCommand(name, description, fn(words) [, owner])` still works: `fn` gets the
words as strings. A name is `[a-z0-9_.-]`, 1-48 characters; adding it again replaces the command and
its aliases. A game command may take a built-in's name (not `help`): a warning says so once, and the
built-in comes back when the game's command goes. An alias another command holds refuses the whole
command (`false, "alias 'g' is taken by 'grab'"`). A command belongs to the script that added it (the
`self` of the calling method, or `owner`) and goes when that script is released; all script commands
go when Play stops. `DebugOverlay.removeCommand(name)` removes one.

`DebugOverlay.executeCommand("give gold 10")` runs a line and returns
`{ok, status, command, lines, log, value, error, usage, cheat}`; `status` is `ok`, `unknown`, `usage`,
`failed`, `error` or `disabled`. `DebugOverlay.getCommands()` lists `{name, description, usage, aliases,
cheat, builtIn, args}`; `DebugOverlay.completeCommand(line [, cursor])` gives `{replacement,
candidates, start}`.

**Cheats.** A command with `cheat = true` that ends `ok` sets `DebugOverlay.cheatsUsed` and calls
`DebugOverlay.onCheatUsed(name)` the first time (turn achievements off, mark the save);
`DebugOverlay.resetCheatsUsed()` clears it for a new game; Play stop clears it too. It is an
honest-player signal, not anti-cheat.

**Console Commands** (Project Settings > Debug, `debug.console_commands`: `development` (default),
`always`, `never`) decides who outside the game's code may type commands: the command field, agents
(`eval_lua {command}`) and the player's `--command`. Off, the field is hidden and the others get
status `disabled`. The game's own `DebugOverlay.executeCommand` calls always run, and a script can
turn the rest on: `DebugOverlay.commandsEnabled = true`.

**`--command "<line>"`** runs a line in a player after its `Engine ready:` line (repeatable, one a
frame; the web player takes `?command=...`), in players where Console Commands allows it (development
players by default; elsewhere it exits 2). Each result is logged (`[command] give gold 10: ok`); one
that is not `ok` logs an error, so `--fail-on-error` exits 3. With `--quit-after`, frames count from
the last command.

## 5. The stats panel

Frame times come from the engine's frame-time ring (the profiler's, recorded in every build: a
game's frame from one draw to the next, the editor's own frames in Play), not a copy of its own.

FPS mode: `60 FPS  16.7 ms` (the average of the last 2 s, yellow over 17.5 ms, red over 34 ms) and a
frame-time graph of the last 120 frames with lines at 16.7 ms (60 FPS) and 33.3 ms (30 FPS). Full mode
adds a table. Initial mode: `debug.overlay_stats`; corner: `debug.overlay_stats_corner` (`top_left`,
`top_right`, `bottom_left`, `bottom_right`). Text refreshes at 4 Hz, the graph at 10 Hz.

`DebugOverlay.getStats()` returns the same figures at any time (the overlay enabled or not):

| Field | Meaning |
|---|---|
| `fps`, `frameMs`, `frameMsAvg`, `frameMsMin`, `frameMsMax`, `frameMs1PercentLow` | over the last 2 s of the frame-time ring (unscaled: what the device took); 1% low is the average of the slowest 1% |
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

- Levels: the engine log's, `LogLevel.TRACE` (= `VERBOSE`), `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`
  (integers 0-5; compare with the names).
- `Log.getEntries([maxCount [, levels]])`: the buffered lines, oldest first (the newest `maxCount`),
  each `{id, category, level, message, stackTrace, time, frame, mainThread, fromLua}`; `id` is the
  log record's (increasing, never reused; gaps are lines of other levels or held back).
- `Log.count(level)`: lines of that level since the last clear. `Log.bufferSize` (rw): the lines kept
  (Project Settings > Debug > Log Buffer Size, default 500; 0 keeps none; at most 100000).
- **Stack traces**: a Lua error's message is split at `stack traceback:` (the trace goes to
  `stackTrace`). `Log.*` lines of Lua get a traceback per **Log Stack Traces**
  (`debug.log_stack_traces`: `none`, `errors`, `warnings` (default: warnings and errors), `all`).
  C++ lines have none (their native backtraces go to the log file).

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
console::CommandSpec give;                 // debug/ConsoleCommands.h: typed arguments, aliases, cheat
give.name = "give";
give.args = {{"count", console::ArgType::Int}};
give.run = [](console::Call& call) { call.print(std::to_string(call.args()[0].i) + " gold"); };
console::add(give);
console::Result result = console::execute("give 10");   // result.ok(), result.lines, result.value
DebugStatsSnapshot stats = DebugStats::sample();
```

`MainThread` listeners (the default) follow the Lua rule above; `AnyThread` listeners run on the
thread that logs, at once (Unity's `logMessageReceivedThreaded`). Listeners get every record of the
engine log (`JE_LOG_*` of any category included; `entry.category`), before `Logger::init` and after
`Logger::shutdown` too, and are never called with the log's locks held (the Logger's or the buffer's). Logs other threads make wait in a queue of 1,000 lines for the next frame; past it
they are counted and reported once. `Log::removeListener` returns once every running call of that
listener has ended (so what it uses can go), unless it is called from inside a listener call: then it
does not wait, and only no new call starts. Do not hold a lock the listener takes while removing it.

## 8. Agents (MCP)

- Settings: `get_project_settings` category `debug`, `set_project_settings` with the `debug.*` keys
  above (undoable, undo scope project; not during Play, like every setting). Changed in the
  Project Settings window during Play, they apply to the running game at once (the console stays
  open, a stats mode picked stays unless the Stats setting changed).
  `get_play_state` shows `debug_overlay: {enabled, console_visible, stats, commands_enabled,
  cheats_used}` while playing.
- Console commands: `eval_lua {command: "give gold 10"}` runs a line as a developer types it and
  returns `command: {name, status, ok, lines, log, value, error, usage, cheat}`; `eval_lua {command:
  "help"}` lists the commands in `command.value`. Not `eval_lua {code: "DebugOverlay.executeCommand(...)"}`:
  that hides the result.
- In Play: `eval_lua "return DebugOverlay.getStats()"` reads FPS and draw calls;
  `eval_lua "return Log.getEntries(20, {LogLevel.ERROR})"` reads errors with their traces;
  `eval_lua "DebugOverlay.showConsole()"` then `screenshot` shows the console; `send_input` with the
  key `backquote` toggles it like a player.
- `read_console` still reads the editor's Console (every line goes there too).

## 9. Costs

| State | Cost |
|---|---|
| Not enabled | per log line: the log's own cost, then one lock and one copy into the buffer (nothing with a buffer of 0); per frame: a branch per input event (the frame-time ring is the profiler's, recorded anyway) |
| Enabled, nothing shown | the same; the overlay Scene stays loaded but is neither updated nor drawn |
| Console open | rows appended as lines come (a full rebuild only when a filter, the search or collapse changes); stats text at 4 Hz and the graph at 10 Hz |
| Memory | the buffer's slots, reserved at start so a line never allocates: about 250 KB for 500 lines and the 1,000 waiting for the main thread (longer lines grow their slot once), plus the overlay document while shown |
