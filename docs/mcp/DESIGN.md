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

Long operations (export/build players, C++ script compile, play startup with a C++ build,
texture synthesis, physics placement) take `wait` (bool, default true) and `timeout_seconds` (default 120). They start a **job**
(`McpJobs`: id, kind, status running/succeeded/failed/cancelled, progress, log tail,
result); if not done within the timeout the tool returns the job id and the agent polls
`get_job` / `cancel_job`. Work that must stay on the main thread (`place_with_physics` steps
a private physics world) advances a slice of a few milliseconds per editor frame, so the editor
keeps drawing while the job runs.

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
  `editor.assets_dir` / `scripting.lua_dir`) must name a plain project-relative folder (no absolute
  path, no `..` step, even one that comes back in) inside the project directory itself once
  resolved (`McpRefs::projectOnlyEscape`): an agent can't make a folder outside the project a
  trusted root. The MCP trusted roots are only the configured roots **as the user opened them**:
  `open_project` (and `create_project`) refuse a project whose project.yaml gives an absolute
  `assetsDir` / `luaDir` or one with `..` ("points outside the project folder; open it from the
  editor"); the GUI keeps opening such a project, and once the user has opened it the agent works
  in its roots. **Deny by default (owned and hidden places).** Inside a strict (MCP) scope the
  writers themselves (`safewrite`: every write, create, rename-to and copy) refuse an editor-owned
  or hidden place — any path component starting with `.` (`.jasperengine` and all of it, including
  `user/build.yaml` and the workspace; `.git`, `.trash`, `.vscode`, ...), a file named
  project.yaml at any depth (the project's own included), and the project's root `CMakeLists.txt`
  (`ProjectBuild.cmake` is the agent's, so it is not owned) — checked on the spelled path AND where
  it resolves (a link inside the project to its `.jasperengine` or its root is no way in). The
  editor's own internal writers (the project.yaml save, the workspace, the asset-database cache,
  build logs, the trash move, generated sources and the engine-API / Lua / clangd tooling) name the
  one place they write with a narrow `safewrite::EditorInternal` token; an agent-named write has
  none, so a tool makes no second project.yaml and reaches nothing under `.jasperengine`. Per-tool
  checks (`McpRefs::protectedPlace`, `import_files`' destination, `screenshot`'s `save_path`) only
  turn this into a clear up-front error. Names the editor owns (project.yaml, CMakeLists.txt,
  `.jasperengine` and other hidden folders, `.scene` / `.prefab`) are compared as the file system
  may see them (`safewrite::comparableName`: letter case, trailing dots and spaces, NTFS stream
  suffixes, Windows short names; Windows folder containment folds case with `CompareStringOrdinal`,
  elsewhere ASCII, macOS an ASCII approximation).
  Machine-level preferences (`preferences.default_export_dir`, `cmake_path`, `emsdk_path`,
  `ffmpeg_path`, `build_c_compiler`, `build_cxx_compiler`: a folder outside the project or a program
  to run) are read-only over MCP (`set_project_settings`: "change it in Edit > Preferences"). The roots are resolved when the project loads (or a root setting is
  applied) and **pinned** (`Project::getTrustedRoots`, `safewrite::pinTrustedRoot`): never resolved
  again, so a root moved away and replaced by a link is refused, not followed. A tool can't move,
  rename or delete a trusted root or a folder holding one, nor move anything into a root's place
  (`McpRefs::trustedRootTouched`); `move_asset` never moves into a hidden or editor-owned place
  (`.jasperengine`, `.trash`, project.yaml ...) and never moves a link that leads out of the
  project, or a folder holding one at any depth (copying what it holds is fine). Build, Play and
  script compiles refuse an internal folder holding a link out of the project (any depth, links
  not followed so loops are harmless, bounded at 200,000 entries: `McpRefs::linkOutInside`). `build_project` /
  `generate_shaders` build only into the project's chosen export folder, the default export
  location (or below it) or a folder of the project, and never save an agent's folder as the
  export folder (both outside folders are the user's: neither can be set over MCP); `run_build`
  runs only the last build's executable or a player inside a folder builds are allowed into, never
  an arbitrary program. `create_project` (and Save As) needs a new or empty folder beside the
  current project or under the editor's default projects/export folder, never inside the open
  project (a nested project would be a trusted root an agent chose). A delete or
  a move's source acts on the entry itself: a link there is trashed or moved, never followed.
  Refused: isError naming the path and where the link leads, with the fix ("use a path that is not
  behind a link to elsewhere, or replace the link with a real folder").
- **Threat model.** The sandbox stops misdirected or accidental agent writes (a wrong path, a
  stale assumption about a folder) and link tricks that would carry a write, delete or move
  outside the trusted roots or into the editor's own files. It does **not** contain a
  deliberately malicious agent: project content executes code by design (C++ scripts are compiled
  and loaded into the editor on Play, `ProjectBuild.cmake` runs at CMake configure time, Lua runs in
  Play), and an agent may edit all of it. What only the user decides stays out of the agent's reach:
  machine-level preferences (programs to run, folders outside the project), the export folders,
  and which project (with which roots) is opened with roots outside its folder.
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
  without changes of its own — except where it writes an editor-owned or hidden place (project.yaml,
  the workspace and other `.jasperengine` files, build logs, the trash, generated sources), which
  the deny-by-default guard refuses unless that writer holds a narrow `safewrite::EditorInternal`
  token for the one path it writes (a token is thread-local and not inherited by a worker, so the
  writer names it where it acts). Paths outside the project (another project, the editor's settings)
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
  the last call. The editor's own writes (origin Editor) are left out; the baseline only follows
  them. When the watcher is off, degraded, fell behind its 65,536-entry
  journal or the flush timed out, the stat scan (`source: "scan"`): stat-based against a cached
  map (mtime + size) of the project's tracked files (Lua, C++
  `.h/.hpp/.hh/.hxx/.inl/.c/.cc/.cpp/.cxx`, shaders `.vert/.frag/.glsl`, `.material`, HTML UI
  `.html/.htm/.css`, pose files `.poseanim/.humanoid`, animation clips `.anim`, Vertex
  Animations `.vat`; models `.gltf/.glb/.obj/.fbx` and glTF `.bin` buffers, picked up but not
  reported; assets: images (`.svg` included), fonts `.ttf/.otf`, `.json/.lottie`) and folders
  (mtime: an added, removed or renamed entry), skipping hidden folders (`.jasperengine`,
  `.git`...), `build`, `node_modules` and `trash`. `.scene`, `.prefab`, `project.yaml` and the
  terrain maps the editor writes from its in-memory terrain (`terrain_maps/terrain_edit_*.png`)
  are not tracked: the editor owns them. Files changed this session, folders and code files (HTML
  UI included) are checked whole on every call (up to 2000 of each); assets rotate within a ~2 ms
  budget. The baseline is taken (in `McpServer::tick`) when a project opens.
- **Pick-up**: changed paths go through `w4::syncFiles` (ScriptTools.cpp), which is
  `HotReloadService::apply` (editor/reload, the one reload funnel of the watcher, `sync_files`,
  Assets > Refresh and the code editor's saves): clean code editor buffers reloaded, script
  properties re-parsed, Lua syntax checked, textures, materials, models, audio and fonts decoded
  on workers and swapped in, the shader forks whose sources or includes hold a changed file
  compiled before they swap, HTML UI documents reloaded, the open `PoseAnimationDocument`s,
  `AnimationClipDocument`s and `VatDocument`s of changed `.poseanim`/`.anim`/`.vat` files
  reloaded (`AnimationClipPool` drops the clips and the Animation Players using them reload,
  `VertexAnimationPool` drops the Vertex Animations and their Vertex Animators reload, during play
  too; a changed model drops its sub-clips the same way, a changed `.vat.glb` its `.vat`), asset
  database and Project window updated. The call waits (`waitUntil`, 10 s at most) for the reloads
  it started; their failures (the previous version kept), the forks' compile errors, the load
  errors and warnings of `.anim`, `.poseanim` and `.vat` files and a `.vat` / `.vat.glb` mismatch
  are reported with the call. A path the watcher reloaded before the call is current (the
  applied-stamp table): reported, with its `reloaded` entry, not reloaded again.
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
  read-only, or with `save_path`), the settle (`McpAutoSync::afterCall`) takes every change it
  finds as the tool's (absorbed, not re-run: a texture reload would undo a terrain tool's in-memory
  edit), except changes stamped before the handler started, which the next call reports. It reads
  the journal behind `FileWatchService::flushJournal` where that barrier only drains what the
  system holds (inotify, ReadDirectoryChangesW: no reload runs in it, the next pump delivers), and
  scans where it does not (FSEvents' marker round trip, polling's walk) or the watcher is off.
  While such a call runs the hot reload holds the watcher's batches, so the call's writes reload
  once, with its sync.
- **The result waits for its settle** (S70): the completion queues `{tool, start, respond}`
  (`McpServer::queueSettle`) and the queue runs on the main thread where no handler is on the
  stack: right after the handler returns (a synchronous call gains no frame), at the end of
  `McpServer::tick` (`waitUntil` / `afterFrames` callbacks), in a task the completion posts when it
  comes from anywhere else (a job's thread, another main-thread task), and in `McpServer::stop`.
  So whatever the client writes once it has the result is newer than the settle: reported by the
  next call, 0 ms later and whatever its stamp. Read-only calls answer at once. A call refused
  before the main thread took it (`answerDuringFilePicker`, `onFilePickerOpened`) never started, so
  it has no settle. A result completed while a file picker holds the main thread goes out at once,
  because its client may have to answer the picker first, and its settle runs when the thread is back.
- **Racy stamps** (S70): a stamp read less than 2 s after its mtime can't rule out a later write
  in the same clock tick, and the watcher's journal can't either (it compares the same stat). A racy
  file's content hash (FNV-1a; past 1 MB, of its first and last 64 KB) is recorded with its stamp and
  checked by every pick-up and settle, newest first within the scan's time budget, until it is read
  2 s after its mtime; the stat scan lists a racy folder again the same way (the journal has
  the new file). Another hash with the same stamp is a modification, reloaded even if the hot reload's
  applied stamp says current (`HotReloadService::forgetApplied`). A folder linked in from outside the
  project (S68) is checked the same way, under the link's name; the watcher's journal names its
  changes by that name too.
- **Not told apart**: a program writing project files while a call runs, stamped from inside the
  call, looks like the tool and is absorbed. Telling them apart would need every editor write path
  (about 200 sites, undo and redo of file commands included) to report its writes.
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
