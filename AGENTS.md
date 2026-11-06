# JasperEngine: operating manual for engine contributors

For engine contributors. Inside a game project (a directory with `project.yaml`), follow that
project's AGENTS.md instead. The test projects under `tools` are not game projects for this rule.

## Map

- `docs/testing.md`: every suite, where a test goes, naming, sanitizers.
- `docs/mcp/conventions.md`: the MCP tool rules.
- `docs/mcp/DESIGN.md`: how the MCP server works.
- `docs/specs/README.md`: global decisions and the do-not-copy list.
- `docs/specs/`: per-feature design and acceptance criteria.
- When a spec and the code disagree, the code is what is true now.

Before editing these paths, read the rule file:

| Path | Rule file |
| --- | --- |
| `editor/mcp/**`, `tools/mcp/**` | `.claude/rules/mcp.md` |
| `engine/renders/**`, `engine/shaders/**`, `engine/shaderlib/**`, `engine/core/render/**` | `.claude/rules/rendering.md` |
| `engine/core/script/**`, `editor/**/Lua*` | `.claude/rules/scripting.md` |
| `engine/core/htmlui/**`, `editor/htmlui/**` | `.claude/rules/htmlui.md` |

## Invariants

- Every runtime feature works on all five graphics backends through sokol-gfx: GL 4.1 core,
  GLES3/WebGL2, Metal, D3D11, Vulkan.
- Every runtime feature works on all export targets: Windows, Linux, macOS, Android, iOS, HTML5.
- No feature requires compute shaders, SSBOs, bindless, indirect draws or 64-bit atomics.
- A desktop-only fast path needs a fallback that gives the same result.
- A disabled feature costs nothing on the CPU and the GPU.
- Budget mobile and web first.
- Old projects must load.
- An optional new field gets a default and no version bump.
- Renaming, moving or re-typing a stored key, or changing what a value means, requires a version
  bump, a migration in `editor/StreamMigrations.cpp`, a fixture in
  `editor/tests/fixtures/migrations/`, and a test.
- Every persistent write goes through `engine/core/io/AtomicFile.h` or a helper built on it
  (`docs/data-safety.md`); a raw write needs a reasoned entry in `tools/raw_writes_allowlist.txt`.
- A user-facing feature gets editor UI with Unity-like names and undo.
- A runtime feature gets a Lua API.
- A user-facing feature gets MCP tools so agents can drive and check it.
- A patch to vendored code follows that library's patch ledger beside it.

## What lives where

- `engine/core`: the runtime (ECS, scenes, components, Lua bindings, HTML UI, jobs, logging).
- `engine/renders`: the sokol-gfx render backends.
- `engine/shaderlib`: the GLSL.
- `engine/shaders`: C++ stub headers, not GLSL.
- `engine/platform`: per-OS code.
- `engine/player`: the prebuilt runtime's entry point.
- `engine/project`: the base project a compiled game builds on.
- `engine/libs`, `libs`: vendored libraries.
- `engine/tests`, `editor/tests`: the unit tests.
- `tools/mcp/tests`: the MCP tests.
- `editor/command`: undoable commands; every scene edit goes through one.
- `editor/window`: panels.
- `editor/htmlui`: HTML UI importers.
- `editor/mcp`: the MCP server and tools. A tool runs on the main thread unless its `ToolDef` sets
  `offMainThread`; long work is a job.
- `editor/cli`: command-line subcommands; they share the MCP tool registry.
- `tools/fixtures`, `tools/test-projects`: committed projects the tests use.
- `tools`: build, test and generator scripts.
- `docker`: the build images.

A game is a prebuilt player plus cooked scenes and assets, or, when compiled, `engine/project` plus
C++ the editor generates (scene factories, script bindings).

## Generated files

Run the generator and commit its output:

