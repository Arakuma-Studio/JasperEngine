# JasperEngine MCP server

The JasperEngine editor has a built-in [Model Context Protocol](https://modelcontextprotocol.io)
server. Coding agents (Claude Code, Codex, Cursor, or any MCP client) use it to drive the real
editor, doing what you would do by hand:

- create and edit scenes, GameObjects, components, prefabs (variants included), materials,
  shaders, and Lua or C++ scripts
- build HTML/CSS game UI (menus, HUDs) and check its layout
- synthesize textures from example images (seamless tiling, inpainting, PBR sets), as Window >
  Texture Synthesis does for you ([guide/texture-synthesis.md](guide/texture-synthesis.md))
- generate normal maps for sprites, sheets, GIF/APNG animations and tilesets so 2D lights give them volume, with a
  lit preview before writing and paint corrections kept on regeneration, as Window > Sprite Normal Map does for you
  ([guide/sprite-normals.md](guide/sprite-normals.md), `generate_sprite_normals`, `check --normals`)
- place objects with physics in edit mode: drop them onto what is below, push overlapping ones
  apart, slide them against walls, snap them onto surfaces, with a preview before the commit
  ([guide/physics.md](guide/physics.md), `place_with_physics`)
- bake animations into Vertex Animations for crowds animated on the GPU, or import Houdini and
  OpenVAT exports, with a preview before writing
  ([guide/vertex-animation.md](guide/vertex-animation.md), `bake_vertex_animation`)
- play the game, send keyboard, mouse and touch input, inspect runtime state, and take
  screenshots of the Scene and Game views
- simulate a phone in the Game view (model, dpi, rotation), another system language, and the
  app's focus, pause and low-memory events, and see the URLs the game opened
  ([guide/platform-services.md](guide/platform-services.md), `send_input` `app` events, `configure_game_view`)
- see the phone's notch, rounded corners and safe area over the game (and in screenshots), and
  check UI against `Screen.safeArea` ([guide/safe-area.md](guide/safe-area.md), `configure_game_view`
  `device`, `screenshot` `safe_area_in_image`)
- read the Console, then export and build the game

Every edit goes through the editor's command system. You can undo it with Ctrl+Z, and if you
have the editor open you see it happen. The in-editor AI chat it replaces is gone: you bring
your own agent, model and API account.

The server also gives the agent a guide to the engine: Unity terms mapped to engine names,
how to address objects, the edit, play and fix loop, scripting rules, and physics rules. The
guide is sent when the agent connects and is also available as the resource `jasper://guide`.
Longer references are under [guide/](guide/), for example the [Lua API](guide/lua-api.md) and
[HTML/CSS UI](guide/html-ui.md) with its generated [support matrix](guide/html-ui-support.md).
The design is in [DESIGN.md](DESIGN.md).

## Two ways to connect

| | stdio (the agent starts the editor) | Streamable HTTP (attach to your open editor) |
|---|---|---|
| Use it for | Agents working alone, CI, servers without a screen | Working next to the agent in the editor |
| Start | The client runs `jasperengine-editor mcp --project <dir> [--headless]` | The MCP Server window (Window > MCP: Serve over HTTP, port 47615 by default), or `jasperengine-editor --mcp-port 47615` |
| Endpoint | The editor process's stdin and stdout | `http://127.0.0.1:47615/mcp` |
| Authentication | None: only the client that started the editor can talk to it | A bearer token, made the first time HTTP is enabled; the MCP Server window copies it and the connect command |
| Lifetime | The editor quits when the client disconnects | The editor stays open and agents come and go |

### The `mcp` command line

```
jasperengine-editor mcp [--project <dir|project.yaml>] [--create <dir> [--name <Name>]]
                        [--headless] [--http-port <N>]
```

- `--project` opens an existing project. You can give the project folder or its
  `project.yaml`.
- `--create <dir> --name <Name>` creates a new empty project in `<dir>` and opens it.
- `--headless` (Linux): if `DISPLAY` is not set, the editor starts its own virtual X server
  (`Xvfb`) and stops it when it exits. The editor window still exists, so screenshots and
  play mode work, but nobody sees it.
- `--http-port N` also serves HTTP on `127.0.0.1:N`, so you can attach a second client to an
  editor that a stdio client started.

In stdio mode, stdout carries only the protocol. Everything else the editor prints, including
Lua `print` output, goes to stderr. The editor keeps `settings.yaml`, `editor.log` and its
window layout `.ini` in its working directory when that directory already has a
`settings.yaml` (or is writable and you have no per-user settings yet), otherwise in your
per-user config directory. When several editors run at once, give each one its own working
directory with its own `settings.yaml`.

## Client setup

In the examples below, replace `/opt/jasperengine/bin/jasperengine-editor` with your editor
binary (in a source build it is `build/linux/editor-install/bin/jasperengine-editor`) and
`/home/me/games/MyGame` with your project folder.

Run the agent with the project folder as its working directory. The project is a normal
folder of files: the agent reads, edits and greps the Lua scripts, C++ sources, shaders and
materials with its own tools (and git), and uses the MCP tools for the editor itself (scenes,
GameObjects, play, screenshots, checks). See [Code](#code-scripts-shaders-materials) below.

### Claude Code

From the project folder (the server is then registered for that folder):

```sh
cd /home/me/games/MyGame
claude mcp add jasper -- /opt/jasperengine/bin/jasperengine-editor mcp --project . --headless
claude
```

Attach to an editor you have open, after enabling HTTP in the MCP Server window (Window >
MCP). Its "Copy connect command" button copies this line with your token filled in:

```sh
claude mcp add --transport http jasper http://127.0.0.1:47615/mcp --header "Authorization: Bearer <token>"
```

To share the setup with your team, put a `.mcp.json` file in the game's repository:

```json
{
  "mcpServers": {
    "jasper": {
      "type": "stdio",
      "command": "${JASPER_EDITOR:-jasperengine-editor}",
      "args": ["mcp", "--project", ".", "--headless"]
    }
  }
}
```

Claude Code expands `${VAR}` and `${VAR:-default}` in `.mcp.json`. The editor runs in the
client's working directory (here the game's repository) and may keep `settings.yaml`,
`editor.log` and `editor_layout_v2.ini` there: add them to the game's `.gitignore`. The editor needs time to
start, and longer on a project's first open because it compiles shaders and C++ scripts. If
the server times out, raise `MCP_TIMEOUT` (startup, in ms), for example
`MCP_TIMEOUT=120000 claude`. Play mode with a C++ build and exports run as jobs, so tool calls
return before `MCP_TOOL_TIMEOUT` and the agent checks on them with `get_job`.

### Codex

Start Codex in the project folder (`cd /home/me/games/MyGame && codex`, or
`codex -C /home/me/games/MyGame`). `~/.codex/config.toml` (or the project's
`.codex/config.toml`):

```toml
[mcp_servers.jasper]
command = "/opt/jasperengine/bin/jasperengine-editor"
args = ["mcp", "--project", "/home/me/games/MyGame", "--headless"]
startup_timeout_sec = 120
tool_timeout_sec = 600
```

Attach to an editor you have open over Streamable HTTP:

```toml
[mcp_servers.jasper]
url = "http://127.0.0.1:47615/mcp"
bearer_token_env_var = "JASPER_MCP_TOKEN"   # export JASPER_MCP_TOKEN=<the token from the MCP Server window>
```

You can also add the stdio server from the command line:
`codex mcp add jasper -- /opt/jasperengine/bin/jasperengine-editor mcp --project /home/me/games/MyGame --headless`.

### Cursor, Claude Desktop, Windsurf and other clients

Most clients use the same JSON shape. Cursor reads it from `~/.cursor/mcp.json` or the
project's `.cursor/mcp.json`, and Claude Desktop from `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "jasper": {
      "command": "/opt/jasperengine/bin/jasperengine-editor",
      "args": ["mcp", "--project", "/home/me/games/MyGame", "--headless"]
    }
  }
}
```

For HTTP, use `{"mcpServers": {"jasper": {"url": "http://127.0.0.1:47615/mcp",
"headers": {"Authorization": "Bearer <token>"}}}}` with the token from the MCP Server window.

VS Code reads `.vscode/mcp.json` and uses a `servers` key with
`"type": "stdio"` or `"type": "http"`.

### From a shell: `jasperengine-editor call`

Without an MCP client (CI scripts, hooks, a terminal), run any tool through the editor binary:
`jasperengine-editor call --project <dir> create_entity --name Box --type box`. It attaches to
the editor that has the project open (or `--port N`), else starts a one-shot headless one.
Flags, `--help` and validation come from each tool's schema; `--json` prints one envelope per
call; exit codes 0 ok, 1 tool error, 2 usage, 3 connection, 4 internal. See [cli.md](cli.md).

### Developing JasperEngine itself

Agents working on the engine read the repository's `AGENTS.md` (build, test, run an isolated
editor, land a change); a game project has its own, generated `AGENTS.md`.

The repository root has a `.mcp.json` that runs the editor from your build output:

```json
{"mcpServers": {"jasper": {"type": "stdio",
  "command": "${JASPER_EDITOR:-build/linux/editor-install/bin/jasperengine-editor}",
  "args": ["mcp", "--project", "${JASPER_PROJECT:-/path/to/your/test/project}", "--headless"]}}}
