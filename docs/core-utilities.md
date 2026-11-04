# Logging, events and timers

Three small pieces of the engine library (`engine/core/log`, `engine/core/util`) that the editor,
games and scripts share: the logger, RAII event subscriptions, and the tick scheduler behind Lua's
`Timer`. The design record is `docs/specs/S09-core-utilities.md`.

## Logging

### Levels and categories

Levels: `Trace < Debug < Info < Warn < Error < Fatal`, and `Off`. Every record belongs to a
category, each with its own minimum level:

| Category | What logs in it |
|---|---|
| `engine` | `Log::print/verbose/debug/warn/error` (C++ engine code) |
| `editor` | `Out::info/success/warning/error` (editor code) |
| `build` | `Out::build`: compiler, linker, CMake and shader builds (Warn or Error for diagnostics) |
| `lua` | Lua's `print`, `Log.*` and Lua errors |
| `script`, `render`, `shader`, `physics`, `audio`, `asset`, `htmlui`, `io`, `mcp` | `JE_LOG_*` calls of those areas |

A category of your own: `JE_LOG_CATEGORY(logCategoryNet, "net", ::jasperengine::LogLevel::Info)` in
C++, `Log.category("net")` in Lua (at most 64 categories in all).

```cpp
#include "log/Logger.h"
JE_LOG_WARN(logCategoryRender(), "%d draw calls dropped", dropped);   // with file:line
JE_LOG_ONCE(Warn, logCategoryAudio(), "no audio device");             // this site logs once
JE_LOG_EVERY(5.0, Info, logCategoryIO(), "still loading %s", name);    // at most every 5 s
```

A call whose level does not reach any sink is not formatted and its arguments are not evaluated
(one atomic load). `JE_LOG_TRACE`/`JE_LOG_DEBUG` are compiled out below
`JE_LOG_MIN_COMPILED_LEVEL` (Trace in every build, Debug in release builds). Format strings are
checked by the compiler (`-Wformat`).

### Sinks

Formatting happens on the thread that logs; the record then goes to the *Inline* sinks right away
and to the *Async* sinks through a bounded queue drained by the `je-log` thread (files are written
once per batch, within ~20 ms; warnings wake the thread at once). The web build delivers every
sink inline.

| Sink | Where | Delivery |
|---|---|---|
| memory | the editor's Console and the MCP `read_console` / `wait_for_log` tools (16 MB, 20,000 records) | Inline |
| console | stdout/stderr: games `(ERROR): msg`, the editor's CLI and headless runs `[ERROR] msg` | Async (editor: Inline) |
| file | `editor.log` next to `settings.yaml` (2 MB x 3), a desktop game's `<userdata>/logs/player.log` (1 MB x 2) | Async |
| mcp | `notifications/message` to MCP clients that called `logging/setLevel` | Async |
| logcat / browser / debugger / oslog | Android logcat (tag `JasperEngine`), the browser console, the Windows debugger, Apple's unified log | Async (browser: Inline) |
| platform | `System::platformLog`, for a `System` that overrides it (install it from `installLogSinks`) | Async |

File lines keep their format: `2026-10-24 12:00:00 [ERROR] message`, then ` [category]` for the
newer categories and ` (file:line)` when the call has a source; an error's backtrace follows as
`  #N module(+0xoffset) symbol` lines. Labels: `INFO `, `WARN `, `ERROR`, `OK   ` (success),
`BUILD`, `DEBUG`, `TRACE`, `FATAL`. A failed rotation (a rename refused) turns rotation off and the
file keeps growing.

### Limits

- **Rate limit:** more than 200 records a second with the same call site (format string), or the
  same text for `"%s"` and Lua, are held back; one `suppressed N similar messages: "..."` record
  follows each second it lasts. Build output is never limited.
- **Full queue** (editor 4 MB, desktop games 1 MB, mobile and web 256 KB): Trace to Info records
  are dropped, warnings and errors wait up to 5 ms. One `[log] dropped N messages (queue full)`
  warning follows.
- **Backtraces:** errors from C++ record up to 32 return addresses (editor and debug builds; games
  by project setting), at most ten a second; Lua errors carry their Lua traceback instead.
- Texts over 64 KB are cut, with `…[truncated N bytes]`, between two characters.
- **UTF-8:** every sink gets valid UTF-8. A text that is not (a tool writing another code page, a
  cut string) is repaired before it is queued: one U+FFFD per bad byte, or on Windows the text read
  in the system's ANSI code page (a game on Windows before 10 1903). Valid text costs one scan.

### Crashes and exit

The editor's crash handler first waits (300 ms, signal-safe) until what was queued is written, so
`editor.log` ends with the lines that led to the crash, then the `FATAL:` block. If it cannot (the
crash is in the logging thread itself) the report says `log tail incomplete: N records
unwritten`. `Logger::shutdown()` drains everything at a normal exit; a process that just exits
still gets 500 ms. A `Fatal` record goes to every sink synchronously.

### Settings

- **Preferences > Logging** (editor `settings.yaml`, `logging:`): a level per category (Reset All),
  the log file's level, Capture Backtrace on Errors, Show Source Location.
