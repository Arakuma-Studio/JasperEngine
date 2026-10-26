# JasperEngine MCP server: design

This replaces the in-editor AI chat agent (`editor/ai/*`, `AiChatWindow`, `AiSettingsWindow`)
with an MCP server embedded in the editor. External agents (Claude Code, Codex, any MCP
client) drive the real editor: everything a human can do in the editor, an agent can do
through MCP tools, well enough to build and ship a game with no human in the loop.

## Goals

- Full coverage: scenes, GameObjects, components (every property type), prefabs (incl.
  variants), assets, materials, shaders, Lua and C++ scripts, play mode, runtime inspection,
  input simulation, screenshots, console, build/export, project lifecycle and settings.
- Agents can *see* (screenshots of Scene and Game views as MCP image content), *test*
  (play, inject input, read logs, query runtime state) and *ship* (export and build players).
- Every editor edit goes through the editor's command system: undoable with Ctrl+Z, prefab
  overrides recorded, Hierarchy/Inspector refreshed. A human watching the editor sees it live.
- Never blocks the UI thread waiting on something the UI thread must do (play startup,
  export, C++ build, rendering a frame).
- Works on Linux, Windows, macOS (portable C++20; only Linux is tested here).

## Process model and transports

The MCP server lives inside the GUI editor process (it needs the real editor: GL context,
windows, Project, command histories). Two transports, same server:

1. **stdio**: `jasperengine-editor mcp [--project <dir|project.yaml>] [--create <dir> [--name N]]
   [--headless] [--http-port N]`. The MCP client launches the editor. The editor starts
   its normal GUI and serves MCP on the *original* stdin/stdout. At startup, stdio mode
   dups the original stdout fd for the transport and `dup2`s stderr onto fd 1, so every
   other stdout write (Out, printf, Lua `print`) goes to stderr and can't corrupt the
   protocol. `--headless` (Linux): if there is no `DISPLAY`, spawn `Xvfb -displayfd`
   (and kill it at exit) so agents on servers need nothing else.
   Client config: `{"command": "jasperengine-editor", "args": ["mcp", "--project", "..."]}`.
2. **Streamable HTTP**: `http://127.0.0.1:<port>/mcp`, for attaching an agent to an editor a
   human already has open. Enabled by `--mcp-port N` on the normal editor command line or
   by Editor Settings > MCP (enable + port, default 47615), persisted in AppSettings.
   Bind 127.0.0.1 only; reject requests whose `Origin` is not localhost (DNS rebinding);
   optional bearer token. POST /mcp answers `application/json` (one JSON-RPC response) or
   `text/event-stream` when the request wants progress notifications; GET /mcp → 405;
   DELETE /mcp ends the session. `Mcp-Session-Id` header issued at initialize.
   Hand-written minimal HTTP/1.1 over BSD sockets / Winsock (no new dependency).

