# Brief for HTML/CSS UI work packages

Read `docs/html-ui/DESIGN.md` (binding), then your phase spec
(`docs/html-ui/phase-N-*.md`), then this file. Your assignment names one work package of
that phase spec: implement all of it, completely and correctly, with tests.
`docs/html-ui/CONTRACTS.md` lists every interface one phase provides to another: use the
names there for everything you consume, and keep the names of everything you provide; if the
code forces a different name or signature, keep the old one working or report the change.

## Branches and worktrees
- Integration branch: `html-ui`. Each phase's packages branch from it and merge back into
  it; when a phase passes its acceptance tests, `html-ui` is merged into `main`.
- Your worktree and branch are given in your assignment (for example
  `/var/lib/orquester/workspaces/jaspersito/JasperEngine-2-wt/p1-render` on
  `hui/p1-render`). Work and commit only there. Never touch the main checkout
  (`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2`) or another worktree. Do not push.
- Commit early and often with clear messages (what and why). Fork patches to RmlUi are
  separate commits prefixed `rmlui:` and recorded in `docs/html-ui/FORK.md`.
- Before you finish: `git merge html-ui` into your branch (other packages may have landed),
  resolve conflicts carefully, rebuild, rerun every test listed below.

## Ownership
Your package lists the files/directories you own. Edit others' files only with small,
additive changes that your package truly needs (a hook, an accessor, a registration line);
list each one in your report. Shared registration points (CMake lists, `Scene::init`,
`EntityRegistry.cpp`, `Catalog.cpp`, `LuaBinding.cpp`, `ShaderType` enum) take one-line
additions only; append, don't reorder.

## Build
- Linux editor + engine (Docker, shared ccache): `JOBS=3 tools/build.sh linux` from your
  worktree. Output in your worktree's `build/linux/`. Never use more than `JOBS=3`: several
  agents build at once on a 12-core host.
- Editor only (faster): `SKIP_ENGINE=1 JOBS=3 tools/build.sh linux`.
- Web: `JOBS=3 tools/build.sh web`. Android: `JOBS=3 tools/build.sh android`.
- CPU cap: `JOBS` also caps the CPUs the build/test container may use (`docker --cpus`;
  `CPUS` overrides it), which is the only cap on what ignores `-j`: the Android gradle/ninja
  build and the test binaries (llvmpipe threads). Always pass `JOBS` (the coordinator gives the
  number), also to `android`, `htmlui-tests` and `run-rmlui-tests.sh`. For the MCP suite
  (`tools/mcp/run-tests.sh`, which runs editors on the host) use `-j 1` or `-j 2` as told and set
  `LP_NUM_THREADS=2` so llvmpipe does not start a thread per host core.
- Vulkan on Linux: configure a second build with `GRAPHIC_BACKEND=vulkan` when your package
  touches rendering (see the phase spec for how).
- Windows, macOS and iOS cannot be built on this host. Keep code portable (no POSIX-only
  calls outside `engine/core/platform` style files, no compiler extensions), and wire CMake
  for those targets by reading the existing per-platform sections. Say in your report what
  you could not build.

## Test
- C++ unit and golden-image tests: `engine/tests/htmlui/` (googletest), target
  `jasperengine-htmlui-tests`, built and run headless under Xvfb by
  `tools/build.sh htmlui-tests` (`HTMLUI_BACKEND=vulkan` for Vulkan, `HTMLUI_TEST_ARGS` for a
  gtest filter). Golden images live in `engine/tests/htmlui/golden/`; regenerate only on
  purpose (`JASPER_HTMLUI_UPDATE_GOLDEN=1`, glcore) and inspect every changed image.
- MCP end-to-end: `tools/mcp/run-tests.sh tools/mcp/tests/test_<file>.py` against the real
  editor (headless Xvfb is automatic). Fixture: `JASPER_FIXTURE=/var/lib/orquester/workspaces/jaspersito/JasperEngine-2/build/fixture-unity`
  (never modify it in place; the harness copies it). Harness API: docstring of
  `tools/mcp/harness.py`.
- Always also run `tools/mcp/run-tests.sh tools/mcp/tests/test_protocol.py` and the existing
  tests your change could affect (the UI ones: `test_ui_anchors.py`).
- Look at the screenshots you produce. A test that passes while the image is wrong is a
  failing test.
- Performance tests (`CompatPerf.*`, `CompatCssPerf.*`, phase 2 spec 3.14) measure this
  process's own work, but a loaded host still disturbs them, and several agents build at
  once. If one fails while other builds are running, rerun it alone
  (`HTMLUI_TEST_ARGS=--gtest_filter=CompatPerf.*:CompatCssPerf.*`) before treating it as a
  regression, and say in your report which runs passed and failed. Never raise a target,
  never commit a slack factor (`JASPER_HTMLUI_PERF_SLACK` is for a local look, not for a
  result), never skip or disable one. They are gated at phase acceptance by the integrator.
- Perf gates (`EffectPerfTest`, `CompatPerf.*`, ...) are judged with the test host to
  themselves: on the remote host take all three locks in a fixed order
  (`flock /root/jasper/lock-a flock /root/jasper/lock-b flock /root/jasper/lock-c ...`, which
  waits for the other agents' jobs and blocks new ones), check that the load average is under 1,
  measure, and release them. `EffectPerfTest` prints the load average at the start of each test.
  Compare a branch against `main` measured the same way, alternating, before calling a result a
  regression.
- Kill only processes you started, by PID. Never `pkill -f jasperengine-editor`. Clean up
  only the scratch directories your own runs created (exact paths), never by wildcard.

## Quality bar
- Complete: everything the package lists, no TODO stubs, no "left for later".
- Correct: tests for every behavior, including error paths and edge cases; zero new compiler
  warnings in our code; no leaks of GPU objects (destroy what you create; test document
  unload/reload).
- Match the surrounding code: naming, comment density, idioms (see existing `UISystem`,
  `RenderSystem`, MCP tools).
- Docs: update the user-facing docs your change affects (`docs/mcp/guide/html-ui.md`,
  `docs/html-ui/FORK.md`, tool docs generated by `tools/mcp/gen_tool_docs.py`).

## Report (your final message)
What you built (files), files changed outside your ownership and why, exact test commands
and their results, commits, anything you could not build or verify. Be honest: say what is
not working.