```

Set `JASPER_PROJECT` to a throwaway project created with `--create` before you start
Claude Code in the repository. The agent edits and saves that project, so use a copy
when starting from a shared fixture. The path is resolved from
the directory where you start the client, so start it at the repository root (or set
`JASPER_EDITOR` to an absolute path).

## Code (scripts, shaders, materials)

Agents edit code with their own tools, not through MCP file tools:

- `editor_status` returns the project's absolute path (`project.path`).
- The engine API is in `.jasperengine/engine-api/`: C++ headers, and Lua API stubs (LuaLS
  annotations) in `.jasperengine/engine-api/lua/`, written whenever the editor creates or
  opens a project. lua-language-server reads the generated `.luarc.json` (whose
  `workspace.library` is the stubs folder, and `runtime.plugin` the plugin that checks
  assignments such as `Engine.timeScale = 0.5`); clangd reads the generated `.clangd`, which points
  it at `.jasperengine/clangd/compile_commands.json`. A Lua script whose module is declared
  `---@class <Name> : ScriptInstance` (the editor's templates do it) gets `self` typed,
  properties included: the editor writes each script's class from its `properties` table
  (`.jasperengine/engine-api/lua/scripts/<Name>.lua`). See
  [guide/editor-integration.md](guide/editor-integration.md).
- **Changes are picked up automatically.** The editor watches the project folder (inotify,
  ReadDirectoryChangesW, FSEvents, or polling; hidden folders such as `.jasperengine` and `.git`,
  `build`, `node_modules` and `trash` skipped) and reloads what an edit touches as it lands
  ([../hot-reload.md](../hot-reload.md)). Before every tool call the server also takes the
  watcher's changes since the last call (after a flush; when the watcher is off or degraded,
  a stat scan of the project) for Lua, C++, shader, `.material`, HTML UI (`.html`, `.htm`,
  `.css`) and asset files (images and `.svg`, fonts `.ttf`/`.otf`, `.json`/`.lottie`). If any
  changed, the editor reloads clean code editor buffers, re-parses script properties, compiles
  the shader forks that use a changed shader, reloads materials, textures, models, audio and
  fonts and refreshes the Project window, waits for those reloads (10 s at most), and that
  call's result carries `external_changes: {changed, added, removed, source, reloaded,
  pending, problems: [{path, line, message}]}` and one text line. A reload that failed keeps
  the previous version and is a problem. A change the watcher reloaded already is reported
  once and reloaded once. UI documents that use a changed file reload before the tool runs,
  in edit mode and while playing (a stylesheet change restyles in place, keeping elements and
  listeners): `external_changes` lists them in `ui_reloaded`, and their findings (and those of
  a changed `.html`/`.css` no document uses) as `problems` with `col`, `severity`, `code` and
  `suggestion`, the same as `jasperengine-editor check --ui`. Script changes made while a scene
  plays are listed as `deferred` and picked up when play stops. Files the MCP tools write
  themselves are not reported. `.scene`, `.prefab`, `project.yaml` and the terrain maps the
  terrain tools write are not picked up: edit them with the tools (an open scene changed on
  disk is never reloaded on its own). `sync_files` forces a full rescan (`wait`,
  `rebuild_models`, `auto_refresh`); `editor_status.file_watcher` shows the watcher and the
  reloads, `editor_status.auto_sync` the pick-up's source, size and cost.
- Validate without playing: `check_script`, `check_shader`, `compile_scripts`, or
  `jasperengine-editor check` from a shell (see [checks.md](checks.md)). As a Claude Code
  PostToolUse hook (or Codex hook), `jasperengine-editor check --project "$CLAUDE_PROJECT_DIR" --hook`
  checks every file the agent edits, through its shell too (matcher `...|Bash`), and feeds the
  errors back to it: the settings snippet is in [checks.md](checks.md#agent-hooks).
- `write_file` / `read_file` / `search_files` are for clients without access to the project
  folder (an HTTP client on another machine).

A scan checks folders and code files whole on every call (up to 2000 of each) and images in
rotation within a ~2 ms budget, so it costs about 0.1 ms per call on a small project and a few
milliseconds on one with tens of thousands of files; an image changed deep in a huge asset
folder can take a while to be noticed (`sync_files` finds it at once).

## Servers without a screen

On a server, use `--headless` and install `Xvfb` (Debian or Ubuntu: `apt install xvfb`). Use
it even if your machine has a display, unless you want the editor window to appear on it. If
there is no GPU, Mesa's software renderer works: set `LIBGL_ALWAYS_SOFTWARE=1` in the
server's `env` if the editor fails to create an OpenGL context. Screenshots and play mode
work under Xvfb. On Windows and macOS the editor needs a desktop session; `--headless` has
no effect there.

## Security

- **stdio** opens no network port. Only the client that started the editor can talk to it.
- **HTTP** listens on `127.0.0.1` only. It rejects requests whose `Origin` header is not
  localhost, which blocks DNS-rebinding attacks from web pages you visit, and every request
  must send `Authorization: Bearer <token>`, so other programs and users on the machine can't
  drive the editor. The editor makes the token the first time HTTP is enabled and keeps it in
  `mcp_http_token` next to `settings.yaml`, a file only your user can read (mode 0600); the MCP Server window shows it, copies it and the connect command,
  and "New token" replaces it (clients then need the new one). To reach an editor on another
  machine, use an SSH tunnel (`ssh -L 47615:127.0.0.1:47615 host`). Never expose the port
  directly.
- **Read-only mode**: turn off "Allow changes" in the MCP Server window and HTTP clients
  only get the tools that change nothing (inspecting, screenshots, reading files and the
  Console, checks): `tools/list` leaves the others out, calling one fails, `batch` is
  not available, and arguments that write files (`screenshot`'s `save_path`) are refused. Clients that cached the tool list may need to list it again (reconnect).
  stdio clients keep every tool: they started the editor themselves.
- Give the agent the same trust you would give a developer on the project. It can write any
  file in the project (with its own tools, or through `write_file`), and C++ scripts it writes are compiled and run inside the editor
  process when you press Play. The agent cannot leave the project folder: asset paths must
  stay inside it, and only the explicit import tools read files from elsewhere.
- Every call is listed in the editor's MCP activity window, and every edit can be undone.

## Tool reference

The tool list is generated: [TOOLS.md](TOOLS.md) is written by `tools/mcp/gen_tool_docs.py`
from the editor's `tools/list` (every group), in sections by module, with each tool's group,
description and parameters. Regenerate it after you change tools (`--check` tells whether
it is stale):

```sh
uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/gen_tool_docs.py
```

**Tool groups.** `tools/list` shows the `core` tools plus the groups a session enabled. The
`tool_groups` tool lists the groups and enables or disables them for the session; the
editor then sends `notifications/tools/list_changed`. A hidden tool is still callable by
name, directly or inside `batch`, so a client that ignores the notification loses nothing.
`jasperengine-editor mcp --tool-groups a,b|all` and the Editor Settings key
`mcp.tool_groups` choose the groups sessions start with.

Main ideas:

- **Scenes** are referred to by id, name or `.scene` path. If you leave the scene out, the
  selected scene is used.
- **GameObjects** are referred to by id or by path: names joined with `/` from the scene
  root, with `#N` for the Nth earlier sibling that has the same name (`Player/Gun`,
  `Enemy#2`).
