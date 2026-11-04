# S23 — Scripting & extensibility

Wave 2. Depends on S01 (C++20, toolchain floor check), S02 (gtest targets), S03 (console issue
format, refuse lossy saves), S05 (watcher → `onScriptChanged`), S06 (background lane), S08
(`remapByName`, field `Unit`, `propertyRow`, multi-edit, `jasper://reflection`), S09 (owner keys,
`releaseOwner`, log `source`), S24 (`UndoGroupScope`, undo names), S25 (`CommandCatalog`).
Feeds S21 (API JSON, Lua doc test), S30 (fingerprint for the prebuilt player), S31 (CLI exposes
every tool here) and future dataflow tools (graph model).

## 1. Goal

Scripts change while the game keeps running. A saved Lua file is checked first and then swapped
into the live instances, which keep their field values; a broken file leaves the old code
running. The same works for C++ scripts as an opt-in, with a toolchain fingerprint that refuses
an incompatible library *before* any of its code runs, and names the field that differs. Around
that sit five smaller tools: a compile-status badge, editor menus defined by project scripts, a
generic graph data model for dataflow tools, math expressions in every numeric field,
and console click-to-source. One JSON description of the Lua API replaces today's two
disagreeing parsers. Humans get Unity-like behaviour, and agents get one MCP tool or resource for
each feature.

## 2. Scope

**In scope**
1. Lua hot reload during Play (component scripts and `require`d modules); C++ script reload
   during Play (opt-in) through a rebuilt library and state migration.
2. Toolchain fingerprint on every load of a native script library (Play and reload), on every
   editor host and compiler (MSVC, clang-cl, MinGW-GCC, GCC, Clang, AppleClang).
3. Script compile/reload status indicator in the main toolbar.
4. Editor menu items, context-menu items and toolbar buttons declared by project Lua scripts
   (annotations) and C++ files (macro).
5. Generic graph data model: `Graph`/`Node`/`Port`/`Edge`/`Variable`, JSON with passthrough,
   kind/node/port type registry, validation, undoable edit ops. No canvas.
6. Math expressions in every editor numeric field: constants, units, relative ops, Unity's
   `L()`/`R()` multi-edit ramps.
7. Console click-to-source for Lua, C++/CMake, shader, HTML UI and S03 asset formats, opening
   the integrated `CodeEditor` or a configured external IDE.
8. One machine-readable `lua-api.json` from one parser, feeding completion, LuaLS stubs, docs and
   MCP.

**Out of scope**
- C#/CoreCLR, their compile server and `HotReloadManager` code (global do-not-copy list). We take
  ideas only.
- Visual scripting, and their game-logic graph (`GameLogicActionCatalog`, `GameLogicGraphCompiler`):
  do-not-copy list.
- Their 10k-line `Tools/ComponentScanner` header parser (do-not-copy). Generating LuaBridge
  registrations is also out of scope; the JSON describes bindings, it does not create them.
- A graph canvas. S21 builds `editor/widget/GraphCanvas` (S21 D14: no imnodes/imgui-node-editor)
  and graph tools reuse it. Their canvas runs on their retained CSS UI and cannot be ported.
  (Note, 2026-11-04, BF23 F26: it lives in `editor/window/widget/GraphCanvas`, as S21 D14 says.)
  S21's animator runtime stays typed and does not use the model (S21 D1); the model serves
  dataflow graphs (shader, VFX, dialogue, tooling) and any S21 editor data it chooses to store.
- Hot reload inside exported games, a Lua debugger/DAP, a runtime Lua sandbox, packages and plugins.
- HTML UI document scripts: they are reloaded by S17's document path and are never re-run by
  this spec.

## 3. Current state

- **No code reload during Play.** Lua changes are deferred until Stop (`editor/mcp/McpAutoSync.cpp:600-669`
  reports `deferred`). Play clears the module cache (`LuaBinding::clearLoadedProjectModules`,
  `editor/Project.cpp:7572`).
  - Each instance is a fresh table whose metatable `__index` is the module table returned by
    running the cached chunk (`engine/core/script/LuaBinding.cpp:847-946`).
  - Chunks are named with the bare path (`:857`), so errors read `[string "scripts/x.lua"]:12:`.
    Modules are named `lua://x.lua` (`:561,571`).
  - `RegisterEvent`/`RegisterEngineEvent` closures **capture the method function value** as
    upvalue 1 (`:452`). Swapping the module alone would therefore keep the old code running.
- **C++ scripts are built only at Play start.** The sequence is `generator.configure` →
  `checkBuildTools` → `generator.build` → `conector.connect` (`editor/Project.cpp:7486-7528`).
  - `Conector::connect` (`editor/Conector.cpp:63-158`) stages a uniquely named copy, `dlopen`s it
    and resolves `initScripts`, `cleanupScripts`, `initEntityScripts` and `cleanupEntityScripts`.
  - The generated sources create instances with an `if (className == …) new X(scene, entity)`
    chain and bind `DPROPERTY` members (`editor/Generator.cpp:595-766`).
  - `ScriptBase` has no virtuals (`engine/core/script/ScriptBase.h`).
- **The ABI guard is weak.** `Generator::getEditorPluginAbiCheck` (`editor/Generator.cpp:853`)
  runs at CMake-configure time, on Windows only, and checks only MSVC vs MinGW and pointer size.
  The build config is forced to match the editor's (`:2723`), but nothing verifies this at load
  time. A stale library, a Debug/Release CRT mismatch, or another STL update loads and then
  crashes.
- **No compile status.** All the editor knows is `Generator::isBuildInProgress`/`didLastBuildSucceed`.
  `MsBuildProgress` (`editor/util/MsBuildProgress.*`) is used only by the Exporter (`editor/Exporter.cpp:953`).
- **No editor-extension API.** The main menu is a data model (`App::buildMenuModel`,
  `editor/App.cpp:406`, `editor/PlatformMenu.h`). The context menus are hard-coded
  (`HierarchyWindow.cpp:1867,2863`, `ProjectWindow.cpp:1676,1761`).
- **No graph model** anywhere in the engine or editor.
- **Numeric fields** are about 108 raw `ImGui::Drag*/Input*` call sites (56 in `InspectorWindow.cpp`).
  ImGui 1.92.8 parses typed text with `sscanf` in `DataTypeApplyFromText`
  (`libs/imgui/imgui_widgets.cpp:2365`). Its own comment says it "is _not_ a full expression
  evaluator". Ctrl+click input has a 32-byte buffer (`:3754-3779`).
- **Console** rows only select (`editor/window/ConsoleWindow.cpp:561,600`). `LogData` is
  `{type, message, id}`.
  - A compiler-line regex parser already exists, `parseDiagnostic` + `projectFileFor`
    (`editor/mcp/tools/BuildTools.cpp:339,387`), but only MCP build tools use it.
  - `CodeEditor::openFileAt(path, line, column)` exists. There is no external-IDE launcher.
