# Scripting workflow: hot reload, script status, editor scripts, numeric fields, click-to-source

This page covers how scripts behave while you work: what happens when you save a script during
Play, the script status badge, menus written as editor scripts, maths in number fields, and
jumping from a Console line to the source. The Lua API itself is in
[mcp/guide/lua-api.md](mcp/guide/lua-api.md). C++ scripts are in
[mcp/guide/cpp-scripting.md](mcp/guide/cpp-scripting.md).

## Lua hot reload during Play

Save a `.lua` file while the game plays and its new code runs in the objects that are already
alive. You can save from the built-in code editor, from another editor (the file watcher sees
it), or with MCP `write_file`. Nothing restarts:

- Every live instance of the script keeps its table, its fields (`self.count`, references held
  by other scripts) and its identity. Only the functions are new.
- Functions you registered by name are registered again on the new code
  (`RegisterEngineEvent(self, "onUpdate")`, `RegisterEvent(self, ev, "onHit")`). Callbacks
  that were anonymous functions or timers created by the old code are dropped, because their
  code no longer exists. The Console line says how many.
- `init` does not run again. Define `onReload(info)` to rebuild what `init` derived:

  ```lua
  local Spinner = { properties = { { name = "speed", type = "float", default = 90 } } }

  function Spinner:init()
      self.angle = 0
      RegisterEngineEvent(self, "onUpdate")
  end

  function Spinner:onUpdate()
      self.angle = self.angle + self.speed * Engine.deltatime
  end

  -- runs once per live instance after a reload; info.path is the script file
  function Spinner:onReload(info)
      Log.print("Spinner reloaded from " .. info.path)
  end

  return Spinner
  ```

- `properties`: a property whose type did not change keeps its runtime value. An int that
  became a float (and other simple conversions) is converted. A property whose type changed
  in another way goes back to its default. A new property gets its default, and a removed one
  is cleared.
- Modules loaded with `require` are updated in place, so code that holds the module table sees
  the new functions. Module-local variables (upvalues) start over, so keep state in fields.
- **A broken file never stops the game.** The editor compiles and test-runs the new file in a
  separate Lua state first. If that fails, the old code keeps running and the Console shows
  `file:line: message (kept the old code)`. Fix the file and save again.

The Console logs each reload:
`[Script Reload] Reloaded Counter.lua (1 instance, ...; 0.4 ms, 160 ms after the save)`.

**Preferences > Scripting > Script Changes While Playing** chooses what a save during Play does:

| Setting | What happens |
|---|---|
| Reload Scripts (default) | the swap described above |
| Reload After Stopping Play | nothing until Play stops (MCP `external_changes.deferred` says so) |
| Stop Play and Reload | Play stops and the change is picked up |

## C++ scripts during Play (opt-in)

C++ scripts are rebuilt at every Play. To reload them while the game runs, turn on
**Preferences > Scripting > Reload C++ Scripts During Play** (off by default). When you save a
`.cpp`/`.h` of a C++ script, the script library is rebuilt in the background, the new library
is loaded next to the old one, and each script object is created again from the new code:

- DPROPERTY values keep their runtime values.
- Other state survives only if the script saves it. Override `saveReloadState` and
  `loadReloadState`, and check `ScriptBase::isReloading()` in the constructor to skip one-time
  setup:

  ```cpp
  void Mover::saveReloadState(ScriptReloadState& state) const { state.values["counter"] = counter; }
  void Mover::loadReloadState(const ScriptReloadState& state) {
      if (auto it = state.values.find("counter"); it != state.values.end()) counter = std::get<int>(it->second);
  }
  ```

- A compile error keeps the old library running. The badge shows "failed, using the last good
  build" and the errors link to their lines.
- Old libraries stay loaded until Play stops, because code from them may still be on the stack.
  A Play session keeps at most 16 of them, and the editor tells you when it is full.

### The toolchain fingerprint

A C++ script library must be built with a compiler and settings that match the editor:
standard library, its ABI, debug iterator level, NDEBUG, architecture, pointer size, and engine
version. Each library carries a small fingerprint. The editor reads it from the file
**before loading the library**, so no code from a mismatched library ever runs, and checks it
again after loading. If it does not match, the editor refuses the library and names each
field, for example `Refused script library: iteratorDebugLevel library=2 editor=0 (...)`.
A difference that is safe,
such as another minor compiler version, is only a warning. To downgrade every refusal to a
warning, at your own risk, set the environment variable `JASPER_FINGERPRINT=warn`.