- **Components** use their catalog names (`transform`, `mesh`, `rigidbody2d`).
- **Properties** use catalog property paths (`position`,
  `submeshes[0].material.baseColorFactor`, `scripts[0].speed`).
- **Assets** use paths relative to the project.
- `batch` runs many calls in one round trip, as one undo step (`undo_group`).
- Every edit is an undo step named after your client ("Claude Code: Move Player"), returned as
  `undo` {id, name}; `undo`/`redo` with `scope: "timeline"` are the editor's Ctrl+Z/Ctrl+Y, and
  `expect_top_id` undoes only your own last call. See [docs/undo.md](../undo.md).
- Long operations return a job: poll it with `get_job` and stop it with `cancel_job`.
- The engine's job system and memory are separate from those jobs:
  - `get_job_system_stats` shows the workers, lanes, the Background gate and the asset reads.
  - `get_memory_stats` shows the named memory records (textures, meshes, Lua, Jolt, arenas).
  - `set_job_system_config` changes Reserved Frame Workers live (undo with scope `project`).

  See [../jobs-memory.md](../jobs-memory.md).

Tool definitions (name, description, input schema) are sent to the agent in every session,
so keep them lean. [conventions.md](conventions.md) has the rules every tool follows (names,
schemas, groups) and the budget: the default listing (`core`) stays within 100,000 characters
(about 25K tokens) and 150 tools, each group within 15,000 characters and 12 tools, each tool
within 1,500 characters; `tools/mcp/budget.json` holds the limits and `tool_budget.py --check`
gates them. Shared conventions (scene/entity references, value shapes, undo) belong in the
server instructions (`McpInstructions.cpp`), not in every tool.

