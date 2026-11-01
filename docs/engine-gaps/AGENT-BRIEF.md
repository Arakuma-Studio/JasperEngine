# Brief for every agent working on the engine-gaps program

Read this, [LEDGER.md](LEDGER.md) and [GAP-REPORT.md](GAP-REPORT.md) first.

## The program

JasperEngine (a C++ engine + Unity-style editor, Lua and C++ scripting, a built-in MCP server for coding agents)
is closing the gaps found while porting a Unity mobile game. 24 features, each with a spec in
`docs/superpowers/specs/2026-10-31-<feature>-design.md` and its own branch `gaps/<feature>`.
The owner's bar: **complete and correct, no bugs, best UX for human users (editor, Inspector, menus) and for AI
agents (MCP tools, the agent guide, docs, Lua stubs)**. Be ambitious: when Unity users would expect it, do it.
Prefer Unity-familiar concepts and names (users come from Unity; `docs/unity-glossary.md` maps names), written in
the engine's existing Lua style (lowerCamelCase methods and properties, PascalCase classes, `Engine.*`/`System.*`
statics, `RegisterEvent`/`RegisterEngineEvent`).

## Shared conventions across features (binding)

- **Per-object script callbacks** are methods on a Lua script table, called by name when the script defines them,
  exactly like Unity messages: `onEnable`, `onDisable` (feature `script-lifecycle`), `onCollisionEnter/Stay/Exit`,
  `onTriggerEnter/Stay/Exit` and their `...2D` variants (feature `physics-contacts`). The engine already calls a
  same-named method on an object's Lua scripts for animation events (`ActionSystemPlayers.cpp` ~941): reuse/extend
  that path rather than inventing another. C++ scripts get the same callbacks as virtual methods if the C++ script
  base supports it.
- **Async results** (HTTP, native bridge, timers, coroutines) are always delivered **on the main thread** at a
  defined point of the frame, never from worker threads; the Lua state is single-threaded
  (`engine/core/script/LuaBinding.cpp:131-165`).
- **JSON**: the `Json` Lua module (feature `script-data`: `Json.encode(value[, opts])`, `Json.decode(text)`) is the
  single JSON<->Lua conversion; `http` and `native-bridge` use it (they merge after `script-data`).
- **Input**: virtual axes/buttons (feature `input`) are the single place that on-screen controls (feature
  `ui-multitouch`) feed, so `Input.getAxis("Horizontal")` sums keyboard, gamepad and on-screen stick like Unity.
- **Time**: scaled vs unscaled is explicit on every timer/tween/coroutine API (default scaled, like Unity).
- **Scene loads**: anything that outlives a scene load (timers, coroutines, HTTP requests, persistent objects) has a
  clear, documented rule for what happens when its owner object or scene is destroyed (default: cancelled with
  the owner).
- Everything new must work in **editor Play mode and in exported builds** (Generator/Exporter code paths), on every
  platform the engine exports (Linux, Windows, macOS, Android, iOS, Web), or the spec must say why not.
  No editor-only caps or behaviours that differ on device.

## Surfaces checklist (each spec states which apply; each implementation covers all that apply)

1. Engine C++ (`engine/core`, `engine/platform/*`), serialization of new component fields (scene/prefab files:
   `editor/Stream*.cpp`, and the exported-build path in `editor/Generator.cpp` / `editor/Exporter.cpp`).
2. Lua bindings (`engine/core/script/binding/*.cpp`); the LuaLS stubs are generated from them by
   `generate_lua_stubs.py` (keep `-- unresolved:` empty; `tools/mcp/tests/test_stubs.py`). C++ scripting API if relevant.
3. Editor: Inspector UI (`editor/window/...`), menus, component add menu, Project settings, Game view, undo via the
   command system.
4. MCP (`editor/mcp`, tools in `editor/mcp/tools`): the component/property surfaces agents use to author the
   feature; `docs/mcp/TOOLS.md` is generated (`tools/mcp/gen_tool_docs.py`), the tool definitions have a size
   budget (`tools/mcp/tool_budget.py`; when new tool text is worth it, raise `TOTAL_LIMIT`/`LIMITS` in
   `tools/mcp/tests/test_html_ui_tools.py` by the measured amount with a comment, as earlier features did; keep
   descriptions tight and move detail to the guides); the agent guide (`editor/mcp/McpInstructions.cpp`, `jasper://guide`) has a
   tight budget - put details in `docs/mcp/guide/*.md` instead.
5. Docs: `docs/mcp/guide/lua-api.md` and the topic guides, `docs/unity-glossary.md` (Unity name -> engine name),
   README feature list when user-visible.
6. Tests: gtest unit tests in `editor/tests` (`EditorTests.cmake`, target `jasperengine-editor-tests`), engine test
   targets where they exist, MCP end-to-end tests in `tools/mcp/tests` (`tools/mcp/run-tests.sh`, needs `uv` at
   `~/.local/bin/uv` and xvfb). Platform code that cannot run here must at least compile (Android: Gradle/NDK;
   Web: emsdk) - see Build.