## Script status badge

The toolbar badge to the right of the Play controls shows the scripts' state at a glance:

- **Lua:** ok, reloading, or error (a file kept its old code).
- **C++:** none, stale (a source changed since the last build), building (with progress when
  the build tool reports it), up to date, failed (using the last good build), or refused
  (fingerprint).
- **Editor C++:** the editor scripts' library (see below).

Hover the badge for a one-line summary. Click it for the per-language rows, the first errors
as links, and **Rebuild C++**, **Cancel Build** and **Show in Console**. When a new failure
happens, the Console comes to the front once. MCP `get_script_status` returns the same model, and
`editor_status.scripts` includes it.

## Editor scripts: your own menus, context items and toolbar buttons

Put a Lua file in a folder named `Editor` (any depth, any case) and annotate functions to
create menu items, like Unity's `[MenuItem]`:

```lua
local Tools = {}

---@menu Tools/Level/Raise Selected [priority=10] [shortcut=Ctrl+Shift+R]
function Tools.raiseSelected(ctx)
    for _, id in ipairs(ctx.selection) do
        local got = Editor.call("get_entity", { entity = id, components = { "transform" } })
        local p = got.data.components.transform.position
        Editor.call("set_transform", { entity = id, position = { p[1], p[2] + 1, p[3] } })
    end
end

---@validate raiseSelected
function Tools.canRaise(ctx)
    return #ctx.selection > 0
end

---@toolbar Snap [icon=magnet] [tooltip=Snap the selection to the grid]
function Tools.snap(ctx)
    Editor.log("snapping " .. #ctx.selection)
end

return Tools
```

- The editor finds items by **reading** the file. None of its code runs when the project opens
  or a menu is drawn. The code runs only when you click an item, or when a `---@validate`
  function runs as its menu opens. A validator has a 2 ms budget: one that is slower or fails
  leaves the item enabled and logs a warning.
- Paths follow Unity: `Tools/...` (or any other top-level name) goes in the main menu.
  `GameObject/...` also goes in the Hierarchy context menu, `Assets/...` in the Project
  window's context menu, `CONTEXT/<Component>/...` in that component's Inspector menu, and
  `SceneView/...` in the Scene view's right-click menu. `---@toolbar` makes a button in the
  toolbar's Tools strip. Icons are Font Awesome names.
- `ctx` holds `menuPath`, `selection` (entity ids), `scene`, `assets` (paths) and `component`.
  `Editor.call(tool, args)` runs any MCP tool and returns `{ok, text, data}`. A tool that
  finishes later takes a callback: `Editor.call(tool, args, function(result) ... end)`.
  `Editor.selection()`, `Editor.select(ids)`, `Editor.log(...)` and `print` (to the
  Console) are available too.
- **One click makes one undo step**, named after the item ("Raise Selected"), however many
  tools it called.
- Editor scripts run in their own Lua state with the math classes (Vector3...), not the game's
  API. They are never part of a game: exports leave them out and `require` refuses them. A
  file in an `Editor` folder without annotations is an ordinary runtime script.
- The stubs include `EditorScript.lua`, which gives the language server `Editor` and
  `EditorMenuContext`.

C++ items work the same way in `.cpp` files under an `Editor` folder:

```cpp
#include "script/EditorMenu.h"

JE_MENU_ITEM("Tools/Bake Lights", 100)
static void bakeLights(const jasperengine::EditorMenuContext& ctx) {
    ctx.log("baking");
    ctx.call("set_transform", R"({"entity": 3, "position": [0, 1, 0]})");
}
```

The item appears before anything is built. The first click builds the project's
editor-scripts library, checks its toolchain fingerprint and runs the item. The badge's
"Editor C++" row shows that build.

## Numbers: expressions and units in every numeric field

Any number field in the Inspector, and any ImGui number field in the editor, takes an
expression when you type into it (Ctrl+click or double-click a drag field):