```sh
uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py [--top 20] [--groups all]
uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py --check tools/mcp/budget.json
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| The client says the server timed out while starting | The first open of a project compiles shaders and C++ scripts. Raise the client's startup timeout (Claude Code: `MCP_TIMEOUT`; Codex: `startup_timeout_sec`). |
| `cannot open display`, or the server exits at once | No `DISPLAY`. Add `--headless` and install `Xvfb`. Read the editor's stderr: Claude Code `claude --debug`, Codex `~/.codex/log`, or run the command yourself in a terminal. |
| OpenGL or GLX errors under Xvfb | Set `LIBGL_ALWAYS_SOFTWARE=1` in the server's env and install Mesa (`libgl1-mesa-dri`). |
| `Failed to parse JSONRPC message` in the client | Something wrote to stdout that is not protocol. Report it as a bug; stdio mode redirects all output to stderr. |
| HTTP: connection refused | HTTP is off in the MCP Server window, the port is different, or another program already uses it (the editor logs this in the Console). |
| HTTP: 401 | The bearer token is missing or wrong (or was replaced with "New token"). Copy the connect command again from the MCP Server window. |
| HTTP: 403 | The request's `Origin` or `Host` is not localhost. |
| HTTP tools fail with "Changes are turned off" | "Allow changes" is off in the MCP Server window: only read-only tools run over HTTP. |
| Edits don't show in the Hierarchy | They went to another scene. Pass `scene` explicitly, or check the selected scene with `editor_status`. |
| A script "works" but nothing happens | Play, then read the Console with the console tool: Lua errors and `Script crash in scene ...` entries are there. |
| Two editors fight over settings | Give each editor its own working directory containing a `settings.yaml`; otherwise they share the per-user one. |

The editor writes `editor.log` in its working directory. The MCP activity window (Window
menu) shows connected clients and recent calls with their results.

## Testing the server (contributors)

`tools/mcp/` has a Python test harness built on the official `mcp` SDK. It runs the editor
in a scratch copy of a project under Xvfb:

```sh
tools/mcp/run-tests.sh                 # every tools/mcp/tests/test_*.py
tools/mcp/run-tests.sh -k batch -j 4   # filter by name, run 4 files at a time
tools/mcp/run-tests.sh --fake tools/mcp/tests/test_protocol.py  # protocol checks against a fake server
tools/mcp/run-tests.sh --suite ci -j 2 # the CI suite (below)
```

The harness API and conventions are described in the docstrings of `tools/mcp/harness.py`
and `tools/mcp/run_tests.py`. Every tool needs a test. The harness's Python packages are pinned
in `tools/mcp/requirements.txt`. The files that drive the editor with real X input (`xvfb:
"harness"`: the Animation window and Record tests, Physics Placement's Scene view gesture) need
`xdotool` (`apt install xdotool`); without it the Animation window and Record tests fail.

Tests that drive the editor with real input (xdotool, `EDITOR = {"xvfb": "harness", ...}`) use
`tools/mcp/ui_input.py`: its `Ui` waits for editor frames after every step instead of sleeping,
puts a press and its release, and each step of a drag, in different frames, keeps two clicks
apart by more than ImGui's double-click time, holds a chord's modifiers across frames, and finds
controls where `find_ui_item` says they are drawn now. A host drawing a frame a second runs them
as a fast one does. `Ui.reveal(label, window)` wheels a panel until the control is in view and
`Ui.click_revealed` clicks it there: no position or offset is measured in a test, so a layout
change cannot move a click off its control (a missing control fails the test, it never skips).
Labels are ImGui's: the whole label, its `##` id, or its text (with or without a leading icon);
an item's `enabled` says whether it is drawn disabled.

