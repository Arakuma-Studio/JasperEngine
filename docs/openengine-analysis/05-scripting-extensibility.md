# 05 — Scripting & Extensibility: JasperEngine (OURS) vs OpenEngine (THEIRS)

> **Note (2026-11-04, BF23 F28).** This report is historical and stays as written; decisions taken since:
> imgui-node-editor and imnodes in the shader graph recommendation were rejected by
> `docs/specs/S21-animation.md` D14 (our own `editor/window/widget/GraphCanvas`), and "no node-graph
> framework" predates `docs/specs/S23-scripting.md`'s graph model (`engine/core/graph/Graph.h`).

Scope: scripting languages, bindings, hot reload, lifecycle and API, debugging, visual and node graphs, plugins and packages, IDE integration, sandboxing.

Paths:
- THEIRS is relative to `external/OpenEngine/`.
- OURS is relative to the repo root.

Method: everything here was found by reading code only. Nothing was built or run, and THEIRS was treated as untrusted data.

---

## 1. What THEIRS has

### 1.1 C# on CoreCLR (main scripting language), about 24k lines of C# plus about 18k lines of C++ glue

**Hosting**
- `Engine/Source/Scripting/CoreCLRHost.cpp` (1888 lines) uses the standard nethost/hostfxr API: `LoadHostfxr()` around line 1345, `GetRuntimeDelegate()` around line 1489, and `hdt_load_assembly_and_get_function_pointer`.
- The host context is process-wide and is never torn down.
- Target framework is net10.0. The build defines `NETHOST_AVAILABLE` only when nethost is found.
- `Engine/Modules/Scripting/Source/*.cpp` are 2–6 line wrappers that `#include` the files in `Engine/Source/Scripting`.

**Native to managed**
- Calls go to about 50 `[UnmanagedCallersOnly]` entry points in `Managed/CoreBridge/CoreBridge.cs` (2351 lines).
- Script exports are called by token: `QueryExport` / `InvokeByToken` / `InvokeInDomain`.

**Managed to native: three mechanisms, mixed**
1. Versioned C function-pointer structs: `GE_Interface_v1` in `Managed/Common/EngineNativeBinding.cs`, and `GE_ECS_Interface_v2` in `Managed/ECS.ABI/Internal/EcsInterfaceTable.cs`.
2. Generated per-module tables `GE_<Module>_GetInterface`, read by `Managed/Scripting.Bindings/BindingTable.cs`. The header (size, format, 64-bit hash, function count) is checked and a mismatch throws.
3. Plain `[DllImport("GameEngine.Native")]`, used by `Scripting.ABI/MainThread.cs`, `AnimatorApi.cs`, `UI/Ui.cs` and `Platform.ABI/PlatformServices.cs`.

**NativeAOT Player path**
- `Scripting.Runtime/GameSystemRunnerExports.cs` exports `ge_scripts_initialize|tick|shutdown`.
- `Engine/Include/Scripting/NativeAotScriptsLibrary.h`: a shipped game can load `GameEngine.Scripts.native.{dll,so,dylib}` instead of starting CoreCLR.

### 1.2 Binding generation and reflection

**`Tools/ComponentScanner`** (about 10k lines of C++ plus 3.6k lines of tests and fixtures)
- Uses a hand-written C++ lexer and parser (`Lexical.cpp`, `StructParser.cpp`, `FunctionParser.cpp`, `MarkerScanner.cpp`, `EnumScanner.cpp`). It does not use libclang.
- Engine mode emits `ComponentReflection.gen.cpp` (native self-registering reflection) plus `components.d.ts` / `components.json` for the web build.
- `--emit-api` reads `// @ge-script` markers in `*ScriptApi.h` headers (see `Engine/Modules/Input/Include/Input/InputScriptApi.h`). Marker variants are `-handle`, `-event <delivery>`, `-try`, `@ge-context`, and threading rules.
- From those it writes a language-neutral JSON IDL, `Engine/Scripting/engine-api/<Module>.json`. Each entry carries id, C++ name, header, line, doc, underlying type and a format hash.
- It also writes native thunks `<Module>.script.gen.{h,cpp}` (layout `static_assert`s, `try/catch`) and C# `<Module>.g.cs`.
- **Only Input and Physics are generated today:** `Managed/Scripting.Bindings/Generated/{Input,Physics}.g.cs`. The rest of the ABI is hand-written: `ECS.ABI/EcsApi.cs` 1558 lines, `Scripting.ABI` about 4.6k lines, and about 5k lines of `*ABI.cpp`.

**`Managed/SourceGenerators/EntitySystemGenerator`** (about 2.65k lines plus 2.7k lines of xunit tests)
- An `IIncrementalGenerator`.
- `partial struct : IEntitySystem` gets chunk-iteration `__Execute`, `__RegisterSystem` and `__Destroy`.
- `GameSystem` subclasses get registration code.
- `ComponentSchemaPipeline.cs` emits `__ComponentSchemas.g.cs` (field offsets and sizes).
- Reports diagnostics GE0001–GE0026.

**`Managed/InteropGenerator/Generator.cs`** (155 lines) uses the older Roslyn `ISourceGenerator` API and calls itself a "minimal example". It emits hardcoded DllImports plus `[GenerateBinding(id)]` callback registration.

