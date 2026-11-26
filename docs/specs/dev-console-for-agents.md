# Dev Console for Agents: typed console commands with structured results, completion, a cheat flag and an MCP command mode

Spec-first. Extends the debug overlay's console (feature debug-overlay) and the `eval_lua` tool
(S31's Play group). Paths are relative to the repository root unless they start with
`craftsman-end-motor/`, which means `/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor/`.
Every `file:line` of section 3 was read for this spec; nothing was built or run, so budgets are
targets the implementer measures, and behaviour no test covers yet is marked *(inferred)*.

## 1. Goal

Games already add console commands (`DebugOverlay.addCommand`), but a command is a bag of words:
it has no usage, no argument checks, no aliases, no completion, and its output goes to the log, so a
human mistypes and an agent has to scrape the console to learn what happened. This spec turns the
console into a small command system: commands declare typed arguments, usage and aliases; a call
returns a structured result (`ok`, output lines, error with usage, a value); the overlay completes
names and arguments and keeps a history; commands that change the game can be flagged as cheats a
game can read; release builds keep typed commands off by default; and agents run commands through
`eval_lua {command: ...}` and get the same structured result. The registry leaves the overlay so it
works in players built without the HTML UI module.

## 2. Scope

**In scope**
- A console command registry in `engine/core/debug/ConsoleCommands.{h,cpp}` that does not depend on
  `JASPERENGINE_HTMLUI`; the overlay becomes one of its front ends.
- Command specs: description, usage, aliases, typed arguments (`string`, `int`, `float`, `bool`,
  `enum`, `vector3`, `gameobject`, `rest`), optional arguments with defaults and ranges, argument-count
  limits for untyped commands, a `cheat` flag, custom argument completion.
- A structured result for every call: status, output lines, warnings and errors logged during the
  call, a returned value, the usage on a usage error.
- Shadowing: a game command may replace a built-in (except `help`) and the built-in returns when the
  game command goes.
- Completion and history in the overlay (Tab, double Tab, Up/Down, suggestion chips on touch).
- `cheatsUsed` / `onCheatUsed` for games (achievements, leaderboards, "modified save" marks).
- Project Settings > Debug > **Console Commands** (Development Builds / Always / Never) gating who
  outside the game's code may run commands: the overlay field, MCP and the player's `--command`.
- Lua API additions and stubs; MCP: `command` on `eval_lua`, two fields in `get_play_state`; player
  option `--command`; docs.

**Out of scope (decisions)**
- A remote console into a running device or exported player over the network. `--command` covers
  scripted runs; a device channel belongs to `build-and-run-on-device.md` and
  `shipped-game-diagnostics.md`, which can call this registry.
- Unity-style console variables (CVars) with persistence: a game writes a command that sets a value.
- Permissions per user or sender kinds (Craftsman's player/console split): a single-player
  engine console has one sender. MCP, the overlay and `--command` are all "the developer".
- Command scripts/batches (`exec file.cfg`) and binding commands to keys: a game adds them as commands.
- Anti-cheat. `cheatsUsed` is an honest-player signal, not a security boundary.
- C++ attribute-based registration (Unity IngameDebugConsole's `[ConsoleMethod]`): C++ games register
  through the spec struct.
- Localising command text: developer-facing English.

## 3. Current state

**The registry lives inside the overlay.** `DebugOverlay::addCommand(name, description, fn, ownerTag)`,
`removeCommand`, `hasCommand`, `getCommands` (name and description pairs), `executeCommand(line)`
(void), `removeCommandsByTag`, `removeOwnedCommands` (`engine/core/debug/DebugOverlay.h:73-88`). The
whole implementation is under `#if JASPERENGINE_HTMLUI` (`engine/core/debug/DebugOverlay.cpp:8`); the
other branch makes `addCommand` return false and `executeCommand` do nothing
(`DebugOverlay.cpp:1908-1912`), so a player built without the HTML UI module (S34 modules) has no
commands at all.

**A command is a name, a description and a function of words.** `struct Command {name, description,
function(const std::vector<std::string>&), ownerTag, builtIn}` (`DebugOverlay.cpp:73-79`). Names are
`[a-z0-9_-]+` (`:304-312`). The line is split on spaces with `"..."`/`'...'` grouping and no escapes
(`:315-345`). `addCommand` refuses a built-in name and replaces a game command of the same name
(`:1548-1580`). `getCommands` hides `lua` when Lua commands are off (`:1590-1603`).

**Execution logs instead of returning.** `executeCommand` logs `> line`, an unknown name logs
*unknown command 'x' (type help)* as a warning and returns; there is no argument check of any kind;
the arguments go to the function as strings (`lua` gets the rest of the line verbatim) and nothing
comes back (`:1605-1636`). Built-ins `help`, `clear`, `stats`, `timescale`, `scene`, `lua`
(`:1090-1186`) report through `Log::print`/`Log::warn`, each parsing its own words (`timescale` with
`strtod`, `stats` by string compare). `lua` needs Allow Lua Commands (`:1144-1148`).

**Lua.** `DebugOverlay.addCommand(name, description, fn [, owner])` passes `fn` a table of words; an
error inside is logged as *Lua Error: console command 'x': ...*; the owner (the calling `self` by
default) removes the command when released (`engine/core/script/binding/CoreClassesLua.cpp:1338-1399`).
`executeCommand` is bound directly and returns nothing (`:3678`). The stub
`engine/core/script/binding/stubs/DebugOverlay.lua` has no command shapes.

**The overlay field.** One text input, placeholder *Command (type help)*
(`engine/core/debug/DebugOverlayResources.cpp:45`); Enter runs it (`DebugOverlay.cpp:1329-1331`,
`runCommandField` `:1241-1248`). No completion, no history (`grep -n history` finds only the stats
graph).

**Settings and build type.** Project Settings > Debug: Overlay (Never / Development Builds / Always)
and Allow Lua Commands (Development / Always / Never) (`editor/DebugSettings.h:19-33`), resolved per
build type in `toOverlaySettings` (`editor/DebugSettings.cpp:70-82`); MCP keys `debug.*`
(`:113-125`); reflected with labels and tooltips (`editor/reflect/DebugReflect.cpp:90-124`).
`Engine::isDevelopmentBuild()` is true in the editor's Play and Debug exports
(`engine/core/Engine.h:824-828`, `editor/Generator.cpp:2922-2923`). Commands are not gated: a release
game's `DebugOverlay.executeCommand` runs them; only the overlay being off hides the field
*(inferred from the absence of any check in `executeCommand`)*.

**Lifetime.** Play stop calls `DebugOverlay::shutdown` (`editor/Project.cpp:11635`), which removes
script commands (`DebugOverlay.h:121-123`).

**Agents.** No MCP tool reaches the registry (`grep -rn executeCommand editor` finds nothing outside
tests, verified). Agents call `eval_lua {code: "DebugOverlay.executeCommand('god on')"}`, get no
result value, and read what the command logged from the console lines `eval_lua` returns since its
cursor (at most 50; `editor/mcp/tools/PlayTools.cpp:2124-2140`). `eval_lua` requires `code`
(`PlayTools.cpp:4424-4444`). `get_play_state` reports `debug_overlay {enabled, console_visible,
stats}` (`PlayTools.cpp:795`). Players accept `--headless`, `--quit-after`, `--fail-on-error` and
friends but nothing that runs a command (`engine/core/player/LaunchOptions.cpp:159-253`; the web maps
its URL query to the same options, `:108-120`).

**Tests.** Commands are exercised only in `tools/mcp/tests/test_debug_overlay.py:155-166` (a script
command and `lua 6*7` through `executeCommand`), a file excluded from the CI suite (`:20-21`). No unit
test covers the registry (verified by grep).

**Lookups available.** `GameObject.find` resolves a name or a `/`-path among active GameObjects of the
running scenes (`engine/core/script/binding/ScriptClassesLua.cpp:462-490`).

## 4. Reference

**Craftsman engine (studied, ideas only).** `craftsman-end-motor` is a port of Minecraft PE 0.15
reconstructed from Mojang's retail binary. Ideas only: no code, data, assets or Minecraft-specific
mechanics (permissions nodes, target selectors, chat routing, command blocks) may be copied.

| Pattern | Where | What we take |
|---|---|---|
| `CommandSpec {name, usage, help, aliases, allowed sender, minArgs, maxArgs, fn}`; registering a name or alias that is taken registers nothing | `include/mcpe015/command_registry.h:117-134` | Spec fields (R2, R4), all-or-nothing alias registration (R2) |
| `CommandResult {Ok, Unknown, Denied, BadUsage, WrongSender, Failed, Inactive}` and `CommandRun {result, output lines}`; `ctx.reply`, `ctx.fail`, `ctx.usage` | `command_registry.h:71-108,198-205` | A status plus output lines returned to the caller (R5); `call:print`, `call:fail` (§6) |
| A lower layer of built-in commands under the plugin layer: a plugin takes a name, the built-in shows again when the plugin lets go | `src/command_registry.cpp:66-80`, `command_registry.h:137-144` | Shadowing (R7) |
| "With the registry empty the engine behaves exactly as before": one relaxed atomic load gates every hook | `command_registry.h:20-31,148-154` | Zero cost when unused (R22) |
| A quote-aware tokenizer that keeps the first character of a line without a prefix | `command_registry.h:228-232` | We keep our splitter and add backslash escapes (R3) |

**Unity and others (names and behaviour only, no code).** Unity has no built-in console;
`Debug.isDebugBuild` is the development-build test our `Engine.isDevelopmentBuild` mirrors. The
overlay already follows the IngameDebugConsole asset (`DebugOverlay.h:40`), whose `[ConsoleMethod]`
commands take typed parameters and show suggestions while typing: R3 and R14 follow that behaviour.
Unreal's `UCheatManager` (cheats off in shipping builds) is the model for the `cheat` flag and the
release default.

## 5. Requirements

### Functional: registry and specs

- **R1 Registry outside the overlay.** `jasperengine::console` (`engine/core/debug/ConsoleCommands.{h,cpp}`)
  owns commands, parsing, execution and completion, and compiles in every player. Without the HTML UI
  module commands register and run (Lua, MCP, `--command`); there is just no overlay field. The
  `DebugOverlay::addCommand/removeCommand/hasCommand/getCommands/executeCommand/removeCommandsByTag/
  removeOwnedCommands` functions stay and forward to it.
- **R2 Spec.** A command has: `name` (`[a-z0-9_.-]`, 1-48 characters, lower case; `.` is new, for
  groups such as `ai.debug`), `description` (one line), `usage` (generated from `args` when empty:
  `give <item> [count=1]`), `aliases` (same rule, at most 8), `args` (R3), `minArgs`/`maxArgs` (R4),
  `cheat` (default false), owner (as today), `run`, optional `complete`. Registering fails as a whole,
  naming the holder, when the name or an alias is another command's alias or an alias is another
  command's name: *DebugOverlay.addCommand: alias 'g' is taken by 'grab'.* Adding a name that a game
  command already has replaces it (today's rule), its aliases included.
- **R3 Typed arguments.** Each arg is `{name, type, optional?, default?, min?, max?, choices?}`.
  Parsing, after the line is split (quotes group words; new: `\"` and `\\` escape inside quotes):
  - `string`: one word. `rest`: the rest of the line verbatim (last arg only; as `lua` today).
  - `int`: a whole decimal (or `0x` hex) in the int64 range; `float`: a finite decimal; both checked
    against `min`/`max`.
  - `bool`: `true/false/on/off/yes/no/1/0`, case-insensitive.
  - `enum`: one of `choices`, case-insensitive; a unique prefix is accepted (`gol` -> `gold`).
  - `vector3`: `x,y,z` as one word, three finite numbers.
  - `gameobject`: `#<id>` (an entity id in the running scenes), else what `GameObject.find` resolves
    (a name or a `/`-path, active only); several GameObjects with that name in different scenes take
    the first, as `GameObject.find`. Passed to Lua as the GameObject handle.
  An error names the argument, what was expected, what came, and ends with the usage:
  *give: count must be a whole number from 1 to 999 (got "lots"). Usage: give <item> [count=1]*;
  *give: item must be one of gold, gem, key (got "ruby").*; *tp: no GameObject "Hero2" (did you mean
  "Hero"?)*. Missing required args and extra words are usage errors too.
- **R4 Untyped commands.** A command without `args` gets its words as strings (today's behaviour); it
  may declare `minArgs`/`maxArgs` (default 0 and unbounded), checked with the same usage error.
- **R5 Result.** Every execution returns `ConsoleResult`: `status` (`ok`, `unknown`, `usage`, `failed`,
  `error`, `disabled`), `command` (the resolved name, after aliases), `lines` (output lines), `log`
  (warnings and errors logged on the main thread during the call, `{level, message}`), `value` (what
  `run` returned, converted for MCP with `LuaJson`; Lua gets the value itself), `error` (one line),
  `usage` (on `usage`), `cheat`. `ok` is `status == ok`.
- **R6 Output lines.** `call:print(...)` (Lua) / `call.print(text)` (C++) appends a line and logs it at
  Info, prefixed in the log only (`[give] 10 gold`). Lines logged at Info or lower on the main thread
  during the call (`print`, `Log.print` and today's commands' `Log::print`) are lines too, so existing
  commands return their output unchanged in code. `call:fail(message)` ends with status `failed`; a Lua
  error in `run` gives `error` with the message and the first traceback line in `error` and the full
  traceback in `log`. The echo `> line` is logged as today but is not a line of the result.
- **R7 Shadowing built-ins.** A game command may take a built-in's name (except `help`): one warning
  per name per session (*console: 'scene' now runs the game's command; the built-in returns when it is
  removed*); removing the game command (or its owner going) brings the built-in back. Today's refusal
  (`DebugOverlay.cpp:1561-1565`) goes (a deliberate change, R31).
- **R8 Help.** `help` prints one line per command (`name - description`, aliases in brackets, `(cheat)`)
  and returns them as `value` (a list of `{name, description, usage, aliases, cheat, builtIn, args}`);
  `help <name or alias>` prints usage, each arg with type, range, choices and default, and returns that
  one entry. `help <prefix>*` filters.
- **R9 Completion.** `console::complete(line, cursor)` returns `{replacement, candidates}` for the word
  at the cursor: the first word completes command names and aliases; later words complete by arg type
  (`enum` choices, `bool` words, `gameobject` names of the running scenes, first 20, sorted) or by the
  command's `complete(argIndex, prefix, words)` function; untyped args complete nothing.
  `replacement` is the longest common prefix of the candidates (with a trailing space when one
  candidate remains). Candidates are sorted and capped at 50.
- **R10 Built-ins as typed commands.** `stats [mode: enum off|fps|full]`, `timescale [x: float 0..100]`,
  `scene <name: rest>`, `lua <code: rest>`, `clear`, `help [name: string]` keep their behaviour and
  gain usage errors; `timescale`, `scene` and `lua` are cheats. `stats` and `timescale` return the
  new value as `value`.
- **R11 Cheats.** A command with `cheat = true` that ends `ok` sets `console::cheatsUsed()` and fires
  `onCheatUsed(name)` the first time per session (then only the flag stays). `resetCheatsUsed()`
  clears it (a new game); Play stop and app start clear it. Commands without the flag never set it.
- **R12 Gate.** Project Settings > Debug > **Console Commands**: Development Builds (default), Always,
  Never. It sets `console::commandsEnabled()` at start; scripts may change it at runtime
  (`DebugOverlay.commandsEnabled = true` after a secret code). When off, the overlay hides its command
  field, MCP's command mode and `--command` get status `disabled` with *Console commands are off in
  this build (Project Settings > Debug > Console Commands).* The game's own Lua and C++ calls of
  `executeCommand` always run: the gate decides who outside the game's code may type commands, and
  existing release games that call their commands keep working.
- **R13 Nesting and safety.** A command may execute others up to depth 4 (deeper: `failed`, *nested too
  deep*). A command that removes itself while running finishes (today's copy of the function stays).
  Main thread only; called elsewhere it returns `error` with *main thread only*.

### Functional: the overlay

- **R14 Completion and history.** In the command field: Tab applies the `replacement`; a second Tab
  within 1 s (or a Tab that changes nothing) prints the candidates as one console line; Up/Down walk the
  last 32 executed lines (session only, duplicates of the previous line not stored). On touch devices a
  row of up to 8 suggestion chips under the field follows the typing; tapping one applies it.
- **R15 Results in the console.** A `usage` result prints the error and the usage in the warning
  colour; `unknown` prints *unknown command 'gve' (did you mean 'give'? type help)* (a suggestion when
  one name is within edit distance 2); `failed` and `error` print in the error colour.

### Functional: player option

- **R16 `--command "<line>"`** (repeatable; web: `?command=...` repeated) runs each line in order, one
  per frame, after "Engine ready:", in players where Console Commands allows typed commands
  (development players by default). Results are logged (`[command] give gold 10: ok`); a non-`ok`
  status logs an error, so `--fail-on-error` exits 3. In a player where commands are off it exits 2:
  *--command needs a development player or Project Settings > Debug > Console Commands: Always.*
  It combines with `--after-ready`, `--quit-after` (frames count from the last command when both are
  given) and `--screenshot`.

### Platform & backends

- **R17 Everywhere.** The registry is plain C++ and Lua: all six targets, no GPU use. The overlay part
  needs the HTML UI module as today; touch chips work on Android, iOS and touch web.
- **R18 Text input.** Tab is read from the field's key events; where no hardware keyboard exists the
  chips are the completion. IME composition is untouched (completion acts only on committed text).

### Performance

- **R19 Dispatch cost.** Split, lookup, alias resolution and typed parsing of a 5-argument line take
  <= 20 us on the desktop reference, the command body excluded (`ConsoleCommandsBench.Dispatch5Args`).
- **R20 Completion cost.** <= 0.2 ms with 500 commands for a first-word completion and <= 1 ms for a
  `gameobject` completion in a scene of 10,000 GameObjects (`ConsoleCommandsBench.Complete`).
- **R21 Capture cost.** The log capture of R5/R6 is a listener installed for the duration of one call
  and removed after it; outside calls nothing listens.
- **R22 Zero cost when unused.** No registry storage until the first registration or execution (the
  built-ins register lazily, as today); no per-frame work; the disabled overlay keeps its single branch
  per input event. `ConsoleCommands.NothingAllocatedUntilFirstUse` counts allocations over engine start
  and 100 frames.

### Editor UX (human)

- **R23 Project Settings > Debug** gains **Console Commands** (Development Builds / Always / Never)
  next to Allow Lua Commands, with the tooltip *Who may type commands: the console field, agents and
  the player's --command option. The game's own scripts can always run their commands.* Undoable like
  every Project Settings row.
- **R24 Console window.** The editor's Console shows command output lines as today (they are log
  lines); a line logged by a command carries its `[name]` prefix, so the Console's search finds them.
  No new editor window: commands are typed in the Game view's overlay during Play.

### Agent UX (MCP, Lua, CLI)

- **R25 `eval_lua` command mode.** New parameter `command` (string): a console line run instead of
  `code` (exactly one of the two; both or neither fails: *Pass code (Lua) or command (a console line),
  not both.*; `code` stops being schema-required, the handler enforces it). Only in Play
  (`allow_edit_mode` does not apply: *Console commands run in the playing game: start it with play
  {action: "start"} first.*). Result `structuredContent`: `{mode: "play", command: {name, status, ok,
  lines, log, value, error, usage, cheat}, console, next_cursor}`; the text is the lines, or the error
  and usage; `isError` when `status` is not `ok`. Errors name the fix (R3's messages; `unknown`
  includes *eval_lua {command: "help"} lists the commands*; `disabled` names the setting and
  `set_project_settings {values: {"debug.console_commands": "development"}}`).
- **R26 Listing commands.** `eval_lua {command: "help"}` returns R8's list in `command.value`; no new tool.
- **R27 Play state.** `get_play_state.debug_overlay` gains `commands_enabled` and `cheats_used`.
- **R28 Budget.** The `command` parameter and description change add at most 110 characters to
  `eval_lua`; if the default listing lacks room, `eval_lua`'s description gives up the same amount
  (recorded in `tools/mcp/budget.json` as S31's rules require).
- **R29 Lua.** §6's additions with stubs in `engine/core/script/binding/stubs/DebugOverlay.lua`
  (`ConsoleCommandSpec`, `ConsoleArgSpec`, `ConsoleCall`, `ConsoleResult`, `ConsoleCompletion`).
- **R30 Docs.** `docs/mcp/guide/debugging.md` section 4 rewritten (typed commands, results, cheats,
  Console Commands setting, completion keys, `--command`); `docs/mcp/guide/lua-api.md` debug-overlay
  lines; `docs/shipping.md` option table gains `--command`; the agent guide's index entry for debugging
  names `eval_lua {command}`; `TOOLS.md` regenerated; an eval case in `tools/mcp/eval/cases.py`
  (`guard_console_command_mode`): asked to give the player 10 gold through the game's `give` command
  and report the new total, the agent calls `eval_lua` with `command` and quotes `command.lines`; it
  fails without R25's description text (agents then wrap `executeCommand` in `code` and miss the
  result).

### Data & compatibility

- **R31 Old projects and scripts.** `DebugOverlay.addCommand(name, description, fn [, owner])` and its
  words table are unchanged; `executeCommand` now returns a result table (callers ignoring it are
  unaffected); C++ `executeCommand` returns `ConsoleResult` instead of `void` (source-compatible for
  callers that ignore it). Behaviour changes, each pinned by a test changed in the same commit: a game
  command may shadow a built-in (R7); `timescale`/`stats` with a bad word now give a usage result
  instead of a warning line (the text stays in the log).
- **R32 New optional setting.** `project.yaml` `debug.consoleCommands` (omitted at the default
  `development`; MCP key `debug.console_commands`), `DebugOverlaySettings::commands` and the generated
  init / prebuilt player config carry it. No version bump, no migration; an old project gets
  Development Builds, which matches today for every build in which the overlay field exists by default.

## 6. Design constraints & decisions

**D1 Registry outside the overlay, overlay as client.** The registry is small, engine-wide state; the
overlay renders and forwards. This also lets MCP and `--command` work in players without HTML UI.

**D2 One entry point, structured result.** `console::execute(line, Source)` with `Source {Game,
Overlay, Agent, LaunchOption}`: the gate (R12) checks the source; everything else is identical, so the
overlay, MCP and the player cannot drift.

**D3 Typed args are parsed by the registry**, never by the command. Lua `run` receives `args` with each
arg both by name and by position (`args.count`, `args[2]`), already converted.

**D4 Output = explicit lines + Info log lines during the call.** That keeps every existing command's
output visible to agents with no change, and gives new commands `call:print`.

**D5 Shadow stack.** Built-ins live in their own table; lookup checks game commands first, then
built-ins (Craftsman's layering). `help` is never shadowed so a human can always list commands.

**D6 The gate is about sources, not code.** Gating the game's own calls would break release games that
call `executeCommand` today and gives no security (the game can call its functions directly).

**D7 Cheats are a flag, not a policy.** The engine only records; the game decides what a cheat costs.

**D8 MCP extends `eval_lua`.** The checklist prefers a parameter on an existing tool; `eval_lua` already
runs code in the playing game and returns console lines, so a console line is the same act in another
language. *Rejected:* a new `run_console_command` tool (near-duplicate, core budget); `run_command`
(the editor's command palette, S25).

### C++ sketch (`engine/core/debug/ConsoleCommands.h`, namespace `jasperengine::console`)

```cpp
enum class ArgType { String, Int, Float, Bool, Enum, Vector3, GameObject, Rest };
struct ArgSpec {
    std::string name;
    ArgType type = ArgType::String;
    bool optional = false;
    std::string defaultValue;                  // parsed like typed text
    double min = -INFINITY, max = INFINITY;    // Int, Float
    std::vector<std::string> choices;          // Enum
};
enum class Status { Ok, Unknown, Usage, Failed, Error, Disabled };
enum class Source { Game, Overlay, Agent, LaunchOption };
struct ArgValue { ArgType type; std::string text; int64_t i = 0; double f = 0; bool b = false;
                  Vector3 v; Scene* scene = nullptr; Entity entity = NULL_ENTITY; };
class Call {
public:
    const std::vector<ArgValue>& args() const;
    const std::vector<std::string>& words() const;   // untyped commands
    void print(const std::string& line);
    void fail(const std::string& message);
    void setValue(nlohmann::json value);             // C++ commands' value
};
struct CommandSpec {
    std::string name, description, usage;
    std::vector<std::string> aliases;
    std::vector<ArgSpec> args;
    int minArgs = 0, maxArgs = -1;                   // untyped only
    bool cheat = false;
    std::string ownerTag;
    std::function<void(Call&)> run;
    std::function<std::vector<std::string>(int argIndex, const std::string& prefix,
                                           const std::vector<std::string>& words)> complete;
};
struct LogLine { LogLevel level; std::string message; };
struct Result {
    Status status = Status::Ok;
    std::string command, error, usage;
    std::vector<std::string> lines;
    std::vector<LogLine> log;
    nlohmann::json value;                            // null when none
    bool cheat = false;
    bool ok() const { return status == Status::Ok; }
};
struct Completion { std::string replacement; std::vector<std::string> candidates; };

JASPERENGINE_API bool add(CommandSpec spec, std::string* error = nullptr);
JASPERENGINE_API void remove(const std::string& name);
JASPERENGINE_API void removeByTag(const std::string& substring);
JASPERENGINE_API void removeOwned();
JASPERENGINE_API const CommandSpec* find(const std::string& nameOrAlias);
JASPERENGINE_API std::vector<const CommandSpec*> list();          // sorted, built-ins shadowed hidden
JASPERENGINE_API Result execute(const std::string& line, Source source = Source::Game);
JASPERENGINE_API Completion complete(const std::string& line, size_t cursor);
JASPERENGINE_API void setCommandsEnabled(bool enabled);
JASPERENGINE_API bool commandsEnabled();
JASPERENGINE_API bool cheatsUsed();
JASPERENGINE_API void resetCheatsUsed();
JASPERENGINE_API extern FunctionSubscribe<void(const std::string&)> onCheatUsed;
JASPERENGINE_API const char* statusName(Status status);           // "usage"
```

`DebugOverlaySettings` gains `bool commands = false` (resolved like `allowLua` by
`DebugSettings::toOverlaySettings`); `DebugOverlay::configure` passes it to `setCommandsEnabled`.

### Lua sketch

```lua
DebugOverlay.addCommand{
    name = "give", description = "adds an item to the player", aliases = {"g"}, cheat = true,
    args = {
        {name = "item", type = "enum", choices = {"gold", "gem", "key"}},
        {name = "count", type = "int", optional = true, default = 1, min = 1, max = 999},
    },
    run = function(args, call)
        self.inventory:add(args.item, args.count)
        call:print(("%d %s"):format(args.count, args.item))
        return self.inventory:count(args.item)                -- result.value
    end,
    complete = nil,                                            -- enum completes itself
    owner = self,                                              -- default: the calling self, as today
}
DebugOverlay.addCommand("god", "god on|off", function(args) ... end)   -- unchanged form

local r = DebugOverlay.executeCommand("give gold 10")   -- {ok, status, command, lines, log, value, error, usage, cheat}
local c = DebugOverlay.completeCommand("give g", 6)     -- {replacement = "g", candidates = {"gem", "gold"}}
for _, cmd in ipairs(DebugOverlay.getCommands()) do print(cmd.name, cmd.usage) end   -- now tables
DebugOverlay.commandsEnabled = true                     -- read/write (R12)
if DebugOverlay.cheatsUsed then achievements.enabled = false end
DebugOverlay.onCheatUsed = function(name) save.meta.modified = true end
DebugOverlay.resetCheatsUsed()
```

`getCommands()` returning tables instead of `{name, description}` pairs is the one Lua shape change:
the pairs were never documented in the guide (section 4 lists only `addCommand`, `removeCommand`,
`executeCommand`); the entries keep `name` and `description` fields, so code reading `cmd.name` works;
code indexing `cmd[1]` breaks (accepted, R31 test).

### MCP sketch

```json
{"command": "give gold 10"}
{"command": "help give"}
```
Result: `{"mode": "play", "command": {"name": "give", "status": "ok", "ok": true, "lines": ["10 gold"],
"log": [], "value": 130, "cheat": true}, "console": [...], "next_cursor": 812}`.
Parameter text (counted in R28): `command: "A console command line instead of code (help lists them)."`

## 7. Acceptance criteria

Unit tests go in `engine/tests/core/debug/` (`jasperengine-core-tests`, no HTML UI needed) unless
stated. Each fails without the change because the API does not exist, plus the reason given.

- **AC1** `ConsoleCommands.TypedArgumentsParseOrNameTheFix`: a table of lines for every `ArgType`
  (ranges, hex, prefixes of enums, `vector3`, `#id` and path `gameobject` in a test scene, `rest`,
  escaped quotes) gives the expected values or R3's exact messages including the usage.
- **AC2** `ConsoleCommands.UntypedCountsAndLegacyWords`: an untyped command gets its words unchanged;
  `minArgs`/`maxArgs` violations give `usage`.
- **AC3** `ConsoleCommands.AliasesRegisterAllOrNothing`: a colliding alias registers nothing and names
  the holder; replacing a game command replaces its aliases.
- **AC4** `ConsoleCommands.ResultCarriesLinesLogAndValue`: `call.print`, an Info log line, a warning and
  an error during the call land in `lines`/`log`; the echo is not a line; a C++ `setValue` arrives.
- **AC5** `ConsoleCommands.GameCommandShadowsBuiltInAndUnshadows`: `scene` replaced by a game command
  (one warning), removed -> the built-in runs again; `help` cannot be shadowed. Pins the R7 change
  (today's refusal).
- **AC6** `ConsoleCommands.HelpListsAndDescribes`: `help`, `help give`, `help a*` text and `value`.
- **AC7** `ConsoleCommands.CompletionOfNamesArgsAndCustom`: first word, enum, bool, gameobject (20 cap,
  sorted), custom `complete`, single candidate adds a space, 50 cap.
- **AC8** `ConsoleCommands.CheatFlagAndEvent`: a failing cheat does not set the flag; the first `ok`
  cheat sets it and fires once; `resetCheatsUsed` clears; non-cheat commands never set it.
- **AC9** `ConsoleCommands.GateBlocksSourcesNotGameCode`: with commands disabled, `Overlay`, `Agent` and
  `LaunchOption` sources get `disabled` with R12's text; `Game` runs.
- **AC10** `ConsoleCommands.NestingLimitAndSelfRemoval`: depth 5 fails; a command removing itself
  finishes and is gone after.
- **AC11** `ConsoleCommands.NothingAllocatedUntilFirstUse` (R22) and `ConsoleCommandsBench.Dispatch5Args`,
  `ConsoleCommandsBench.Complete` (R19, R20; `[bench]` lines, enforced under `JASPER_TEST_PERF_SLACK`).
- **AC12** `ConsoleCommands.*` in a build without HTML UI: a new `tools/check-htmlui-off.sh`, following
  `tools/check-navigation-off.sh` (the standalone engine with `-DJASPERENGINE_HTMLUI=OFF` and its core
  tests, filter `ConsoleCommands.*`, failing when fewer than 10 ran), registers and runs commands;
  today the stub returns false. Whether the engine builds with HTML UI off was not checked for this
  spec: if it does not, fixing that is out of scope and AC12 becomes a link test of
  `ConsoleCommands.cpp` without the HTML UI sources (recorded in Deviations).
- **AC13** `engine/tests/core/script/ConsoleCommandsLuaTest.cpp`: `TableFormTypedArgs` (args by name and
  position), `ExecuteReturnsResultTable`, `RunErrorGivesErrorStatusWithTraceback`,
  `OwnerTakesCommandAndAliases`, `GetCommandsEntriesKeepNameAndDescription`.
- **AC14** HTML UI `engine/tests/htmlui/TestDebugOverlay.cpp`: `TEST_F(DebugOverlayTest,
  TabCompletesAndHistoryRecalls)` (Tab, double Tab line, Up/Down 32 cap) and `TEST_F(DebugOverlayTest,
  TouchSuggestionChips)` (chips appear, tap applies); `TEST_F(DebugOverlayTest,
  CommandFieldHiddenWhenCommandsOff)`.
- **AC15** `engine/tests/core/player/LaunchOptionsTest.cpp`: `TEST(LaunchOptions, CommandRepeatsInOrder)`
  and the refusal text in `RefusalsSayHowToFixThem`; `engine/tests/core/player/RunControlTest.cpp`:
  `TEST(RunControl, CommandsRunAfterReadyOnePerFrame)` and `FailedCommandLogsAnError`.
- **AC16** Editor: `editor/tests/reflect/DebugReflectTest.cpp` gains `ConsoleCommandsKeyRoundTrips`
  (`debug.console_commands`, omitted at default; the `ReflectParity` golden gains the key);
  `editor/tests/DebugSettingsTest.cpp` gains `GeneratedInitSetsConsoleCommands`.
- **AC17** MCP `tools/mcp/tests/test_console_commands.py` (`CI = True`, own project; a new file because
  `test_debug_overlay.py` is excluded from CI for its 75-minute export test): `test_command_mode` (a
  script's typed `give` through `eval_lua {command}`: lines, value, cheat; `get_play_state` shows
  `cheats_used`), `test_errors_name_the_fix` (unknown with suggestion, usage, both `code` and `command`,
  not playing, `disabled` after `set_project_settings debug.console_commands = never`),
  `test_help_value` (R26), `test_legacy_command_lines` (an old-style command's `print` output arrives as
  `lines`). `test_debug_overlay.py:155-166` keeps passing unchanged in `MCP_SUITE=all`.
- **AC18** `tool_budget.py --check tools/mcp/budget.json` passes with R28's limit; `gen_tool_docs.py
  --check` passes; `test_tool_conventions.py` passes (no new keyword).
- **AC19** Eval `guard_console_command_mode` (R30) passes on the Orquester-managed accounts and fails
  with the `command` description removed.
- **AC20** Prebuilt player: `tools/mcp/tests/test_console_commands.py::test_player_command_option`
  (prebuilt method, `PREBUILT_PLAYERS = ["glcore-development", "glcore-release"]`, shared runtimes per
  docs/testing.md (c); runs with `JASPER_SLOW_BUILD=1`): the development player run with `--headless
  --after-ready --command "give gold 10" --quit-after 5 --fail-on-error` exits 0 with
  `[command] give gold 10: ok` in its log, `--command "nope"` exits 3, and the Release player exits 2
  with R16's message. Fails today: `--command` is an unknown option (exit 2 for both).

## 8. Dependencies & conflicts

- **Depends on** the debug overlay feature (landed), S31 (`eval_lua`, CLI), S30 (player options and
  `PlayerConfig`), S08 (reflected settings), S24 (Project Settings undo).
- **Siblings.** `project-test-runner.md` and `input-recorder-and-input-scripts.md`: a test or input
  script can call commands through `DebugOverlay.executeCommand` and assert on the result table (no
  dependency either way). `build-and-run-on-device.md`: a later command channel into a device reuses
  its device runner (its D4) and port forwarding, calls `console::execute(line, Source::Agent)` and
  returns `Result` as JSON; this spec defines that contract and no transport.
  `shipped-game-diagnostics.md`: may register a built-in such as `journal` returning `Journal.getText`
  as `value`; nothing here depends on it. `project-test-runner.md`: a game command may start
  `run_tests` in a development build through that spec's Lua API. `build-profiles.md`: Console Commands is a natural
  per-profile value (Development vs Release profile); if that spec lands first, the setting joins its
  table. `savegame.md`: games may store `cheatsUsed` in a slot's `meta`; no coupling.
  `cancellable-prioritised-events.md`: `onCheatUsed` is an ordinary `FunctionSubscribe`.
- **Merge hotspots:** `engine/core/debug/DebugOverlay.{h,cpp}` (registry extraction),
  `engine/core/script/binding/CoreClassesLua.cpp` (DebugOverlay block), `editor/mcp/tools/PlayTools.cpp`
  (`eval_lua`, `get_play_state`), `editor/DebugSettings.*`, `editor/reflect/DebugReflect.cpp`,
  `editor/Generator.cpp`, `engine/core/player/LaunchOptions.*`/`RunControl.*`, `tools/mcp/budget.json`,
  `docs/mcp/guide/debugging.md`, the engine's CMake source list and Xcode sync for the new files.

## 9. Risks & defaults

- **Agents running cheats by accident** in tests that check achievements. Default: the result says
  `cheat: true` and `get_play_state` reports `cheats_used`; nothing blocks.
- **Info log capture picks up unrelated lines** from systems that log during the call (a scene load's
  Info lines in `scene`). Default: accepted; `call:print` lines come first in `lines`, captured lines
  after, each in log order. A command that wants clean output prints explicitly.
- **`gameobject` ambiguity** (two "Enemy" objects). Default: `GameObject.find` semantics (first match)
  plus `#id`; completion lists duplicates once with their path.
- **Release builds with the overlay set to Always** but commands left at Development: the field is
  hidden. Default and documented; a game opts in with `DebugOverlay.commandsEnabled = true`.
- **The `.` in names** could collide with future syntax. Default: allowed; `;` and `|` stay reserved
  for later chaining and are refused in names already.
- **Core listing budget** for `eval_lua`'s new parameter. Default: R28's trade within `eval_lua`.

## Deviations

- **`onCheatUsed` is `console::Events::onCheatUsed`**, a static member of a class:
  `tools/check_event_clears.py` refuses an event at namespace scope (no clear walks it). Engine's
  `clearAllSubscriptions` and `removeSubscriptionsByTag` clear it. The Lua name is as specified.
- **API additions the sketch lacks**: `Completion::start` (where the replacement goes, so the overlay
  and Lua can apply it), `Call::raise` (status `error`, used by the Lua binding), `Call::name`,
  `Call::setScriptValue` and `Result::scriptValue` (Lua gets its own value back), `ArgValue::given`
  (an optional argument left out), `usageOf`, `describe`, `typeName`, `parseType`, `disabledMessage`,
  `endSession` (Play stop) and the test hooks `reset`/`isAllocated`.
- **`args.count` in D3 is the argument named `count`** (the Lua sketch's): `run` gets arguments by name
  and position, no count field.
- **Usage and unknown results are logged as warnings, failed and error ones as errors, by
  `console::execute` itself**: the overlay's colours (R15) are the log levels, and every source sees
  the same text in the log.
- **`lua` while Allow Lua Commands is off** stays hidden from `help`, completion and `getCommands`, but a
  typed `lua` resolves and fails with the setting's name instead of `unknown`.
- **The `--command` refusal of R16/AC15 is not a parse error**: the parser does not know the gate, which
  the game's init (Compile) or `game.json` (prebuilt) sets after it. RunControl refuses at the first
  drawn frame (exit 2); the text is `player::commandsOffRefusal()`, checked in
  `LaunchOptions.RefusalsSayHowToFixThem`, the exit in `RunControlFixture.FailedCommandLogsAnError`
  (the RunControl suite is `RunControlFixture`). `--command ""` is the parse-time refusal.
- **Prebuilt players never configured the overlay**: Console Commands reaches them as `game.json`
  `consoleCommands` (omitted at `development`), applied in `PlayerMain`'s init against the runtime's
  `JASPERENGINE_DEVELOPMENT_BUILD`.
- **AC16**: no `ReflectParity` golden covers `DebugSettings`, so the key is checked in
  `DebugReflect.ConsoleCommandsKeyRoundTrips` only.
- **AC11's "engine start and 100 frames"** is measured in the core test binary, which cannot restart the
  engine: the start's gate setting, then 100 frames of the overlay's per-frame hooks and the session's
  end, with no storage made and no allocation on the thread.
- **`editor/players/ModuleUsage.cpp` is unchanged**: a script naming `DebugOverlay` still keeps the HTML
  UI module in a prebuilt variant (conservative; the registry itself needs none).
- **AC12 is a link check, not a test run**: the core test binary refuses `JASPERENGINE_HTMLUI=OFF`
  (`engine/CMakeLists.txt`: "The tests cover the HTML UI and its video"), so no `ConsoleCommands.*`
  test can run in such a build. As the fallback the spec names, `tools/check-physics-off.sh
  htmlui-off` (the engine library and its sample project with the HTML UI module off) compiles and
  links `ConsoleCommands.cpp`, its Lua bindings and the overlay's no-HTML-UI branch, which forwards
  every command call to the registry; no new script was added.
