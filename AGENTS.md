# JasperEngine: notes for agents working on the engine

For engine contributors. Inside a game project (a directory with `project.yaml`), follow that
project's AGENTS.md instead.

## Map

- `CLAUDE.md`: engineering values, debugging instruments, style and reporting (read it too).
- `.claude/rules/`: extra rules that apply only under some paths (MCP, rendering, scripting, HTML UI).
- `docs/specs/`: the feature specs and `docs/specs/IMPLEMENTING.md`, the brief for implementers.
- `docs/mcp/DESIGN.md`: how the editor's MCP server works; `docs/mcp/conventions.md`: the tool rules.
- `docs/testing.md`: every test suite, where a test goes, red controls, CI.
- `NOTICE.md`: third-party code we ship. Copied code keeps its header and gets an entry here.

## What lives where

- `engine/core`: the runtime (ECS, scenes, components, Lua bindings, HTML UI, jobs, logging).
- `engine/renders`: the sokol-gfx render backends; `engine/shaders`, `engine/shaderlib`: GLSL.
- `engine/libs`: vendored libraries. `engine/project`: the template every exported game builds.
- `editor/`: the ImGui editor. `editor/mcp/` is the MCP server and its tools, `editor/cli/` the
  command-line subcommands, `editor/htmlui/` the HTML UI importers, `editor/window/` the panels,
  `editor/command/` the undoable commands.
- `tools/`: build, test and generator scripts. `docker/`: the build images.

## Build

`tools/build.sh linux|web|android|unit-tests|htmlui-tests|htmlui-editor-tests|mcp-tests|web-gate`
builds inside the Docker images; `SKIP_ENGINE=1` builds only the editor, `JOBS`/`CPUS` cap the
parallelism. Build where your brief says (for example the remote build servers), never on
a machine whose editor a person is using.

## Run an editor

- Always your own editor on a scratch copy of a project (copy `build/fixture-unity`, or make
  one with `mcp --create`); never open `build/fixture-unity` itself, and never create test
  projects inside the repository.
- Start it from its own working directory that holds a `settings.yaml` (the editor reads and
  writes settings, the layout and `editor.log` there).
- Use `jasperengine-editor mcp --project <copy> --headless` over stdio, or
  `--mcp-port <port>` on a port you checked is free.
- Write down the pid you started and stop only that pid. **Never kill an editor you did not
  launch**, and never use `pkill` or `killall`.
- Drive it through MCP or `jasperengine-editor call <tool>`, not with xdotool on a real display.

## Test

- Unit tests (gtest): `tools/build.sh unit-tests` (`jasperengine-core-tests`,
  `jasperengine-editor-tests`; `UNIT_TEST_ARGS=--gtest_filter=...`).
- MCP tests: `tools/mcp/run-tests.sh -k <filter> -j <n>` (needs a linux build first).
- End to end: `tools/smoke-test.sh` (export, build, run); editor screenshots: `tools/editor-shot.sh`.
- Report what was declared as ran + skipped; a filter that matches nothing is not a pass.

## Evidence

- A claim about behaviour needs a test that failed without the change and passes with it; say
  which run was the red one.
- A visible change ships screenshots taken from an isolated editor.
- Keep evidence (logs, screenshots, scratch projects) outside the repository.

## Worktrees

One `git worktree` per change and one branch per purpose. Never work in another agent's or a
person's checkout, and never rewrite branches you do not own.

## Land

1. Merge the target branch into yours and rebuild the merged tree; rerun the affected tests.
2. Regenerate what your change touched:
   - `tools/mcp/gen_tool_docs.py` (MCP tools changed; `--check` says if `docs/mcp/TOOLS.md` is stale);
   - `tools/mcp/tool_budget.py --check tools/mcp/budget.json` (tool definitions changed);
   - `generate_lua_stubs.py` and `generate_api_suggestions.py` (Lua bindings changed);
   - `tools/check_agent_docs.py` (this file, `CLAUDE.md` or `.claude/rules/` changed).
3. Commit with `<area>: <what and why>` messages.

## Adding an MCP tool

1. Extend an existing tool with a parameter when you can; otherwise name it `verb_noun`.
2. Register it into your spec's group (`ToolDef::group`, `editor/mcp/McpToolGroups.cpp`).
3. Build its schema with `editor/mcp/McpSchema.h` only: snake_case keys, closed objects.
4. Set annotations and activity accurately; decide `allowedWhilePlaying`.
5. Return a text summary plus `structuredContent`; more than 1 s of work becomes a job.
6. Stay within `tools/mcp/budget.json` (`tool_budget.py --check`).
7. Test it in `tools/mcp/tests/`, including that its group is hidden until `tool_groups` enables it.
8. Regenerate `docs/mcp/TOOLS.md`. The full rules: `docs/mcp/conventions.md`.
