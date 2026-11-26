# Game tests: Tests/, the Test Runner and run_tests

A game made with JasperEngine keeps its scenarios ("Start opens Level 1 with three lives", "the
shop refuses a purchase without coins") as test files that anyone (a person, an agent, CI) re-runs
on the editor and on each export target, with a pass or a fail and its reason. The design is
`docs/specs/project-test-runner.md`; agents read `docs/mcp/guide/testing.md`
(`jasper://docs/testing`).

## The folder

```
<project>/Tests/
  tests.yaml                  marker and defaults (version: 1)
  menu.test.lua               cases
  Golden/<case id>/<name>.png goldens of captures (also .<backend>.png, .<platform>-<backend>.png)
  Data/<snapshot>/...         data:// snapshots
  Input/*.input.json          input scripts and recordings
<project>/.jasperengine/test-results/   ledger.json, runs/, captures/ (per machine, not committed)
```

`Tests` is the root only when it holds `tests.yaml`: then the asset scans, imports, the Project
window and exports skip it. Without the marker an existing `Tests` folder keeps its assets (Create
> Test Folder then makes `GameTests/` instead). In a case id `<file>#<name>`, paths replace `/` and
`#` with `__` and other characters outside `[A-Za-z0-9_.-]` with `_`.

`tests.yaml` (every key optional): `targets` ([editor]), `fixed_fps` (60), `resolution`
(1280x720), `seed` (1), `timeout_frames` (18000), `wait_frames` (600), `hang_seconds` (600),
`image` (`tolerance` 8, `max_fraction` 0.001, `max_diff` 48, `max_mean` 0.25), `console_errors`
and `divergence` (fail or warn). Unknown keys are kept and warned once; a newer version is refused.

## The API

The `Test` table of a test file (`engine/core/script/binding/stubs/Test.lua` has every function):
`Test.case`, `Test.config`, `Test.setup`, `Test.teardown`, `Test.check`, `Test.assert`,
`Test.equal`, `Test.near`, `Test.fail`, `Test.skip`, `Test.waitFrames`, `Test.waitGameSeconds`,
`Test.waitUntil`, `Test.capture`, `Test.expectError`, `Test.log`, `Test.target`.

- **Play Mode** cases run each in a fresh Play session: the case's scene (default the start scene),
  the fixed delta `1/fixedFps`, `math.randomseed(seed)` before any script's `init`, the Game view at
  the case's resolution, Error Pause off, the Game view's input ignored, and clean data. Stop
  discards every play-mode change. The case's function is a coroutine resumed after each game
  frame's update.
- **Edit Mode** cases (`mode = "edit"`) run in a sandboxed editor state where `Editor.call` reaches
  only read-only MCP tools; they cannot wait. Players leave them out (`excluded_edit`).
- Statuses: `passed`, `failed`, `skipped`, `error` (the runner could not run it: a syntax error, a
  missing scene, Play did not start, a duplicate name, a call that ran 10 s without yielding),
  `hung` (still running after `hang_seconds` of wall time: a hang guard, never an expected
  duration). A case also fails at `timeout_frames`.
- An error line logged during a case fails it unless expected; with `console_errors: warn` it is
  only attached.
- **Replay cases** (`replay = "Input/<name>.input.json"`, a recording in the test root) start the
  session with the recording's frame rate, and its seed and scene when it has them, hold the game
  until the scene runs, and play the recording from the first frame while the case's function
  runs. A checkpoint that differs fails the case as `divergence`, naming the frame and the probes
  or values that differ; with `divergence: warn` it is a warning. Checkpoints compare only for a
  recording from Play's start made on the same platform.

## Play Data

The Game view's toolbar has Play Data: Project (the project's `.jasperengine/user/rundata`, the
default), Clean (an empty folder each Play) and Snapshot > `<name>` (a copy of `Tests/Data/<name>/`
each Play). Save Snapshot... copies the Play data (the session's while playing) into
`Tests/Data/<name>/` as one undo step; Open Data Folder shows it. The choice is kept per user and
project in `.jasperengine/user/game_view.yaml`. Every test case runs on Clean or a snapshot; nothing
a case does changes the project's data or a snapshot.

## The Test Runner window

Window > General > Test Runner: Run All, Run Selected (the checked cases, else the selected one),
Rerun Failed, Cancel, the Target, a filter (`text` or `tag:<t>`), a status filter (All, Failed,
Skipped, Unverified On...) and Create (Test Script, Test Folder). The Edit Mode and Play Mode tabs
list each file's cases with their status, time, Last Passed On (one chip per platform with a pass;
the tooltip has the date, backend and revision) and tags. The detail pane shows the failures
(click to open the script at the line), the console lines and each capture's golden, actual and
diff with Accept Image and Accept For This Target Only (undoable). A case's context menu has Run,
Open Script, Copy ID and Reveal Golden.

## CI: the CLI and JUnit

```sh
jasperengine-editor test --project <dir> --filter tag:smoke --junit out.xml --json results.json
jasperengine-editor test --project <dir> --import device-results.json --junit out.xml
jasperengine-editor test --project <dir> --list --json
```

It runs `run_tests` in the editor that has the project open over HTTP, else in a one-shot headless
editor. Exit codes: 0 every selected case passed or was skipped, 1 a case failed, errored or hung,
2 bad arguments or it could not run, 3 no case matched the filter. JUnit: one `<testsuite>` per
file, `<failure type="<kind>">` for a failed case, `<error>` for error and hung, `<skipped>`.

## Players

Build Settings > Include Tests (under Development Build; the project setting `player.include_tests`)
makes a prebuilt Development build carry the test root as `assets/tests/` (without `Golden/`, with
`tests/index.json`); a release build leaves it out and says so. `run_tests {target: "desktop"}` (the
Test Runner's Target: This Computer) builds such a player of this computer, runs it and imports its
results. A Development player runs the content's Play Mode cases with `--run-tests` (`--test-filter F`,
`--test-results PATH`, `--keep-test-data`); a release player refuses it (exit 2). The web reads them
from the page query (`?run-tests&test-filter=smoke`), Android from `debug.jasperengine.args`. Each
case gets `data://` in `<user data>/.jasper-test-runs/<run>/<n>/`; the results default to
`<user data>/.jasper-test-results/<run id>.json` with `captures/` beside them (status `pending`,
compared at import). The log carries `[test] begin`, one `[test] case <json>` per case and
`[test] summary <counts>`. Exit codes: 0, 8 (a case failed, errored or hung), 9 (no case).

## The ledger

Every run and import updates `.jasperengine/test-results/ledger.json`: per case and platform the
last status, run and pass times, backend, editor version and revision (the VCS commit and a dirty
flag, else the test file's SHA-256). A case is unverified on a `tests.yaml` target with no pass at
the current revision; `run_tests` reports `unverified` and says how to verify the others.