### The CI suite (`CI = True`)

This suite is the test files that declare `CI = True` at module level
(`tools/mcp/run-tests.sh --suite ci`, or `MCP_SUITE=ci tools/build.sh mcp-tests -j 2`). A clean
checkout has no copy of the default fixture project (`build/fixture-unity`, which is not in git), so
such a file must not need it: its editor tests run on a project they make, `EDITOR = {"create": True, ...}`,
or on a committed one, `EDITOR = {"project": ".../tools/fixtures/<name>"}`. Tests without an
editor parameter need neither. The runner checks this and exits with status 2 when a `CI = True`
file relies on the default fixture.

No file is left out of this suite silently: a file that is not `CI = True` either needs the default
fixture (the runner finds that itself) or declares why, `CI_EXCLUDED = "long-running (...); in the
full suite"`. A file left out for neither reason stops `--suite ci` with status 2. Every run of it
prints how many files it runs and lists each one it leaves out with its reason, and the summary
line and `--json` (`left_out`) repeat it. `tools/build.sh mcp-tests` runs the CI suite by default:
a gate that means the whole suite runs `MCP_SUITE=all`. `tools/ci/check-mcp-ci-suite.py` says the
same in a second, without a build: check-changed runs it when a diff touches an MCP test file.

Write new test files CI-capable by default: create the project (or use `tools/fixtures/*`), then
declare `CI = True` when the file takes under a minute or so on a loaded server, and `CI_EXCLUDED`
with the reason when it takes longer or builds a real player. The full suite, default-fixture files
included, runs with `MCP_SUITE=all tools/build.sh mcp-tests`, which finds `~/je/fixture-unity` or
`JASPER_FIXTURE=<fixture>`. A test reaches the fixture through `harness.default_fixture()` (or
`Editor()` with no project): `JASPER_FIXTURE`, `build/fixture-unity` of this or the main checkout,
`/fixture` (the build image's mount) or `~/je/fixture-unity`; with none of them its tests skip.
`docs/testing.md` covers every other test suite.
