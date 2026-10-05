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
  McpSchema.{h,cpp}       JSON Schema builders (objectSchema, vec3Schema, refSchema...)
  McpInstructions.cpp     const char* mcpServerInstructions(): the agent guide text
  McpActivityWindow.{h,cpp}  editor panel: server status, connected clients, call log
  legacy/                 the old EditorActionRegistry/Executor + helpers, namespace mcp::legacy
  tools/
    LegacyTools.cpp       adapter: registers every legacy action as an MCP tool
    CoreTools.cpp         batch, get_job, cancel_job, undo/redo, editor_status
    ProjectTools.cpp      W5
    SceneTools.cpp        W1
    EntityTools.cpp       W1
    ComponentTools.cpp    W1
    PrefabTools.cpp       W2
    PlayTools.cpp         W3
    ViewTools.cpp         W3
    ConsoleTools.cpp      W3
    ScriptTools.cpp       W4
    AssetTools.cpp        W4
    DocsTools.cpp         W4
    BuildTools.cpp        W5
    WorldTools.cpp        W6
```

Every `tools/*.cpp` defines one function, e.g. `void registerSceneTools(McpRegistry&)`;
`McpServer` calls them all in a fixed order (Legacy first, then the rest). The core creates
every file above (empty register functions where not yet implemented) and lists them all
in `CMakeLists.txt`, so workstreams never need to touch CMake or each other's files.

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
    void addTool(ToolDef def);           // same name replaces (modules override legacy tools)
    void removeTool(const std::string& name);
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

`batch` (CoreTools): `{calls: [{tool, arguments}], undo_group: bool, stop_on_error: bool}`
runs several tool calls in one round-trip, sequentially on the main thread; with
`undo_group` all scene edits are one Ctrl+Z step where possible.

## Addressing (McpRefs)

- **Scene**: `scene` = numeric id, scene name, or project-relative `.scene` path. Omitted →
  the selected scene. During play, tools that read runtime state resolve to the playing
  runtime scene.
- **Entity** (GameObject): `entity` = numeric id, or a *path*: `/`-joined names from the
  scene root with `#N` for the Nth earlier same-name sibling (`Player/Gun`, `Enemy#2`) —
  the same scheme as `scene-dump` (`dumpPath`/`findDumpEntity` move to
  `editor/util/EntityPath.{h,cpp}` so CLI and MCP share it). Ambiguous names are an
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

## Shared editor enablers (done before the tool workstreams)

- `editor/util/EntityPath.{h,cpp}` shared with the CLI.
- Console: structured, cursor-based log reads (`id`, `type`, `message`, timestamp) and
  clear; Lua `print` redirected into the Console (and stderr) in editor and play.
- `SceneRender::getFramebuffer()`; PNG encode to memory (stb `stbi_write_png_to_func`).
- Non-interactive Project APIs: create a scene and get its id, open/close a scene, save
  a scene to a path, create/open a project, without modal prompts (`choice` parameters).

## Workstreams

| W | Owner files | Scope |
|---|---|---|
| Core | `editor/mcp/*` (not tools/ modules), `editor/main.cpp`, `App.*`, `AppSettings.*`, CMake, removal of `editor/ai`, `AiChatWindow`, `AiSettingsWindow`, libcurl | server, transports, registry, calls/jobs/batch, legacy adapter, activity window, CLI |
| Enablers | `editor/util/EntityPath.*`, `Out.*`, `ConsoleWindow.*`, Lua print, `SceneRender.*`, `GraphicUtils.*`, `Project.*` non-interactive APIs, `CommandLine.cpp` | see above |
| Harness | `tools/mcp/*`, `docs/mcp/*` (except this file), `.mcp.json`, `McpInstructions.cpp` | Python MCP test harness, agent guide, client setup docs |
| W1 | `SceneTools.cpp`, `EntityTools.cpp`, `ComponentTools.cpp`, `McpValues.*`, `McpRefs.*` | scenes, hierarchy, GameObjects, components, all property types, selection, clipboard, scene settings |
| W2 | `PrefabTools.cpp` | full prefab workflow incl. variants, overrides, Prefab Mode |
| W3 | `PlayTools.cpp`, `ViewTools.cpp`, `ConsoleTools.cpp` | play control, runtime inspection + Lua eval, input injection, screenshots, scene camera, console |
| W4 | `ScriptTools.cpp`, `AssetTools.cpp`, `DocsTools.cpp` | Lua/C++ scripts, compile check, code editor sync, assets/import, materials, shaders, engine API docs/resources |
| W5 | `ProjectTools.cpp`, `BuildTools.cpp` | project lifecycle and settings, scenes in build, async export/build/run players |
| W6 | `WorldTools.cpp` | terrain, tilemaps, sprites/animation, keyframes, particles, physics colliders/joints, UI widgets, audio |

Rules for every workstream:
- Prefer fewer, more powerful tools with excellent descriptions and schemas over many
  narrow ones. A tool description says what it does, what it returns, and the common
  mistakes. Target well under 150 tools in total.
- Replace legacy tools in your area by registering the same name (or `removeTool` the
  legacy name and add a better one). Do not edit `editor/mcp/legacy/*` except for a bug fix.
- Edits through commands (undoable); file writes refresh the Project window, the asset
  database and open code editor buffers.
- Every tool has a test in `tools/mcp/tests/` run through the harness against a real
  editor under Xvfb.

## Agent guide (instructions)

`initialize.instructions` and the `jasper://guide` resource carry the agent guide: how
the engine maps to Unity, scene/entity addressing, the build loop (edit → play → screenshot
→ read console → fix), Lua and C++ scripting conventions, physics rules, shader forking —
ported from the old in-editor agent's system prompt (`AiService::buildSystemPrompt`) and
extended for the new tools.