**Reflection sharing**
- C# components send their generated schema through `GE_ECSABI_RegisterBlobComponentWithSchema` (`Engine/Source/Scripting/ECSABI.cpp:565`). They then show up in the inspector, the serializer and the Add Component menu.
- Built-in C++ components are mirrored by hand in C# (`Scripting.ABI/BuiltInComponents.cs`).

### 1.3 Hot reload

**Compile server**
- `Managed/CompileServerHost/Program.cs` (1480 lines) is an out-of-process Roslyn 4.10 compile server.
- It listens on a named pipe or Unix socket, exits after 120 s idle, and exchanges JSON `BuildRequest`/`BuildResponse` messages that carry the assembly and PDB bytes.
- It keeps a cached `GeneratorDriver` and re-parses only the changed syntax trees.
- Client: `Engine/Source/Jobs/CompileServerClient.cpp`, `PipeTransport.cpp`, `Scripting/JsonCompileTransport.cpp`.

**Loading**
- `Managed/HotReload/HotReloadManager.cs` (about 3.1k lines) loads into a collectible `AssemblyLoadContext` (`CollectibleAssemblyContext`, line 3066) with `LoadFromStream(asm, pdb)`, so files are not locked.
- Reload is two-phase: `PreloadAssemblyContext` runs off the main thread, then `SwapPreloadedContext` swaps atomically.
- The old context is unloaded and a `WeakReference` is polled; a context that survives is counted by `GetLeakMetrics`.

**Trigger**
- `Engine/Source/Scripting/ScriptManager.cpp` (3129 lines) watches `.cs`/`.csproj` with FileWatcher.
- One reload is in flight at a time (with a stuck reset), newer changes supersede older ones, and the changed files are passed on for an incremental compile.
- On failure the old assembly stays loaded, the state becomes `ScriptAssemblyState::Failed`, and `Apps/Editor/Source/Scripting/ScriptStatusIndicator.cpp` shows it.

**State preservation**
- Component data lives in native ECS chunks, so it survives a reload.
- If a component's layout changed, `ECSABI.cpp:642-688` calls `World::MigrateComponentLayoutAcrossWorlds`. Same-name fields are re-packed and new fields get zero bytes. This covers 22 field types.
- `GameSystem` instances and static fields are **not** preserved; systems are re-created (`ReconcileAfterDomainSwap`).
- `[InitializeOnLoad]` and `[OnScriptsUnload]` hooks run around a reload.

### 1.4 Script model and API surface

**Two models**
- `abstract class GameSystem` (`Managed/Scripting.ABI/GameSystem.cs`) with `OnCreate`, `OnEnable`, `OnUpdate(float)`, `OnDisable`, `OnDestroy`, `Order` and `Enabled`.
- `partial struct : IEntitySystem`: `Execute(ref T, in U, float, uint)` whose parameters define the query. Supports `[After]`/`[Before]`/`[Without]`.

**Other characteristics**
- No per-entity MonoBehaviour-style scripts. Components are unmanaged `IComponent` structs.
- Structural changes are deferred through `EntityCommands`.
- `MainThread.Post`.
- Generated physics contact/trigger event subscriptions (`Physics.g.cs`), plus UI events.
- No coroutines or async scheduling.

**Editor extension from C#:** `Managed/Editor.Scripting.ABI/{EditorContextMenuAttribute,EditorToolbarMenuAttribute}.cs`, registered by `Apps/Editor/Source/Scripting/EditorScriptMenuRegistry.cpp`.

### 1.5 Native C++ user modules: `Engine/Modules/NativeScripting` (about 6.1k lines)

**Build**
- `UserProjectGenerator` writes a CMake project, which `CMakeInvoker` / `MsvcToolchain` build.
- Build cache pieces: `BuildCacheRecord`, `ToolchainFingerprint`, `EngineBuildIdentity`. Shadow copies are pruned by `PruneStaleShadowCopies`.

**Boundary contract**
- `Include/NativeScripting/NativeScriptingABI.h`: ABI version 2, plus `GE_ToolchainFingerprint` (compiler vendor and version, debug/DLL CRT flags, `_ITERATOR_DEBUG_LEVEL`, `/Zc` / EH / RTTI flags, STL ABI tag).
- The fingerprint is filled by a `static inline` function compiled on both sides and compared field by field.

**Loading**
- `NativeScriptManager::LoadModule` (around line 708) does `dlopen`, then the ABI check, then the fingerprint check, then `Register_v1`. If any check fails it purges everything stamped with that module id and generation.
- The old image is unmapped only after a quiesce check (`UnloadSupersededModuleVersions`).

**Incomplete**
- `Register_v1`/`Unregister_v1` are no-ops and `hostApi.Services = nullptr`.
- The SEH guard (`GuardedUserInvoke.h`) exists on Windows only.

### 1.6 Debugging and IDE integration

**Debugging**
- No DAP, debugger launch, or vsdbg/netcoredbg wiring anywhere.
- PDBs are loaded with the assembly, so stack traces carry line numbers. Attaching an external debugger is possible in principle.
- `Tools/ScriptingDiagnostics/ScriptingDiag.cpp` (171 lines) is an internal routing and performance probe, not a user tool.