Protocol: JSON-RPC 2.0, MCP versions `2025-11-25`, `2025-06-18`, `2025-03-26`,
`2024-11-05` (negotiate: echo the client's if supported, else the newest). Capabilities:
`tools`, `resources`, `prompts`, `logging`. Methods: `initialize`, `notifications/initialized`,
`ping`, `tools/list`, `tools/call`, `resources/list`, `resources/templates/list`,
`resources/read`, `prompts/list`, `prompts/get`, `logging/setLevel`,
`notifications/cancelled`, `notifications/progress` (server → client).
`initialize` returns `instructions` (the agent guide, see below).

## Code layout (`editor/mcp/`)

```
editor/mcp/
  McpJson.h               using Json = nlohmann::json (engine/libs/json)
  McpServer.{h,cpp}       JSON-RPC dispatch, lifecycle, sessions, tools/resources/prompts
  McpTransportStdio.{h,cpp}
  McpTransportHttp.{h,cpp}
  McpRegistry.{h,cpp}     tool / resource / prompt registries
  McpCall.{h,cpp}         per-call context: args, async completion, deferral, progress
  McpContext.{h,cpp}      access to App, Project, windows from tool handlers
  McpRefs.{h,cpp}         scene + entity + component + asset reference resolution
  McpValues.{h,cpp}       JSON <-> engine property values, PropertyCmd building
  McpUndo.{h,cpp}         agent undo attribution: folding a call's steps, undo history JSON
  McpSchema.{h,cpp}       JSON Schema builders (object, vec3, entityRef...)
  McpToolGroups.{h,cpp}   the tool groups tools/list hides until a session enables them
  McpInstructions.cpp     const char* mcpServerInstructions(): the agent guide text
  McpActivityWindow.{h,cpp}  editor panel: server status, connected clients, call log
  McpJobs.{h,cpp}         long operations (export, build, compile, play startup) as jobs
  McpLaunch.{h,cpp}       `jasperengine-editor mcp` command line, stdio fd setup, --headless Xvfb
  McpAutoSync.{h,cpp}     picks up files the agent edited on disk before every tools/call (the watcher's journal)
  EngineApiContext.{h,cpp}, TerrainMaps.{h,cpp}   helpers of the docs and terrain tools
  tools/
    McpTools.h            the register functions; W4Shared.h, PlayViewCommon.h, HtmlUiShared.h: helpers shared by modules
    McpTools.cpp          buildToolRegistry: the module table (docs section, group), no App needed
    CoreTools.cpp         batch, get_job, cancel_job, undo/redo, get_undo_history, editor_status, respond_dialog, tool_groups
    ProjectTools.cpp      project lifecycle and settings
    SceneTools.cpp        scenes and scene settings
    EntityTools.cpp       hierarchy, GameObjects, selection and clipboard
    ComponentTools.cpp    components and properties
    PrefabTools.cpp       prefabs, variants, overrides and Prefab Mode
    PlayTools.cpp         play control, runtime inspection, Lua evaluation and the Play Mode Changes review
    ViewTools.cpp         screenshots and scene camera
    ConsoleTools.cpp      console logs
    ScriptTools.cpp       Lua/C++ scripts and compilation
    AssetTools.cpp        assets, imports, materials and shaders
    DocsTools.cpp         engine API docs and resources
    BuildTools.cpp        export, build and player jobs
    WorldTools.cpp        terrain, tilemaps, animation, physics, UI and audio
    HtmlUiTools.cpp       HTML/CSS UI: inspect_ui, assert_ui, create_ui_document, the example resources
```

Every `tools/*.cpp` defines one function, e.g. `void registerSceneTools(McpRegistry&)`;
`buildToolRegistry` (`tools/McpTools.cpp`) calls them all in the fixed order of its module
table (Core first, then the rest; a later module may replace a tool by registering the same
name) and stamps each tool's module and default group. The server calls it at start, the
`jasperengine-editor call` CLI offline. Every file above is listed in `CMakeLists.txt`.

## Registry API

```cpp
namespace jasperengine::editor::mcp {

struct ToolAnnotations { bool readOnly = false, destructive = false, idempotent = false, openWorld = false; };

struct ToolDef {
    std::string name;          // snake_case verb_noun, e.g. "create_entity"
    std::string title;         // human label
    std::string description;   // what it does, when to use it, gotchas. Agents read this.
    Json inputSchema;          // JSON Schema object; put real constraints here
    Json outputSchema;         // optional
    ToolAnnotations annotations;
    bool allowedWhilePlaying = true;   // registry refuses the call during play if false
    ToolActivity activity = ToolActivity::Auto;  // busy policy
    std::string group;         // McpToolGroups.cpp; empty = core (always listed)
    std::string module;        // docs section, stamped by buildToolRegistry
    std::function<void(CallPtr)> handler;  // runs on the MAIN thread
};

class McpRegistry {
    void addTool(ToolDef def);           // same name replaces
    void addResource(ResourceDef);        // static uri
    void addResourceTemplate(ResourceTemplateDef);  // uri template e.g. jasper://scene/{id}
    void addPrompt(PromptDef);
};
}
```

## Calls: main thread and async completion

The transport thread parses a request and, for `tools/call`, enqueues a job on the editor
main thread (`EditorHost::enqueueMainThreadTask`) and wakes the event loop
(`App::setWakeCallback` machinery; the editor idles waiting for events). The handler runs
on the main thread between frames with a `CallPtr` (`std::shared_ptr<McpCall>`):

```cpp
class McpCall {
    const Json& args() const;
    McpContext& ctx();
    void ok(const std::string& text, Json structured = nullptr);    // success
    void fail(const std::string& message, Json structured = nullptr);// isError: true
    void reply(ToolResult r);                // text + images + structuredContent
    // Deferral: poll each frame on the main thread until done() returns true or the
    // timeout passes; then calls then(timedOut). The handler returns immediately.
    void waitUntil(std::function<bool()> done, int timeoutMs, std::function<void(bool timedOut)> then);
    void afterFrames(int frames, std::function<void()> fn);   // e.g. screenshots
    void progress(double done, double total, const std::string& message);  // best effort
    bool cancelled() const;                  // notifications/cancelled
};
```

A call completes exactly once. Exceptions thrown by a handler become `fail()`.
Errors that are the agent's fault (bad args, unknown entity) are tool results with
`isError: true` and a message that says how to fix it (list the valid choices), never
JSON-RPC errors. JSON-RPC errors are only for protocol problems (unknown tool, bad params
shape).

Long operations (export/build players, C++ script compile, play startup with a C++ build)
take `wait` (bool, default true) and `timeout_seconds` (default 120). They start a **job**
(`McpJobs`: id, kind, status running/succeeded/failed/cancelled, progress, log tail,
result); if not done within the timeout the tool returns the job id and the agent polls
`get_job` / `cancel_job`.

`batch` (CoreTools): `{calls: [{tool, arguments}], undo_group: bool, undo_name: string,
stop_on_error: bool}` runs several tool calls in one round-trip, sequentially on the main
thread; with `undo_group` each history the batch changed gets one step (`mark`/`groupSince`),
named `undo_name` (default "Batch (N calls)") and linked across histories, so one Ctrl+Z
undoes the whole batch; `complete` is false when a history refused to group.

Undo attribution (`McpUndo`, docs/undo.md): `McpServer::execute` runs every edit tool's handler
and its deferred continuations under an `UndoOriginScope` {agent, client name, request id,
tool}; the steps are named "<client>: <name>". When the call replies, each history's
contiguous top run of the call's steps is folded into one step ("<client>: <Tool Title>
(<subject>)") and the folded steps are linked; `structured.undo` {id, name} names the newest.
Read-only and control tools (batch, play) are not attributed. `undo`/`redo` take `scope:
"timeline"` (UndoTimeline, Ctrl+Z), `to_id` and `expect_top_id`; `get_undo_history` lists the
steps; the play-change tools (`get_play_changes`, `apply_play_changes`, `discard_play_changes`,
`play stop keep_changes`) drive the Play Mode Changes review.

## Addressing (McpRefs)

- **Scene**: `scene` = numeric id, scene name, or project-relative `.scene` path. Omitted →
  the selected scene. During play, tools that read runtime state resolve to the playing
  runtime scene.
- **Entity** (GameObject): `entity` = numeric id, or a *path*: `/`-joined names from the
  scene root with `#N` for the Nth earlier same-name sibling (`Player/Gun`, `Enemy#2`) —
  the same scheme as `scene-dump`, shared through `editor/util/EntityPath.{h,cpp}`.
  Ambiguous names are an
  error listing the matching paths. Results always return both `id` and `path`.
- **Component**: `Catalog::getComponentType` names (`transform`, `mesh`, `rigidbody2d`, ...),
  validated (unknown → error listing valid names; never silently Transform).
- **Property**: Catalog property paths (`position`, `submeshes[0].material.baseColorFactor`,
  `scripts[0].speed`).
- **Asset**: project-relative path. Paths are sandboxed to the project (no `..`, no
  absolute paths outside it) except explicit import-from-disk tools.

## Values (McpValues)

JSON ↔ every `PropertyType`: Bool, Int, UInt, Float, Double, String; Vector2/3/4 as
`[x,y,(z,(w))]` (also accept `{x,y,z,w}`); Quat as `[x,y,z,w]` or `{"euler":[x,y,z]}`
degrees; colors as Vector3/4; Enum by name (string) or int; Ease by name; Texture /
Font by asset path (or `{"camera": <entity>}` for render textures); Material as an object
or a `.material` asset path; Entity / EntityReference by entity ref; Custom via the
component's YAML encoding (Stream) round-trip so nothing is unsettable.
Reads return the same shapes. Writes build typed `PropertyCmd<T>`s inside a
`MultiPropertyCmd` and use `addCommandNoMerge` (one discrete undo step per call).

## Shared editor APIs

- `editor/util/EntityPath.{h,cpp}` shared with the CLI.
- Console: structured, cursor-based log reads (`id`, `type`, `message`, timestamp) and
  clear; Lua `print` redirected into the Console (and stderr) in editor and play.
- `SceneRender::getFramebuffer()`; PNG encode to memory (stb `stbi_write_png_to_func`).
- Non-interactive Project APIs: create a scene and get its id, open/close a scene, save
  a scene to a path, create/open a project, without modal prompts (`choice` parameters).

## Tool conventions

[conventions.md](conventions.md) is the rule book (names, schemas, groups, budget, docs and
coverage gates); in short:

- Prefer fewer, more powerful tools with excellent descriptions and schemas over many
  narrow ones. A tool description says what it does, what it returns, and the common
  mistakes. The default listing (`core`) stays within 150 tools and 100,000 characters;
  other tools sit in groups (`McpToolGroups.cpp`) that tools/list shows once a session
  enables them (`tool_groups`), and every tool stays callable by name.
- `buildToolRegistry` (`tools/McpTools.cpp`) builds the registry without an `App`, so the
  CLI lists and validates tools offline exactly as the server serves them.
- Edits through commands (undoable); file writes refresh the Project window, the asset
  database and open code editor buffers.
- Every tool has a test in `tools/mcp/tests/` run through the harness against a real
  editor under Xvfb (`test_tool_coverage.py`).

## Code lives on disk: agents edit it with their own tools

The project is a normal folder. Agents (run with the project folder as their working
directory) read, edit, grep and git its Lua scripts, C++ sources, shaders and materials with
their own tools and language servers (`.luarc.json` for lua-language-server with the Lua API
stubs in `.jasperengine/engine-api/lua/`, `.clangd` -> `.jasperengine/clangd/compile_commands.json`
for clangd, the engine API snapshot in `.jasperengine/engine-api/`). MCP is for what needs the editor: making it notice
the changes, validating with the engine's own compilers (`check_script`, `check_shader`,
`compile_scripts`, or the `jasperengine-editor check` CLI), attaching scripts and setting
their properties, playing and observing. `write_file` / `read_file` / `search_files` remain
for clients without file access (remote HTTP).