- **Project Settings > Player > Logging** (`project.yaml`, `playerLogging:`): the built game's
  level, the desktop log file, backtraces on errors.
- **Command line:** `jasperengine-editor --log-level warn,render=debug ...` (anywhere on the line,
  every subcommand), the same in exported desktop players; `JASPER_LOG_LEVEL` is the fallback.
  The first item without `=` sets every category.
- **MCP:** `set_log_level {level, category?, sink?}`, `get_log_config`, `read_console` filters
  `levels`, `categories`, `has_source`, `wait_for_log`, `logging/setLevel`.

## Text encoding

Every `std::string` in the engine and the editor that holds text or a path is UTF-8, on every
platform (`docs/editor/text-and-languages.md` for users):

- Windows executables run in the UTF-8 code page (`engine/platform/win/jasperengine.manifest`);
  the engine runtime still converts its paths explicitly (`CP_UTF8`, the `-W` functions), since
  games also run on Windows before 10 1903.
- A typed character is a `char32_t` code point: `Engine::systemCharInput` takes one UTF-16 unit or
  one code point and joins halves, so `onCharInput`, `UISystem::eventOnCharInput` and documents get
  whole characters. `StringUtils::toUTF8(char32_t)`, `isValidUtf8`, `repairUtf8` and
  `legacyToUtf8` are the helpers.
- Paths stored in project files use `/`: `Stream::portablePath` writes them so and reads `\` as a
  separator.
- Text from outside: `TextEncoding::consoleToUtf8` (child output), vswhere through
  `CommandRunner::vswhere` (`-utf8`), Apple paths through `UTF8String`.
- `tools/check_text_encoding.py` (a lint to run when you change these areas; `--self-test` is its red control) fails on `CP_ACP`, ANSI Win32 calls that take
  text or paths, `cStringUsingEncoding:` other than UTF-8, and vswhere run outside its helper.

## Events

`Signal<Args...>` is the event type for new C++ code; a subscriber holds the `Subscription` it
gets back (move-only, unsubscribes when destroyed, inert when the signal is gone):

```cpp
Signal<const std::string&> onAssetChanged;
Subscription watch = onAssetChanged.subscribe([this](const std::string& path) { reload(path); });
onAssetChanged.invoke("textures/a.png");
```

The engine's existing events are `FunctionSubscribe` (tagged subscribers, Lua bindings,
`REGISTER_*` macros), now built on the same core. For both:

- invoking allocates nothing and goes over the subscribers there were when it started;
- one added during an invoke waits for the next one; one removed (or cleared) during an invoke is
  skipped if it did not run yet, and the callback running stays alive until it returns (removing
  yourself no longer skips your neighbour);
- nested invokes are fine; the event may even be destroyed by a subscriber.

`ThreadSafeSignal` takes subscribers and invokes from any thread (a copy-on-write snapshot).

**Owners.** A subscription can name an owner key (the address of a script instance or a Lua
table). `SubscriptionOwnerIndex::releaseOwner(key)` removes every subscription of that owner, in
every event, and cancels its timers: script release does this (Lua scripts by their table,
`RegisterEvent` with or without an explicit tag; C++ scripts through the generated glue and the
`REGISTER_*` macros, which pass `this`). `Engine::removeSubscriptionsByTag` and
`Scene::removeSubscriptionsByTag` remain for one release for C++ glue generated before.

**Crash guard.** While the editor's Play runs, `CrashHook` is set: every callback and timer runs
under `callWithCrashGuard`; one that faults is removed (cancelled) and reported, Play pauses and
then stops.

## Timers

`Scheduler::game()` runs on the engine's main thread, on five clocks: `Game` (scaled by the time
scale, stopped while paused, held or in the editor's Pause), `Unscaled`, `Real` (always),
`Frames` and `FixedSteps`. Engine::systemDraw processes the posted tasks and `Real` at frame
start, `FixedSteps` after each fixed update, `Game`/`Unscaled`/`Frames` after the scenes'
update and before `onPostUpdate`. Play's Stop clears it.

```cpp
TimerHandle h = Scheduler::game().after(2.0, [this]() { explode(); });
TimerGroup timers;                                        // cancels what it holds when destroyed
timers.add(Scheduler::game().every(0.5, [this]() { blink(); }, TimeDomain::Unscaled, {.count = 10}));
Scheduler::game().post([] { /* from a worker: runs on the main thread next frame */ });
h.cancel();   // from any thread
```

Work scheduled while the scheduler runs waits for its next run; equal due times run in order; a
repeating timer fires once per run and keeps its phase unless `catchUp` (up to 8 missed periods).
Tasks up to 48 bytes are stored without allocating.

The editor has its own scheduler (`editor::scheduler()`): `EditorHost::enqueueMainThreadTask`
posts to it, and it runs once per editor frame (FIFO, a throwing task is logged and the rest
run), on the `Real` and `Frames` (editor frames) clocks; the idle editor sleeps until its next
`Real` timer.

Lua has `Timer` (`docs/mcp/guide/lua-api.md`, section 2.13): `Timer.after/every/nextFrame/
afterFrames/afterFixedSteps`, coroutines with `Timer.run` and `Timer.wait*`, `{ owner = self }`.
`list_timers` (MCP) shows what is pending.