**Console click-to-source:** `Apps/Editor/Source/ScriptEditor/SourceLocation.cpp` (86 lines) parses `file.cs:L:C` and `file.cs(L,C)`; `SourceLocationOpener.cpp` (83 lines) opens the file.

**External IDE**
- `Apps/Editor/Source/ExternalScriptEditorLauncher.cpp` (52 lines) calls `Engine/Modules/Platform/Source/Shell.cpp:938`.
- On Windows it detects VS (devenv), VS Code (`--goto`) and Rider (`--line`) from file associations. Other platforms use the OS opener.
- A `.csproj` is generated; no `.sln`.

**Built-in editor:** `Apps/Editor/Source/Panels/ScriptEditorPanel.cpp` (1822 lines). Completion is **not semantic**: `BuildSuggestions` (around line 1514) prefix-matches keywords, common types and words scraped from the file.

### 1.7 Graph framework (about 37k lines in total across model, editor and compilers)

**Model: `Engine/Modules/Graph` (about 4.5k lines plus 2.1k lines of tests)**
- `GraphModel.h`: `Model{Version=2, KindId, Nodes, Links, Variables, Extensions, Passthrough}`, with string-typed ports.
- `GraphValue.h`: a variant type.
- `GraphTypeRegistry`.
- Serialized as nlohmann JSON. Unknown keys go into `Passthrough` and survive a load and save. There is no version migration.

**Graph kinds** (`GraphPanelFactory.cpp`)
- **Material**: real and complete.
  - Compiler: `Engine/Modules/Rendering/Source/ShaderGraph/SgGraphCompiler.cpp` (773 lines) compiles the graph to GLSL `EvaluateSurface()` / `ModifyVertex()`.
  - Nodes: 79 nodes defined as annotated GLSL functions (`// @sgnode`, `@param`, `@out`, `@pure`) in `Engine/Modules/Rendering/Shaders/Graph/Nodes/*`, reflected by `SgNodeReflector.cpp` / `SgTagParser.cpp`.
  - Type coercion: `SgTypes.cpp`.
  - Origin: a port of Godot 4.6.3 VisualShader semantics (MIT; see `Engine/Modules/Graph/ThirdParty/visual-shader-upstream/UPSTREAM.md`).
  - Previews: per-node previews drawn into a shared atlas (`GraphPreviewAtlas.cpp`).
- **Animation**: real.
  - `Engine/Source/Engine/Rendering/Systems/AnimationGraphCompileFromModel.cpp` (653 lines) converts the model into a runtime `AnimGraphNode` tree.
  - Node types: state machines, 1D/2D blend spaces, IK and others.
- **Game logic**: a prototype.
  - `GameLogicActionCatalog.cpp` defines about 71 nodes.
  - `GameLogicGraphCompiler::CompileToCpp` is called **only from `Engine/Modules/Graph/Tests/MaterialGraphCompilerTests.cpp`**, and nothing implements `GameLogicRuntimeContext`.
  - `GraphFsmRuntime` is used only for the editor play-mode preview in `GameLogicGraphPanel`.

**Editor canvas**
- `Apps/Editor/Source/Graph/GraphCanvas.cpp` (4.9k lines) and `Panels/GraphPanel.cpp` (5.6k lines) are a **custom canvas on their own CSS/yoga retained UI**, with no ImGui and no node-editor library.
- Features: undo, copy/paste, palette search, marquee selection, wire routing (`GraphRouteSolver.cpp`), and per-node diagnostics.
- Missing: minimap and auto-layout. Comment/group nodes are stubs.

**NumericExpression:** `Engine/Modules/NumericExpression` (68-line .cpp) wraps tinyexpr (zlib) so numeric fields accept "2+2" or "sin(pi/2)". It is used only by `UI/Controls/FloatField.h` and `IntField.h`.

### 1.8 Packages and plugins (about 4.6k lines in `Engine/Source/Assets/Packages`, plus the panel and controller)

**Package manifest** (`Engine/Include/Assets/Packages/PackageManifest.h`)
- An npm-compatible `package.json`: name (npm rules, frozen because asset GUIDs derive from it), semver version, displayName, `dependencies` (semver ranges), `assets`, `modules[]` (`kind` Runtime/Editor, `lang` Cpp/CSharp, `root`, `prebuilt`), `runtimeAssets`, `platforms`, `enabledByDefault`.
- Retired keys are rejected by name.

**Versioning:** `PackageVersion.{h,cpp}` (214 lines, standalone) supports exact, `^`, `~`, `>=` and `*` ranges and partial versions.

**Resolver** (`PackageResolver.{h,cpp}`, 669 lines)
- Reads `<project>/Packages/manifest.json` and supports `file:`, embedded, git and engine sources.
- Topo-sorts by dependency and assigns mount priorities in the 30–69 band (dependents above their dependencies; the project is 100).
- Resolution is degraded rather than all-or-nothing: a failing package and its dependents are skipped with one loud error each.
- Derives one compile define per package (`GE_PACKAGE_<ALIAS>`).
- Git packages are pinned in `packages-lock.json` (`PackagesLockFile.cpp`, `PackageGitFetcher.cpp`).
- Supporting files: `PackagePublish.cpp`, `PackagesIndex.cpp`, `PackageNativeCache.cpp`.