- **Two API parsers.**
  - `generate_api_suggestions.py` (763 lines, regex) writes `engine_api_suggestions.h`, consumed
    by `CustomTextEditor.cpp:8` and `editor/mcp/EngineApiContext.cpp:6`.
  - `generate_lua_stubs.py` (3170 lines, real tokenizer and type resolver) writes `engine_lua_stubs.h`.
  - Both run from `CMakeLists.txt:390-531` over the same `engine/core/script/binding/*.cpp` and
    can disagree. `tools/mcp/tests/test_stubs.py` guards only the stubs.
- **Reusable pieces:** `ProjectChecks::checkLuaScript` / `newIsolatedLuaState` (sandboxed syntax,
  top-level and property check, any thread; `editor/util/ProjectChecks.h:83-110`), `CommandRunner`
  (cancel, streamed lines), and `ScriptEvents`.

## 4. Reference (OpenEngine, MIT; `OE` = `external/OpenEngine`)

| File | Take | Improve |
|---|---|---|
| `OE/Managed/HotReload/HotReloadManager.cs`, `OE/Engine/Source/Scripting/ScriptManager.cpp` (~2208, ~2886) | Ideas only: preload off-thread then swap atomically; keep the old code on failure; one reload in flight, newer changes supersede; `[InitializeOnLoad]`/`[OnScriptsUnload]` hooks | Lua needs no compile server. We keep instance identity (they re-create systems) |
| `OE/Engine/Source/Scripting/ECSABI.cpp:642-688` | Snapshot the old field table, then re-pack by field name; new fields get defaults | Ours migrates by name+kind through S08 `remapByName` (conversions + report), not raw bytes |
| `OE/Engine/Modules/NativeScripting/Include/NativeScripting/NativeScriptingABI.h:55-160` | `GE_ToolchainFingerprint` fields and the `static inline` fill compiled on both sides; refuse naming the field | Add pointer size/arch, `NDEBUG` (Jolt layouts), libstdc++ dual ABI, libc++ ABI version, engine ABI hash. Check an **embedded marker before `dlopen`** (their check runs after static initializers already ran). Hard/soft field classes |
| `OE/Apps/Editor/Source/Scripting/ScriptStatusIndicator.cpp` (116 l.) | States (Building / Loaded / Failed "using last good build"); open the log when a state newly fails; refresh only on change | ImGui badge with a popover, per-language rows, progress, stale state |
| `OE/Managed/Editor.Scripting.ABI/{EditorContextMenuAttribute,EditorToolbarMenuAttribute,EditorContextMenuTarget}.cs`, `OE/Apps/Editor/Source/Scripting/EditorScriptMenuRegistry.cpp` (284 l.) | Path + priority + target mask; snapshot replace with a fingerprint so an identical rescan is a no-op | Static discovery (no code runs to build menus). Unity path prefixes instead of a mask enum. Undo grouping |
| `OE/Engine/Modules/Graph/Include/Graph/{GraphModel,GraphValue,GraphTypeRegistry}.h`, `Source/GraphModel.cpp` (520 l.), `Tests/GraphSerializationTests.cpp` | `Model{Version, KindId, Nodes, Links, Variables, Extensions, Passthrough}`; passthrough at every level; registry with string kind ids | Drop material-specific fields (`LightingModel`, `UnknownSgTags`…). Don't store static ports per node. Add kind versioning + migrations (they have none), node/port type descriptors, validation codes, edit ops with inverses. Values are `nlohmann::ordered_json` (no 600-line `GraphValue` port) |
| `OE/Engine/Modules/NumericExpression/` (68 l. + tests) | The idea and test cases | Vendor tinyexpr from upstream (codeplea/tinyexpr, **zlib**; theirs is a vcpkg port). Hook ImGui once instead of two custom fields. Add units, relative ops, `L`/`R` |
| `OE/Apps/Editor/Source/ScriptEditor/SourceLocation.cpp` (86 l.), `SourceLocationOpener.cpp` (83 l.) | Path-start scan and `:L:C` / `(L,C)` readers; built-in first, then external | Find **all** locations in a message (tracebacks), many languages, resolution against project roots |

Licence: MIT. Code adapted from `SourceLocation.cpp` and the fingerprint fill keeps the OpenEngine
copyright header. Add a `NOTICE.md` entry "OpenEngine — toolchain fingerprint, source-location
parser" and a separate entry "tinyexpr (zlib) — Lewis Van Winkle".

## 5. Requirements

### Functional — Lua hot reload (item 1)
- **R1 Trigger.** During Play, a `.lua` change reaches `ScriptReloadService::notifyChanged(path)`.
  The sources are: the S05 hook `onScriptChanged`, `CodeEditor` save, and MCP `write_file`/
  `create_script`/`sync_files`. The editor's own writes count, because they are real changes.
  - Changes are debounced per path (150 ms) and handled one reload at a time.
  - A newer change to a path supersedes a queued one.
  - The swap runs on the main thread at the frame boundary (before `Engine` update), never inside
    a Lua call.
- **R2 Check first.** On a background-lane job, the new text goes through
  `ProjectChecks::checkLuaScript` in an isolated state (syntax, top level, properties). Then the
  live state compiles it (`luaL_loadbuffer`) and runs it once as a probe; the result must be a table.
  - If any step fails, **every** instance keeps the old code.
  - The error goes to the Console as `path:line: message`.
  - The Lua status becomes Error (R20).
  - Nothing is partially applied.
- **R3 Swap keeps identity.** For each live instance of that path in every runtime scene
  (including instantiated GameObjects), the chunk runs again. The instance's metatable `__index`
  is then pointed at the new module table.
  - The instance table, its registry ref, `__scriptRef` and every Lua reference other scripts
    hold to it stay valid.
  - `scriptChunks` is updated, so later instantiations use the new code.
- **R4 Field migration by name.** Non-function fields stored in the instance table are kept
  unchanged. Script properties are reconciled against the new property list with S08
  `remapByName`:
  - Same name and kind: keeps the **runtime** value.
  - Kind changed: converted, or reset to the default with a report entry.
  - New property: the declared default (an EntityReference is resolved like pass 2 of
    `initializeLuaScripts`).
  - Removed property: set to nil.
  - The play-time `ScriptEntry.properties` are replaced, so the Inspector shows the new set.
- **R5 Callbacks.** Every `RegisterEvent`/`RegisterEngineEvent` call records
  `{event registry ref, methodName, tag, guarded}` per instance. On reload:
  1. The instance's owner is released (S09 `releaseOwner`, which also cancels its timers; until
     S09 lands, `removeScriptSubscriptions` plus exact recorded tags).
  2. Recorded registrations whose method exists in the new module are replayed against it.
  3. Missing methods are warned once.
  4. Anonymous closures and timers created by the old code are dropped. Their count is reported:
     "Player.lua: 2 anonymous callbacks dropped; re-create them in onReload".