| You type | Result |
|---|---|
| `2+2*3`, `2^3^2` (= 512), `sqrt(2)`, `sin(pi/2)`, `log(e)` | maths (tinyexpr; `^` is right-associative, `log` is natural) |
| `+=10`, `-=1`, `*=2`, `/=4` | relative to the current value |
| `-5` | an absolute value (a plain number is never relative) |
| `25cm`, `1.5ft`, `3in`, `2km` | on distance fields (stored in metres) |
| `90deg`, `pi rad`, `0.25turn` | on angle fields (stored in degrees) |
| `250ms`, `2min` | on time fields (stored in seconds) |
| `L(0, 10)` | a linear ramp over a multi-object selection: 0, 2.5, 5, 7.5, 10 for five objects |
| `R(1, 2)` | a random value per object, from a seed per edit |

With several objects selected, `+=` and the ramps apply to each object from the value it had.
Integer fields round the result. A unit on a field without units (`5cm` on a plain number) is
an error. Errors show as a tooltip at the position they were found, and the field keeps its
value. The field's tooltip lists the accepted suffixes. MCP `eval_expression` uses the
same evaluator.

## Console: jump to the source

Console lines that mention a source location link to it:

- Lua errors and tracebacks (`scripts/Player.lua:12:`, `[string "..."]:3:`), GCC/Clang
  (`file:line:col:`), MSVC (`file(line,col):`), CMake (`CMake Error at file:line`), glslang,
  HTML UI diagnostics, and asset paths.
- Double-click a row to open its first location in the project. The context menu has
  **Open file:line**, **Open in External Editor** and **Copy Location**. The details pane
  lists every location of a message (all the frames of a traceback).
- Build-tree paths (the generated copies the C++ build compiles) map back to your project
  files.

**Preferences > External Tools > External Script Editor** chooses where these open: the
built-in editor (default), VS Code, Visual Studio, Rider, CLion, Xcode, Sublime Text, Zed, the
system default application, or **Custom**. A custom command uses `{file}`, `{line}`,
`{column}` and `{project}`. Each placeholder fills one argument even when the path has spaces,
for example `myedit --goto "{file}:{line}" -p {project}`. MCP `code_editor open` opens the
built-in editor unless you pass `target: "preferred"`, and `read_console` entries carry
`locations` (`file`, `line`, `column`, `in_project`).

## Graph files (for tools that build on them)

`engine/core/graph` is a generic data model for node graphs: a JSON file of nodes, ports,
edges, variables and groups. It keeps keys it does not know, and its saves are deterministic,
so files diff cleanly. On top of the file it has per-kind versions with migrations, validation
codes (`type-mismatch`, `cycle`, `missing-required-input`, ...) and edit operations with
inverses for undo. Graph kinds (a material graph, an animation graph) register their node types
with `GraphRegistry`. MCP `list_graph_kinds`, `get_graph`, `validate_graph` and `edit_graph`
work on any kind. This spec ships no graph editor and no visual scripting.

A kind sets `strictParams` to have a misspelled parameter or node type and an out-of-range value
count as errors, each issue with a `suggestion` ("did you mean 'value'?"), so `edit_graph` refuses
them; other kinds keep them for a newer build. Loading never refuses a file for them. `edit_graph`
returns `added: [{op_index, id}]`, the id of each new node, edge and group, and an `addNode` with
its own id still gets its type's defaults. A kind's `fileExtension` is enough for its files to be
text for `read_file` and `write_file`, and tracked: another program's edit reports its problems in
`external_changes`; `write_file` and `edit_graph` refuse a file whose `kind` is not its extension's.
An open window or a pool reacts to `edit_graph` and undoable graph edits with
`GraphEditCmd::addChangeListener`, called on execute and on undo, and to a file written any other way
through `HotReloadService`, kind `graph`.

## The Lua API as data

One parser (`tools/lua_api`) reads the Lua bindings at build time and writes `lua-api.json`.
The code editor's completion, the lua-language-server stubs, and the generated Markdown
reference (MCP doc `lua-api-reference`) are all produced from it. Each project gets a copy in
`.jasperengine/engine-api/lua-api.json`. MCP serves it as `jasper://lua-api` (one class:
`jasper://lua-api?class=Vector3`). `python3 -m lua_api doctest` (run from `tools/`) checks the
```` ```lua ```` blocks of the docs against it.