**Loading** (`Apps/Editor/Source/Packages/PackageManagerController.cpp`, 380 lines)
- Mounts the package asset roots.
- Hands C# modules to `ScriptManager` (topo-ordered compile followed by one ALC swap).
- Builds and loads C++ modules through NativeScripting.
- UI: `Panels/PackageManagerPanel.cpp` (639 lines).

**Plugin interfaces**
- `Engine/Modules/PluginAPI/Include/PluginAPI/EnginePlugin.h`: `IEnginePlugin` with `RegisterEngineComponents`, `RegisterSceneSchemas`, `RegisterRenderPipelineNodes`, `AddSystemsToSchedule`, `OnRuntimeInitialized` and `OnRuntimeShutdown`.
- `EnginePluginRegistry` has:
  - same-id replace-forward,
  - late-registration replay for DLLs loaded after startup,
  - a registration hold so nothing runs under the OS loader lock,
  - `PurgeModulePlugins(moduleId, generation)`,
  - a quiesce ledger.
- `Apps/Editor/Include/Editor/Registries/EditorPluginRegistry.h`: `IEditorPlugin` with `RegisterInspectors`, hierarchy context-menu and command hooks, and selection-mask contribution.
- Plugins register through a static initializer (`Packages/eztree/Native/EZTreeModuleInit.cpp`).

**Shipped packages** (`Packages/`)
- Assets-only post effects: chromatic-aberration, fidelityfx-dof, film-simulation.
- Runtime plus editor C++: eztree.
- Editor-only C++: git-vcs, svn-vcs, lore-vcs, diversion-vcs, ai-assistant (about 19k lines).
- unity-import, which ships a **prebuilt `UnityConverter.dll`** binary.

### 1.9 Sandboxing

- None. User C#, C++ and package code runs in-process with full trust; `AllowUnsafe` can be set per project.
- Errors are contained in these ways:
  - Native ABI functions catch everything and return a `GE_Result` code, which C# turns into `EngineCallException`.
  - Every `UnmanagedCallersOnly` entry point wraps its body in try/catch.
  - Native user code gets an SEH guard on Windows only.

### 1.10 Tests

- NUnit: `Managed/InteropTests` (48 files, 5.8k lines) covers the ABI handshake, ALC leak detection, preload/swap, and `InitializeOnLoad`.
- xunit for the source generators.
- GoogleTest: `Tests/NativeScripting` (33 files, about 10.3k lines) plus hot-reload stress and benchmark tests.

---

## 2. What OURS has

### 2.1 Lua 5.x via LuaBridge3: hand-written bindings

- The binding files are in `engine/core/script/binding/*.cpp` (12 files, 8617 lines):

  | File | Lines |
  |---|---|
  | `CoreClassesLua.cpp` | 1729 |
  | `GameObjectClassesLua.cpp` | 1799 |
  | `HtmlUIClassesLua.cpp` | 1551 |
  | `HtmlUIAnimeLua.cpp` | 1343 |
  | `ECSClassesLua.cpp` | 743 |
  | `MathClassesLua.cpp` | 707 |
  | `ActionClassesLua.cpp`, `ThreadClassesLua.cpp`, `IOClassesLua.cpp`, `UtilClassesLua.cpp`, and others | the rest |

  They are registered from `engine/core/script/LuaBinding.cpp::registerClasses` (1160 lines in that file).
- There is one Lua state, bound to the main thread. `LuaBinding::setThreadCheck` reports or aborts on use from another thread (env `JASPER_LUA_THREAD_CHECK=abort`); see `LuaBinding.h`.
- The module loader replaces the default searchers with a project-asset loader (`LuaBinding.cpp:360`).
- Compiled script chunks are cached per path (`scriptChunks`).

**Lifecycle**
- `initializeLuaScripts` runs in three passes: create instances, resolve EntityReference properties, call `init`.
- `onDestroy` is called on cleanup.
- Everything else goes through `RegisterEvent` / `RegisterEngineEvent` subscriptions with per-script tags (`luaRegisterEventImpl`). Examples: `onUpdate`, `onPostUpdate`, `onStart`, `onPause`, `onResume`, `onShutdown`, `onSceneLoaded`, physics, UI and input events.
- `isActiveAndEnabled` gating mirrors Unity.
- `print` is routed to `Log::print`, so it appears in the Console.
- Errors use `pcallWithTraceback`.

**Inspector-exposed properties:** `engine/core/script/ScriptProperty.h`
- Types: Bool, Int, Float, String, Vector2/3/4, Color3/4, EntityReference, Prefab.
- C++ scripts declare them with the `DPROPERTY(...)` macro.

**HTML UI documents** can run `<script type="text/lua">`, started by `UIDocumentSystem::startDocumentScripts`.

### 2.2 C++ scripting: whole-project shared library, rebuilt on Play

- Scripts derive from `ScriptBase` (`engine/core/script/ScriptBase.h`). Events use `REGISTER_EVENT` / `REGISTER_ENGINE_EVENT` (`engine/core/util/FunctionSubscribe.h:32,47`).
- `editor/util/ScriptParser.cpp` (630 lines) scans headers for `DPROPERTY` members, class names and `ScriptBase` inheritance.
- `editor/Generator.cpp` (2808 lines) writes the project's CMake and generated sources.
  - `buildInitSceneScriptsSource` (line 595) emits `createCppScripts` (an `if (className == "...") new X(scene, entity)` chain) and `bindCppScriptProperties` (wires members and resolves entity references).