### Automatic pick-up (McpAutoSync)

`McpServer::handleToolsCall` runs `McpAutoSync::beforeCall` on the main thread before every
top-level tools/call (not for calls nested in `batch`, nor before `sync_files`, which does
the same work and reports it itself):

- **What changed**: the file watcher's journal (`FileWatchService::journalSince` after
  `flush(250 ms)`, S05 R29; `source: "watcher"`), its external changes to tracked files since
  the last call. The editor's own writes (origin Editor) and changes stamped inside a writing
  call's run are left out. When the watcher is off, degraded, fell behind its 65,536-entry
  journal or the flush timed out, the stat scan (`source: "scan"`): stat-based against a cached
  map (mtime + size) of the project's tracked files (Lua, C++
  `.h/.hpp/.hh/.hxx/.inl/.c/.cc/.cpp/.cxx`, shaders `.vert/.frag/.glsl`, `.material`, HTML UI
  `.html/.htm/.css`; assets: images (`.svg` included), fonts `.ttf/.otf`, `.json/.lottie`) and
  folders (mtime: an added, removed or renamed entry), skipping hidden folders
  (`.jasperengine`, `.git`...), `build`, `node_modules` and `trash`. `.scene`, `.prefab`,
  `project.yaml` and the terrain maps the editor writes from its in-memory terrain
  (`terrain_maps/terrain_edit_*.png`) are not tracked: the editor owns them. Files changed this
  session, folders and code files (HTML UI included) are checked whole on every call (up to
  2000 of each); assets rotate within a ~2 ms budget. The baseline is taken (in
  `McpServer::tick`) when a project opens.
