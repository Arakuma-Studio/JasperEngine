@AGENTS.md

## Engineering values

- Every runtime feature works on all five graphics backends (GL 3.3 core, GLES3/WebGL2, Metal,
  D3D11, Vulkan, through sokol-gfx) and all export targets (Windows, Linux, macOS, Android, iOS,
  HTML5). No feature may require compute shaders, SSBOs, bindless, indirect draws or 64-bit
  atomics; a desktop-only fast path needs a fallback that gives the same result.
- A disabled feature costs nothing, on the CPU and the GPU. Budget mobile and web first.
- Read the do-not-copy list in `docs/specs/README.md` before taking anything from another
  engine: it records architecture decisions.
- Old projects must load: new fields get defaults, format changes get a version bump and a
  migration.
- Users and agents are both first-class: a feature gets editor UI with Unity-like names and
  undo, a Lua API when it is runtime behaviour, and MCP tools so agents can drive and check it.

## Debugging instruments

- `jasperengine-editor call <tool>` runs any MCP tool from a shell against your editor
  (`--project` starts a one-shot headless editor; `call <tool> --help` shows its parameters).
- `read_console` (cursor-based), `screenshot` (Game or Scene view), `editor_status`, `get_job`.
- Sanitizer builds: `SANITIZE=address,undefined tools/build.sh unit-tests` (or `linux`), and
  `SANITIZE=thread`; `docs/testing.md` lists the presets.
- `editor.log` in the editor's working directory; `JASPER_MCP_TRACE=1` logs every MCP request.
- The harness keeps its scratch directory (project copy, settings, editor stderr) when a test
  fails, or always with `JASPER_KEEP_SCRATCH=1`.

## Architecture on one screen

- The engine (`engine/core`) is a library: scenes hold an ECS registry, systems update
  components, the render layer draws through sokol-gfx, Lua scripts bind through LuaBridge.
- A game is `engine/project` plus generated C++ (scene factories, script bindings) and the
  project's assets; the editor generates that code on Play and on export.
- The editor (`editor/`) hosts one project, edits scenes through undoable commands
  (`editor/command/`), plays scenes in-process and exports standalone builds.
- The MCP server (`editor/mcp/`) runs inside the editor: transports on their own threads,
  every tool on the main thread, long work as jobs. The CLI (`editor/cli/`) shares its registry.

## Code style

- C++20 within the portable subset (`tools/check-cxx-subset.py`); 4-space indent; code in the
  `jasperengine::` namespaces (`jasperengine::editor::mcp` ...).
- Match the surrounding naming and comment density. Comments say why, not what.
- No TODO stubs, dead code or disabled tests.

## Working discipline

- Label every claim you report as verified (you ran it, say how), inferred (from reading code)
  or assumed. Do not present inferred behaviour as tested.
- Follow your brief's scope; record a necessary deviation in the spec's Deviations section.
- When a check fails, find the cause before changing the test.

## Reporting

End a task with: what changed (files, commits), how it was verified (commands and their
results, which run was the red control), what is left or uncertain, and anything the next
person must know (migrations, follow-ups).
