# JasperEngine MCP server

The JasperEngine editor has a built-in [Model Context Protocol](https://modelcontextprotocol.io)
server. Coding agents (Claude Code, Codex, Cursor, or any MCP client) use it to drive the real
editor, doing what you would do by hand:

- create and edit scenes, GameObjects, components, prefabs (variants included), materials,
  shaders, and Lua or C++ scripts
- build HTML/CSS game UI (menus, HUDs) and check its layout
- play the game, send keyboard, mouse and touch input, inspect runtime state, and take
  screenshots of the Scene and Game views
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
| Start | The client runs `jasperengine-editor mcp --project <dir> [--headless]` | Editor Settings > MCP (enable it, port 47615 by default), or `jasperengine-editor --mcp-port 47615` |
| Endpoint | The editor process's stdin and stdout | `http://127.0.0.1:47615/mcp` |
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

Attach to an editor you have open, after enabling MCP in Editor Settings:

```sh
claude mcp add --transport http jasper http://127.0.0.1:47615/mcp
# with a token set in Editor Settings > MCP:
claude mcp add --transport http jasper http://127.0.0.1:47615/mcp --header "Authorization: Bearer $JASPER_MCP_TOKEN"
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
# bearer_token_env_var = "JASPER_MCP_TOKEN"   # if you set a token in Editor Settings > MCP
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

For HTTP, use `{"mcpServers": {"jasper": {"url": "http://127.0.0.1:47615/mcp"}}}`. Add
`"headers": {"Authorization": "Bearer <token>"}` if you set a token.

VS Code reads `.vscode/mcp.json` and uses a `servers` key with
`"type": "stdio"` or `"type": "http"`.

### Developing JasperEngine itself

The repository root has a `.mcp.json` that runs the editor from your build output:

```json
{"mcpServers": {"jasper": {"type": "stdio",
  "command": "${JASPER_EDITOR:-build/linux/editor-install/bin/jasperengine-editor}",
  "args": ["mcp", "--project", "${JASPER_PROJECT:-/path/to/your/test/project}", "--headless"]}}}
```

Set `JASPER_PROJECT` to a throwaway project before you start Claude Code in the repository,
for example a copy of `build/fixture-unity`, or a folder made with `--create`. The agent edits
and saves that project, so never point it at the fixture itself. The path is resolved from
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
- **Changes are picked up automatically.** Before every tool call the server looks for Lua,
  C++, shader, `.material`, HTML UI (`.html`, `.htm`, `.css`) and asset files (images and
  `.svg`, fonts `.ttf`/`.otf`, `.json`/`.lottie`) changed on disk since the last call (a stat
  scan of the project, skipping hidden folders such as `.jasperengine` and `.git`, `build`,
  `node_modules` and `trash`). If any changed, the editor reloads clean code editor buffers,
  re-parses script properties, compiles the shader forks that use a changed shader, refreshes
  materials, textures and the Project window, and that call's result carries
  `external_changes: {changed, added, removed, problems: [{path, line, message}]}` and one
  text line. UI documents that use a changed file reload before the tool runs, in edit mode
  and while playing (a stylesheet change restyles in place, keeping elements and listeners),
  once even though the editor's own watcher also sees the change: `external_changes` then
  lists them in `ui_reloaded`, and their findings (and those of a changed `.html`/`.css` no
  document uses) as `problems` with `col`, `severity`, `code` and `suggestion`, the same as
  `jasperengine-editor check --ui`. Other changes made while a scene plays are listed as
  `deferred` and picked up when play stops. Files the MCP tools write themselves are not reported. `.scene`, `.prefab`,
  `project.yaml` and the terrain maps the terrain tools write are not watched: edit them with
  the tools. `sync_files` forces a full
  rescan; `editor_status.auto_sync` shows the scan's size and cost.
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
  localhost, which blocks DNS-rebinding attacks from web pages you visit. You can set a
  bearer token in Editor Settings > MCP, after which clients must send
  `Authorization: Bearer <token>`. To reach an editor on another machine, use an SSH tunnel
  (`ssh -L 47615:127.0.0.1:47615 host`). Never expose the port directly.
- Give the agent the same trust you would give a developer on the project. It can write any
  file in the project (with its own tools, or through `write_file`), and C++ scripts it writes are compiled and run inside the editor
  process when you press Play. The agent cannot leave the project folder: asset paths must
  stay inside it, and only the explicit import tools read files from elsewhere.
- Every call is listed in the editor's MCP activity window, and every edit can be undone.

## Tool reference

The tool list is generated: [TOOLS.md](TOOLS.md) is written by `tools/mcp/gen_tool_docs.py`
from the editor's `tools/list`, grouped by module, with each tool's description and
parameters. Regenerate it after you change tools:

```sh
uv run --no-project --with mcp --with pyyaml python tools/mcp/gen_tool_docs.py
```

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
- `batch` runs many calls in one round trip, as one undo step.
- Long operations return a job: poll it with `get_job` and stop it with `cancel_job`.

Tool definitions (name, description, input schema) are sent to the agent in every session,
so keep them lean: `tools/mcp/tool_budget.py` prints each tool's size and the total (target:
under ~100K characters, about 25K tokens; the HTML UI tools brought it to ~99K: new tools must
pay for themselves by trimming). Shared conventions (scene/entity references, value
shapes, undo) belong in the server instructions (`McpInstructions.cpp`), not in every tool.

```sh
uv run --no-project --with mcp --with pyyaml python tools/mcp/tool_budget.py [--top 20]
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| The client says the server timed out while starting | The first open of a project compiles shaders and C++ scripts. Raise the client's startup timeout (Claude Code: `MCP_TIMEOUT`; Codex: `startup_timeout_sec`). |
| `cannot open display`, or the server exits at once | No `DISPLAY`. Add `--headless` and install `Xvfb`. Read the editor's stderr: Claude Code `claude --debug`, Codex `~/.codex/log`, or run the command yourself in a terminal. |
| OpenGL or GLX errors under Xvfb | Set `LIBGL_ALWAYS_SOFTWARE=1` in the server's env and install Mesa (`libgl1-mesa-dri`). |
| `Failed to parse JSONRPC message` in the client | Something wrote to stdout that is not protocol. Report it as a bug; stdio mode redirects all output to stderr. |
| HTTP: connection refused | MCP is off in Editor Settings > MCP, the port is different, or another program already uses it (the editor logs this in the Console). |
| HTTP: 403 | The request's `Origin` is not localhost, or the bearer token is missing or wrong. |
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
```

The harness API and conventions are described in the docstrings of `tools/mcp/harness.py`
and `tools/mcp/run_tests.py`. Every tool needs a test.