## Build and test on a remote build box (preferred)

The host is saturated, so builds and tests run on rented Linux boxes (Ubuntu 24.04, gcc 13, CMake 3.28, ccache,
xvfb, xdotool, dbus-x11, uv, FFmpeg: the same toolchain as the host). From inside your worktree:

- `jbox run '<command>'` syncs your worktree to the box (changed files only; `build/` and `.git` are not copied)
  and runs `<command>` from the worktree root there. Examples:
  `jbox run 'cmake -S . -B build/editor -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_EDITOR_TESTS=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && cmake --build build/editor --target jasperengine-editor jasperengine-editor-tests -j8'`
  `jbox run 'xvfb-run -a build/editor/jasperengine-editor-tests --gtest_filter=Foo*'`
  `jbox run 'cmake --install build/editor --prefix build/linux/editor-install >/dev/null && JASPER_EDITOR=$PWD/build/linux/editor-install/bin/jasperengine-editor tools/mcp/run-tests.sh -j4 tools/mcp/tests/test_x.py'`
- `jbox get <path> [<dest>]` copies a file back (e.g. a regenerated `docs/mcp/TOOLS.md`, screenshots).
- Your box is given in your task (`JBOX_PORT`); export it in every command (`JBOX_PORT=2202 jbox run ...`) or rely
  on `/etc/jbox/assign`. Boxes are shared by several agents: 16-core boxes (ports 2201, 2202) use `-j5`, 8-core
  boxes (2203, 2204) `-j3` (more parallel jobs ran a box out of memory). MCP tests: `-j2`.
- The box copy is NOT a git repository of your branch (git state there is stale): commit locally, never on the box.
  Long commands: keep them under the tool timeout or run them in the background locally (`jbox run ... > log &`).
- Android (Gradle/NDK) and Web (emsdk) compile checks still run on the host (toolchains only here): keep them few.
- Apple builds use the Xcode project, which lists sources itself: after adding sources run ALL of
  `python3 tools/xcode_sync_engine.py`, `tools/htmlui/xcode_sync_rmlui.py`, `tools/htmlui/xcode_sync_compat.py`,
  `tools/htmlui/xcode_sync_effects.py` (each has `--check`); a missing file only fails the Apple link.

## Build and test on this host (fallback)

- Native Linux toolchain: gcc 13, CMake 3.28, Ninja, ccache (shared cache, `base_dir=/var/lib/orquester/workspaces/jaspersito`),
  xvfb (`xvfb-run -a`), nasm, JDK 21, Android SDK at `/opt/android-sdk` (AGP installs the NDK), emsdk 6.0.10 at
  `/opt/emsdk` (`source /opt/emsdk/emsdk_env.sh`). `tools/build.sh` uses Docker, which is NOT available: run the
  equivalent cmake/gradle commands natively (read `tools/build.sh` for the exact flags).
- Editor + unit tests (from a worktree root):
  `cmake -S . -B build/editor -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_EDITOR_TESTS=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
  then `cmake --build build/editor --target jasperengine-editor jasperengine-editor-tests -j3`
  and `xvfb-run -a build/editor/jasperengine-editor-tests`.
- The machine has 8 cores / 15 GB RAM shared by up to 6 agents: **use `-j3` at most** and don't run several big
  builds at once from one agent.
- MCP end-to-end tests from a worktree: install your build first
  (`cmake --install build/editor --prefix build/linux/editor-install`) and ALWAYS pass your own editor:
  `JASPER_EDITOR=$PWD/build/linux/editor-install/bin/jasperengine-editor tools/mcp/run-tests.sh -j2 <files>`
  (without it the harness silently uses the MAIN checkout's editor). xdotool, dbus-x11, uv, FFmpeg (tools + dev
  libs) are installed. Known baseline failures on `main` (LEDGER "Baseline"): the 5 tests of
  `test_animation_assets.py` and 5 of `test_lattice_scene.py`; every other MCP test passes on `main`.
- Baseline on `main` (`b32a3c578`): editor + tests build with 0 warnings; unit tests 936 passed, 8 skipped, 0 failed.
- Never use `git push`, never touch `main` or other branches/worktrees: the lead merges.
- Before you hand off (and again when the lead asks), merge the current local `main` into your branch
  (`git merge main` inside your worktree; `main` is shared through the repository), resolve conflicts keeping both
  features' intent, rebuild, re-run the unit suite and your MCP tests, and regenerate TOOLS.md / re-check the tool
  budget against the merged tree. The lead then merges your branch with no conflicts.

## Commits

Small, meaningful commits on your branch, messages in the repo's style
(`<Feature>: <what changed, plainly>`), e.g. `Audio mixer: buses with volume and mute`.