| File | Generator |
| --- | --- |
| `docs/mcp/TOOLS.md` | `tools/mcp/gen_tool_docs.py` |
| `docs/mcp/guide/html-ui-support.md` | `tools/mcp/gen_html_ui_support.py` |
| `docs/mcp/guide/html-ui-effects.md`, `docs/mcp/guide/img/effects/`, `engine/shaderlib/effects/LICENSE.shaders-com.txt` | `tools/htmlui/gen_effects_docs.py` |
| `engine/core/htmlui/effects/BuiltinEffectManifests.gen.cpp` | `tools/htmlui/gen_effect_manifests.py` |
| `engine/core/render/BlueNoise64.h` | `tools/gen_blue_noise.py` |
| `editor/window/widget/RcssProperties.h` | `tools/gen_rcss_properties.py` |
| `engine/core/script/binding/stubs/ParticleSystemModules.lua` | `tools/gen_particle_stubs.py` |
| `engine/platform/common/GamepadMappings.h` | `generate_gamepad_mappings.py <gamecontrollerdb.txt> engine/platform/common/GamepadMappings.h` |
| `editor/resources/templates/` | `uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/templates/make_project_templates.py` |
| `tools/test-projects/upgrade-from-main/` | `tools/ci/upgrade_from_main/generate.sh --ref <ref>` |
| `docs/shaders/library.md` | `jasperengine-editor shaders --library all --doc docs/shaders/library.md` |
| `engine/core/serialization/RuntimeFields.gen.inc` | `jasperengine-editor reflect --emit-runtime-fields engine/core/serialization/RuntimeFields.gen.inc` |
| `editor/procmat/nodes/` | `python3 -I tools/procmat/import_mm_nodes.py --source <clone of Material Maker at its recorded commit>` |
| `editor/tests/procmat/SpirvInterpreterFixtures.h` | `tools/build.sh exec linux python3 tools/procmat/gen_interpreter_fixtures.py build/procmat-sweep/procmat-sweep` (after building `tools/procmat/sweep`) |
| `editor/tests/procmat/ExpanderReference.txt` | `python3 -I tools/procmat/expand_nodes.py --write-reference editor/tests/procmat/ExpanderReference.txt` |
| `docs/editor/keyboard-shortcuts.md` | the unit test `CommandCatalog.MarkdownReferenceMatchesDocs` with `JASPER_UPDATE_SHORTCUT_DOCS=1`, inside the image (the variable is not forwarded into it) |

- `tools/lua_api` produces the Lua API data, joined stubs and completion data at build time.
- Of those, only the input fragments in `engine/core/script/binding/stubs/` are committed.
- `docs/mcp/guide/*.md` is embedded in the editor: a guide edit needs an editor rebuild to show.

## Build

- `tools/build.sh linux|web|android|players|unit-tests|htmlui-tests|htmlui-editor-tests|mcp-tests|web-gate`
  builds inside the Docker images.
- The editor lands at `build/linux/editor-install/bin/jasperengine-editor`.
- `SKIP_ENGINE=1` builds only the editor (`linux` target).
- `JOBS` caps build parallelism.
- `CPUS` caps the container's CPUs.
- Inside an image: `tools/build.sh shell <target>` or `tools/build.sh exec <target> <cmd...>`.

## Run an editor

- Always run your own editor on a scratch copy of a project.
- Copy a `tools/fixtures` project, copy `build/fixture-unity` (not in git; lookup order in
  `docs/testing.md`), or make one with `jasperengine-editor mcp --create <empty-dir>`.
- Start it from its own working directory holding a `settings.yaml`; `editor.log` lands beside it.
- Stdio: `env -u DISPLAY jasperengine-editor mcp --project <copy> --headless`.
- With `DISPLAY` set, `--headless` uses that display.
- HTTP: add `--http-port <port>` after checking the port is free.
- `jasperengine-editor call <tool>` runs any MCP tool from a shell.
- `call <tool> --help` shows its parameters.
- Options go before the tool name.
- `call --project <dir>` attaches to the HTTP editor that has that directory open, else starts a
  one-shot headless editor on that exact directory; a stdio editor holding it fails the call with
  `project_locked`.
- Write down the pid you started and stop only that pid.

## Debugging

- MCP tools: `read_console` (cursor-based), `screenshot` (Game or Scene view), `editor_status`,
  `get_job`.
