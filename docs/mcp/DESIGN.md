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
- Works on Linux, Windows, macOS (portable C++17; only Linux is tested here).

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
   by the MCP Server window (Window > MCP: Serve over HTTP + port, default 47615), persisted
   in AppSettings. Bind 127.0.0.1 only; reject requests whose `Origin` or `Host` is not
   localhost (DNS rebinding). Every request needs `Authorization: Bearer <token>` (401 and
   `WWW-Authenticate: Bearer` otherwise, compared in constant time): settings.yaml
   64 random hex digits made the first time HTTP is enabled, in `mcp_http_token` next to
   settings.yaml (POSIX mode 0600, set before the token is written and on a readable file at
   load; an `mcp.http_token` older editors left in settings.yaml moves there); the MCP Server
   window shows and copies it, copies the `claude mcp add` connect command and makes a new
   one (applies to the next request). POST /mcp answers `application/json` (one JSON-RPC
   response) or `text/event-stream` when the request wants progress notifications (2026-07-28:
   once the first one comes; an answer before any goes as JSON);
   GET /mcp → 405; DELETE /mcp ends an initialize-era session. Hand-written minimal
   HTTP/1.1 over BSD sockets / Winsock (no new dependency).

   **Read-only mode**: the MCP Server window's "Allow changes" switch (settings.yaml
   `mcp.allow_changes`, default on). Off, HTTP clients only get the tools annotated
   `readOnlyHint` in `tools/list` and `tools/call` (`batch` is not one, and every nested call
   of a batch is gated the same way: `McpCall::changesGated`), checked again when a queued
   call runs. A read-only tool's call is refused too when its arguments write files
   (`ToolDef::writesFiles`: screenshot `save_path`, get_asset_info on an .fbx;
   `McpServer::mayChange`, which auto-sync's settle uses as well). stdio clients are not
   affected: they launched the editor themselves (and the spec has stdio take credentials
   from the environment, so stdio has no token either).

Protocol: JSON-RPC 2.0, dual-era.

- **2026-07-28** (stateless): no `initialize`; every request names the version in
  `params._meta["io.modelcontextprotocol/protocolVersion"]` and carries
  `io.modelcontextprotocol/clientCapabilities` (else -32602, HTTP 400); an unknown version
  is `UnsupportedProtocolVersion` (-32022, `data.supported` / `data.requested`, HTTP 400).
  `server/discover` returns `supportedVersions` (2026-07-28 first), `capabilities`
  (`tools`, `resources`, `prompts`), `_meta["io.modelcontextprotocol/serverInfo"]` and
  `instructions`. Every result has `resultType: "complete"` and the serverInfo `_meta`; the
  cacheable ones (`server/discover`, `tools/list`, `prompts/list`, `resources/list`,
  `resources/templates/list`, `resources/read`) `ttlMs: 0, cacheScope: "private"` (the tool
  list changes with "Allow changes"). An unknown method (`initialize`, `logging/setLevel`
  included) is -32601 with HTTP 404; a missing resource is -32602. Over HTTP there is no
  session (no `Mcp-Session-Id` minted or echoed; each request gets a stateless McpSession of
  its own), the `MCP-Protocol-Version`, `Mcp-Method` and `Mcp-Name` (`params.name` of
  `tools/call` / `prompts/get`, `params.uri` of `resources/read`; `=?base64?…?=` decoded)
  headers must mirror the body (`HeaderMismatch` -32020, HTTP 400:
  `McpTransportHttp::checkModernHeaders`), and closing the connection before the answer
  cancels the request. Over stdio the same requests are told apart by their `_meta`, and
  `notifications/cancelled` cancels.
- **2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05** (initialize handshake; negotiate: echo
  the client's if supported, else the newest). Capabilities: `tools`, `resources`, `prompts`,
  `logging`. Methods: `initialize`, `notifications/initialized`, `ping`, `tools/list`,
  `tools/call`, `resources/list`, `resources/templates/list`, `resources/read`,
  `prompts/list`, `prompts/get`, `logging/setLevel`, `notifications/cancelled`,
  `notifications/progress` (server → client). HTTP: `Mcp-Session-Id` issued at initialize
  and required afterwards; an `MCP-Protocol-Version` header that is none of these is 400.

`initialize` and `server/discover` return `instructions` (the agent guide, see below).

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
  McpSchema.{h,cpp}       JSON Schema builders (object, vec3, entityRef...)
  McpInstructions.cpp     const char* mcpServerInstructions(): the agent guide text
  McpActivityWindow.{h,cpp}  editor panel: server status, connected clients, call log
  McpJobs.{h,cpp}         long operations (export, build, compile, play startup) as jobs
  McpLaunch.{h,cpp}       `jasperengine-editor mcp` command line, stdio fd setup, --headless Xvfb
  McpAutoSync.{h,cpp}     picks up files the agent edited on disk before every tools/call
  EngineApiContext.{h,cpp}, TerrainMaps.{h,cpp}   helpers of the docs and terrain tools
  tools/
    McpTools.h            the register functions; W4Shared.h, PlayViewCommon.h, HtmlUiShared.h: helpers shared by modules
    CoreTools.cpp         batch, get_job, cancel_job, undo/redo, editor_status, respond_dialog
    ProjectTools.cpp      project lifecycle and settings
    SceneTools.cpp        scenes and scene settings
    EntityTools.cpp       hierarchy, GameObjects, selection and clipboard
    ComponentTools.cpp    components and properties
    PrefabTools.cpp       prefabs, variants, overrides and Prefab Mode
    PlayTools.cpp         play control, runtime inspection and Lua evaluation
    ViewTools.cpp         screenshots and scene camera
    ConsoleTools.cpp      console logs
    ScriptTools.cpp       Lua/C++ scripts and compilation
    AssetTools.cpp        assets, imports, materials and shaders
    SynthesisTools.cpp    texture synthesis (synthesize_texture; the recipe summary get_asset_info shows),
                          sharing texsynth/SynthesisSession with Window > Texture Synthesis
    SpriteNormalsTools.cpp sprite normal maps (generate_sprite_normals; the recipe summary get_asset_info shows),
                          sharing spritenormals/SpriteNormalsSession with Window > Sprite Normal Map and the
                          generate-sprite-normals CLI
    DocsTools.cpp         engine API docs and resources
    BuildTools.cpp        export, build and player jobs
    WorldTools.cpp        terrain, tilemaps, animation, physics, UI and audio
    AnimationClipTools.cpp the clip-file mode of edit/get/preview_animation (.anim through
                          AnimationClipDocument, bindings, preview through AnimationClipController),
                          the .anim parts of get_asset_info / write_file (no tools of its own)
    LatticeTools.cpp      edit_lattice (lattice deformer cages)
    PlacementTools.cpp    physics placement (place_with_physics: drop, separate, move, snap, gather,
                          distribute in edit mode), sharing physics/PlacementSession with the Scene
                          view's Physics tool and the place-physics CLI
    HtmlUiTools.cpp       HTML/CSS UI: inspect_ui, assert_ui, create_ui_document, the example resources
    VatTools.cpp          bake_vertex_animation (vat/VatBakeSession and the vat/import importers, sharing
                          vat/VatFrontEnd with the bake-vat CLI; the contact sheet: vat/VatContactSheet)
                          and the .vat parts of get_asset_info, write_file, view_asset, move/delete_asset
                          and the file sync
```

Every `tools/*.cpp` defines one function, e.g. `void registerSceneTools(McpRegistry&)`;
`McpServer` calls them all in a fixed order (Core first, then the rest; a later module may
replace a tool by registering the same name). Every file above is listed in `CMakeLists.txt`.

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

Long operations (export/build players, C++ script compile, play startup with a C++ build,
texture synthesis, physics placement) take `wait` (bool, default true) and `timeout_seconds` (default 120). They start a **job**
(`McpJobs`: id, kind, status running/succeeded/failed/cancelled, progress, log tail,
result); if not done within the timeout the tool returns the job id and the agent polls
`get_job` / `cancel_job`. Work that must stay on the main thread (`place_with_physics` steps
a private physics world) advances a slice of a few milliseconds per editor frame, so the editor
keeps drawing while the job runs.

`batch` (CoreTools): `{calls: [{tool, arguments}], undo_group: bool, stop_on_error: bool}`
runs several tool calls in one round-trip, sequentially on the main thread; with
`undo_group` all scene edits are one Ctrl+Z step where possible.

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
- **Symbolic links** (and Windows junctions). Reads follow them wherever they lead: an assets
  folder deliberately linked in from a shared library reads, lists, previews and imports fine
  (`McpRefs::resolveProjectPath` is lexical). Every tool that **writes, creates, deletes, moves or
  renames** a file resolves its path through `McpRefs::resolveProjectWritePath` /
  `McpRefs::writeEscape`: once every link on the way is followed (a dangling one too; the shared
  walk `safewrite::resolvedPath` in `editor/util/SafeWrite.h`, the same one sprite normals'
  output containment uses), the location must be inside one of the project's **trusted roots**:
  the project directory, its configured assets directory and its configured Lua scripts
  directory (project.yaml `assetsDir` / `luaDir`, as loaded), each resolved once. So a project
  opened through a link works, and so does a project whose assets or Lua folder is itself a link
  (to a shared drive): agents write its assets and scripts there. Links
  BELOW those roots are what is checked (one pointing further out is refused); links pointing
  inside a trusted root work. A tool that changes an asset root (`set_project_settings`
  `editor.assets_dir` / `scripting.lua_dir`) must name a folder inside the project directory
  itself once resolved (`McpRefs::projectOnlyEscape`): an agent can't make a folder outside the
  project a trusted root. A delete or
  a move's source acts on the entry itself: a link there is trashed or moved, never followed.
  Refused: isError naming the path and where the link leads, with the fix ("use a path that is not
  behind a link to elsewhere, or replace the link with a real folder").
- **Writes beneath the project.** The check and the write are two steps; a link planted between
  them must not redirect the write. `editor/util/SafeWrite.h` never follows a link below the root
  at the moment it acts (POSIX: `openat` with `O_NOFOLLOW` folder by folder, `O_PATH` / `O_SEARCH`
  for search-only folders, an exclusive temporary file renamed with `renameat` in the same folder,
  `renameat2(RENAME_NOREPLACE)` / `renameatx_np(RENAME_EXCL)` for no-replace moves, tree removal
  with `fdopendir` + `unlinkat`; Windows: every folder opened relative to the handle of the one
  before (`NtCreateFile` with `RootDirectory`, `FILE_OPEN_REPARSE_POINT`), name-surrogate reparse
  points refused, cloud-file placeholders treated as ordinary entries, files created relative to
  the held folder and renamed by handle with `FileRenameInfoEx`): a folder on the way that became a
  link is refused (the error names it: remove the link, then try again); a link at the final name
  is replaced, not followed.
- **The write scope.** `McpServer` runs every tool's main-thread handler, its deferred
  continuations (`waitUntil` / `afterFrames`) and resource reads inside a `safewrite::Scope` of the
  project directory (`McpWriteScope`); the tools' own continuations (enqueued main-thread tasks,
  the remote importers' worker thread) open one too. Inside it `FileUtils::writeFileReplacing`,
  `writeFile` / `writeIfChanged` and the `FileUtils::createDirectories` / `removePath` /
  `removeAll` / `renamePath` / `copyPath` helpers route paths inside the project to the
  resolve-then-beneath functions, so the editor code the tools call (scene, prefab and project.yaml
  saves, the clip, pose and Vertex Animation documents, shader forks, code editor saves, the asset
  root relocation, the clip-hint and `.vat` mesh-field rewrites, thumbnails, HTML UI imports, the
  workspace file, terrain maps, lattice bakes, AGENTS.md / CLAUDE.md) writes beneath the project
  without changes of its own. Paths outside the project (another project, the editor's settings)
  and everything outside a scope (the GUI) keep their plain behaviour.
- **Commands remember the scope.** A `Command` made inside a scope records its root
  (`Command::writeRoot`); `CommandHistory` runs its execute, undo and redo in a scope of that root
  again, also when the GUI triggers them. The file commands (`WriteFilesCmd`, `CreateDirCmd`,
  `DeleteFileCmd`, `RenameFileCmd`, `CopyFileCmd`, the copy of `move_asset`) list the paths they
  act on (`Command::touchedPaths`): a step whose path now resolves outside its root is refused
  (`canUndo` / `canRedo`: the history keeps its place; the console and the `undo` tool say which
  file, with "remove the link, then undo again"); a restore that still fails after the check is
  reported (`CommandHistory::lastUndoProblem`, the `undo` tool's `problems`). The `undo` tool also
  checks a step the GUI made (no root of its own) against the project (`Command::escapeFor`) before
  taking it. A redo whose execute fails is not taken.
- **Checked up front only.** What external programs write: CMake and the compilers in a build
  (`build_project`, `generate_shaders` refuse an internal or output folder inside the project that
  resolves outside it; their logs are opened once beneath the project and kept open), FFmpeg's
  video conversions (the destination folder is checked).

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

- Prefer fewer, more powerful tools with excellent descriptions and schemas over many
  narrow ones. A tool description says what it does, what it returns, and the common
  mistakes. Target well under 150 tools in total.
- Edits through commands (undoable); file writes refresh the Project window, the asset
  database and open code editor buffers.
- Every tool has a test in `tools/mcp/tests/` run through the harness against a real
  editor under Xvfb.

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

- **Scan**: stat-based against a cached map (mtime + size) of the project's tracked files
  (Lua, C++ `.h/.hpp/.hh/.hxx/.inl/.c/.cc/.cpp/.cxx`, shaders `.vert/.frag/.glsl`,
  `.material`, HTML UI `.html/.htm/.css`, pose files `.poseanim/.humanoid` (their open
  `PoseAnimationDocument`s reload), animation clips `.anim` (their open
  `AnimationClipDocument`s reload, `AnimationClipPool` drops them and the Animation Players
  using them reload, during play too; load errors and warnings of `.anim` and `.poseanim`
  files become `problems`), Vertex Animations `.vat` (`VertexAnimationPool` drops them and the
  Vertex Animators reload, during play too; parse errors, a `.vat` / `.vat.glb` mismatch and the
  lenient reader's warnings become `problems`; a changed `.vat.glb` drops its `.vat`); models `.gltf/.glb/.obj/.fbx` and glTF `.bin` buffers (picked up
  but not reported: only `AnimationClipDocument::filesChanged` sees them, which drops the
  pooled parse, the model's sub-clips and their documents and reloads the players using them;
  no full sync, so no thumbnail and no FBX import); assets: images (`.svg` included), fonts `.ttf/.otf`,
  `.json/.lottie`) and folders (mtime: an added, removed or renamed entry), skipping
  hidden folders (`.jasperengine`, `.git`...), `build`, `node_modules` and `trash`.
  `.scene`, `.prefab`, `project.yaml` and the terrain maps the editor writes from its
  in-memory terrain (`terrain_maps/terrain_edit_*.png`, written asynchronously after a
  sculpt) are not tracked: the editor owns them. Files changed
  this session, folders and code files (HTML UI included) are checked whole on every call (up to
  2000 of each); assets rotate within a ~2 ms budget, resuming where the last scan stopped. The
  baseline is taken (in `McpServer::tick`) when a project opens.
- **Pick-up**: changed paths go through `w4::syncFiles` (ScriptTools.cpp; the same as
  `sync_files` with paths): clean code editor buffers reloaded, script properties re-parsed,
  Lua syntax checked, materials refreshed, shader forks invalidated, textures reloaded, HTML UI
  documents reloaded, asset database and Project window refreshed. The editor's own part of that
  (documents, materials, shader forks, textures, asset database, Project window, the sprite normals
  watcher) is `ProjectFileWrites::refresh` (editor/util/ProjectFileWrites), which the editor's
  windows run without the server too. Shader forks using a changed
  shader file are then compiled (`w4::checkShadersAsync`; the call waits for it through
  `waitUntil`) so their errors are reported with the call.
- **HTML UI**: UI documents reload live, before the tool runs, in edit mode and during play
  (`w4::syncHtmlUi`: `UIDocumentSystem::reloadChangedFiles` of every open scene, prefab stage and
  running Play scene), sharing the per-document stamps with the editor's own poller
  (`HtmlUIWatcher`, 0.25 s): whichever sees an edit first reloads it, the other finds it current,
  so a change is reloaded once. A CSS-only change restyles in place (elements and listeners
  kept); any other change rebuilds the document (`onLoad` again). The diagnostics located in the
  changed files, from the documents' (re)load, or from `ProjectChecks::checkUiFiles` for a changed
  `.html/.css` no live document uses (the same code as `check --ui`), become `problems`.
- **Report**: the call's result gets `structuredContent.external_changes` = `{changed,
  added?, removed?, changed_count?, problems: [{path, line, message}], script_properties?,
  reloaded_buffers?, ui_reloaded?, ui_current?, deferred?}` and one text content item; nothing
  when nothing changed. UI problems also carry `col`, `severity`, `code` and `suggestion` (the
  text prints a warning as `path:line: warning: message [code]`); `ui_reloaded` =
  `[{scene, entity: {id, name, path}, html, stylesheet_only}]`, `ui_current` = how many documents
  reading a changed file the poller had already reloaded (text: `Reloaded UI: assets/ui/menu.html
  in "Menu" (styles only); 1 document(s) were already current.`).
- **Not the agent's changes**: `syncFiles` (the funnel of `write_file`, `create_script`,
  the material and asset tools, `code_editor` save, undo of file writes) restamps what it
  handled (`McpAutoSync::filesSynced`). The editor's own undoable writes
  (`ProjectFileWrites::writeUndoable`: the Sprite Normal Map and Texture Synthesis windows' Save,
  the sprite normals watcher's regeneration) need no server; while it runs they go through
  `syncFiles` as well (the sync hook `McpServer::start` installs), so they are not external
  changes either. After a call that may write files (not read-only, or
  with `save_path`), a settle scan posted to the main thread attributes changes stamped
  inside the call's time window to the tool, which handled them (`fork_shader`, screenshots
  saved into the project, terrain maps): their stamps are taken, nothing is re-run (a texture
  reload would undo a terrain tool's in-memory edit). Older ones are reported by the next
  call. Limitation: an external edit made *while* a writing call runs is taken as the tool's
  (neither picked up nor reported; `sync_files` or the editor's own poll catch it).
- **Play**: the editor does not re-parse scripts during play (Play restores the scene from a
  snapshot when it stops; `CodeEditor::checkExternalScriptChanges` waits too). Changes found
  then are reported with `deferred` (Lua syntax still checked) and picked up by the first
  call after play stops. HTML UI files are not deferred: `.html/.htm/.css`, fonts and
  `.json/.lottie` go through the pick-up at once (the running and the edit-time documents
  reload); a changed image reaches the documents at once and the scene textures after play;
  `deferred` is set only when non-UI files wait.
- **The editor's own 1 s poll** (`CodeEditor::checkFileChanges` for open buffers,
  `checkExternalScriptChanges` for attached scripts) keeps running for humans. Buffers are
  reloaded through the same `checkFileChanges` (one shared `lastWriteTime`, so never twice);
  after a sync `CodeEditor::markScriptFilesCurrent` updates the script watch so it does not
  re-parse the same file again; if the poll was first, the re-parse finds no property change.
- Cost: `editor_status.auto_sync` = `{files, dirs, checked, complete, scan_ms, settle_ms,
  scan_cpu_ms, settle_cpu_ms}`; ~0.1 ms per call (0.25 ms with the settle) on the fixture
  project (`tools/mcp/tests/test_autosync.py` measures it, and a 25k-file project).

## Agent guide (instructions)

`initialize.instructions` and the `jasper://guide` resource carry the agent guide
(`McpInstructions.cpp`): how the engine maps to Unity, scene/entity addressing, the loop
(inspect → edit → save → play → screenshot → read console → fix), working on code with the
agent's own tools, play and time control, Lua and C++ scripting conventions, 2D/3D and UI,
physics, prefabs, the shader library and forking, shipping. It is sent every session: keep it
tight (at most 16 KB, 16,384 bytes); deeper references are the `jasper://docs/...` resources
(`docs/mcp/guide/*.md`): move detail there rather than growing the guide.
`test_autosync.py::test_instructions_name_real_tools` checks the size and that every tool and
parameter name it mentions exists.
