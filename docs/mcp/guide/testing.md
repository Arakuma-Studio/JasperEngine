# Testing a game: Tests/ and run_tests

A scenario you checked by hand ("Start opens Level 1 with three lives") is kept as a case in the
project's test folder and re-run with `run_tests` (group `testing`): on the editor now, on players
later, with a pass or a fail and its reason.

## When to write a test

- After you fixed a bug or finished a feature you verified by playing: write the case that would
  have caught it, run it, keep it.
- Before you change a system other cases cover: run them first (`run_tests {filter: "tag:<t>"}`).

## The folder

`Tests/tests.yaml` marks the root (`version: 1` is enough; without it a `Tests` folder is an asset
folder). Every `*.test.lua` below it is a test file; `Golden/` holds the captures' goldens,
`Data/<name>/` the data snapshots, `Input/` input scripts. `tests.yaml` keys: `targets`,
`fixed_fps` (60), `resolution` (1280x720), `seed` (1), `timeout_frames` (18000), `wait_frames`
(600), `hang_seconds` (600), `image` tolerances, `console_errors` and `divergence` (fail or warn).

## A case

```lua
Test.config{ tags = {"smoke"} }

Test.case("Start opens Level 1 with three lives", function(t)
    Test.waitUntil(function() return GameState and GameState.level == "Level1" end,
                   {gameSeconds = 5, message = "Level1 never loaded"})
    Test.equal(GameState.lives, 3, "lives at start")
    Test.capture("level1_start")
end)

Test.case("Every scene has a camera", {mode = "edit"}, function(t)
    for _, s in ipairs(Editor.call("list_scenes", {}).data.scenes) do
        Test.check(s.main_camera ~= nil, s.name .. " has no camera")
    end
end)
```

- A Play Mode case (the default) gets a fresh Play session of its `scene` (default: the start
  scene) at `fixed_fps`, `math.randomseed(seed)` before any script's `init`, and clean data.
- Waits count game frames and game seconds: `Test.waitFrames`, `Test.waitGameSeconds`,
  `Test.waitUntil` (checked once per frame; past its cap the case fails, naming the message).
  Wait for state, never for time; `os.time`, HTTP and async loads are not deterministic.
- `Test.check` records and goes on; `Test.assert` and `Test.fail` end the case; `Test.equal`
  compares tables deeply and prints both values.
- An error line in the Console fails the case unless `Test.expectError(pattern)` (or
  `expectErrors`) expects it; an expectation never met fails too.
- Edit Mode cases (`mode = "edit"`) run in the editor without playing: `Editor.call` reaches only
  read-only tools and they cannot wait.
- `skip = "reason"`, `Test.skip(reason)` and `platforms = {...}` give skipped, never passed.
- `replay = "Input/<name>.input.json"` plays a recording from the session's first frame with its
  frame rate (and its seed and scene when it has them) while the case runs; wait with
  `Test.waitUntil(function() return not InputRecording.isReplaying() end)`, then check. A
  checkpoint that differs fails the case as `divergence` (frame, probes) unless `divergence: warn`.

## The loop

1. Write the case in `Tests/<area>.test.lua` (`write_file`).
2. `run_tests {filter: "<file>#<case>"}` (or `tag:<t>`; `action: "list"` shows the ids).
3. Read `failures` (file, line, frame, message) and the console lines; fix the game or the case.
4. Run again until it passes; then run the whole folder.

A filter matching nothing is an error, never a pass. Stop Play first: every case starts its own
session. The run is a job past a second (`get_job`, `cancel_job`).

## Captures

`Test.capture(name)` waits for shaders and textures, reads the frame at the case's resolution and
compares it with `Tests/Golden/<case>/<name>.png` (then `<name>.<backend>.png`,
`<name>.<platform>-<backend>.png` first). No golden is a failure (`new`). Look at the panel the
result carries, or `diff_images` with the paths in `next`; when the change is intended,
`run_tests {action: "accept", captures: ["<case id>/<name>"]}` writes the golden as one undo step
(`for_target: true`: this platform and backend only).

## Data

A case's `data://` (PlayerPrefs included) is an empty folder, or a copy of
`Tests/Data/<name>/` with `data = "<name>"`; it is deleted after the case. `data = "project"`
is refused. Save a snapshot from the Game view's Play Data menu (Save Snapshot...), or play with
`play {action: "start", data: "clean"}` to try a game on empty data without touching the
project's own. `user_settings` then reports that profile in `profile`; its `store` is the store it
acted on (`prefs` or `saves`).

## Players and import

`run_tests {target: "desktop"}` builds a Development player of this computer with the tests, runs it
and imports its results (`player_exit_code`). A Development build with Include Tests runs the Play
Mode cases with `--run-tests`
(`--test-filter`, `--test-results <file>`; the web from `?run-tests&test-filter=...`, Android from
`debug.jasperengine.args`). It exits 0, 8 (a case failed) or 9 (no case), logs `[test]` lines and
writes the results JSON with `captures/` beside it. `run_tests {action: "import", results_file}`
compares its captures and records the platform. `unverified` lists the `tests.yaml` targets where a
case has no pass at the current revision.