- **R6 Hooks.** After the swap, `self:onReload(info)` is called if defined, with
  `info = {path, addedProperties, removedProperties, changedProperties, droppedCallbacks}`.
  - `init` is **not** re-run.
  - An error in `onReload` is logged; the reload still counts as done.
- **R7 Modules.** A changed `require`d module runs once. `package.loaded[name]` is patched **in
  place**:
  - Functions are replaced or added; removed functions become nil.
  - New non-function keys are added; existing ones keep their values.
  - Every `local M = require(...)` therefore sees the new functions.
  - A module that returns a non-table replaces the entry, with a warning that existing holders
    keep the old value.
- **R8 Policy.** The Preferences ▸ Scripting setting "Script Changes While Playing" has three
  values: **Reload And Continue Playing** (default), Reload After Stopping Play (today's
  behaviour), Stop Playing And Reload. It applies to Lua and, when enabled, to C++.
- **R9 Chunk names.** All chunks are named `@<project-relative path>`, so messages read
  `scripts/player.lua:12: …`, which is standard Lua and LuaLS format. Tests that match the old
  `[string "…"]` form are updated; R26 still parses both forms.

### Functional — C++ reload (item 1, opt-in)
- **R10** The Preferences ▸ Scripting setting "Reload C++ Scripts During Play" defaults to
  **off** (experimental in this release). When it is on and the R8 policy is Reload And Continue,
  a `.cpp/.h` change under the script dirs during Play starts a background rebuild after a 1 s
  debounce.
  - The rebuild uses `Generator::build` without `configure`: Play reuses the edit-time scene, so
    regenerating scene factories mid-Play would capture runtime state.
  - If the set of script classes or DPROPERTYs changed (`ScriptParser`), only the init/cleanup
    script sources (`buildInitSceneScriptsSource`/`buildCleanupSceneScriptsSource`) and
    `SCRIPT_SOURCES` are rewritten.
  - The old code keeps running during the build.
- **R11** If the build or the fingerprint check fails, the old library stays and the status shows
  "C++ build failed — using last good build". On success the new copy is staged and checked
  (R14–R16) and loaded **next to** the old one. Then, at the frame boundary, for every C++
  `ScriptEntry` in every runtime scene:
  1. `syncFromMember` on all properties, then `saveReloadState(state)`.
  2. Old library `cleanupEntityScripts` (destructors, owner release).
  3. New library `initEntityScripts`: constructors run with `ScriptBase::isReloading() == true`,
     and properties are bound from the synced values.
  4. `loadReloadState(state)`.
  5. The `ObjectManager` hooks switch to the new library.
- **R12** `ScriptBase` gains `virtual ~ScriptBase()`, `virtual void saveReloadState(ScriptReloadState&) const {}`,
  `virtual void loadReloadState(const ScriptReloadState&) {}` and `static bool isReloading()`.
  `ScriptReloadState` is a name → `ScriptPropertyValue` map plus a `std::string` blob slot.
  Members that are not DPROPERTYs and are not saved by the hook are lost, and this is documented.
- **R13** Superseded library images are **never unmapped during Play**: callbacks or statics may
  still point into them. All generations unload at Stop. A process holds at most 16 generations;
  the 17th reload asks to stop Play.

