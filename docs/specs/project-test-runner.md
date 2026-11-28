# Project Test Runner: scenario tests a game project owns, run in the editor, from the CLI and in every player, with one result format

Spec-first. Builds on S22 (input), S23 (editor menus and their Lua sandbox), S27 (the Recorder),
S30 (launch options and exit codes), S31 (`jasperengine-editor call`), S34 (player flavours). Its
input steps and replay cases use the event format of `input-recorder-and-input-scripts.md`, which
owns that format; this spec only consumes it. Paths are relative to the repository root unless they
start with `<project>/`.

## 1. Goal

Every test suite in the repository today is for engine contributors. A game made with JasperEngine
has no way to keep a scenario ("Start opens Level 1 with three lives", "the shop refuses a purchase
without coins") as a file that anyone (a person, an agent, CI) can re-run on the editor and on each
export target and get a pass or a fail with a reason. The building blocks exist only as separate MCP
calls (`play`, `wait` with `until`, `send_input`, `eval_lua`, `diff_images`) and player exit codes.
This spec gives projects Unity's Test Runner: `*.test.lua` files in a committed test folder, Edit
Mode and Play Mode cases that wait for state in game time, golden captures of the real renderer, a
data sandbox so no test touches a player's saves, one result schema (JSON and JUnit) for the editor,
the CLI and the six player targets, and a ledger that shows on which platforms each test last passed.

## 2. Scope

**In scope**
- The test root: `<project>/Tests/` holding `tests.yaml` (the marker and the defaults), `*.test.lua`
  files, `Golden/` images, `Data/` snapshots and `Input/` files.
- The Lua `Test` API (cases, checks, waits in frames and game seconds, input, captures, expected
  errors, skips, setup and teardown), bound only inside test chunks.
- Edit Mode cases (the editor's sandboxed Lua state, read-only MCP tools, no game time) and Play Mode
  cases (a fresh Play session per case at a fixed frame rate).
- Console errors fail a case unless expected; a hang guard and Lua's runaway interrupt.
- Golden captures with per-platform and per-backend fallbacks, Accept Image (undoable).
- Replay cases (`replay = "Input/x.input.json"`) with divergence reported as a failure.
- Play Data profiles: Project, Clean, Snapshot, in the Game view toolbar and `play {data}`; every test
  case runs on Clean or a Snapshot, never on the project's data.
- The Test Runner window (Window > General > Test Runner).
- MCP `run_tests` (new group `testing`), `play` `data`, CLI `jasperengine-editor test`.
- Players: `--run-tests`, `--test-filter`, `--test-results`, `--keep-test-data` on all six targets
  (Development players), a `[test]` log protocol, exit codes 8 and 9.
- The results ledger (`.jasperengine/test-results/`), the Last Passed On column and the Unverified On
  filter; `run_tests {action: "import"}` for results produced elsewhere.
- Build setting Include Tests (Development builds only).

**Out of scope (default decisions)**
- Test cases written in C++. A C++ game tests what it binds to Lua; native unit tests stay the game
  developer's own CMake business.
- Driving web, Android and iOS from the editor: `build-and-run-on-device.md` adds those targets to
  `run_tests`. Until then CI (or a person) runs the player and imports the results file.
- Capture Presets (stored camera, scene, frame and size bundles) and `screenshot preset`: a capture is
  the case's frame at the case's resolution.
- Benchmarks and frame-time budgets (`release-performance-and-gpu-quirks.md`), code coverage, mocks,
  multiplayer sessions, running cases in parallel inside one editor (one Play session at a time).
- Committing the ledger: it is per machine. Teams share results through JUnit files in CI.
- Nothing from the do-not-copy list of `docs/specs/README.md` is involved.

## 3. Current state

Read from the code (not run) unless marked.

- **No test command.** `printMainUsage` lists export, build, players, publish, check, call, batch and
  more, but no `test` (`editor/cli/CommandLine.cpp:298-349`). `check` (`:312`) checks scripts, shaders
  and assets compiler-style; nothing runs game scenarios. No file in the repository writes JUnit XML
  (only `tools/ci/test_gtest_gate.py` reads googletest's).
- **No Lua test API.** The hand-written stubs are `Asset`, `Audio`, `Timer`, `Navigation`... in
  `engine/core/script/binding/stubs/`; none for tests.
- **Agent guidance is manual.** `docs/mcp/guide/authoring.md:128-132` "Check what you built" is three
  bullets: `get_scene_tree`, a Scene view screenshot, Play and read the Console.
- **The building blocks.** `wait` runs frames, game seconds or until a Lua condition, with `input`
  from the first frame (`docs/mcp/TOOLS.md:1511-1533`, `waitTool` at
  `editor/mcp/tools/PlayTools.cpp:3899`); `play` sets `fixed_fps` and `auto_pause` so the game runs
  only inside `wait`/`send_input` (`PlayTools.cpp:4303-4328`, clock state `TimeControl` at `:204-209`);
  `send_input` drives keys, mouse, touch, gamepad, actions and app events (`:4515-4538`);
  `eval_lua` evaluates in the running game (`TOOLS.md:1361-1376`); `diff_images` compares two PNGs
  with defaults tolerance 8, max_fraction 0.001, max_diff 48, max_mean 0.25
  (`TOOLS.md:5104-5124`, `editor/mcp/tools/ImageDiffTools.cpp`). An agent strings these together by
  hand each time; nothing stores the sequence.
- **Engine clock.** `Engine::setFixedDeltatime` makes every frame advance exactly that much game time
  (`engine/core/Engine.h:791-796`); `getGameTime`/`getGameFrames` count only frames the update loop
  ran (`:828-831`); `setSimulationHeld` holds the simulation for a host (`:853-858`).
- **Player verdicts.** Launch options `--headless`, `--quit-after`, `--after-ready`, `--fixed-fps`,
  `--screenshot`, `--fail-on-error`... (`engine/core/player/LaunchOptions.cpp:73-93`); exit codes
  0, 2-7 (`engine/core/player/RunControl.h:22-31`, documented in `docs/mcp/guide/shipping.md:89-101`).
  The same options reach the web build from the page query (`argsFromQuery`,
  `engine/platform/emscripten/main.cpp:131-133`), Android from the property `debug.jasperengine.args`
  (`engine/platform/android/NativeEngine.cpp:567-575`), iOS and macOS from the process arguments
  (`engine/platform/apple/ios/ViewController.m:27`). RunControl reads the composite frame for a
  screenshot (`FrameReader`, `RunControl.h:33-34, 50-58`).
- **Player flavours.** Release variants are module subsets; "the development flavour is the full
  player" (`engine/player/variants.json:2-9`); `tools/mcp/build_cache.py:79` builds `release` and
  `development` runtimes. Optional runtime features are CMake options whose sites compile to nothing
  when off (precedent: `JASPERENGINE_AUDIO_CAPTURE`, `engine/CMakeLists.txt:1224-1232`).
- **Play's data folder is shared.** `Project::getUserDataPath` is always
  `<project>/.jasperengine/user/rundata` (`editor/Project.cpp:9831-9843`), served to the game by
  `editor::Platform::getUserDataPath` (`editor/Platform.cpp:99-100`); `UserSettings.xml` (PlayerPrefs)
  lives there and the MCP `user_settings` tool edits that same store "shared with the game in Play"
  (`editor/mcp/tools/DataTools.cpp:6, 56`; `docs/mcp/guide/lua-api.md:771, 843`). Every playtest and
  every agent experiment writes into one folder; there is no clean or snapshot mode.
- **Editor Lua sandbox.** Project editor menus run Lua "in a sandboxed editor state with an Editor
  table: Editor.call runs an MCP tool, so every edit is undoable"; discovery is a text scan that runs
  no project code (`editor/util/EditorScripts.h:6-22`).
- **Runaway Lua.** `LuaWatchdog` interrupts a function that ran 10 s without returning in editor
  Play; games never interrupt (`engine/core/script/LuaWatchdog.h:7-12`).
- **Non-asset folders.** `Recordings` and `EditorSettings` at the project top are skipped by scans and
  exports (`Project.cpp:5989-5998`); the generated `.gitignore` ignores `.jasperengine/`
  (`editor/util/VersionControlFiles.cpp:17-42`). A top-level `Tests` folder is today an ordinary asset
  folder.
- **Undoable file writes.** `WriteFilesCmd` writes files as one project undo step and restores the
  previous bytes on undo (`editor/command/type/WriteFilesCmd.h:9-26`).
- **Window menu.** Window > General holds Scene, Game, Inspector, Hierarchy, Project, Console,
  Bookmarks, Undo History, Play Mode Changes, Color Picker, Recorder (`editor/App.cpp:822-835`).
- **Game view toolbar** right group: Textures, HTML UI Debugger, Network, Simulate Touch, Maximize On
  Play, Stats, Physics (`editor/window/GameWindow.cpp:937-1040`); Simulate Touch is disabled while
  playing (`:1013-1018`).

## 4. Reference

**Craftsman (`/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor`).** A Minecraft PE 0.15
port reconstructed from a retail binary. Ideas only: no code, data, assets or Minecraft-specific
mechanics may be copied; everything here is genre-agnostic.

| Pattern studied | Take | Improve |
|---|---|---|
| ~100 per-feature "rig" executables (`src/*_rig_test.cpp`, 379 `add_executable` in `CMakeLists.txt`), each a list of numbered cases printing `N ok, M FAIL` (`src/clips_rig_test.cpp:1-24, 89, 1071`) | Scenario tests per feature, each case named, runnable alone (`clips_rig_test 2 3`) | One framework and one result schema instead of a hand-rolled counter per file; JUnit for CI |
| A save sandbox env var `MCPE015_SAVES` (`src/app_platform.cpp:349`) added after a self-test mutated a real world folder and then failed on its own leftovers (`src/android_main.cpp:5991-5999`) | Tests never touch a player's real data | The runner owns the sandbox on every target; no test-chosen roots (Craftsman tests fall back to fixed `/tmp/...` roots, `src/bed_test.cpp:1032`) |
| "141 rigs green" while the shipped content rendered wrong (the comparison report): host tests without real content or rendering | — | Cases run on the project's real scenes in real Play and real players; captures read the real renderer; the ledger shows which platforms a test never passed on |
| Replay of recorded play through the real engine (`src/clips_replay.cpp`) | Recordings as test input | Via the input spec's format and checkpoints |

**Unity.** Unity Test Framework: Window > General > Test Runner with Edit Mode and Play Mode tabs,
Run All / Run Selected / Rerun Failed, `[UnityTest]` coroutines (`yield return`), `LogAssert.Expect`
for expected errors (a logged error otherwise fails the test), `[UnityPlatform]` to limit platforms,
the command line `-runTests -testPlatform PlayMode -testResults results.xml`, and running Play Mode
tests in a player build. Names taken: Test Runner, Edit Mode, Play Mode, Run All, Run Selected,
Rerun Failed, expected logs. No code.

## 5. Requirements

### Functional: test files and discovery

- **R1 Test root.** The test root is `<project>/Tests/` when it contains `tests.yaml`. With the marker
  it is a non-asset folder (skipped by asset scans, imports and the Project window, as `Recordings`);
  without it, a `Tests` folder stays an asset folder (old projects keep their assets). Test Runner >
  Create > Test Folder writes `Tests/tests.yaml`; when `Tests/` already holds assets without the
  marker it refuses, naming the files, and offers `GameTests/`, which is then the root (the root is
  the first of `Tests`, `GameTests` holding the marker).
- **R2 Test files.** Every `*.test.lua` under the root, recursively, is a test file. Its id is its
  root-relative path (`ui/menu.test.lua`); a case's id is `<file id>#<case name>`. A chunk runs with
  an environment where `Test` is bound and every other global falls back to `_G`; `require` resolves
  the game's modules (`lua://`) and then the root.
- **R3 Static listing.** The Test Runner, `run_tests {action: "list"}` and the CLI list cases by a
  text scan of `Test.case("<literal>"` (and `Test.case('...'`), with their line, `mode` and `tags`
  when given as literals. No project code runs to list. A case with a computed name appears after its
  file first runs (its result names it). 200 files scan in at most 50 ms on desktop.
- **R4 `tests.yaml`.** Version 1, every key optional with the defaults of §6.2: `targets`, `fixed_fps`,
  `resolution`, `seed`, `timeout_frames`, `wait_frames`, `hang_seconds`, `image` tolerances,
  `console_errors` (fail|warn), `divergence` (fail|warn). Unknown keys are kept on rewrite and warned
  once; a newer `version` is refused with "tests.yaml version N needs a newer editor".

### Functional: the `Test` API and case semantics

- **R5 Cases.** `Test.case(name, [opts], fn)` registers a case; `Test.config(opts)` sets file
  defaults; `Test.setup(fn)` / `Test.teardown(fn)` run before and after each case of the file inside
  its session (teardown also after a failure). Options: `mode` (`"play"` default, `"edit"`), `scene`,
  `data`, `replay`, `fixedFps`, `resolution`, `timeoutFrames`, `tags`, `platforms`, `skip`,
  `expectErrors`. A duplicate case name in one file is an error result for both.
- **R6 Checks.** `Test.check(cond, msg)` records a failure and continues; `Test.assert(cond, msg)`,
  `Test.fail(msg)` record one and end the case; `Test.equal(actual, expected, msg)` and
  `Test.near(a, b, eps, msg)` print both values (tables compared deeply, at most 8 levels, the first
  differing path printed). Each failure carries the file, line (from the traceback) and the case
  frame.
- **R7 Waits in game time.** `Test.waitFrames(n)`, `Test.waitGameSeconds(s)` and
  `Test.waitUntil(pred, {frames?, gameSeconds?, message?})` resume after game frames (the predicate is
  checked once per frame after the scenes' update). A `waitUntil` past its cap (default `wait_frames`,
  600) ends the case: "waitUntil timed out after 600 frames (10.0 game s): <message>". No wait reads
  the wall clock.
- **R8 Input.** `Test.input(events | path, [opts])` applies events through the input spec's shared
  interpreter (the same objects `send_input` takes, or a `.input.json` script file) and returns when
  they are done; `until` waits inside them count against the case's caps. A recording file is
  refused here ("a recording replays from Play's start: use Test.case{replay = ...}").
- **R9 Replay cases.** `replay = "Input/x.input.json"` starts the case's session with the recording's
  scene, seed and frame rate (overriding the case's) and feeds it from frame 0; the body runs beside
  it (typically `Test.waitUntil(function() return not InputRecording.isReplaying() end)` then checks).
  A checkpoint mismatch is a `divergence` failure (frame, probes that differ) unless `divergence: warn`.
- **R10 Captures.** `Test.capture(name, [opts])` reads the case's frame (the screenshot path that waits
  for loading shaders and textures, `docs/testing.md` "A screenshot is of loaded content") and
  compares it with the golden (§6.5) using `image` tolerances, overridable per capture
  (`tolerance`, `maxFraction`, `maxDiff`, `maxMean`, `region {x, y, w, h}`). Outcomes: `passed`,
  `failed` (the diff numbers and a panel), `new` (no golden: a failure, never a pass), `pending`
  (a player run: compared at import). A capture never ends the case.
- **R11 Console.** An error-level log line during a case (game, engine or Lua) is a `console` failure
  unless it matches a pattern of `expectErrors` or of `Test.expectError(pattern)` (Lua pattern, case
  sensitive, each matching once); an expectation still unmatched at the end is a failure. With
  `console_errors: warn` the lines are attached without failing. Warnings are attached (first 20).
- **R12 Skips.** `skip = "reason"`, `Test.skip(reason)` and `platforms = {...}` not containing the
  target give `skipped` with the reason. A skipped case is never counted as passed.
- **R13 Context.** `fn(t)` gets the case: `t.name`, `t.id`, `t.frame` (frames since the case began),
  `t.gameTime`; `Test.target` is `{platform, backend, editor, resolution}` (read-only);
  `Test.log(...)` attaches lines to the result.

### Functional: running

- **R14 Play Mode session.** In the editor each Play Mode case gets its own Play session: Play starts
  its `scene` (default: the project's start scene) with fixed delta `1/fixedFps`, Error Pause off,
  `auto_pause` off, the Game view rendering at `resolution`, `math.randomseed(seed)` in the fresh Lua
  state before any script's `init`, the case's data folder (R20), and editor input to the Game view
  ignored (a banner says why). The body runs as an engine coroutine owned by the runner, resumed after
  the scenes' update each frame. Teardown, then Play stops discarding every play-mode change
  (`keep_changes: none`). Frames run unthrottled (VSync off), one game frame per editor frame.
- **R15 Edit Mode.** An Edit Mode case runs synchronously in the S23 sandbox state with `Editor.call`
  limited to tools annotated read-only; any other tool raises "Edit Mode tests are read-only:
  <tool> edits the project; test it in a Play Mode case". A wait raises "Edit Mode tests cannot wait:
  make it a Play Mode case". No Play session starts. Players exclude Edit Mode cases from their plan
  (`excluded_edit` count), never as skipped.
- **R16 Limits.** A case ends as `failed` at `timeoutFrames` (default `timeout_frames`, 18,000 frames
  = 5 game minutes at 60). A Lua call that runs 10 s without yielding is interrupted by LuaWatchdog
  (in players too, during a test run only): `error`. A case still running after `hang_seconds` of wall
  time (default 600) is `hung`: the guard is a hang detector, never an expected duration.
- **R17 Statuses.** `passed` (no failure), `failed`, `skipped`, `error` (the runner could not run it:
  missing scene, Lua syntax error, Play failed to start, duplicate name), `hung`. One error does not
  stop the run; the next case starts from a clean session.
- **R18 Selection.** A run takes a filter (case-insensitive substring of the case id, or `tag:<t>`,
  several joined by `,` meaning or), a mode (all|edit|play) and `failed_only` (the previous run's
  failed, error and hung cases). A selection of zero cases is not a pass (R30, R33, R35).
- **R19 Results.** Every run (editor, desktop player through the editor, any player, the CLI) yields
  one results document of the §6.4 schema: run (target, revision, versions, filter, wall time), counts,
  and per case status, frames, game seconds, wall ms, failures, captures, console lines, skip reason.

### Functional: data sandbox and Play Data

- **R20 Case data.** A case's `data` is `"clean"` (default: an empty folder) or a snapshot name
  (`Tests/Data/<name>/` copied into an empty folder). The folder is the game's `data://` for the
  case, PlayerPrefs included, and is deleted after the case (kept with `--keep-test-data` or
  `keep_data`). `data = "project"` is refused: "tests never use the project's Play data: save a
  snapshot (Game view > Play Data > Save Snapshot...) and use data = \"<name>\"". Nothing a case does
  changes `.jasperengine/user/rundata` or a snapshot.
- **R21 Play Data profiles.** The Game view toolbar gets Play Data: Project (today's folder, the
  default), Clean (an empty session folder, deleted at Play stop), and Snapshot > `<name>` (a copy of
  `Tests/Data/<name>/`, deleted at stop). While a profile other than Project is active,
  `Project::getUserDataPath`, the UserSettings store, `user_settings` and Edit > Clear User Settings
  act on the session folder, and the toolbar shows the profile's name in the accent colour. The menu
  also has Save Snapshot... (copies the project's data, or while playing the session's data at the
  next frame boundary, into `Tests/Data/<name>/`; one undo step) and Open Data Folder.
- **R22 Snapshot names.** `[A-Za-z0-9_-]{1,64}`; an existing name asks to replace (undoable). A
  snapshot over 64 MB is saved with a warning (it is committed data).

### Functional: players

- **R23 Player flags.** Development players gain `--run-tests` (run every Play Mode case in the
  content), `--test-filter F` (R18's syntax), `--test-results PATH` (write the results JSON) and
  `--keep-test-data`. They imply `--after-ready` and `--fixed-fps` from `tests.yaml` unless given.
  The web reads them from the query (`?run-tests&test-filter=smoke`), Android from
  `debug.jasperengine.args`, iOS and desktops from the command line.
- **R24 Player sessions.** For each case the player resets the game session (unloads every scene,
  re-creates the Lua state, resets Input, InputActions, the clocks and the application state, as the
  editor's Play stop and start do), applies the case's data folder, seed and fixed delta, loads its
  scene and runs it as R14. Captures write PNGs of the composite frame into
  `<results dir>/captures/<case id>/<name>.png` with status `pending`.
- **R25 Player sandbox.** A player test run sets `data://` to
  `<user data root>/.jasper-test-runs/<run id>/<case n>/` (the web: `/datafs/.jasper-test-runs/...`,
  never synced to IndexedDB), deletes it after each case, and never reads or writes the game's real
  user data folder or its PlayerPrefs. `--test-results` defaults to
  `<user data root>/.jasper-test-results/<run id>.json` (outside the sandbox, so `adb`/`simctl` can
  fetch it from a debuggable build).
- **R26 Log protocol.** Category `test`, info level: `[test] begin {"cases": N, "run": "<id>"}`, one
  `[test] case <json>` per case (its §6.4 object, console list cut to 20 lines, the whole line at most
  16 KB), `[test] summary <counts json>`. These lines are the only channel guaranteed on every
  target (logcat, the browser console, stdout, the iOS log).
- **R27 Exit codes.** `EXIT_TESTS_FAILED = 8` (a case failed, errored or hung), `EXIT_NO_TESTS = 9`
  (the filter matched nothing, or the content holds no tests: "no tests in this build: build a
  Development Build with Include Tests"). All passed or skipped: 0. A release player given
  `--run-tests` exits 2: "this player has no test runner (release): build a Development Build with
  Include Tests". `--fail-on-error` is ignored during a test run (R11 owns errors).

### Functional: results ledger

- **R28 Ledger.** Each finished run (and each import) updates
  `<project>/.jasperengine/test-results/ledger.json`: per case id, per target platform (`editor`,
  `windows`, `linux`, `macos`, `android`, `ios`, `web`), the last status, last run time, last pass
  time, backend, editor version, project revision (S29's VCS revision id and a dirty flag; without
  VCS the test file's SHA-256). The last 20 runs' result documents are kept in `runs/`, the last run's
  captures in `captures/`. Written through AtomicFile.
- **R29 Unverified.** A case is unverified on a target of `tests.yaml` `targets` when the ledger has
  no pass there at the current revision (clean tree), or, without VCS, with the current file hash.
  `run_tests` reports `unverified` per case for the listed targets only; the Test Runner shows Last
  Passed On and an Unverified On filter.
- **R30 Import.** `run_tests {action: "import", results_file}` (and `test --import`) validates a
  results document (schema version, project name, case ids it knows or new), compares its pending
  captures (PNGs beside it) with the goldens, updates the ledger and returns the counts as a run would.

### Platform & backends

- **R31** Play Mode cases, captures, the sandbox and the log protocol work on Windows, Linux, macOS,
  Android, iOS and the web, on every backend through sokol-gfx; nothing uses a desktop-only API.
  Captures use RunControl's composite readback, which every backend has.
- **R32** The player-side runner is compiled under `JASPERENGINE_TEST_RUNNER` (ON for the editor, the
  unit tests and development players; OFF for release players): its sites compile to nothing when
  off.

### Performance

- **R33** A game not running tests pays nothing: `Test` is not bound, no hook runs per frame, and a
  release player's stripped size is unchanged (byte-identical code sections apart from the build id).
- **R34** The runner adds at most 0.05 ms CPU per frame (coroutine resume plus a predicate of one
  table read) on desktop, 0.2 ms on a mid Android device, measured over 600 frames.
- **R35** A Play Mode case's overhead over a bare `play start` + `play stop` of the same scene is at
  most 50 ms wall on desktop (data folder, seed, render size, result), reported through
  `harness.over_budget`.
- **R36** A snapshot of 10 MB is copied into a session folder in at most 100 ms on desktop SSD and
  1 s on Android (a test's scale; larger snapshots warn, R22).
- **R37** The static listing (R3) of 200 files takes at most 50 ms; the ledger read and write at most
  10 ms each for 2,000 cases.

### Editor UX (human)

- **R38 Test Runner window** (Window > General > Test Runner, command id `window.testRunner`):
  - toolbar: Run All, Run Selected, Rerun Failed, Cancel (while running), Target (Editor, This
    Computer), a filter field, a status filter (All, Failed, Skipped, Unverified On...), Create
    (Test Script, Test Folder);
  - tabs Edit Mode and Play Mode (Unity's), each a tree of files and cases with status icons and
    columns Name, Time (game seconds and frames), Last Passed On (platform chips, tooltip: date,
    backend, revision), Tags;
  - a counts bar ("12 passed, 1 failed, 2 skipped, 3 unverified") and a progress bar while running;
  - a detail pane for the selected case: failures with click-to-source (S23's console source links),
    console lines, and per capture the golden, the actual and the diff with Accept Image (undoable,
    `WriteFilesCmd`) and Accept For This Target Only;
  - context menu: Run, Open Script, Copy ID, Reveal Golden;
  - "No tests yet: Create Test Folder" when there is no root.
- **R39** Create > Test Script writes `Tests/<name>.test.lua` from a commented template (one Play
  Mode case on the start scene, one Edit Mode case) as one undo step.
- **R40** Build Settings shows Include Tests under Development Build (key `build.include_tests`,
  default false); greyed with a tooltip when Development Build is off.
- **R41** Game view toolbar Play Data (R21), disabled while playing (as Simulate Touch), with the
  label shown in full when not Project even when the toolbar collapses to icons.

### Agent UX (MCP, Lua, CLI)

- **R42 `run_tests`** (group `testing`): actions `run` (default), `list`, `import`, `accept`; a job
  past 1 s. Returns a text summary, `structuredContent` with `counts`, `failures` (first 20: id,
  status, kind, message, file, line, frame), `unverified`, `results_file`, `next`; at most one image
  (the first failed capture's a | b | diff panel). Errors name the fix (§6.6).
- **R43 `play {data}`**: `"project"` (default), `"clean"`, or a snapshot name; start only.
  `get_play_state` gains `data: {profile, folder}`; `user_settings` results gain `profile`.
- **R44 CLI** `jasperengine-editor test` (§6.7), built on `run_tests` through the `call` machinery
  (attach to the HTTP editor that has the project, else a one-shot headless editor); `--junit` and
  `--json` files; exit 0, 1, 2, 3.
- **R45 Lua stubs** `engine/core/script/binding/stubs/Test.lua` (hand-written: bound with the Lua C
  API), joined by `tools/lua_api`; `Test` documented in `docs/mcp/guide/lua-api.md`.
- **R46 Guide.** New `docs/mcp/guide/testing.md` (`jasper://docs/testing`): when to write a test,
  the loop (write case, `run_tests`, read `failures`, fix, re-run), captures and Accept, data
  snapshots, players and import. One "Other areas" line in the agent guide naming `run_tests` and
  `jasper://docs/testing`, with an eval case. `authoring.md` "Check what you built" gains one bullet:
  "Keep what you checked: a case in `Tests/` and `run_tests`".
- **R47 Human docs.** `docs/game-tests.md`: the folder, the API, the window, CI with the CLI and
  JUnit, players and the ledger.

### Data & compatibility

- **R48** New: `Tests/tests.yaml` (v1), `*.test.lua`, `Tests/Golden/**.png`, `Tests/Data/<name>/**`,
  the ledger (v1, not committed: under `.jasperengine/`), the results document (v1), the project
  setting `build.include_tests` (default false; no version bump), the per-user workspace key
  `game_view.play_data` (default `project`).
- **R49** Old projects load unchanged: no marker, no root; a top-level `Tests` asset folder without
  `tests.yaml` keeps its assets and GUIDs (R1).
- **R50** Exports leave the root out unless the build is a Development build with Include Tests; then
  `tests/` (the root minus `Golden/`) goes into the content pack. A release build with the key set
  builds without tests and warns once: "Include Tests applies to Development builds only".
- **R51** A newer results document or ledger version is refused with "made by a newer editor" (the
  ledger is then moved aside to `ledger.v<N>.json` and a new one started).

## 6. Design constraints & decisions

### 6.1 Layout

```
<project>/Tests/
  tests.yaml                    marker + defaults (committed)
  menu.test.lua                 cases
  shop/purchase.test.lua
  Golden/<case id>/<name>.png                  any platform and backend
  Golden/<case id>/<name>.<backend>.png        e.g. level1_start.vulkan.png
  Golden/<case id>/<name>.<platform>-<backend>.png
  Data/<snapshot>/...           copied into a case's data:// folder
  Input/*.input.json            recordings and scripts (input spec)
<project>/.jasperengine/test-results/   ledger.json, runs/<run id>.json, captures/ (not committed)
```

`<case id>` in a path replaces `/` and `#` with `__` and other characters outside `[A-Za-z0-9_.-]`
with `_` (`ui__menu.test.lua__Start opens the level` becomes `ui__menu.test.lua__Start_opens_the_level`).

### 6.2 `tests.yaml` defaults

```yaml
version: 1
targets: [editor]        # editor, windows, linux, macos, android, ios, web
fixed_fps: 60
resolution: 1280x720
seed: 1
timeout_frames: 18000
wait_frames: 600
hang_seconds: 600
image: {tolerance: 8, max_fraction: 0.001, max_diff: 48, max_mean: 0.25}
console_errors: fail     # fail | warn
divergence: fail         # fail | warn
```

### 6.3 Lua API (stub sketch)

```lua
---@meta _
-- Hand-written: Test is bound with the Lua C API (engine/core/test/TestLua.cpp), only inside test chunks.

---@class TestCaseOptions
---@field mode? "play"|"edit"          # default "play"
---@field scene? string                # Build Settings name or path; default the start scene
---@field data? string                 # "clean" (default) or a Tests/Data snapshot name
---@field replay? string               # an Input/*.input.json recording (input spec)
---@field fixedFps? integer
---@field resolution? string           # "WxH"
---@field timeoutFrames? integer
---@field tags? string[]
---@field platforms? string[]          # only on these targets (editor, linux, android, web...)
---@field skip? string                 # skip with this reason
---@field expectErrors? string[]       # Lua patterns of error lines this case expects

---@class TestCase
---@field name string    # read-only
---@field id string      # read-only: "<file>#<name>"
---@field frame integer  # read-only: frames since the case began
---@field gameTime number

---@class TestTarget
---@field platform string ---@field backend string ---@field editor boolean ---@field resolution string

Test = {}
---@type TestTarget
Test.target = nil
---@param name string ---@param opts TestCaseOptions|fun(t: TestCase) ---@param fn? fun(t: TestCase)
function Test.case(name, opts, fn) end
---@param opts TestCaseOptions
function Test.config(opts) end
function Test.setup(fn) end
function Test.teardown(fn) end
function Test.check(cond, message) end      -- records, continues
function Test.assert(cond, message) end     -- records, ends the case
function Test.equal(actual, expected, message) end
function Test.near(a, b, epsilon, message) end
function Test.fail(message) end
function Test.skip(reason) end
---@return integer frames
function Test.waitFrames(n) end
function Test.waitGameSeconds(seconds) end
---@param opts? {frames?: integer, gameSeconds?: number, message?: string}
function Test.waitUntil(predicate, opts) end
---@param events table[]|string  # send_input events, or an Input/*.input.json script
function Test.input(events, opts) end
---@param opts? {tolerance?: integer, maxFraction?: number, maxDiff?: integer, maxMean?: number, region?: {x: integer, y: integer, w: integer, h: integer}}
function Test.capture(name, opts) end
function Test.expectError(pattern) end
function Test.log(...) end
```

Example (the template of R39):

```lua
Test.config{ scene = "Scenes/Menu.scene", tags = {"smoke"} }

Test.case("Start opens Level 1 with three lives", function(t)
    Test.input{ {type = "touch", action = "tap", target = "#start"} }
    Test.waitUntil(function() return SceneManager.getActiveScene().name == "Level1" end,
                   {gameSeconds = 5, message = "Level1 never loaded"})
    Test.equal(Player.lives, 3, "lives at start")
    Test.capture("level1_start")
end)

Test.case("Every scene in the build has a camera", {mode = "edit"}, function(t)
    for _, s in ipairs(Editor.call("list_scenes", {}).scenes) do
        local cams = Editor.call("find_entities", {scene = s.id, component = "camera"})
        Test.check(#cams.entities > 0, s.name .. " has no camera")
    end
end)
```

### 6.4 Results document (v1)

```json
{"format": "jasperengine-test-results", "version": 1,
 "run": {"id": "20261119-101500-3f2a", "project": "Garden",
         "target": {"platform": "linux", "arch": "x64", "backend": "glcore", "editor": true, "device": ""},
         "revision": {"vcs": "git", "id": "97d4506", "dirty": false},
         "editor_version": "…", "engine_version": "…", "started": "2026-11-19T10:15:00Z",
         "wall_seconds": 12.3, "filter": "tag:smoke", "mode": "all"},
 "counts": {"total": 7, "passed": 5, "failed": 1, "skipped": 1, "error": 0, "hung": 0, "excluded_edit": 0},
 "cases": [{"id": "menu.test.lua#Start opens Level 1 with three lives", "file": "menu.test.lua",
            "line": 3, "name": "…", "mode": "play", "status": "failed", "tags": ["smoke"],
            "frames": 312, "game_seconds": 5.2, "wall_ms": 840, "skip_reason": "",
            "failures": [{"kind": "assert", "message": "lives at start: expected 3, got 2",
                          "file": "menu.test.lua", "line": 6, "frame": 300}],
            "captures": [{"name": "level1_start", "status": "failed", "actual": "captures/…/level1_start.png",
                          "golden": "Golden/…/level1_start.png",
                          "diff": {"max": 112, "mean": 0.9, "fraction_over": 0.004, "bbox": [10, 20, 64, 48]}}],
            "console": [{"level": "error", "message": "…", "source": {"file": "player.lua", "line": 40}}],
            "log": ["…"]}]}
```

Failure `kind`: `check`, `assert`, `equal`, `timeout`, `error` (Lua error with traceback), `console`,
`expected_error_missing`, `capture`, `divergence`, `hung`. JUnit mapping: one `<testsuite>` per file,
`<testcase classname="<file id>" name="<case name>" time="<wall s>">`; failed → `<failure
message type="<kind>">`, error and hung → `<error>`, skipped → `<skipped message>`; `<system-out>`
holds the log and console lines.

### 6.5 Goldens

Lookup for a capture on (platform, backend): `<name>.<platform>-<backend>.png`, then
`<name>.<backend>.png`, then `<name>.png`. The capture must have the golden's size, else `failed`:
"capture is 1280x720, golden is 1920x1080 (resolution changed?)". Comparison is `diff_images`' code
(`ImageDiffTools.cpp`) moved into a function the runner and the tool share; premultiplied mode. Accept
writes `<name>.png` when no golden exists, else the file that was compared; Accept For This Target
Only writes `<name>.<platform>-<backend>.png`. The actual images of the last run stay in
`.jasperengine/test-results/captures/`.

### 6.6 MCP

```text
run_tests  group testing  ≤ 1,400 characters, Control activity, allowedWhilePlaying false
  action?: "run"|"list"|"import"|"accept"   default "run"
  filter?: string        substring of "<file>#<case>", or tag:<t>; "," = or
  mode?: "all"|"edit"|"play"    default "all"
  target?: "editor"|"desktop"   default "editor" (desktop: a Development player with tests, built prebuilt)
  failed_only?: boolean
  results_file?: string  import: a results JSON (its captures/ beside it)
  captures?: string[]    accept: "<case id>/<name>", or ["all"] for every new/failed capture of the last run
  for_target?: boolean   accept: write the platform-backend golden
  junit?: string         also write JUnit XML here (absolute or project-relative)
  keep_data?: boolean
  wait?: boolean (true), timeout_seconds?: number (600)
```

Description (≤ 400 chars): "Run the project's tests (Tests/*.test.lua) as a job: Edit Mode and Play Mode cases, each Play case
in a fresh session at a fixed frame rate on clean data. Returns counts, failures (file:line, frame),
unverified targets and next. list: cases without running them. import: a player's results file.
accept: make captures the goldens. A filter matching nothing is an error."

| Situation | Text (error) or `next` (success) |
|---|---|
| no root | "No tests in this project: create Tests/tests.yaml (Test Runner > Create > Test Folder, or a file with `version: 1`)." |
| nothing matched | "No test matched \"<filter>\" (mode <m>): run_tests {action: \"list\"} shows the case ids." |
| Play running | "Stop Play first: tests start their own sessions (play {action: \"stop\"})." |
| a capture failed or is new | next: "Look: diff_images {a: \"<actual>\", b: \"<golden>\"}; if the change is intended: run_tests {action: \"accept\", captures: [\"<id>/<name>\"]}." |
| unverified | next: "Unverified on android, web: run the Development build there with --run-tests, then run_tests {action: \"import\", results_file}." |
| target web/android/ios | "This editor runs editor and desktop targets; for <t>: build with Include Tests, run the player with --run-tests (or build-and-run-on-device), then import." |

`play` gains `data` (`stringEnum`-free string: "project", "clean" or a snapshot name; the handler
validates and lists the snapshots in its error). Its bytes are found by trimming `play`'s description;
if that cannot cover them, `budget.json`'s `play` exception rises by the measured bytes with this
spec's reason and eval case. `run_command` reaches `window.testRunner` (no `focus_window` change).

### 6.7 CLI

```text
jasperengine-editor test --project <dir> [--filter F] [--mode all|edit|play] [--target editor|desktop]
                         [--failed-only] [--junit FILE] [--json FILE] [--keep-data]
jasperengine-editor test --project <dir> --import <results.json> [--junit FILE]
jasperengine-editor test --project <dir> --list [--json]
exit 0  every selected case passed or was skipped
     1  a case failed, errored or hung
     2  bad arguments, or it could not run (project locked by a stdio editor, no editor binary)
     3  no case matched the filter
```

Prints one line per case (`PASS`, `FAIL`, `SKIP`, `ERROR`, `HUNG` + id + first failure as
`file:line: message`) and the counts; `--json` writes the results document.

### 6.8 Architecture

```cpp
// engine/core/test/TestRunner.h (JASPERENGINE_TEST_RUNNER)
namespace jasperengine::test {
    struct CaseSpec { std::string id, file, name; int line = 0; Mode mode = Mode::Play; std::string scene, data, replay;
                      int fixedFps = 60; Size resolution; int64_t timeoutFrames, waitFrames; uint64_t seed;
                      std::vector<std::string> tags, platforms, expectErrors; std::string skip; };
    struct CaseResult { /* §6.4 case object */ };
    class Host {   // the editor (Play sessions) or a player (session reset)
    public:
        virtual bool beginSession(const CaseSpec&, std::string& error) = 0;   // scene, data, seed, delta, size
        virtual void endSession() = 0;
        virtual bool capture(const std::string& name, Image& out, std::string& error) = 0;
        virtual std::string dataFolder() const = 0;
        virtual ~Host() = default;
    };
    class TestRunner {
    public:
        static std::vector<CaseSpec> scan(const std::filesystem::path& root);   // R3, no Lua
        void start(std::vector<CaseSpec> plan, Host& host, RunOptions options);
        bool frame();             // after the scenes' update: resume the case coroutine; false when done
        const RunResult& result() const;
        void cancel();
    };
}
```

- One runner for editor and players; the editor's `TestRunHost` (`editor/test/`) starts Play
  through `Project`, sets the Game view render size through the Recorder's host functions
  (`editor/recorder/RecorderSession.h:48-60`), captures through the screenshot pipeline; the player
  host resets the session (R24) and reads RunControl's composite.
- Edit Mode cases run in `editor/test/EditModeTests.cpp` on the S23 sandbox; they never reach the
  core runner's frame loop.
- The `Test` table is created per chunk environment (R2), never in `_G`: a game defining a global
  `Test` is untouched.
- The console watch is a log sink active only during a case (as RunControl's `--fail-on-error` sink,
  `RunControl.cpp:98`).
- If part of the editor's Play start/stop reset lives in editor code, it moves into
  `Engine::resetGameSession()` so the player host calls the same code (one reset, R24).
- `run_tests` runs one run at a time; a second call while running is refused through the busy policy
  ("A test run is in progress: get_job <id> or cancel_job").

## 7. Acceptance criteria

The fixture is `tools/test-projects/game-tests`: a menu scene with a Start button (HTML UI), a level
with a player, three lives, a coin pickup that writes `data://progress.json`, a deliberately failing
case tagged `red`, a skipped case, an Edit Mode case, goldens for `level1_start`, and a snapshot
`one_coin`. Red controls are named per criterion; each run's log is kept with the PR's evidence.

- **AC1 Static listing.** `TestScriptScan.ListsCasesWithoutRunningCode` (editor gtest,
  `editor/tests/test/TestScriptScanTest.cpp`): a file whose top level calls `error()` still lists its
  three literal cases with lines, modes and tags; 200 generated files scan under 50 ms (`perfSlack()`).
  Red: the scan replaced by loading the chunk raises.
- **AC2 Case semantics.** `TestRunnerCore.*` (core gtest, `engine/tests/core/test/TestRunnerTest.cpp`,
  a fake Host stepping frames): check continues and assert ends; `waitUntil` fails at exactly its cap
  frame; a logged error fails the case and `expectError` absorbs exactly one; an unmatched
  expectation fails; skip and `platforms` give skipped; setup and teardown order, teardown after a
  failure; duplicate names give error. Red: each assertion against a runner with that rule removed.
- **AC3 Editor run.** `tools/mcp/tests/test_project_tests.py::test_run_tests_counts_and_failures`
  (CI = True, a scratch copy of the fixture): `run_tests {filter: "tag:smoke,tag:red"}` returns exact
  counts, the red case's failure with file, line and frame, a results file of schema v1; the
  project's `.jasperengine/user/rundata` hash is unchanged after the run. Red: run on the base tree
  (no `run_tests`).
- **AC4 Nothing matched.** Same file, `test_filter_matching_nothing_is_an_error`: isError with the
  `list` hint; the CLI exits 3. Red: a runner returning an empty pass.
- **AC5 Captures.** `test_captures_new_accept_and_fail`: with the golden removed the case fails
  `new`; `run_tests {action: "accept", captures: [...]}` writes it as one undo step (`undo` removes
  it); the next run passes; moving the player 2 m in the scene fails with a diff and one image.
  Red: comparison skipped (would pass `new`).
- **AC6 Data sandbox.** `test_case_data_is_sandboxed`: the coin case on `clean` writes
  `progress.json` into its case folder only; the `one_coin` case reads the snapshot's coin and its
  write leaves `Tests/Data/one_coin/` byte-identical; `data = "project"` gives error with the fixed
  text. Red: a host that leaves `getUserDataPath` at `rundata` (the hash check fails).
- **AC7 Play Data profiles.** `test_play_data_profiles`: `play {action: "start", data: "clean"}`,
  `user_settings set` reports `profile: "clean"` (and `store: "prefs"`) and the project's `UserSettings.xml` is unchanged after
  stop; the session folder is gone after stop; `get_play_state.data` names the profile. Editor gtest
  `PlayDataProfile.SnapshotSessionIsACopy` checks the copy and deletion. Red: profile ignored.
- **AC8 Edit Mode is read-only.** `test_edit_mode_cases`: the camera case passes; a case calling
  `Editor.call("set_properties", ...)` fails with the read-only text and the scene is unchanged
  (`get_undo_history` has no new step). Red: an unrestricted `Editor.call` (the property changes).
- **AC9 CLI and JUnit.** `test_cli_test_command`: `jasperengine-editor test --project <copy>
  --filter tag:smoke --junit out.xml` exits 0; with `tag:red` exits 1; `out.xml` parses
  (`xml.etree`) with the right `tests`, `failures`, `skipped` counts and `<failure type="assert">`.
- **AC10 Desktop player.** `test_project_tests.py::test_desktop_player_runs_tests`
  (`PREBUILT_PLAYERS = ["glcore-development"]`, prebuilt per `docs/testing.md` (b)): `run_tests
  {target: "desktop"}` builds with Include Tests, the player exits 8 with the red case and 0 filtered
  to smoke; the `[test] summary` line matches the results file; the pending capture is compared at
  import; the player's real user data folder (`$XDG_DATA_HOME/<id>`) has the same listing before and
  after. Red: a player run without `--run-tests` support (exit 2).
- **AC11 Release player refuses.** Core gtest `LaunchOptions.RunTestsNeedsTheTestRunner` (built with
  the option OFF): `--run-tests` parses to exit 2 with the fixed message; ON: accepted with filter
  and results path. Plus `LaunchOptions.TestFlagsFromTheQuery`: `?run-tests&test-filter=smoke`.
- **AC12 Web.** Web gate `GATE=game-tests` (after `WEB_PROJECT=game-tests DEVELOPMENT=1 tools/build.sh
  web` with Include Tests): `tools/web/gates/game-tests.json` lists the console lines `[test] summary
  {"total":4,"passed":3,...}` and `JasperEngine exit code 0` in order (query
  `?run-tests&test-filter=tag:smoke`). Red: the gate on a build without tests (exit 9 line).
- **AC13 Android.** `tools/android-emulator-test.sh build/android-game-tests/app-debug.apk --setprop
  "debug.jasperengine.args=--run-tests --test-filter tag:smoke" --expect-file
  tools/test-projects/game-tests/android-expect.txt` (`[test] summary` with `"failed":0`); documented
  in `docs/testing.md`'s emulator section; run on a KVM server for the PR evidence. The script's
  `--setprop` value check (`tools/android-emulator-test.sh:136`, today `[A-Za-z0-9_.-]*`) widens to
  spaces, `:` and `,` (quoted for `adb shell setprop`), with a case in its own self-test.
- **AC14 Ledger and Unverified.** `test_ledger_unverified`: with `targets: [editor, android]`, after
  the editor run every passing case reports `unverified: ["android"]`; importing the AC13 results file
  (checked-in sample `tools/test-projects/game-tests/samples/android-results.json`) clears it for the
  cases it passed; the window's Last Passed On chips show both (screenshot evidence).
- **AC15 Limits.** `TestRunnerCore.HangAndTimeoutAreDifferent`: a never-true `waitUntil` without a cap
  fails at `timeout_frames`; the fake host's wall clock past `hang_seconds` gives `hung`. MCP
  `test_runaway_lua_is_an_error`: `while true do end` in a case is interrupted (error, Play stopped,
  next case runs).
- **AC16 Replay case.** `test_replay_case_divergence` (uses the input spec's recorder): a recording
  replays green; after an obstacle in the scene is moved, the case fails with `divergence` naming the
  frame and a probe. Red: `divergence: warn` passes it.
- **AC17 Exports.** Editor gtest `ExportTests.IncludeTestsOnlyInDevelopmentBuilds`: a release export
  with the key set has no `tests/` entries and one warning; a development export has `tests/` without
  `Golden/`. `ProjectNonAssetDirs.TestsFolderNeedsItsMarker` (extends
  `editor/tests/ProjectNonAssetDirsTest.cpp`): `Tests/` without `tests.yaml` is scanned as assets,
  with it is not. Red: marker check removed (the asset folder disappears).
- **AC18 Zero cost.** `test_test_api_absent_outside_runs`: in a normal Play `eval_lua {code: "return
  Test == nil"}` is true and a game global `Test = 5` survives a test run; the release player's
  `.text` size equals the base tree's (S34's size report, `tools/perf/` as S34 measures). Benchmark:
  `TestRunnerBench.FrameOverhead` ≤ 0.05 ms/frame (`perfSlack()`), `test_case_overhead` via
  `harness.over_budget` ≤ 50 ms.
- **AC19 Window.** `test_test_runner_window`: `run_command {command: "window.testRunner"}`, Run All
  through `find_ui_item` + `send_input`, then a screenshot of the window with counts and a failed
  capture's panel (PR evidence); Accept Image through the UI is undone by `undo`.
- **AC20 Docs and budgets.** `tool_budget.py --check tools/mcp/budget.json` (new group entry,
  `play` within its limit), `gen_tool_docs.py --check`, `test_tool_conventions.py`,
  `test_tool_coverage.py`; eval cases `guard_tests_run_tests` (an agent asked to make a fix stick
  writes a case and runs `run_tests`) and `bad_tests_project_data` (an agent using `data =
  "project"` reads the error and switches to a snapshot), each failing without its text;
  `tools/lua_api/tests/test_lua_api.py` with the `Test` stub and the guide's Lua blocks.

## 8. Dependencies & conflicts

- **Consumes** `input-recorder-and-input-scripts.md`: the event format, the shared interpreter
  (`Test.input`), recordings and checkpoints (replay cases). That spec lands its format and
  interpreter first (its P1); this spec's replay cases (R9, AC16) land after its recorder.
- **Extended by** `build-and-run-on-device.md` (web, Android, iOS targets for `run_tests`, fetching the
  results file; until then its `run_build device` with `args: ["--run-tests"]` runs a device and the
  results are imported), `build-profiles.md` (Include Tests per profile: it reads `build.include_tests` as a
  profile field), `savegame.md` (its save slots live under `data://`, so the sandbox and Play Data
  profiles cover them; it should name `Tests/Data` snapshots as its test fixtures),
  `dev-console-for-agents.md` (a console command may run `run_tests`), `shipped-game-diagnostics.md`
  (the log protocol's category `test` is excluded from shipped crash reports).
- **Existing specs**: S23 (editor Lua sandbox; script editor click-to-source), S24 (undo of Accept
  and snapshots), S27 (Recorder host functions for the render size), S29 (revision ids), S30 (launch
  options, exit codes, web query), S31 (CLI on the registry), S34 (development flavour, size report),
  S35 (a test may assert on an audio session later; nothing now), S62 (captures never use stand-ins).
- **Merge hotspots**: `editor/mcp/tools/PlayTools.cpp` (`play` schema, `get_play_state`; the input
  spec edits `send_input` and `play` too), `engine/core/player/LaunchOptions.cpp` and `RunControl.*`
  (both specs add flags; the input spec adds `--input-script`), `editor/window/GameWindow.cpp`
  toolbar, `editor/App.cpp` Window menu, `editor/Project.cpp` (`getUserDataPath`, non-asset dirs),
  the exporters (`editor/Exporter.cpp`, `ExporterPrebuilt.cpp`), `editor/mcp/tools/McpTools.cpp`,
  `McpToolGroups.cpp`, `tools/mcp/budget.json`, `docs/mcp/TOOLS.md`, `editor/cli/CommandLine.cpp`,
  `tools/android-emulator-test.sh` (the `--setprop` value check; the input spec needs it too).

## 9. Risks & defaults

- **Tests flake on nondeterminism.** Default: fixed delta, fixed seed, game-time waits, state waits;
  wall clock only as a hang guard. Remaining sources (async loads finishing on different frames,
  HTTP, `os.time`) are documented in `testing.md`; the advice is `waitUntil` on state.
- **Fresh Play per case is slow on big scenes.** Default kept (isolation beats speed); `Test.config{
  session = "file"}` is not offered in v1. Measured on the fixture by AC18; revisit with numbers.
- **Goldens differ across GPUs.** Default: the fallback chain and per-target accept; tolerances from
  `diff_images`. Device goldens are accepted from imported runs of that device.
- **Player session reset may not be complete** (static state outside the engine reset). Default:
  `Engine::resetGameSession()` is the single reset; a leak found by a test is a bug in it, not a
  reason to restart processes.
- **`Tests` collides with an existing asset folder.** Default: the marker rule (R1) and `GameTests`.
- **Edit Mode tests need writes** (generate then check). Default: no; write such checks as Play Mode
  cases or as editor menu items. Revisit if a real project needs it.
- **Results on devices are hard to fetch.** Default: the log protocol is enough to build the
  document (`[test] case` lines are complete objects); the file is a convenience.
- **Ledger committed by teams for shared coverage.** Default: not committed (merge conflicts); JUnit
  from CI is the shared record.

## Deviations

- **Namespace.** The runner lives in `jasperengine::gametest`, not `jasperengine::test`: the unit-test
  support library already owns `jasperengine::test` (`TempDir`, `TestSystem`), and inside
  `jasperengine::editor` the name `test::` resolves to `editor::test` first.
- **The option's macro.** `JASPERENGINE_TEST_RUNNER` reaches the engine's sites through the generated
  header `TestRunnerConfig.h` (the engine's private `generated/` folder), not a compile definition on
  every source: the few sites include it, and turning the option over does not rebuild the engine.
  The runner's sources (`core/test/*`, `core/player/PlayerTests.cpp`) are left out of a build without it.
- **Edit Mode console watch.** The console watch (R11) applies to Play Mode cases; Edit Mode cases run
  synchronously and report Lua errors and failed checks only.
- **Runaway interrupt.** R16's 10 s interrupt is the runner's own count hook on the case's coroutine
  (and on `waitUntil`'s predicate), in the editor and in players alike; game scripts keep the editor's
  hang watchdog. The message is `script interrupted: <file>:<line> ran for 10.0 s without yielding
  (test runner)`, and the case is an `error`.
- **Play Data choice.** The Game view's Play Data choice is kept per user and project in
  `.jasperengine/user/game_view.yaml` (`play_data`), not in the workspace file, and the session folder
  is read by `Project::getUserDataPath` from `gametests::PlayData` (no `Project` member): no
  project-wide header changes for a per-session value.
- **The Project window.** As `Recordings` and `EditorSettings` are, the test root stays visible in the
  Project window (its files are edited there); asset scans, imports and exports skip it.
- **Replacing a snapshot.** Save Snapshot over an existing name writes the new files as one undo step;
  files of the earlier snapshot that the new data lacks are kept and named in a warning (a
  `WriteFilesCmd` step cannot delete).
- **`run_tests` while playing.** `run` is refused by the handler with §6.6's text ("Stop Play first...")
  instead of `allowedWhilePlaying: false`, whose server text differs; `list`, `import` and `accept`
  work while playing.
- **The fixture.** The MCP tests build the fixture project through the tools on a fresh project
  (`test_project_tests.py` `setup`: a Player cube and a Coin script, `Tests/` written beside them),
  since `tools/test-projects/*` are engine-only projects; only the checked-in Android sample
  (`tools/test-projects/game-tests/samples/android-results.json`) lives there.
- **Player session reset.** A player's case releases the scripts of every running scene (the
  persistent scene's too), makes a new Lua state, seeds `math.random`, sets the fixed delta and
  reloads the case's scene (`player::PlayerSession`, `engine/core/player/PlayerTests.cpp`), not
  `Engine::resetGameSession()`: Input, InputActions and the clocks are not reset (the runner counts a
  case's frames and game time from its own start), and the persistent scene's objects stay without
  scripts. The editor makes a new Lua state before each case's Play too
  (`LuaBinding::recreateLuaState`), since its Play keeps one state across sessions.
- **The build setting.** Include Tests is the project setting `player.include_tests` (stored as
  `export.includeTests` in project.yaml, beside `developmentBuild`), not `build.include_tests`: the
  publishing settings live there. It is a row under Development Build in Build Settings.
- **Where an export puts the tests.** A prebuilt Development build with Include Tests carries the
  root as `assets/tests/` (not `tests/` beside the content), with `tests/index.json` (the settings,
  the test files and each snapshot's files): a player reads `asset://tests/` on every platform,
  from the content pack too, and cannot list folders there. Compile builds do not carry the tests
  (`run_tests {target: "desktop"}` builds prebuilt). A player copies a snapshot by the index's names.
- **`run_tests {target: "desktop"}`** chains `build_project {mode: "desktop", method: "prebuilt",
  development: true}` with Include Tests forced for that build, `run_build {headless: true,
  seconds: 300, args: ["--run-tests", ...]}` and the import of the results file the player names in
  its log (`[test] results <path>`, an absolute path); a run longer than run_build's 300 s is cut.
- **`user_settings` names the profile `profile`, not `store`.** R43 said its results gain `store`,
  but SaveGame's `store` input ("prefs" | "saves") selects the store on the same tool. A result's
  `store` is the store it acted on (SaveGame slot results too), and `profile` is the Play Data
  profile of a prefs result.
- **Test-project hooks.** `tools/test-projects/game-tests` is the engine project of AC12 and AC13
  (four smoke cases in `assets/tests/`, `android-expect.txt`, `tools/web/gates/game-tests.json`); the
  summary line's keys are in alphabetical order (the results document's JSON).
- **Replay cases.** A replay case's session holds the simulation until its scene runs (the editor's
  Play, a player's scene load), then the runner starts the replay with real input blocked, so its
  frame 0 is the session's first update, as `play {input}` does. A recording without a seed or a
  scene (Lua's) keeps the case's. A replay that fails on an event is an `error`; checkpoints a
  recording cannot compare (another platform, not from Play's start) are a console warning. A
  divergence names the first checkpoint that differs: the state hash covers the whole scene, so a
  moved obstacle diverges at the first checkpoint after it, named by its probe when it has one.
  The editor's Record Input (input spec R13) records a person's Play sessions but stands aside while
  a test run owns the session (its clock, seed and input), so AC16's test records inside a test
  session with Lua `InputRecording` from a script's `init` and marks the file `start: "play"`; a
  file Record Input wrote replays as a case the same way.
- **Not done in this change** (each named in the PR): the web gate, the Android run and the release
  `.text` size (AC12, AC13, AC18) run in the integration pass.
- **AC12, AC13, AC18 (integration pass).** AC12: the web gate first wedged the page after the first
  case's `[test] begin` and the Android player crashed (SIGSEGV in `ChunkAccess::testConfig`): `Test.config`
  iterated `items()` of a temporary JSON already destroyed. With the table kept, `GATE=game-tests`
  passes (3 passed, 1 skipped, exit code 0). AC18: the release glcore player (`tools/build.sh players`,
  full variant) of the tree before this spec's merge (5eb7d4088) against the tree after it (bd427002c):
  `.text` 39,219,810 against 39,271,938 bytes (+52,128). None of it is the runner (no `gametest::` or
  `PlayerTests` symbol in the release player); the input scripts that came with the merge
  (`input::Script*`, about 22.5 KB of functions, compiled in every player for `--input-script`), the
  `data://` override and the link's layout make it, so `.text` is not equal to the base tree's. The input scripts are now a module
  (input-recorder-and-input-scripts.md Deviations): a release player whose project uses none of them
  leaves them out (lite3d `.text` -253,536 bytes); the full player keeps them.