- On Play, `editor/Project.cpp` around lines 7480–7530 does:
  1. `generator.configure`
  2. `checkBuildTools`
  3. `generator.build` (CMake with `JASPERENGINE_EDITOR_PLUGIN=ON`)
  4. `conector.connect`
- `editor/Conector.cpp` copies the built `.so`/`.dll` to a uniquely named file in `build/hotreload/` (or temp) so the loader can't hand back stale code.
  - It resolves `initScripts`, `cleanupScripts`, `initEntityScripts` and `cleanupEntityScripts`.
  - `init`/`cleanup` are wrapped in try/catch.
- ABI guard: `Generator::getEditorPluginAbiCheck()` (line 853), Windows only, at CMake configure time. It checks MSVC vs MinGW and pointer size, nothing else.
- Build plumbing:
  - `editor/util/CommandRunner.cpp` (527 lines): process runner with cancel.
  - `MsBuildProgress.cpp` (358 lines): MSBuild progress.
  - `ShellEnv.cpp` (410 lines): PATH repair for GUI launches on macOS and Linux.
  - CMake kit detection: `Generator::detectAvailableKits`.

**Hot reload status**
- No reload of Lua or C++ during Play. Code changes take effect on the next Play: C++ rebuilds the library, and Lua is re-read after `LuaBinding::clearLoadedProjectModules()`.
- Hot reload exists only for HTML UI documents, styles, effects and fonts (`editor/util/HtmlUIWatcher.*`, `engine/core/subsystem/UIDocumentSystemReload.cpp`).

### 2.3 Editor tooling and IDE integration (stronger than THEIRS)

**Built-in code editor**
- `editor/window/CodeEditor.cpp` (1828 lines) on `widget/CustomTextEditor.cpp` (4170 lines).
- Features:
  - Lua, C++ and HTML/CSS lexers.
  - External-change detection with a conflict prompt.
  - Drag an entity into the editor to insert a typed property.
  - Header/source companion files.
  - Shader invalidation on save.
  - Background project-symbol parsing for Lua and C++.

**Completion**
- `widget/SemanticSuggestions.cpp` (478 lines) is **type-aware member completion**: after `.`, `:`, `->` and `::`, it uses the target type, inheritance, enclosing type and the namespace.
- Engine symbols come from `generate_api_suggestions.py` (763 lines, regex over the LuaBridge binding files), which produces `engine_api_suggestions.h`.

**Event-handler insertion:** `editor/util/ScriptEvents.cpp` (1326 lines) is a catalog of events per source (Engine, Input, UI, Button, Physics2D/3D, and others). It scans whether a Lua or C++ script already handles each event and can insert the handler stub and registration.

**Lua language-server stubs**
- `generate_lua_stubs.py` (3170 lines) has a real C++ tokenizer.
- It resolves the C++ types of every LuaBridge binding into LuaLS/EmmyLua stubs, one file per class and enum.
- It also writes a LuaLS plugin that checks writes to static fields.
- Unresolved types become `any`, and `tools/mcp/tests/test_stubs.py` checks that the unresolved list stays empty.

**`Generator::writeEditorTooling`** writes:
- `.luarc.json`, merged into any existing file.
- Per-script `---@class` stubs (`writeLuaScriptStubs`), obtained by running each script's top level in an isolated state.
- `compile_commands.json` plus `.clangd` for C++ scripts.
- `.vscode/settings.json`.
- `AGENTS.md` / `CLAUDE.md` for AI agents.

**`check` CLI:** `editor/cli/CommandLine.cpp:1650` with `editor/util/ProjectChecks.cpp`. It runs Lua syntax and property checks in a **sandboxed isolated `lua_State`**:
- memory-limited allocator,
- stubbed engine globals,
- no `dofile`/`loadfile`/`loadlib`,
- a reduced `os`,
- text-only `load`.

**MCP server for agents:** `editor/mcp`, with buffer sync through `CodeEditor::syncWithDisk` / `openFileAt`.

### 2.4 What OURS does not have

- **Runtime:** no sandbox for the runtime state (`luaL_openlibs` opens io, os and debug; `LuaBinding.cpp:634`), no coroutine or wait scheduler beyond raw Lua coroutines, and no debugger.
- **Editor:** no Console click-to-source (`editor/window/ConsoleWindow.cpp` only selects the row), no external-IDE launcher, and no editor-extension API (menus, inspectors) for user code.
- **Graphs:** no node-graph framework (no imnodes or imgui-node-editor in `libs/` or `engine/libs/`) and no shader graph.
- **Distribution:** no package or plugin system.

---

## 3. Gaps: THEIRS has, OURS lacks