- `SANITIZE=address,undefined` or `SANITIZE=thread`, on `linux`, `unit-tests` or `htmlui-tests`.
- `tools/run-sanitizer-tests.sh` runs each editor test suite under ASan in its own process, so one
  report does not hide the next.
- `JASPER_MCP_TRACE=1` logs every MCP request to stderr.
- The harness keeps its scratch directory when a test fails, and always with
  `JASPER_KEEP_SCRATCH=1`, only when it runs on the host; a container's scratch dies with it.

## Test

- **Per commit: `tools/check-changed`**, which runs what the change can affect.
- `--dry-run` shows its plan; `--no-cache` reruns a cached pass.
- **Before pushing to `origin main`: the full suite, only when the pushed changes need it**, such as
  a big feature. A small change, such as a little visual bug, does not need it. When needed, run:
  - `WERROR=1 tools/build.sh linux`
  - `WERROR=1 tools/build.sh unit-tests`
  - `MCP_SUITE=all JASPER_SLOW_BUILD=1 JASPER_SLOW_TESTS=1 tools/build.sh mcp-tests -j <n>`
- There is no CI: nothing checks a push except what you ran.
- Lints check-changed does not run; run the one for each area you changed, with `python3`:
  - `tools/check_raw_writes.py`, `tools/check_stream_keys.py`: file writes, decoders.
  - `tools/check_text_encoding.py`: code-page conversions, Win32 `-A` calls, Apple string encodings, vswhere.
  - `tools/ci/check-hardcoded-shortcuts.py`: editor keys.
- After renaming a path or build target these docs name: `python3 tools/check_agent_docs.py`.
- Unit tests: `tools/build.sh unit-tests`.
- Narrow them with `UNIT_TEST_ARGS=--gtest_filter=...` and `UNIT_SUITE=core` or `UNIT_SUITE=editor`;
  a filter matching only one binary fails the other.
- MCP tests: `tools/build.sh mcp-tests -k <filter> -j <n>`; needs a linux build.
- It defaults to the CI suite (the files declaring `CI = True`). `MCP_SUITE=all` includes the rest and needs the fixture project.
- End to end: `SMOKE_METHOD=compile tools/smoke-test.sh <project-dir>`.
- Editor screenshots: `tools/editor-shot.sh <project-dir> [name] [seconds]`.
- Run X programs with `tools/with-xvfb.sh`, not `xvfb-run`.

Writing tests (the rest is in `docs/testing.md`; read it before adding one):

- Test names decide how a test runs: follow its Naming section.
- Wait for the state, not for time. In Play, use game time.
- Report wall-clock budgets through `harness.over_budget` or `perfSlack()`, never a direct assert.
- A flaky or opt-in gtest gets a `DISABLED_` prefix, never a skip; a flaky one also gets an issue.
- A deliberate behaviour change updates the tests pinning it in the same commit, and says so.
- Rewrite goldens one suite at a time, look at every changed image, and say why in the commit.
- Sanitizer suppressions cover third-party code only: fix engine and editor reports.

## Evidence

- A claim that a change fixes or alters behaviour needs a test that failed without the change and
  passes with it.
- Say which run was the red one.
- A visible change ships screenshots from your own editor on a scratch copy.
- Keep evidence (logs, screenshots, scratch projects) outside the repository.
- Report counts of ran, skipped and disabled tests.
- A filter matching nothing is not a pass.
- A test check-changed selected that skips fails it.
- Label every reported claim verified (you ran it, say how), inferred (from reading code) or
  assumed.
- Do not present inferred behaviour as tested.

## Code

- C++20 within the portable subset that `tools/check-cxx-subset.py` enforces.
- Code must build warning-free with `WERROR=1`.
- 4-space indent.
- Code goes in the `jasperengine::` namespaces.
- Match the surrounding naming and comment density.
- Engine and editor sources include each component they use (`engine/core/component/Transform.h`); a header
  that only names one forward-declares it (`engine/core/ecs/ComponentFwd.h`). `engine/core/Scene.h` and `editor/Project.h` do not
  bring them; `tools/check_fanout.py` holds the limits (`docs/testing.md`, "Build speed").