- **Pick-up**: changed paths go through `w4::syncFiles` (ScriptTools.cpp), which is
  `HotReloadService::apply` (editor/reload, the one reload funnel of the watcher, `sync_files`,
  Assets > Refresh and the code editor's saves): clean code editor buffers reloaded, script
  properties re-parsed, Lua syntax checked, textures, materials, models, audio and fonts decoded
  on workers and swapped in, the shader forks whose sources or includes hold a changed file
  compiled before they swap, HTML UI documents reloaded, asset database and Project window
  updated. The call waits (`waitUntil`, 10 s at most) for the reloads it started; their
  failures (the previous version kept) and the forks' compile errors are reported with the call.
  A path the watcher reloaded before the call is current (the applied-stamp table): reported,
  with its `reloaded` entry, not reloaded again.
- **HTML UI**: UI documents reload live, before the tool runs, in edit mode and during play
  (`UIDocumentSystem::reloadChangedFiles` of every open scene, prefab stage and running Play
  scene, whose per-document stamps make a change reload once, whoever sees it first). A CSS-only
  change restyles in place (elements and listeners kept); any other change rebuilds the document
  (`onLoad` again). The diagnostics located in the changed files, from the documents' (re)load,
  or from `ProjectChecks::checkUiFiles` for a changed `.html/.css` no live document uses (the
  same code as `check --ui`), become `problems`.
- **Report**: the call's result gets `structuredContent.external_changes` = `{changed,
  added?, removed?, changed_count?, source, reloaded: [{path, kind, ms}], pending: [path],
  problems: [{path, line, message}], script_properties?, reloaded_buffers?, ui_reloaded?,
  ui_current?, deferred?}` and one text content item; nothing when nothing changed. UI problems
  also carry `col`, `severity`, `code` and `suggestion`; `ui_reloaded` =
  `[{scene, entity: {id, name, path}, html, stylesheet_only}]`, `ui_current` = how many documents
  reading a changed file were already reloaded.
- **Not the agent's changes**: the editor's writes go through the atomic writer, whose publishes
  feed the watcher's self-write ledger: delivered as the editor's own, their echoes dropped
  (`editor_status.file_watcher.echoes_suppressed`). `syncFiles` (the funnel of `write_file`,
  `create_script`, the material and asset tools, `code_editor` save, undo of file writes)
  restamps what it handled (`McpAutoSync::filesSynced`). After a call that may write files (not
  read-only, or with `save_path`), the settle scan stays as a safety net: changes stamped inside
  the call's time window are the tool's (absorbed, not re-run: a texture reload would undo a
  terrain tool's in-memory edit). While such a call runs the hot reload holds the watcher's
  batches, so the call's writes reload once, with its sync.
- **Play**: the editor does not re-parse scripts during play (Play restores the scene from a
  snapshot when it stops; S23 will register a hook for Lua in Play). Script changes found then
  are reported with `deferred` (Lua syntax still checked) and picked up after play stops.
  Textures, materials, shaders, models, audio, fonts and HTML UI reload at once.
- Cost: `editor_status.auto_sync` = `{source, files, dirs, checked, complete, scan_ms, settle_ms,
  scan_cpu_ms, settle_cpu_ms}`; `editor_status.file_watcher` = the watcher's counts and the
  last reload; `editor_status.frame_ms` = the frame times since the previous `editor_status`
  (`tools/mcp/tests/test_autosync.py` and `test_hot_reload.py` measure them).

## Agent guide (instructions)

`initialize.instructions` and the `jasper://guide` resource carry the agent guide
(`McpInstructions.cpp`): how the engine maps to Unity, scene/entity addressing, the loop
(inspect → edit → save → play → screenshot → read console → fix), working on code with the
agent's own tools, play and time control, Lua and C++ scripting conventions, 2D/3D and UI,
physics, prefabs, the shader library and forking, shipping. It is sent every session, so it
holds the workflow and a short digest per area; the detail is in the `jasper://docs/...`
resources (`docs/mcp/guide/*.md`) the digests name. Its limits are the `guide` entry of
`tools/mcp/budget.json` (16,384 bytes in all, 1,500 per `##` section), gated by
`tool_budget.py --check` ([conventions.md](conventions.md), "The agent guide").
`test_autosync.py::test_instructions_name_real_tools` checks that every tool and parameter name
it mentions exists.
