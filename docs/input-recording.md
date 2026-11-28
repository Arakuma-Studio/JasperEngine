# Input recording and input scripts

The engine runs input from a file: a `.input.json` either recorded or written by hand. It goes in
through the engine's own input entry points (`Engine::system*`), so the same file behaves the same
in the editor, a desktop player, the web and a phone. The format and its rules are in
`docs/specs/input-recorder-and-input-scripts.md` (section 6.1) and `docs/mcp/guide/input.md`
("Recordings and input scripts").

## Scripts

A script lists steps that follow each other, in game frames:

```json
{"format": "jasperengine-input", "version": 1, "kind": "script", "fixed_fps": 60, "space": "safe_area",
 "events": [
   {"type": "wait", "until": "SceneManager.getActiveScene().name == 'Menu'", "frames": 600},
   {"type": "shot", "name": "01_menu"},
   {"type": "touch", "action": "tap", "target": "#play"},
   {"type": "wait", "frames": 120},
   {"type": "shot", "name": "02_game"}
 ]}
```

- Steps are `send_input` events. `ms` and `hold_ms` become frames at `fixed_fps`, rounded up.
- `wait {until}` checks a Lua condition once per frame; past `frames` (600 by default) the run fails
  naming the condition. Prefer it to fixed waits: a load that takes longer on a phone still passes.
- `shot` saves the game frame as a PNG.
- `target` taps the centre of a UI Document element (CSS selector); `safe_area` coordinates are
  0..1 of `Screen.safeArea`, so a notch does not move them.

Run one in the editor with `send_input {file: "Tests/Input/menu.input.json"}` during Play, or in a
player:

```sh
./MyGame --input-script tests/Input/menu.input.json --quit-after-input --input-shots shots/
```

The player waits for "Engine ready:", runs at the file's `fixed_fps`, logs `[input] begin`,
`[input] shot`, `[input] done` (or `[input] failed`) and exits 0, 10 when a step failed, or 11 when a
checkpoint differed with `--input-strict`. Real input is ignored meanwhile unless
`--input-allow-real`. On the web the file is a content path or a same-origin URL
(`?input-script=/tests/menu.input.json&quit-after-input`), fetched before the start scene loads; a
fetch that fails exits 10 naming the URL. From the editor, `run_build {input_script, resolutions,
headless: true}` runs the last desktop build once per size and returns the shots, which land in
`Recordings/Input/<file stem>/player/<WxH>/`.

Input scripts are an engine module (`inputscripts`): Development Builds and the full player always
have them, and a release build has them when the project's scripts use `InputRecording` or
`InputReplay` (an attract mode). A release build of a project that uses neither leaves the
interpreter out: its `--input-script` logs `[input] failed` ("this build has no input scripts") and
exits 10, and those two Lua names do nothing (one warning). For store screenshots or automation,
run a Development Build.

## Record Input in the editor

The red-ringed **Record Input** button sits on the main toolbar right of Play, Pause, Step and the
Recorder's button (the command `playmode.recordInput`, also in Search and `run_command`):

- **Off**: click it before Play to arm it. **Armed** (lit): "Records from the next Play".
- **Recording**: Play runs at the Record Input frame rate with a new random seed (`math.randomseed`
  before any script's `init`), holds the game until its scene runs and records from the frame after.
  The button reads "REC 00:12 · 734 events"; click it to Stop Recording (Play goes on).
- While playing without it armed the button is disabled: stop, turn it on, press Play.

Stop Recording, or Play's stop, writes `<scene>-<yyyymmdd-hhmmss>.input.json` into `Tests/Input/` when
the project has a test root (`Tests/tests.yaml`), else `Recordings/Input/`, as one undo step; the
Console prints the path. The file is encoded off the main thread; a write that fails keeps the
recording until **Retry Saving Recording** in the button's menu (or the next recording).

The caret beside the button opens:

- **Replay Input...**: pick a file; it replays from Play's start in a new session (the recording's
  scene, seed and frame rate; a script from the frame after the scene runs). The Game view shows
  "Replaying <file> · frame N/M" and, once a checkpoint differs, where it diverged; the editor's input
  to the game is ignored meanwhile. Its `shot` steps are saved to `Recordings/Input/<file stem>/`.
- **Capture Screenshots...**: below.
- **Open Recordings Folder**.
- **Record Input Settings...**: Preferences > Play Mode > Record Input: **Frame Rate** (60),
  **Checkpoint Every** (60 frames, 0 = off) and **Probes** (up to 16 GameObject paths whose positions
  checkpoints store; empty = chosen when recording starts).

Selecting a `.input.json` in the Project window shows it in the Inspector: kind, start, scene, frame
rate, frames and game seconds, events by type, checkpoints, platform, seed and whether it was
truncated, with **Replay**, **Capture Screenshots...** and **Open As Text** (the Inspector is
read-only: edit the file as text).

## Store screenshots

**Capture Screenshots...** takes an input file whose `shot` steps name the screenshots, the
resolutions (the Game view's list as checkboxes, plus custom `WxH` sizes) and an output folder
(default `Recordings/Screenshots/<file stem>/`). **Capture** replays the file once per resolution,
each in a new session with the Game view rendered at that size, writes `<output>/<WxH>/<shot>.png`
and `<output>/input-capture.json` (per resolution: the files and the replay's divergence), shows
its progress with **Cancel**, then opens the folder. Agents do the same with `record_game {input,
format: "shots", resolutions}`; `format: "png"` or a movie records the replay's frames instead.

## Recordings

`InputRecording.start()` and `InputRecording.stop(path)` (Lua) record what reaches the game from now
into `data://`. Each event carries the update frame it arrived on; a replay applies it in the same
frame's input window. `play {action: "start", input: "<file>"}` replays a file in the editor from
the first frame the scene runs, with the file's `fixed_fps` and, for a recording that has one, its
Lua seed.

A recording made from Play's start keeps checkpoints every 60 frames: a hash of the running scenes'
transforms and the game's `InputRecording.checkpoint` values, and the positions of up to 16 probe
GameObjects. A replay compares them and reports the first frame that differs, which probes moved and
how far, and which values changed. It does not promise the same run: wall-clock reads, loads and
worker results that land on other frames, HTTP, audio-driven logic and physics across platforms all
diverge. Checkpoints compare only on the platform that recorded them.

## Attract mode

```lua
local demo = InputRecording.play("asset://demo/attract.input.json", { loop = true, stopOnRealInput = true })
```

The recording loops until the first real key, button or touch, which then reaches the game.