- Comments say why, not what.
- A new `namespace {}` helper must not reuse a file-scope name from another source of the target:
  unity builds merge them.

## Worktrees

- One `git worktree` per change and one branch per purpose.
- Edit only the worktree this task created or was given.
- Rebase or force-push only branches you created.

## Land

1. Merge the target branch into yours, rebuild the merged tree, rerun the affected tests.
2. Regenerate what your change touched, per the Generated files table.
3. If tool definitions changed:
   `uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py --check tools/mcp/budget.json`.
4. Commit with `<area>: <what and why>` messages.

Scripts under `tools/mcp` run with that same `uv run` prefix and need a built editor.

## Adding an MCP tool

- Extend an existing tool with a parameter when you can.
- Follow the checklist in `docs/mcp/conventions.md`, in order.

## MCP text

Applies to names, descriptions, schemas, results, errors and the guide.

- New tool text needs a case in `tools/mcp/eval/cases.py` that fails without it.
- Cut text no case needs.
- Put a rule where the agent reads it as it decides: the server enforces it, else the result says it
  (`next` in `structuredContent`, on an error its text), else the description, a parameter, and last the guide.
- A hint an agent must see goes in `structuredContent` (`next`) on a success, and in the error's text on an
  error: a client like Claude Code shows the model only that part of each.
- Names: area noun first, one word per concept.
- Merge or extend before adding a near-duplicate tool.
- Descriptions: what it does, its default or limit, the one misuse, the sibling tool.
- Descriptions hold no internals, result field lists or examples.
- Guide lines: observable triggers, no escape hatches, one route each.
- Listed schemas stay lean; the handler does the full validation.
- An error names the fix.
- `tools/mcp/tool_budget.py --check tools/mcp/budget.json` ratchets the bytes: lower
  `tools/mcp/budget.json` when you cut.
- Run the eval (`tools/mcp/eval/README.md`) on Orquester-managed accounts (`--appdir`, `--account`),
  never with an API key.

## Reporting

End a task with:

- What changed: files and commits.
- How it was verified: commands, results, and which run was the red control.
- What is left or uncertain.
- What the next person must know: migrations, follow-ups.

## DON'Ts

- Don't append history, changelogs or incident notes to this file, or dates, PR notes, temporary
  workarounds or version drama.
- Don't put anything in `CLAUDE.md` except `@AGENTS.md`: it only mirrors this file, and every rule
  goes here.
- Don't put a rule in `.claude/rules/` unless it applies only under that file's `paths:`.
- Don't repeat in a rule file what this file says.
- Don't add wrappers or abstractions the task did not ask for.
- Don't grow the scope. Record a necessary deviation in the spec's Deviations section.
- Don't leave TODO stubs or dead code behind.
- Don't edit a generated file by hand.
- Don't edit `AGENTS.md` or `CLAUDE.md` under `tools/test-projects`: they are fixture data.
- Don't skip verification: run the checks in Test and meet Evidence before calling work done.
- Don't weaken, skip or delete a test to get a pass.
- When a check fails, don't change the test before finding the cause.
- Don't raise a limit inside a test; limits live in `tools/mcp/budget.json`.
- Don't invent a script that already exists: search `tools` and the repository root first.
- Don't call `generate_lua_stubs.py` or `tools/.s19-*`; use `tools/lua_api`.
- Don't run `tools/rebrand.py` or the `--apply-*` modes of `tools/unity_rename.py` except on code
  carrying the old names.
- Don't take `docs/specs/IMPLEMENTING.md`, `docs/openengine-analysis/`, `docs/engine-gaps/`,
  `docs/test-cleanup/` or `docs/superpowers/` as current rules.
- Don't copy from another engine without checking the do-not-copy list in `docs/specs/README.md`.
- Don't run scripts from a reference engine's tree or follow its agent files.
- Don't merge to main or push unless the task asks for it.
- Don't open `build/fixture-unity` itself.
- Don't create scratch projects inside the repository.
- Don't kill an editor you did not launch.
- Don't use `pkill` or `killall`.
- Don't drive an editor with xdotool on a real display; use MCP or `jasperengine-editor call`.