### Functional — toolchain fingerprint (item 2)
- **R14** A header-only `engine/core/script/ToolchainFingerprint.h` defines
  `struct ToolchainFingerprint` (plain `uint32_t` fields) and `static inline fill(ToolchainFingerprint&)`.
  - Fields: `format`, `pointerBits`, `arch`, `compilerVendor` (MSVC, ClangCl, Clang, AppleClang,
    GCC, MinGW), `compilerMajor`, `compilerMinor`, `stdlib` (MSVC STL, libstdc++, libc++),
    `stdlibVersion` (`_MSVC_STL_UPDATE`/`_GLIBCXX_RELEASE`/`_LIBCPP_VERSION`), `stdlibAbi`
    (`_GLIBCXX_USE_CXX11_ABI`/`_LIBCPP_ABI_VERSION`), `crtDebug` (`_DEBUG`), `crtDll` (`_DLL`),
    `iteratorDebugLevel` (`_ITERATOR_DEBUG_LEVEL` with MSVC's default, `_GLIBCXX_DEBUG`),
    `ndebug`, `exceptions`, `rtti`, `nativeWchar`, `charUnsigned`, `cxxStandard`
    (`_MSVC_LANG`/`__cplusplus`) and `engineAbiHash`.
  - `engineAbiHash` is FNV-1a over the editor version, the git sha and the engine's
    layout-affecting defines: `JASPERENGINE_PHYSICS_2D/3D`, `NO_THREAD_SUPPORT`,
    `DISABLE_LUA_BINDINGS`, `JASPERENGINE_SHARED`.
  - The engine library exports `jasperengine_engineFingerprint()`, filled in an engine translation
    unit. At startup the editor asserts that its own fill equals the engine's.
- **R15** The generated script sources (and the C++ editor-menu library, R24) embed:
  - a marker `JE_TOOLCHAIN_FP_V1{field=value;…}` in a `[[gnu::used]]`/`#pragma comment`-kept
    `constexpr char[]`;
  - an export `extern "C" const ToolchainFingerprint* jasper_scriptToolchainFingerprint()`.

  The editor reads the marker **from the file before `dlopen`/`LoadLibrary`**, then compares the
  export after loading. Both must match.
- **R16 Rules.**
  - **Hard** fields (refuse, unload, keep the old library or fail Play): `format`, `pointerBits`,
    `arch`, the vendor ABI family (MSVC-ABI vs Itanium; MinGW vs MSVC), `stdlib`, `stdlibAbi`,
    `crtDebug`, `crtDll`, `iteratorDebugLevel`, `ndebug`, `exceptions`, `rtti`, `nativeWchar`,
    `charUnsigned`, `engineAbiHash`. MSVC adds one more: a library `stdlibVersion` **newer** than
    the editor's.
  - **Soft** fields (Console warning, then load): compiler major/minor, an older or equal
    `stdlibVersion`, and `cxxStandard`. S01's configure-time floor check (`getToolchainFloorCheck`)
    remains the gate for minimum versions.
  - The message names every differing field, for example: `Refused script library: iteratorDebugLevel
    library=2 editor=0 (Debug STL vs Release editor). Rebuild with the editor's configuration
    (Preferences ▸ External Tools)`.
  - The check runs on every Play connect, not only on reload. `getEditorPluginAbiCheck` stays as
    the early configure-time message.

### Functional — status, menus, graph, expressions, sources, API (items 3–8)
- **R17 Status model.** `ScriptStatus` (main thread, observable) holds two rows.
  - Lua: `Ok`, `Reloading`, `Error(n findings)`.
  - C++: `None` (no C++ scripts), `Stale` (sources newer than the loaded/last build), `Building(fraction|unknown)`,
    `UpToDate`, `Failed(n errors) — using last good build`, `Refused(fingerprint fields)`.
  - Progress: `MsBuildProgress` for Visual Studio generators, `[n/m]` lines for Ninja/Make, and
    "unknown" otherwise.
  - `Generator` publishes state changes through a `ThreadSafeSignal` (S09).
- **R18 Editor scripts (Lua).** A `.lua` file is an *editor script* when it lies under a folder
  named `Editor` (case-insensitive) **and** contains at least one annotation. Existing runtime
  scripts in `Editor/` folders therefore stay runtime scripts. Annotations are placed on module
  functions:
  `---@menu <Path> [priority=N] [shortcut=Ctrl+Shift+K]`, `---@toolbar <Label> [icon=<fa-name>] [tooltip=…]`,
  `---@validate <menuFunction>`.
  - Paths follow Unity:
    - `Tools/…` or any top-level name: the main menu.
    - `GameObject/…`: also the Hierarchy item context menu.
    - `Assets/…`: also the Project context menu.
    - `CONTEXT/<Component>/…`: that component's Inspector header menu.
    - `SceneView/…`: the Scene view context menu.
  - Discovery is a **static text scan**: no code runs to build menus. Editor scripts are excluded
    from exports and from runtime `require`, and the `check` CLI warns if one is attached to a
    GameObject.
- **R19 Editor script execution.** On invoke, the module loads into a dedicated editor `lua_State`
  built like `ProjectChecks::newIsolatedLuaState` (no io/os.execute/debug/loadlib), plus the math
  classes and an `Editor` table:
  - `Editor.call(tool, args[, callback])` runs any MCP tool in-process. A tool that completes
    later requires the callback form.
  - `Editor.selection()`, `Editor.select(ids)`, `Editor.log(...)`.
  - The function receives `ctx = {menuPath, selection = {ids}, scene = id, assets = {paths}, component}`.
  - All edits made by one invocation run inside one S24 `UndoGroupScope`, so they form **one undo
    step** named after the item ("Snap To Grid"). Errors go to the Console with a location.
  - `---@validate` functions run only when a menu containing their item opens, with a 2 ms budget.
    Over budget, the item stays enabled and a warning is logged.
- **R20 Editor scripts (C++).** `JE_MENU_ITEM("Tools/Bake Lights", 100)` placed before a
  `static void fn(const jasperengine::EditorMenuContext&)` in `.cpp` files under `Editor/` is found
  by `ScriptParser`, so the item shows without a build.
  - On first invoke, a separate `<lib>_editor` library is built (status row "C++ editor scripts"),
    fingerprint-checked and loaded. It receives a C ABI host table `JE_EditorHostApi{abiVersion,
    call(tool, jsonArgs, outJson)}`.
  - It is rebuilt and reloaded when its sources change. Edit mode only; never exported.
- **R21 Graph model** (`engine/core/graph/`, namespace `jasperengine::graph`, compiled on every
  platform because a graph kind's runtime compiler may read it in games):
  - `Graph{formatVersion=1, kind, kindVersion, nodes, edges, variables, groups, viewport, metadata, passthrough}`.
  - `Node{id, type, position, size?, params (ordered_json object), dynamicPorts?, passthrough}`.
  - `Port{id, direction, type, name, multi, optional}`, declared by the node type. Ports are stored
    per node only for dynamic-port types.
  - `Edge{id, from{node,port}, to{node,port}, passthrough}`.
  - `Variable{name, type, default, exposed, group, passthrough}`.
  - `Group{id, title, rect, nodeIds}`.
  - Ids are stable strings (`n12`, `e7`) and are never reused within a file.
- **R22 Registry.** `GraphKindDesc{id, displayName, fileExtension, kindVersion, migrations[{toVersion, fn(json&)}], acyclic, validate}`,
  `NodeTypeDesc{id, category, displayName, ports, params[ParamDesc{name, kind, default, min, max, enumValues}], dynamicPorts(params)}`,
  `PortTypeDesc{id, color, implicitTo[]}`. `canConnect(out, in)` is true when the types are equal,
  a declared implicit conversion exists, or one side is `any`.
- **R23 JSON & preservation.**
  - Unknown keys at **every** level are written back verbatim.
  - An unknown node type is kept as an opaque node and reported. It is never deleted, which
    honours S03's no-lossy-save rule.
  - Older `kindVersion` files are migrated on load and written at the current version.
  - Output is deterministic: known keys in fixed order, nodes and edges in id order, LF line
    endings.
  - Asset references inside params use S04 `{guid, path}`.
- **R24 Validation and ops.**
  - `validate(graph) -> [Issue{severity, code, nodeId, portId, edgeId, message}]`. Codes:
    `dangling-edge`, `duplicate-id`, `direction`, `type-mismatch`, `multiplicity`, `cycle` (when
    acyclic), `unknown-node-type`, `missing-required-input`, `param-range`, plus kind-specific ones.
  - `apply(graph, GraphOp) -> GraphOp inverse` for `addNode`, `removeNode` (+edges), `moveNodes`,
    `setParam`, `connect`, `disconnect`, `addVariable`, `removeVariable`, `renameVariable`,
    `setVariable`, `group`, `ungroup`.
  - The editor wraps a batch of ops into one `GraphEditCmd` (S24 name, e.g. "Connect Speed → Blend").
- **R25 Numeric expressions.** `editor/util/NumericExpression.{h,cpp}` wraps vendored
  `libs/tinyexpr` (compiled with `TE_POW_FROM_RIGHT`, `TE_NAT_LOG`).
  - Grammar: tinyexpr operators and functions, plus `min max clamp round lerp`.
  - Constants: `pi tau e`.
  - Units after a number, used only when the field declares an S08 `Unit`:
    - Meters: `mm cm m km in ft`.
    - Degrees: `deg ° rad turn`.
    - Seconds: `ms s min h`.
    - Pixels: `px`.

    A unit on a field without one is an error.
  - Leading relative ops `+= -= *= /=` apply to the current value. `+5` and `-5` are absolute.
  - Integer fields round half away from zero and clamp to the type.
  - Multi-edit (S08 R9): relative ops apply per object; `L(a,b)` ramps linearly over the selection
    order and `R(a,b)` gives a uniform random value per object.
  - An invalid expression leaves the value unchanged and shows a 3 s tooltip at the field, e.g.
    "Unknown name 'pii' at 3".
- **R26 One hook for all fields.** A ≤ 30-line patch to `libs/imgui` adds
  `ImGui::SetDataTypeApplyFromTextHook(fn)`, consulted at the top of `DataTypeApplyFromText`, and
  enlarges the `TempInputScalar` buffer to 256 bytes. The patch is listed in `libs/imgui/JASPER_PATCHES.md`
  and re-applied on upgrades.
  - Every `Drag*`, `Slider*` (Ctrl+click) and `Input*` scalar/N field therefore accepts
    expressions with no call-site change.
  - S08's `propertyRow` pushes a `NumericFieldScope{unit, multiEdit}` for unit and multi-edit
    context.
- **R27 Source locations.** `editor/util/SourceLocation.{h,cpp}` provides
  `findSourceLocations(text) -> vector<SourceLocation{path, line, column, kind, begin, end}>`,
  covering every location in a message.
  - Lua: `p.lua:L:`, `[string "p.lua"]:L:`, `lua://p.lua:L:`, traceback frames `\tp.lua:L: in …`;
    `[C]` frames are skipped.
  - GCC/Clang: `f:L:C:` and `In file included from f:L`.
  - MSVC: `f(L,C):` and `f(L):`.
  - CMake: `CMake Error at f:L (cmd)`.
  - glslang: `ERROR: f:L:`.
  - HTML UI: `f.html:L:C`.
  - S03 asset issues: `f.scene:L:`.

  It replaces `parseDiagnostic`'s location part in `BuildTools.cpp`, so there is one parser.
  - Resolution order: absolute path; then project root, Lua dir, assets dir, build tree through
    `projectFileFor`, and engine sources (read-only).
  - S09 `source{file,line}` wins when present.
- **R28 Opening.** Double-clicking a Console row opens its first in-project location. In the
  details pane, every location is a link. The row context menu offers "Open *file:line*",
  "Open in External Editor" and "Copy Location".
  - Preferences ▸ External Tools ▸ "External Script Editor" options: **Built-in Code Editor**
    (default), Visual Studio Code (`code <project> -g f:L:C`), Visual Studio (via vswhere,
    `devenv /edit f` + `Edit.GoTo L`), Rider (`--line L f`), CLion (`--line L f`), Xcode
    (`xed -l L f`), Sublime Text (`subl f:L:C`), Zed (`zed f:L:C`), System Default, and Custom
    (template with `{file} {line} {column} {project}`).
  - Launches use a new `Platform::launchDetached(argv)`: no shell, argv vector, detached.
- **R29 API description.** `tools/lua_api/` (Python package) holds the **one** binding parser: the
  tokenizer and type resolver moved out of `generate_lua_stubs.py`, with `MANUAL_SYMBOLS` and
  `OVERRIDES` ported.
  - `extract` writes `lua-api.json`:
    `{format:"jasper-lua-api", formatVersion:1, engineVersion, hash, classes[{name, cpp, base, file, line, doc, constructors[], methods[{name, static, overloads[{params[{name,type,optional}], returns[]}], doc, cpp}], properties[{name, type, readonly, static, copy}], cppOnlyMethods[]}], enums[{name, values[{name,value}]}], globals[], events[], unresolved[]}`.
  - Three emitters read only that JSON: `engine_api_suggestions.h` (same header API for
    `CustomTextEditor`/`EngineApiContext`), the LuaLS stubs header and files (byte-identical to
    today's output on the same tree), and a Markdown reference.
  - `generate_api_suggestions.py` is deleted. `generate_lua_stubs.py` becomes a 10-line wrapper
    for one release.
- **R29a Lua doc test** (S21 relies on it). `tools/lua_api/doctest.py` checks every ```` ```lua ````
  block in `docs/mcp/guide/*.md` and `docs/**/lua*.md`:
  - **Statically:** each block compiles, and every `Class.member`/`obj:method` it names exists in
    `lua-api.json`.
  - **At runtime:** blocks fenced as ```` ```lua run ```` are executed in a Play fixture through
    MCP `eval_lua` and must not raise.

  A failure names the doc file, its line and the unknown symbol.

### Platform & backends
- **R30** Everything here is editor-side, except `engine/core/graph` and the Lua reload core
  (`engine/core/script/LuaReload.{h,cpp}`, which works on a `lua_State` and has no editor
  dependency). Both compile on Windows, Linux, macOS, Android, iOS and Emscripten; only the
  editor calls reload.
- **R31** The fingerprint works with MSVC, clang-cl, MinGW-GCC, GCC, Clang and AppleClang, on
  x86_64 and arm64. Exported games link scripts statically and need no check. S30's prebuilt player
  calls the same `checkScriptLibrary(path)` if it loads a script library.
- **R32** External-editor launch works on all three desktop OSes. Detection uses the PATH repaired
  by `ShellEnv` and `CommandRunner::findVswherePath`.

### Performance
- **R33** Lua reload of one script with 200 live instances: ≤ 2 ms + 15 µs per instance on the
  main thread (the check runs on the background lane). From save to new code running: ≤ 300 ms
  at the default S05 quiet window.
- **R34** Fingerprint marker scan: ≤ 20 ms for a 64 MB library (chunked `memmem`). Cost while not
  loading: zero.
- **R35** Expression evaluation: ≤ 10 µs. The hook costs nothing unless text is committed.
- **R36** Graph: load+validate+save of 1,000 nodes / 2,000 edges ≤ 25 ms; `apply` ≤ 5 µs per op.
- **R37** Console: locations are parsed once at insert (no regex, ≤ 2 µs per line) and cached in
  `LogData`. Drawing does not re-parse.
- **R38** Status badge: no per-frame allocation; it reads cached state.
- **R39** `extract` + all emitters: ≤ 15 s on CI. CMake rebuilds only when the bindings, engine
  headers, stub fragments or `tools/lua_api` change.

### Editor UX (human)
- **R40** The main toolbar (`MainToolbar.cpp`, right of the play controls) shows a script badge.
  - Icon and colour: green ok, spinner while building or reloading (with % when known), amber
    stale or soft warning, red failed or refused.
  - Tooltip: one line per language, for example "Lua: 2 errors · C++: build failed — using last
    good build".
  - Click: a popover with per-language rows, the last build time and duration, the first 5 errors
    as click-to-source links, and **Rebuild C++**, **Cancel Build** and **Show in Console**
    (filtered) buttons.
  - The Console opens automatically when a state **newly** turns to failed (OE behaviour).
- **R41** A Console notice per reload: "Reloaded Player.lua (12 instances, 1 property added, 2
  callbacks dropped)". During Play, the Inspector updates its script properties without losing
  selection.
- **R42** Each script menu item is registered as a command `script.<path>` in S25's
  `CommandCatalog`, so it appears in the command palette and can be rebound in the shortcut map.
  It also appears in the main menu (via `PlatformMenuCommand{id=ScriptMenu, payload=itemId}`,
  native menus included) and in the context menus of R18. The same applies to the S23 commands
  `scripts.reload`, `scripts.rebuildCpp` and `scripts.cancelBuild`. Toolbar items go in a "Tools" strip right of the
  layout button.
- **R43** Expression errors use the standard tooltip style. Field tooltips for units list the
  accepted suffixes.

### Agent UX (MCP, Lua, CLI)
- **R44 MCP tools** (registered in `editor/mcp/tools`, documented in `docs/mcp/TOOLS.md` and the
  agent guide, and available through S31's `call` CLI):
  - `reload_scripts {paths?, language?: "lua"|"cpp"|"all"="all", wait=true, timeout_seconds=300}` →
    `{reloaded:[{path, instances, properties:{added,removed,converted}, dropped_callbacks}], kept_old:[{path, findings[]}], cpp?:{state, fingerprint}}`.
    Allowed while playing.
  - `script_status {}` → the R17 model, plus `fingerprint{ok, mismatches[{field, editor, library, severity}]}`.
    The same object is added to `editor_status.scripts`.
  - `list_editor_menus {target?}` → `[{id, path, kind, priority, shortcut, source{file,line}, enabled?}]`.
    `run_editor_menu {id|path, selection?, assets?}` → `{ok, undo_name, log[]}`.
  - `list_graph_kinds`, `list_graph_node_types {kind}`, `get_graph {path}`,
    `validate_graph {path}`, `edit_graph {path, ops[], dry_run=false}` → `{issues[], applied, undo_name}`.
    `edit_graph` refuses an op batch that would add `error` issues unless `allow_invalid:true`.
  - `evaluate_expression {expression, current?, unit?, integer?}` → `{value}` or `{error, position}`.
  - `read_console` entries gain `locations:[{file, line, column, in_project}]`. `code_editor open`
    gains `target:"builtin"|"preferred"="builtin"`.
- **R45** McpAutoSync: during Play, `external_changes` reports `script_reload:{reloaded, kept_old}`
  instead of `deferred` for Lua (and for C++ when it is enabled).
- **R46 Resources:** `jasper://lua-api` (the merged JSON with `components` from `jasper://reflection`;
  `?class=<Name>` returns one class). `search_engine_api` reads that JSON. `doc lua-api-reference`
  serves the generated Markdown. The editor also writes the JSON to
  `<project>/.jasperengine/engine-api/lua-api.json` next to the stubs (`Generator::writeEditorTooling`).
- **R47 Lua:** the `onReload(self, info)` hook; editor scripts' `Editor` table (R19). LuaLS stubs
  for both, generated from the JSON (`Script.lua`, a new `EditorScript.lua`). No runtime Lua API
  for graphs or expressions in this spec.

### Data & compatibility
- **R48** Old projects load unchanged. The new Preferences and Project Settings keys default as
  stated. No scene or asset format change. Graph files are new (`formatVersion` 1).
- **R49** The chunk-name change (R9) changes only message text. `ScriptBase` gaining virtuals
  changes C++ script layout; scripts are always rebuilt at Play, so this is invisible to users.
- **R50** `lua-api.json` follows SemVer on `formatVersion`: additive fields keep 1. Consumers
  ignore unknown keys.

## 6. Design constraints & decisions

- **D1 Identity-preserving Lua swap**, not instance re-creation. It keeps every reference valid at
  zero cost. The price is that module-local upvalues reset, which `onReload` documents.
- **D2 Release-and-replay** of callbacks, not a per-dispatch name lookup. Dispatch stays as fast as
  today, and the approach matches S09's owner model (S09 R26 expects reload to call `releaseOwner`).
- **D3 Never unmap old C++ images during Play** (R13). We do not attempt a quiesce ledger.
- **D4 C++ reload is opt-in** this release. Lua reload is on by default.
- **D5 Fingerprint is checked before load** through an embedded marker, and again after load
  through the export.
- **D6 Static discovery for editor menus.** Opening a project never runs project code; only a
  click (or a validate on menu open) does.
- **D7 The editor-script API is the MCP tool set.** This gives undo, validation and docs for free
  and avoids a second editor binding layer.
- **D8 Graph values are `nlohmann::ordered_json`** (`engine/libs/json`). There is no custom
  variant type. Static ports live in the registry, not in files.
- **D9 Patch ImGui once** (R26) rather than wrapping 108 call sites.
- **D10 One parser:** the tokenizer from `generate_lua_stubs.py` survives; the regex parser goes.

```cpp
// engine/core/script/LuaReload.h
namespace jasperengine::lua_reload {
  struct Registration { int eventRef; std::string method, tag; bool guarded; };
  struct Report { std::string path; int instances = 0; std::vector<std::string> added, removed, converted;
                  int droppedCallbacks = 0; bool keptOld = false; std::string error; int errorLine = 0; };
  Report reloadScript(lua_State* L, const std::string& path, std::string_view code,
                      std::span<ScriptInstanceRef> instances, const std::vector<ScriptProperty>& newProps);
  Report reloadModule(lua_State* L, const std::string& moduleName, std::string_view code);
}
// engine/core/script/ToolchainFingerprint.h
struct ToolchainFingerprint { uint32_t format, pointerBits, arch, compilerVendor, compilerMajor, compilerMinor,
  stdlib, stdlibVersion, stdlibAbi, crtDebug, crtDll, iteratorDebugLevel, ndebug, exceptions, rtti,
  nativeWchar, charUnsigned, cxxStandard, engineAbiHash; };
struct FingerprintMismatch { const char* field; uint32_t editor, library; bool hard; std::string hint; };
std::vector<FingerprintMismatch> compare(const ToolchainFingerprint& editor, const ToolchainFingerprint& lib);
std::optional<ToolchainFingerprint> scanMarker(const std::filesystem::path& library);
// editor/util/NumericExpression.h
struct NumericContext { Unit unit = Unit::None; bool integer = false; int objectIndex = 0, objectCount = 1; uint32_t seed = 0; };
struct NumericResult { bool ok; double value; std::string error; int position; };
NumericResult evaluate(std::string_view text, double current, const NumericContext&);
// engine/core/graph/Graph.h
Graph load(std::string_view json, LoadReport&);  std::string save(const Graph&);
std::vector<Issue> validate(const Graph&);       GraphOp apply(Graph&, const GraphOp&);  // returns inverse
```

New files: `engine/core/script/{LuaReload,ToolchainFingerprint}.*`, `engine/core/graph/*`,
`editor/util/{ScriptReloadService,ScriptStatus,SourceLocation,NumericExpression,ExternalCodeEditor,EditorScripts}.*`,
`editor/window/ScriptStatusBadge.*`, `libs/tinyexpr/`, `tools/lua_api/`.

## 7. Acceptance criteria

- **AC1 Lua field migration.** gtest `LuaReload.KeepsInstanceFieldsAndIdentity`: an instance with
  `self.count = 7` and another table holding it is reloaded with new code. `count` is 7, the
  holder sees the same table, and the new method runs. → R3, R4
- **AC2 Keep old on failure.** gtest `LuaReload.SyntaxErrorKeepsOld`, `…RuntimeErrorAtLoadKeepsOld`,
  `…NonTableKeepsOld`. All instances run the old method, and the report has `keptOld` with line.
  → R2
- **AC3 Callbacks.** gtest `LuaReload.ReplaysNamedRegistrations`: an `onUpdate` registered via
  `RegisterEngineEvent` calls the new method after reload. An anonymous `Engine.onUpdate:add`
  closure is dropped and counted. A removed method warns once. → R5
- **AC4 Properties.** gtest `LuaReload.PropertyRemap`: kept, int→float converted, a new property
  gets its default, a removed property becomes nil. → R4
- **AC5 Modules.** gtest `LuaReload.ModulePatchedInPlace`: a holder of `require("util")` sees the
  new function, and existing data keys keep their values. → R7
- **AC6 End-to-end Lua.** MCP `test_script_reload.py`:
  1. Play a fixture where `Counter.lua` adds 1 per frame.
  2. `write_file` it to add 10 and define `onReload`.
  3. `wait`; `get_runtime_entity` shows the count kept and rising by 10, and `onReload` printed once.
  4. A syntax error → `reload_scripts.kept_old` with `{path,line}`, `script_status.lua.state=="error"`,
     and the count keeps rising by 10.
  5. Fixing it reloads.
  6. The policy "Reload After Stopping Play" → `external_changes.deferred`.

  → R1–R9, R44, R45
- **AC7 Perf.** gtest benchmark `LuaReload.Bench200Instances` ≤ 5 ms on the CI runner, and MCP
  save-to-effect ≤ 300 ms (timestamped log lines). → R33
- **AC8 C++ reload** (build-server MCP `test_cpp_reload.py`, setting on):
  1. Play.
  2. Edit `Mover.cpp` speed logic.
  3. `reload_scripts {language:"cpp"}` → `reloaded` lists the class.
  4. A DPROPERTY value set at runtime survives, a `saveReloadState` counter survives, and
     `isReloading()` was true in the constructor.
  5. A compile error → `kept_old`, old behaviour continues, and the badge state is `failed`.
  6. 3 successive reloads, then Stop: no crash, all copies unloaded.

  → R10–R13
- **AC9 Fingerprint unit.** gtest `ToolchainFingerprint.*`:
  - compare names each differing field;
  - hard vs soft per R16 (including the MSVC newer-STL rule, and GCC minor = soft);
  - `scanMarker` parses a synthetic file with the marker at offset 50 MB within the R34 budget;
  - a missing marker → refused, "library predates fingerprinting, rebuild".

  → R14–R16, R34
- **AC10 Fingerprint e2e.** On the build servers (Linux GCC, macOS AppleClang, Windows MSVC and
  MinGW), MCP `test_cpp_reload.py::test_fingerprint_refused`: the test env
  `JASPER_TEST_FINGERPRINT_OVERRIDE=iteratorDebugLevel=2` makes the generated marker differ, and
  Play fails with a message containing `iteratorDebugLevel`, before any script constructor
  prints. A soft override (`compilerMinor`) plays with a warning. → R15, R16, R31
- **AC11 Status.** MCP `test_script_status.py` sees the states none → stale (touch .cpp) →
  building (fraction or null) → up_to_date, and failed after a bad edit, with errors carrying file
  and line. Manual (each OS): the badge colours and popover buttons match R40, and the Console
  pops once on a new failure. → R17, R40
- **AC12 Editor menus.** MCP `test_editor_menus.py`:
  1. Create `Editor/Tools.lua` with `---@menu Tools/Test/Raise` and `---@menu GameObject/Test/Raise Selected`.
  2. `list_editor_menus` returns both with `source.line`.
  3. `run_editor_menu` moves the selection +1 Y through `Editor.call("set_component_property")`.
  4. One `undo` reverts all of it, with the name "Raise Selected".
  5. A file in `Editor/` without annotations stays a runtime script.
  6. An `export` build lacks `Editor/Tools.lua`.
  7. Opening the project runs no editor-script code (a top-level `print` never appears).

  → R18, R19, R42
- **AC13 C++ menus.** Build-server MCP: a `JE_MENU_ITEM` appears before any build; the first run
  builds `<lib>_editor`, runs and logs. → R20
- **AC14 Graph model.** gtest `Graph.*`:
  - round trip byte-identical;
  - unknown keys on graph, node, edge, port and variable preserved;
  - an unknown node type kept and reported;
  - a `kindVersion` 1→2 migration applied once;
  - every validation code triggered by a fixture;
  - `apply`+inverse restores byte-identical JSON for every op;
  - the 1,000/2,000 benchmark within R36.

  → R21–R24, R36
- **AC15 Graph MCP.** `test_graph.py` with a test kind registered by the editor test build:
  `edit_graph` connect/setParam produces one undo step, `validate_graph` reports a
  `type-mismatch`, `dry_run` writes nothing, and passthrough keys survive an MCP edit. → R44
- **AC16 Expressions.** gtest `NumericExpression.*`: `2+2`, `2^3^2==512`, `log(e)==1`, `90deg`/`pi rad`
  on Degrees fields, `25cm==0.25` on Meters, `5cm` on a None field → error, `+=10`, `*=2`,
  `-5` absolute, integer rounding, `L(0,10)` over 5 objects = 0, 2.5, 5, 7.5, 10, `R` within
  bounds and seeded, and the error position. gtest `ImGuiNumericHook.DragFloatCtrlClick` drives
  `DataTypeApplyFromText` in a headless ImGui context with a 200-char expression. → R25, R26, R35
- **AC17 Source parser.** gtest `SourceLocation.Table`: ≥ 40 real lines (Lua errors and
  tracebacks, both chunk forms, GCC, Clang, MSVC, CMake, glslang, HTML UI, S03 issues) give the
  expected `{path,line,column}` lists. Build-tree paths map to project files. The insert-time
  benchmark is within R37. → R27, R37
- **AC18 Console e2e.** MCP `test_console_locations.py`: a Lua runtime error in Play → `read_console`
  `locations[0]` is the script path and line, `in_project: true`; a C++ compile error → the
  project `.cpp` line; `code_editor open` with that location opens it. Manual (each OS):
  double-clicking the row opens the built-in editor at line and column; with VS Code selected it
  opens VS Code at the line with the project folder; Custom with `{file}:{line}` works with a
  path containing spaces. → R28, R32
- **AC19 API JSON.** `tools/lua_api/tests` (pytest, run in the MCP image):
  - schema-valid;
  - deterministic (two runs, same hash);
  - stubs byte-identical to the last `generate_lua_stubs.py` output on the same commit;
  - the suggestion set a superset of the old regex output (diff recorded in the PR);
  - `unresolved` empty.

  MCP `test_stubs.py` passes unchanged, and `test_lua_api_json.py` reads `jasper://lua-api` and
  `?class=Vector3`. `doctest.py` passes on the repo docs, and fails with file:line on a fixture doc
  that calls `Vector3:nope()`. → R29, R29a, R39, R46
- **AC20 Regression.** `tools/smoke-test.sh` passes, as do all existing MCP Lua/Play tests (the
  message-format updates from R9 are listed in the PR). → R48, R49

## 8. Dependencies & conflicts

- **Needs:** S05 (`onScriptChanged`, ledger), S06 (background lane for checks and builds), S08
  (`remapByName`, `Unit`, `propertyRow`, multi-edit, `jasper://reflection`), S09 (`releaseOwner`,
  owner keys on Lua registrations, log `source`, `ThreadSafeSignal`), S24 (`UndoGroupScope`,
  names), S25 (`CommandCatalog`), S01 (floor check D9), S02 (`jasperengine-core-tests`,
  `jasperengine-editor-tests`). Fallbacks: without S08 `remapByName`, a local same-name+kind rule;
  without S09, the tag fallback in R5.
- **Blocks / feeds:** S21 (API JSON, Lua doc test R29a; S23 graph tools reuse S21's
  `GraphCanvas`), S25 (palette entries and rebindable shortcuts), S30 (`checkScriptLibrary`), S31
  (tools via `call`). S17 coordinates on `onReload` naming and on never re-running document scripts.
- **Merge hotspots:** `engine/core/script/LuaBinding.cpp` (S07, S09); `editor/Generator.cpp`
  (S01, S09, S30, S31); `editor/Conector.cpp`; the `editor/Project.cpp` Play path (S03, S24);
  `editor/window/ConsoleWindow.*` and `editor/Out.*` (S09); `editor/App.cpp:406`
  `buildMenuModel` (S25); `MainToolbar.cpp` (S15, S25); the `HierarchyWindow.cpp` and
  `ProjectWindow.cpp` context menus (S25, S26); `InspectorWindow.cpp` (S08); `libs/imgui` (any
  ImGui upgrade); root `CMakeLists.txt:385-535`;
  `editor/mcp/tools/{ScriptTools,BuildTools,ConsoleTools,DocsTools}.cpp` and `McpAutoSync.cpp`
  (S05, S31); `Exporter.cpp` (editor-script exclusion; S30).

## 9. Risks & defaults

- **Lua semantics surprise** (module locals reset, anonymous callbacks dropped). *Default:* keep
  instance fields, drop and report anonymous callbacks, document `onReload`, and give the Console
  notice exact counts.
- **C++ reload crashes** (dangling pointers into recreated instances, statics). *Default:* opt-in,
  never unmap during Play, a generation cap of 16, and `isReloading()` for constructors with side
  effects. If AC8 is flaky on any OS, ship with the setting hidden on that OS.
- **Fingerprint false refusals** (GCC/Clang minor bumps). *Default:* versions are soft; only
  layout and ABI fields are hard. `JASPER_FINGERPRINT=warn` downgrades hard to warn for support
  cases only (logged loudly).
- **Windows PDB locking** across rebuilds while a debugger is attached. *Default:* the plugin PDB
  name carries the build generation (`<lib>_<n>.pdb`).
- **Editor-script trust.** *Default:* static discovery plus a sandboxed state. Writes happen only
  through MCP tools, which are undoable and project-scoped.
- **ImGui patch drift.** *Default:* the patch is tiny, documented, and covered by AC16's hook test,
  which fails loudly on upgrade.
- **tinyexpr quirks** (power associativity, `log` base). *Default:* the compile flags in R25,
  pinned by tests.
- **Parser port losing regex-only knowledge** (`MANUAL_SYMBOLS`, copy detection). *Default:* AC19's
  superset diff gates deletion of the old script.
- **Open: per-language external editors** (for example Lua in VS Code, C++ in Visual Studio).
  *Default:* one setting; revisit with S25 preferences search.

## Deviations

Recorded by the S23 implementation.

- **S08 (reflection) has not landed.** The numeric unit of a field is a local
  `editor::NumericUnit` (None, Meters, Degrees, Seconds, Pixels) that `InspectorWindow` rows set
  through `RowSettings::unit`: Transform position uses Meters and rotations use Degrees. Lua
  property remapping (R6) matches by name and kind (the script's `type` string), not by S08
  type ids. `jasper://lua-api` has no `components` section merged from `jasper://reflection`,
  because that resource does not exist yet. S08 can add it, since the format is additive (R50).
- **S25 (menus) has not landed.** Editor-script items go into the existing main menu model: a
  top-level path becomes its own menu, and the Assets menu gets a Scripts submenu (Reload Lua,
  Rebuild C++, Cancel Build). The four context menus (Hierarchy, Project, Inspector component,
  Scene view) call `EditorScripts::drawContextItems`. Shortcuts in annotations are only displayed
  next to the item and are not bound, because S25 owns the shortcut map. The Scene view's
  context menu opens on a short right click (no drag), since that view had no context menu.
- **Settings tool.** `set_script_settings` sets the Preferences > Scripting values and the
  External Script Editor (with its custom command). Tests and agents need that without a
  Preferences tool.
- **AC12 tool.** `Editor.call("set_transform")` replaces `set_component_property`, which does
  not exist in this tree. `set_transform` is the undoable transform edit.
- **AC15 test kind.** The `test` graph kind is registered when the editor starts with
  `JASPER_TEST_GRAPH_KIND=1`, not by a separate test build. The MCP harness passes environment
  variables to the editor (`Editor(env=...)`).
- **AC19 stubs.** The stubs are byte-identical to the last `generate_lua_stubs.py` output
  except in two intended places: `Script.lua` gains `ScriptReloadInfo` and `onReload`, and there
  is a new `EditorScript.lua` (R47). `tools/lua_api/tests/fixtures/regex_symbols.txt` holds the
  old regex suggestion set, and the test checks that the new set is a superset. The diff was
  recorded once with the old scripts on this commit (see the PR).
- **R2/R33 debounce.** The 150 ms debounce counts from the file's modification time, not from
  when the watcher reported it, so the watcher's own latency does not add to the 300 ms budget.
- **R10 Windows PDBs.** The plugin's CMake gives each reload generation's MSVC PDB the name
  `<lib>_<n>.pdb` (`.jasperengine/PluginGeneration.cmake`). The file is included on MSVC only, so
  a reload on other platforms does not re-run the configure step.
- **R25 ImGui patch.** Beyond the hook and the 256-byte `TempInputScalar` buffer, `InputScalar`'s
  buffer is also 256 bytes. When the hook is installed, `,` is not turned into a decimal point,
  because expressions need commas for function arguments (`L(0, 10)`). See
  `libs/imgui/JASPER_PATCHES.md`.
- **R46 `search_engine_api`** reads `engine_api_suggestions.h`, which is now generated from
  `lua-api.json`, so it serves the same data without a second load path.
- **R44 names and group (S31 landed during S23).** The S31 naming rule (verb_noun) renames
  `script_status` to the read-only `get_script_status`, and the settings it set move to
  `set_script_settings`. `evaluate_expression` becomes `eval_expression`. All S23 tools are in
  the new tool group `scripting` (hidden from the default listing, callable by name).
  `editor_status.scripts` stays in core.
- **AC20 test updates.** `test_autosync.py::test_play_deferred_then_picked_up` now sets Reload
  After Stopping Play first, because the default policy reloads a Lua script in place during
  Play (R45).