| Feature | Their location | Value to us | Effort |
|---|---|---|---|
| Package system (npm-style `package.json`, semver, deps, topo mount priorities, file/git sources, lock file, per-package define, Package Manager panel) | `Engine/Source/Assets/Packages/*`, `Apps/Editor/Source/Packages/PackageManagerController.cpp`, `Apps/Editor/Source/Panels/PackageManagerPanel.cpp` | H | L |
| Engine and editor plugin interfaces with late-registration replay and module/generation purge | `Engine/Modules/PluginAPI/*`, `Apps/Editor/Include/Editor/Registries/EditorPluginRegistry.h` | H (only with packages) | M |
| Live code hot reload during Play, keeping component data (layout migration by field name) | `Managed/HotReload/HotReloadManager.cs`, `Engine/Source/Scripting/ScriptManager.cpp`, `ECSABI.cpp:642-688` | H (as an idea for Lua; their code is C#) | M for Lua, XL for C++ |
| Toolchain fingerprint check on native module load (CRT, iterator debug level, `/Zc`, STL tag) | `Engine/Modules/NativeScripting/Include/NativeScripting/NativeScriptingABI.h` | M–H | S |
| Console log line opens the source at the line | `Apps/Editor/Source/ScriptEditor/SourceLocation.cpp`, `SourceLocationOpener.cpp` | H | S |
| External IDE launcher (VS / VS Code / Rider with file:line) | `Apps/Editor/Source/ExternalScriptEditorLauncher.cpp`, `Engine/Modules/Platform/Source/Shell.cpp:938` | M | S |
| Shader graph (GLSL node library with `@sgnode` annotations, reflector, compiler, type coercion, per-node previews) | `Engine/Modules/Rendering/Source/ShaderGraph/*`, `Engine/Modules/Rendering/Shaders/Graph/Nodes/*`, `Apps/Editor/Source/ShaderGraph/*` | M | XL |
| Generic graph data model (Model/Port/Edge/Variable, JSON with passthrough, type registry) | `Engine/Modules/Graph/Include/Graph/{GraphModel,GraphValue,GraphTypeRegistry}.h` | M (prerequisite for any graph tool) | S–M |
| Animation graph (state machine, blend spaces, IK) compiled from the graph model | `Engine/Source/Engine/Rendering/Systems/AnimationGraphCompileFromModel.cpp` | M (belongs to the animation domain) | L |
| Header-marker driven binding generation with a JSON IDL (`@ge-script` to `engine-api/<Module>.json`) | `Tools/ComponentScanner/*ApiScanner*`, `Engine/Scripting/engine-api/*.json` | M | L |
| Editor extension from user scripts (context and toolbar menu attributes) | `Managed/Editor.Scripting.ABI/*.cs`, `Apps/Editor/Source/Scripting/EditorScriptMenuRegistry.cpp` | M | M |
| Script assembly status indicator (compiling / failed / ok) | `Apps/Editor/Source/Scripting/ScriptStatusIndicator.cpp` | L–M | S |
| Math expressions in numeric fields ("2*pi") | `Engine/Modules/NumericExpression` (tinyexpr) | L–M | S |
| Out-of-process incremental compile server | `Managed/CompileServerHost/Program.cs` | L (C# only) | n/a |
| Data-oriented ECS systems in script (`IEntitySystem` query codegen) | `Managed/SourceGenerators/EntitySystemGenerator` | L (does not fit the Lua per-object model) | XL |
| Interop and hot-reload test suites | `Managed/InteropTests`, `Tests/NativeScripting` | M (as a pattern: we lack C++-plugin load and reload tests) | M |

Neither engine has a debugger or DAP, or a working gameplay visual-scripting runtime. THEIRS has the gameplay graph editor UI only.

---

## 4. Things THEIRS does better, and what OURS does better

### THEIRS is better at

1. **Reloading without leaving Play.**
   - Their path: file watch, then the Roslyn server, then in-memory assembly and PDB, then preload off-thread, then atomic swap.
   - On failure the old code keeps running.
   - Component data stays in native chunks and migrates by field name.
   - Ours: every code change means Stop, Play, and a full CMake build for C++.
2. **Native module safety.**
   - Their `GE_ToolchainFingerprint` is compared field by field at load and names the field that differs.
   - Registrations carry the module id and generation, so a failed handshake purges them (`PurgeModulePlugins`).
   - The old image stays mapped until nothing references it (quiesce).
   - Ours: a configure-time CMake check on Windows only (MSVC vs MinGW, pointer size). A Debug/Release CRT mismatch or a different MSVC STL update would load and then crash.
3. **Extensibility architecture.**
   - Packages with manifests, semver, dependency ordering, asset-mount shadowing, lock files, enable/disable, and per-package compile defines.
   - Plugin interfaces for both engine and editor.
   - Ours has none of this; the only extension point is a project's own scripts.
4. **One description of the API for every binding.**
   - `engine-api/<Module>.json` is a single machine-readable description, with a format hash checked at runtime by `BindingTable.cs`.
   - Ours parses the LuaBridge `.cpp` files **twice** with different parsers: `generate_api_suggestions.py` uses regex, `generate_lua_stubs.py` uses a tokenizer. They can disagree.
5. **Shader graph.** A shipped node library where each node is an annotated GLSL function, a compiler with node-attributed diagnostics, and per-node previews. Ours has hand-written shader forks only.
6. **Tests for the scripting boundary.** About 16k lines of interop, native-scripting and hot-reload tests, including ALC leak detection. Ours tests Lua through MCP Python tests (`tools/mcp/tests/test_lua_*`, `test_stubs.py`), but not the C++ plugin load path.

### OURS is better at

1. **Completion and IDE support.**
   - Ours: type-aware member completion (`SemanticSuggestions`), typed LuaLS stubs for the whole API with a static-field plugin, per-script `---@class` stubs, and clangd `compile_commands.json`.
   - Theirs: the built-in editor only prefix-matches words and keywords. They do get good IntelliSense in VS or Rider through the generated `.csproj`.
2. **Unity-style workflow.** Per-GameObject scripts with inspector properties (`DPROPERTY` / Lua properties, entity and prefab references) and event-handler insertion (`ScriptEvents`). Theirs has no per-entity script objects, only systems and components.
3. **A sandboxed check pass.** An isolated, memory-limited Lua state reads script properties and lints without running engine code (`editor/util/ProjectChecks.cpp:320-360`). THEIRS has no equivalent sandbox.
4. **Simplicity and footprint.**
   - Lua plus LuaBridge is a few hundred KB, runs on Web, Android and iOS, and needs no .NET SDK.
   - Theirs needs net10.0, Roslyn and hostfxr. NativeAOT is a separate path for Players, and the license status of some NuGet packages is still open (`Managed/THIRD_PARTY_NOTICES.md`).
5. **AI and agent tooling.** Per-project `AGENTS.md`/`CLAUDE.md`, the MCP server, and buffer sync. THEIRS has an ai-assistant package plus `mcp/`, so this is roughly even, but ours is integrated into the code-editor buffers.
6. **Honest scope.** THEIRS ships unfinished pieces that look done:
   - the game-logic graph's code generator is reached only from tests,
   - `Register_v1` is a no-op,
   - `InteropGenerator` is a hardcoded stub,
   - only 2 of the API modules are generated.

---

## 5. Copy and port candidates (ranked by value / effort)

All OpenEngine code is MIT, "Copyright (c) 2026 Gamecrafters Guild Limited and contributors" (`LICENSE`). Keep the notice, for example by adding a section to `NOTICE.md` and keeping per-file headers.

1. **Console click-to-source.** Value H, effort S.
   - Source: `Apps/Editor/Source/ScriptEditor/SourceLocation.cpp` (86 lines) and `SourceLocationOpener.cpp` (83 lines).
   - Dependencies: none (std only).
   - Adaptation:
     - Generalize `kCSharpExtension` to `.lua`, `.cpp` and `.h`.
     - Add the Lua traceback form `chunk:LINE:` and the `lua://` prefix.
     - Handle GCC/Clang `file:L:C:` and MSVC `file(L,C)` (already handled).
     - Hook into `editor/window/ConsoleWindow.cpp` around line 600 (row click) and call `CodeEditor::openFileAt(path, line, col)`.
   - Trivial.
2. **Toolchain fingerprint on C++ script library load.** Value M–H, effort S.
   - Source: the `GE_ToolchainFingerprint` struct and `GE_FillToolchainFingerprint` from `Engine/Modules/NativeScripting/Include/NativeScripting/NativeScriptingABI.h` (lines 55 to about 140). It is header-only.
   - Adaptation:
     1. Emit an `extern "C" PROJECT_API` fingerprint export into the generated sources in `Generator::buildInitSceneScriptsSource`.
     2. Compare it in `editor/Conector.cpp::connect` before `initScripts`, and refuse with the name of the field that differs.
     3. Add an engine build-identity or ABI integer, for example a hash of `EditorVersion.h` plus the git sha, to catch a stale library against a newer editor.
3. **Package-system core.** Value H, effort L overall; the pieces below are S–M each.
   - `Engine/Source/Assets/Packages/PackageVersion.cpp` + `.h` (214 lines): **std-only, drop-in.**
   - `PackageManifest.cpp` + `.h` (458 lines): needs nlohmann json (we have `engine/libs/json/json.hpp`); replace `AssetCore/SharedFileRead.h` with `std::ifstream`.
   - `PackagesLockFile.cpp` (173 lines): same dependencies.
   - `PackageResolver.cpp` (669 lines): depends on Logger, `PathNormalization`, `Application`, and the git fetcher and source. Port the topo sort, priority bands, degraded resolution and the `SanitizePackageAlias`/`PackageDefine` rules; stub git sources at first.
   - `PackageGitFetcher.cpp` (326 lines): depends on `CancellableShellProcess`; map it to our `editor/util/CommandRunner`.
   - Our integration points:
     - Asset mounts: `editor/util/AssetDatabase` / `Project::getScriptDirs`.
     - Lua: add package `lua/` roots to the project searcher in `LuaBinding::configureProjectLuaLoader`.
     - C++: add the package sources and `JASPERENGINE_PACKAGE_<X>` defines in `Generator::writeSourceFiles`.
   - Copy the design of `IEnginePlugin`/`IEditorPlugin` (`EnginePlugin.h`, `EditorPluginRegistry.h`, about 400 lines) for the hook set, not the code. Their hooks name their ECS scheduler and render graph; ours would be `Engine::registerSystem`, Inspector drawers, and ImGui menus.
4. **Lua hot reload during Play.** Value H, effort M. This is an idea, not a code port.
   - Their pattern:
     - debounced file-watch trigger,
     - compile or load the new code first, keep the old on failure,
     - swap,
     - migrate state by field name,
     - re-run `[InitializeOnLoad]`-style hooks.
   - For us:
     1. On a `.lua` change during Play, compile the chunk with `luaL_loadbuffer` (on error, log it and keep the old code).
     2. For each live `ScriptEntry` of that class, build the new instance table and copy the old instance's non-function fields by name.
     3. Re-bind `RegisterEvent` subscriptions (`removeScriptSubscriptions` then re-register).
     4. Call an optional `onReload(old)`.
   - Reference files: `Managed/HotReload/HotReloadManager.cs` (preload/swap/rollback) and `Engine/Source/Scripting/ScriptManager.cpp` around lines 2208 and 2886 (watch, single in-flight reload, supersede).
5. **Shader-graph backend: node format plus compiler.** Value M, effort XL including the UI.
   - Sources:
     - Node library: `Engine/Modules/Rendering/Shaders/Graph/Nodes/**/*.glsl` (79 `@sgnode` functions).
     - Reflection and compile: `Engine/Modules/Rendering/Source/ShaderGraph/{SgTagParser,SgNodeReflector,SgGlslSignatureReader,SgTypes,SgGraphCompiler,SgPseudoNodes}.cpp` (about 1.95k lines).
     - Model: `Engine/Modules/Graph/Source/{GraphModel,GraphValue,GraphTypeRegistry}.cpp` (about 1.2k lines, nlohmann only).
   - Adaptation: retarget the `EvaluateSurface`/`surface_io.glsl` output contract to our shaderlib and `@property` manifests and our shader compiler (`engine/libs/shadercompiler`).
   - **The editor canvas cannot be ported.** It is built on their CSS/yoga retained UI (about 15k lines). We would need imgui-node-editor (thedmd, MIT) or imnodes (MIT) and a new panel.
   - License: their MIT, plus Godot MIT attribution for the VisualShader-derived semantics (`Engine/Modules/Graph/ThirdParty/visual-shader-upstream/UPSTREAM.md`, Godot 4.6.3 `35e80b3a`).

**Smaller lifts**
- `Engine/Modules/NumericExpression` (tinyexpr is **zlib** licensed: keep its notice) for Inspector `DragFloat` text entry.
- `ExternalScriptEditorLauncher.cpp` plus the Windows association logic in `Shell.cpp:938`, generalized to pass `.luarc`/`.clangd` workspaces, using `code -g file:line`.
- `ScriptStatusIndicator.cpp` as UI design only: a toolbar badge for the C++ build state of `Generator::isBuildInProgress` / `didLastBuildSucceed`.

**Process idea**
- Unify `generate_api_suggestions.py` and `generate_lua_stubs.py` behind one emitted JSON IDL, modeled on `Engine/Scripting/engine-api/Input.json` (id, cpp, header, line, doc, a format hash).
- Both the code editor's suggestions and the LuaLS stubs would read it, and MCP docs could too.
- No code to copy.

---

## 6. Not worth copying

- **C#/CoreCLR stack wholesale** (`Managed/*`, `CoreCLRHost.cpp`, `ScriptManager.cpp`, CompileServerHost): about 40k lines.
  - Requirements: the .NET 10 SDK at edit time, hostfxr at runtime or a separate NativeAOT Player path, and Roslyn 4.8 and 4.10 together.
  - NuGet license status is unresolved for `Microsoft.NETCore.Platforms` 1.1.0 (.NET Library terms) and `xunit.abstractions`.
  - It does not run on our Web, Android or iOS targets without large extra work.
  - Their script model is ECS-system-oriented (no per-entity scripts), which conflicts with our Unity-style GameObject plus `ScriptBase`/Lua-instance model.
  - Lua already fills the managed-language role.
- **`Tools/ComponentScanner`** (about 10k lines of hand-rolled C++ parser): the idea is good, but `generate_lua_stubs.py` already has a C++ tokenizer and type resolver. Extending that to emit LuaBridge registration is cheaper than adopting a second parser.
- **InteropGenerator:** a hardcoded stub on the deprecated `ISourceGenerator` API.
- **Game-logic visual scripting** (`GameLogicActionCatalog`, `GameLogicGraphCompiler`, `GameLogicRuntime`, `GraphFsmRuntime`):
  - It is a prototype: code generation is reached only from tests and there is no runtime implementation.
  - Copying it would mean writing the runtime and the editor ourselves.
  - If we want visual scripting, design it to emit Lua, which can be reloaded and has no compile step, rather than C++.
- **The graph and shader-graph editor canvas:** tied to their retained CSS/yoga UI, input, undo service and render graph. A rewrite on ImGui is cheaper than a port.
- **NativeScripting module as a whole:** many parts are Windows/MSVC-specific (`MsvcToolchain`, the SEH guard), `Register_v1` is still a no-op, and our CMake plus `Conector` path already works on all three desktop OSes. Take only the fingerprint and the generation-stamp ideas.
- **The `EntitySystemGenerator` source generators:** C#-specific.
- **`Packages/unity-import`:** it ships a prebuilt `Tools/UnityConverter.dll` (MIT source, but a binary blob from an untrusted download). Never vendor binaries; a Unity importer, if wanted, belongs to another domain.
- **`Packages/ai-assistant`** (about 19k lines): an editor chat UI. Our MCP-server approach (external agents drive the editor) is the better fit and is already in place.
- **Their four VCS packages:** useful only as example packages once a package system exists. Out of scope here.
