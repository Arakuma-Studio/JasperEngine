# Brief for MCP tool workstreams (W1–W6)

Read `docs/mcp/DESIGN.md` first (binding), then this. The core server, transports,
registry, legacy tools, enablers and harness are merged on branch `mcp`.

## Where things are
- Framework: `editor/mcp/` — `McpRegistry.h` (ToolDef, ResourceDef, PromptDef), `McpCall.h`
  (CallPtr: args/ok/fail/reply/waitUntil/afterFrames/detach/progress/cancelled), `McpContext.h`
  (app(), project(), windows, jobs(), isPlaying(), legacyExecutor()), `McpJobs.h` (long jobs +
  `replyWhenJobDone`), `McpRefs.h` (resolveScene/resolveEntity/entityJson/resolveComponent/
  resolveProjectPath), `McpValues.h` (JSON <-> property values, buildPropertyCommand),
  `McpSchema.h` (`schema::object/string/vec3/sceneRef/entityRef/...`), `tools/McpTools.h`.
  Read these headers before writing code; follow `tools/CoreTools.cpp` as the style model.
- Legacy tools (the old in-editor agent's 90+ actions) are registered by
  `tools/LegacyTools.cpp` from `legacy/EditorActionRegistry.cpp` + `legacy/EditorActionExecutor.cpp`.
  They're a useful reference for how to call editor APIs. Replace the ones in your area:
  register the same name (replaces) or `removeTool(old)` and add a better one. Do not edit
  `legacy/*` (bug fixes only, keep them minimal).
- Enablers: `editor/util/EntityPath.h`, `Out::getLogsSince/getRecentLogs/lastLogId/clearConsole`,
  `SceneRender::getFramebuffer()`, `GraphicUtils::encodeFramebufferPng(fb, out, flipY=false,
  maxWidth, &w, &h)`, Project `createSceneNow/openSceneNow/closeSceneNow/saveSceneNow/
  createProjectAt/openProjectNow` (+ `UnsavedChoice`), `EditorHost::requestProjectChange`.
- Undo: `CommandHandle::get(sceneId)` per-scene history; `project->getProjectCommandHistory()` for
  files. `addCommandNoMerge` returns void — for a success bool do `cmd->setNoMerge();
  bool ok = history->addCommand(cmd);` (addCommand deletes cmd on failure). Prefab stages
  have their own history (stage sceneProject id).
- Main-thread only: handlers run on the main thread between frames. Never block waiting for
  something the main thread must do (play startup, export, a rendered frame) — use
  `waitUntil`/`afterFrames`/jobs.

## Ownership (do not edit files you don't own; if you need a shared change, write a local
helper in your own file and mention the shared refactor in your report)
- W1: `tools/SceneTools.cpp`, `tools/EntityTools.cpp`, `tools/ComponentTools.cpp`, `McpValues.*`, `McpRefs.*`
- W2: `tools/PrefabTools.cpp`
- W3: `tools/PlayTools.cpp`, `tools/ViewTools.cpp`, `tools/ConsoleTools.cpp`
- W4: `tools/ScriptTools.cpp`, `tools/AssetTools.cpp`, `tools/DocsTools.cpp`, `docs/mcp/guide/*`
- W5: `tools/ProjectTools.cpp`, `tools/BuildTools.cpp`
- W6: `tools/WorldTools.cpp`
- Each workstream: `tools/mcp/tests/test_<your area>.py` (new files only).
- Small, surgical edits to editor files outside `editor/mcp/` are allowed when a tool truly
  needs a missing accessor/API (e.g. a getter on a window, a non-interactive variant). Keep
  them minimal and additive, avoid widely-included headers where possible (Project.h edits
  rebuild ~110 files), and list them in your report — other workstreams edit in parallel.
- If you add new .cpp files, prefer not to: put everything in your owned files. (CMake lists
  are owned by nobody now; a one-line addition is acceptable if unavoidable.)

## Tool design rules
- snake_case `verb_noun` names. Fewer, more powerful tools; excellent descriptions (what it
  does, what it returns, gotchas, an example argument object for anything non-obvious).
  Real constraints in the JSON Schema (`required`, enums). Errors are `call->fail(...)` with
  the fix (valid choices); never crash; never hang.
- Inputs use McpRefs conventions: `scene` (id/name/path, default selected), `entity` (id or
  path `A/B#1`), component names, project-relative asset paths. Outputs always include
  entity `{id,name,path}` via `McpRefs::entityJson`, and `structured` JSON.
- Edits are undoable (commands), one call = one undo step (MultiPropertyCmd for multi-edits).
- Set `annotations.readOnly` / `destructive` / `idempotent` correctly; set
  `allowedWhilePlaying=false` on edits that make no sense during play.
- Keep responses compact (agents pay for tokens): no giant dumps by default; offer
  `detail`/`limit` params.

## Build & test
- Your worktree: `/var/lib/orquester/workspaces/jaspersito/JasperEngine-wt/<name>` on branch
  `mcp-<name>`. Never touch the main checkout or other worktrees. Commit there (no push).
- Build: `SKIP_ENGINE=1 JOBS=3 tools/build.sh linux` (docker, shared ccache; first build in a
  new worktree a few minutes, incremental ~30 s). Host is heavily loaded: never more than JOBS=3.
- Test with the harness against the real editor (headless Xvfb is automatic):
  `PATH=/var/lib/orquester/.local/bin:$PATH tools/mcp/run-tests.sh tools/mcp/tests/test_<area>.py`
  Harness API: see the docstring of `tools/mcp/harness.py` and `tools/mcp/tests/test_protocol.py`
  (`async with Editor() as ed`, `await ed.call(tool, **args)`, `expect_error=True`,
  `ed.screenshot(...)`, `ed.check/check_eq`, module-level `EDITOR = {...}` e.g.
  `{"create": True}` for an empty new project). Default project: a scratch copy of the
  TappyPlane fixture (`build/fixture-unity` in the main checkout; it's symlinked into your
  worktree's build/). Never modify fixtures in place.
- Kill only processes you started, by PID. Never `pkill -f jasperengine-editor`.
- Also run `tools/mcp/run-tests.sh tools/mcp/tests/test_protocol.py` before finishing.
- Before finishing: `git merge mcp` into your branch (others may have landed) and rebuild +
  retest if it brought changes; resolve conflicts carefully.

## Report (your final message)
Tool list (name — one line), files changed outside your owned files and why, test command +
results, commits, known gaps. Be honest about anything not working.
