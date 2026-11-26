# Input Recorder and Input Scripts: record Play input, replay it with divergence checks, and drive every player from one engine-level event file

Spec-first. Builds on S22 (input edges and actions), S27 and S62 (the Recorder), S30 (launch options),
S35 (the input marker sites). This spec owns the event file format and its interpreter;
`project-test-runner.md` consumes both (`Test.input`, replay cases, captures). Paths are relative to
the repository root unless they start with `<project>/`.

## 1. Goal

`send_input` already drives keys, mouse, touch, gamepads, actions and app events into editor Play,
but only from live MCP calls: nothing records what a person played, nothing replays it, and outside
the editor the only scripted input is `run_build input`, which sends X11 events to a Linux desktop
window with wall-clock gaps. This spec makes one JSON event format, stamped in game frames, that a
person records with a toolbar button, an agent or tester writes by hand (tap, hold, drag, wait for a
state, shot), and the engine itself executes on all six targets through its own input entry points,
so a script behaves the same in the editor, on the web, on a phone and in a desktop build. A replay
is checked against state hashes recorded beside the input and reports where it diverged instead of
promising determinism. The same files make store screenshots at a list of resolutions.

## 2. Scope

**In scope**
- The `.input.json` format v1: kinds `recording` (frame-stamped, from Play's start or from a Lua
  call) and `script` (relative steps), the send_input event objects plus `frame`, and new steps
  `shot`, `wait {until}`, `gamepad connect/disconnect`, space `safe_area`.
- One interpreter in `engine/core/input/` used by `send_input`, `Test.input`, players and Lua.
- The recorder: a one-branch site at every `Engine::system*` input entry point, frame stamps, caps.
- Checkpoints: a state hash, probe positions and game values every N frames; divergence reports.
- Determinism setup for record and replay: fixed delta, Lua seed, ready-frame start, Simulate Touch.
- Editor: Record Input on the main toolbar with a menu (Replay Input..., Capture Screenshots...,
  settings); a summary Inspector for `.input.json` assets.
- MCP: `play {input}`, `send_input {file}` and the new steps, `record_game {input, format: "shots",
  resolutions}`, `run_build {input_script, resolutions}`, `get_play_state.input`.
- Players: `--input-script`, `--input-shots`, `--quit-after-input`, `--input-strict`,
  `--input-allow-real`; `[input]` log lines; exit codes 10 and 11.
- Lua `InputRecording` (start, stop, play, checkpoint values) for attract modes and game tools.

**Out of scope (default decisions)**
- OS-level injection. `run_build input` stays as it is (XTest on Linux): it tests the window
  system path, which an engine-level script cannot.
- "Last N seconds" ring recordings attached to bug reports: `shipped-game-diagnostics.md` may build
  them on this recorder. Here a recording runs from its start to its stop.
- State snapshots to seek into a recording or replay from its middle; cross-platform bit-exact
  simulation (Jolt is built without `JPH_CROSS_PLATFORM_DETERMINISTIC`); network replay.
- Motion sensors, cameras and microphones (no `Engine::system*` entry point carries them).
- A timeline editor for recordings: the file is JSON; the Inspector summarises it.
- Turning recorded points into CSS-selector targets.

## 3. Current state

Read from the code (not run) unless marked.

- **send_input's events** are key (press, down, up, `hold_frames`, `hold_ms`, mods), text
  (`per_unit`), ime, ime_commit, ime_cancel, mouse_enter/leave, mouse (click, double_click, down, up,
  move, drag, scroll), touch (tap, start, move, end, cancel, swipe), `target`/`to_target` CSS
  selectors of a UI Document, gamepad (button or axis), input_action, action/axis/axis_delta, app
  (pause, resume, focus, low_memory, hide, show, orientation), wait (`frames`, `ms`), release_all;
  spaces canvas, normalized and view (`docs/mcp/TOOLS.md:1463-1508`). The error lists them
  (`editor/mcp/tools/PlayTools.cpp:3635`).
- **The interpreter is editor code.** `buildEventSteps` turns each JSON event into `InputStep`
  lambdas (`PlayTools.cpp:2826-2831, 2978-3635`) that call `Engine::system*` (key: `:3013-3040`;
  touch tap is start, `hold_frames`, end: `:3353-3358`). `ms` waits are wall-clock deadlines
  (`InputRun::msDeadline`, `:2846-2847`). Targets resolve through `htmlui_mcp::elementCenter`
  (`:2957-2970`, `editor/mcp/tools/HtmlUiTools.cpp:1774`). Held input is tracked in `HeldInput`
  (`:173-191`) and released at stop (`:573-594`). `input_action` forces an action with
  `InputActions::inject` (`:3522-3556`; `engine/core/input/InputActions.h:255-258`). None of it is
  in a player.
- **The engine's input boundary.** Platforms call `Engine::systemTouch*`, `systemMouse*`,
  `systemKey*`, `systemCharInput`, `systemTextInput`, `systemIme*`, `systemGamepad*`,
  `systemFocus*`, `systemPause/Resume`, `systemLowMemory`, `systemOrientationChanged`
  (`engine/core/Engine.h:984-1099`). Events add to a pending window that the next running frame's
  `Input::beginFrame` makes its own; "the editor's Play input and the MCP tools go through
  Engine::system*" (`engine/core/Input.h:274-288`). The Game view forwards the editor's devices the
  same way (`editor/window/PlayInput.h:13-40`).
- **A tap precedent.** Audio captures mark input at those entry points with
  `if (AudioMarkers::isEnabled()) AudioMarkers::input(...)` (`engine/core/Engine.cpp:3679, 3731,
  3874, 4077`), sites that compile to nothing without `JASPERENGINE_AUDIO_CAPTURE`
  (`engine/core/audio/AudioMarkers.h:18-27`, `engine/CMakeLists.txt:1224-1232`).
- **record_game** enters Play and records the Game view at a fixed fps, "game time k/fps per frame,
  deterministic", PNG or movie, no input (`TOOLS.md:5062-5100`,
  `editor/mcp/tools/EditorToolsTools.cpp:690-714`); the session renders at the exact output size with
  VSync off (`editor/recorder/RecorderSession.h:6-21`).
- **run_build input** is Linux/X11 only: XTest, else XSendEvent, to the player's window
  (`editor/mcp/tools/BuildTools.cpp:124, 2717-2731`), timed by `gap_ms` and `input_delay_seconds`
  in wall seconds (`TOOLS.md:3078-3104`).
- **Players** take `--fixed-fps`, `--after-ready`, `--screenshot-frame`, `--resolution` and more but
  no input (`engine/core/player/LaunchOptions.cpp:73-93`); options reach the web from the query,
  Android from `debug.jasperengine.args`, iOS from the process arguments
  (`engine/platform/emscripten/main.cpp:131-133`, `engine/platform/android/NativeEngine.cpp:567-575`,
  `engine/platform/apple/ios/ViewController.m:27`).
- **Determinism ingredients.** `Engine::setFixedDeltatime` (`Engine.h:791-796`); a shader clock that
  advances by the fixed delta (`:798-806`); `getGameFrames` counts update frames only (`:830-831`);
  `setSimulationHeld` (`:853-858`). Lua 5.4.7 seeds `math.random` from `time()` and an address when
  the state opens (`engine/libs/lua/lmathlib.c:628-661`, opened at
  `engine/core/script/LuaBinding.cpp:946-947`); nothing in `engine/core` reseeds it. Navigation has a
  fixed seed (`engine/core/subsystem/NavigationSystem.h:138`). Jolt's cross-platform determinism is
  an option the engine does not set (`engine/libs/joltphysics/CMakeLists.txt:578-579`; inferred from
  no `CROSS_PLATFORM_DETERMINISTIC` in `engine/CMakeLists.txt`).
- **Safe area** is `Screen::getSafeArea()` in canvas units (`engine/core/Screen.h:140`), shown as
  `safe_area` in `get_play_state`.

## 4. Reference

**Craftsman (`/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor`).** A Minecraft PE 0.15
port reconstructed from a retail binary. Ideas only: no code, data, assets or Minecraft-specific
mechanics may be copied; everything here is genre-agnostic.

| Pattern studied | Take | Improve |
|---|---|---|
| Kill clips (`include/mcpe015/clips.h:1-40`, `src/clips_format.cpp`, `src/clips_replay.cpp`): a fixed 1 MB ring of typed, time-stamped records (packets, camera samples, markers), a once-a-second keyframe of entity state, a versioned file with a JSON header read without the rest, CRC'd sections, hard caps (64 MB a section, 200,000 records) and a decoder that fails clean on a broken file; replayed through the real engine path that received the data, two replays giving the same positions tick by tick | Typed frame-stamped records, a fixed memory cap, periodic state samples, versioned header, caps and clean failure, replay through the real input path | Stamps in game frames, not milliseconds; our samples are hashes plus a few probes (detection, not correction); JSON instead of a binary container (diffable, hand-editable); the replay path is the engine's own input boundary on every platform |
| iOS smoke script (`tools/ios/smoke_test.txt`): `tap`, `wtap`, `hold`, `drag`, `wait <ms>`, `shot <name>` in fractions of the play area excluding the safe-area bands, run on the simulator | Tap/hold/drag/shot steps, coordinates in safe-area fractions | `wait` is in frames or until a Lua condition, never milliseconds (its 20-second sleeps are the flakiness the testing rules forbid); the same script runs on every target |

**Unity.** Input System's `InputEventTrace` (records device events, replays them with
`ReplayController`, `WithAllDevicesMappedToNewInstances`, paced by original timing or frame) and its
"Input Recorder" sample component (Record / Play buttons, `.inputtrace` files); `InputTestFixture`
(`Press`, `Release`, `Move` in tests); Unity Recorder for movies. Names taken: Record Input, Replay,
Input Recorder. Unity replays by time or by frame and does not verify state; we stamp by frame and
verify with checkpoints. No code.

## 5. Requirements

### Functional: the format

- **R1 File.** A UTF-8 JSON object with `format: "jasperengine-input"`, `version: 1`, `kind`
  (`recording` | `script`) and `events`; optional `fixed_fps` (default 60), `space` (default
  `normalized` for recordings, `canvas` for scripts), `gap_frames` (scripts, default 1), and for
  recordings `start` (`play` | `now`), `seed`, `scene`, `canvas`, `simulate_touch`, `platform`,
  `engine_version`, `probes`, `checkpoints`, `truncated`, `frames` (the update frames from start to
  stop: a replay runs that long, not only to its last event; missing in older files: to the last
  event or checkpoint). Extension `.input.json`. Unknown keys are
  kept by tools that rewrite a file. Field reference: §6.1.
- **R2 Events.** Each event is a `send_input` event object (same keys, same meanings). Recordings
  stamp every event with `frame` (non-decreasing); scripts stamp none (each event follows the
  previous one's own frames plus `gap_frames`, as `send_input` does). Mixing is refused.
- **R3 Frames, not milliseconds.** In a file, `ms` and `hold_ms` become
  `ceil(ms × fixed_fps / 1000)` frames. Live `send_input` calls keep wall-clock `ms` (unchanged).
- **R4 New steps** (files and `send_input`):
  - `shot {name}`: a capture of the game frame at that point (where it goes: R13, R18, R21, R24, and
    a test capture in `project-test-runner.md`);
  - `wait {until: "<lua>", frames}`: resumes when the condition, evaluated in the game's Lua state
    once per frame, is truthy; `frames` caps it (default 600 in files); past the cap the run fails
    "wait until timed out after N frames: <expression>";
  - `gamepad {action: "connect", gamepad, name}` / `{action: "disconnect", gamepad}` (today
    connection is implicit in `send_input`; recordings need it explicit; implicit stays for calls).
- **R5 Space `safe_area`**: `x`, `y`, `to` in 0..1 of `Screen.safeArea` (canvas units, top-left
  origin). `view` is refused in files (it depends on the editor's window).
- **R6 Validation.** A file fails to load, naming the fix, when: `version` is newer ("made by a newer
  engine"), `kind` is missing, a recording event lacks `frame` or goes back in time (its index), a
  script event has one, a type or key is unknown (send_input's own messages), the file exceeds 64 MB
  or 2,000,000 events. A file that fails never runs a partial prefix.

### Functional: one interpreter

- **R7 Shared interpreter.** `engine/core/input/InputScript` parses a file or an events array into
  typed frame-scheduled actions and applies them through `Engine::system*` (and `InputActions` for
  `input_action`/`action`/`axis`), with its own held-input bookkeeping released at its end, at stop
  and on error. `send_input`, `wait {input}`, `Test.input`, `play {input}`, players and Lua all use
  it; the editor keeps only what depends on the editor (`view` space, the Game view's pointer
  ownership, `mouse_leave` when the view loses the pointer).
- **R8 Targets.** `target`/`to_target` resolve through a host callback: the editor passes
  `htmlui_mcp::elementCenter`; a player resolves the selector against its running UI Documents
  (`htmlui` module) with the same rules (topmost document first, the element's centre); without the
  module the step fails "target needs the htmlui module in this player".
- **R9 Parity.** For every event type, the editor's `send_input` and the core interpreter make the
  same sequence of `Engine::system*` calls on the same game frames (AC3).

### Functional: recording

- **R10 Recorder sites.** Every `Engine::system*` input entry point (touch, mouse, key, char, text,
  IME, gamepad connect/disconnect/button/axis, focus, pause/resume, low memory, orientation) starts
  with `if (InputRecorder::isActive()) InputRecorder::record(...)`, before dispatch, beside the
  AudioMarkers site. `input_action`/`action`/`axis` events count only when they come from the
  interpreter (an agent's forced action), never from game code (an on-screen stick a script drives
  re-derives from the recorded touches).
- **R11 Stamps.** An event's `frame` is the number of update frames run since the recording began
  when it arrived; a replay applies it in the same pending window (`Input.h:274-283`), so the frame
  that consumed it on record consumes it on replay. Frames while paused or held do not count
  (`getGameFrames`).
- **R12 Coalescing and encoding.** Consecutive mouse moves within one frame keep only the last (off
  while the pointer is captured); key repeats are dropped (the engine regenerates them); points are
  stored in `normalized` space with 4 decimals; text keeps `per_unit` when it came as units.
- **R13 Editor recording** (Record Input): armed before Play; the session runs at the Record Input
  frame rate (fixed delta, default 60) with a new random 32-bit `seed` applied as R16, holds the
  simulation until the engine's ready signal, and starts recording at the first frame after it.
  Stop Recording (or Play stop) writes the file: into `Tests/Input/` when the project has a test root
  (`project-test-runner.md` R1), else `Recordings/Input/`, named `<scene>-<yyyymmdd-hhmmss>.input.json`,
  as one undo step (`WriteFilesCmd`); the Console prints the path. Real and MCP input are both
  recorded.
- **R14 Caps.** The editor recorder holds at most 64 MB of records (about 2 M events); a Lua
  recording 1 MB by default (`maxBytes`, at most 16 MB). At the cap recording stops with a warning
  naming the frame, and the file is written with `truncated: true`.
- **R15 Writing.** Stop swaps the buffer on the main thread (at most 0.1 ms); JSON encoding and the
  AtomicFile write run on a job; a failed write keeps the buffer until a retry or Play ends, and says
  why (S03).

### Functional: determinism and checkpoints

- **R16 Replay setup.** A `start: "play"` recording replays in a new session with its `scene`, fixed
  delta `1/fixed_fps`, `math.randomseed(seed)` in the fresh Lua state before any script's `init`,
  its `simulate_touch`, the simulation held until ready, and its events from the first frame after
  ready. A canvas of another aspect than `canvas` gives the warning `aspect_mismatch` (normalized
  points scale; anchored UI may move). While replaying, real device input to the game is ignored
  (editor: the Game view banner; player: unless `--input-allow-real`); lifecycle events still pass.
- **R17 Checkpoints.** While recording, every `checkpoint_frames` frames (default 60; 0 = none), at
  the end of the frame's update, the recorder stores `{frame, hash, probes, values}`:
  - `hash`: XXH3-64 over each running scene (load order), each entity with a Transform (id order):
    id, active in hierarchy, world position, rotation and scale each as `int32(round(v × 1024))`,
    then the game values (name-sorted, numbers as above, strings as bytes);
  - `probes`: world positions of up to 16 probe GameObjects, chosen at start (explicit paths, else
    the first 16 by id with a Rigidbody, Character Controller, NavMesh Agent or script), named by
    path in the header;
  - `values`: the latest `InputRecording.checkpoint(name, value)` values (numbers, booleans,
    strings, vectors; at most 64 names).
- **R18 Divergence.** A replay recomputes each checkpoint at its frame. The first mismatch sets
  `diverged_at_frame`, and `differences` lists probes farther apart than 1/1024 (path, recorded,
  replayed, distance) and values that differ; it is logged once (`[input] diverged {...}`), reported
  by the calling tool, `get_play_state.input` and the Lua handle, and the replay goes on (input keeps
  flowing) unless strict (R25, `stop_on_divergence`). Later checkpoints are not compared.
- **R19 Comparability.** Checkpoints compare only when `platform` (OS and CPU architecture) equals the
  replaying one's; otherwise the report says `comparable: false` with both tags and no divergence is
  claimed. `start: "now"` recordings never compare (no known starting state).
- **R20 Documented sources of divergence** (in `docs/mcp/guide/input.md`, not fixed by this spec):
  wall-clock reads (`Time.realtimeSinceStartup`, `os.time`, `os.clock`, real-clock timers), loads
  that finish on other frames, HTTP, logic driven by audio position, worker results landing on other
  frames, physics across platforms or compilers. The advice: `wait {until}` on state in scripts.

### Functional: tools and players

- **R21 Editor replay.** `play {action: "start", input: path}` (project-relative or absolute): a
  recording as R16 (its `scene` unless `scene` is given, then `scene_override` is warned); a script
  from the first frame after ready. The play result's `next` gives the `wait` call that ends after
  the last event (`until: "not InputRecording.isReplaying()"`, frames = last event frame + 60).
  `shot` steps save PNGs to `Recordings/Input/<file stem>/<shot>.png` and list them in
  `get_play_state.input.shots`.
- **R22 `send_input {file}`** runs a script file's events now, as if passed in `events` (with
  `space` from the file); a recording is refused: "a recording replays from Play's start: play
  {action: \"start\", input: \"<path>\"}". Its `shot` steps save as R21 and return `shots: [{name,
  path}]`.
- **R23 `record_game`** gains `input` (replay a file while recording; the Recorder enters Play as R21;
  `fps` defaults to the file's `fixed_fps` and a different one is refused naming it), `format:
  "shots"` (write only the frames of `shot` steps, named `<shot>.png`), and `resolutions`
  (`["1290x2796", "2048x2732"]`: the whole run once per size, Game view rendered at each, output
  `<output>/<WxH>/`). The sidecar lists, per resolution, the files and the replay's divergence.
- **R24 `run_build`** gains `input_script` (a file path; passes `--input-script`, `--input-shots`,
  `--quit-after-input` and implies `after_ready`, `fixed_fps` from the file) and `resolutions` (one
  launch per size with `--resolution`); works headless on Windows, Linux and macOS; returns the shots
  (the first 4 as images, all as paths), the `[input]` lines, the exit code. `input` keeps its X11
  behaviour; the description names `input_script` as the engine-level route.
- **R25 Player flags.** Every player (release included) takes `--input-script PATH` (a file path, or
  a content path such as `tests/Input/menu.input.json` or `asset://demo/attract.input.json`; the web:
  a content path or a same-origin URL, fetched before the start scene), `--input-shots DIR`
  (default `data://input-shots/`), `--quit-after-input` (exit when the events are done),
  `--input-strict` (a divergence fails), `--input-allow-real`. The script implies `--after-ready` and
  `--fixed-fps <fixed_fps>` unless given.
- **R26 Player output.** Category `input`, info level: `[input] begin {"file", "kind", "events",
  "frames"}`, `[input] shot {"name", "path", "frame"}`, `[input] diverged {...}`, `[input] done
  {"frames", "shots", "checkpoints": {"compared", "comparable", "diverged_at_frame"}}`, `[input]
  failed {"event": i, "error"}`. Exit codes with `--quit-after-input`: 0 done;
  `EXIT_INPUT_FAILED = 10` (bad file, a step failed, a target not found, a wait timed out);
  `EXIT_INPUT_DIVERGED = 11` (with `--input-strict`). Codes 8 and 9 are the test runner's.

### Functional: Lua

- **R27 `InputRecording`** (all builds):
  - `start([opts]) -> true | nil, err` (`checkpointFrames`, `probes`, `maxBytes`): a `start: "now"`
    recording from the next frame; refused while one runs;
  - `stop([path]) -> path | nil, err`: writes `data://` (default
    `data://recordings/<yyyymmdd-hhmmss>.input.json`) on a job; `lua://` and `asset://` are read-only;
  - `play(source [, opts]) -> InputReplay`: `source` a path (`lua://`, `asset://`, `data://`) or a
    decoded table; opts `loop` (false), `blockRealInput` (true), `stopOnRealInput` (false),
    `onFinished`; a `start: "play"` recording plays from now without reseeding (one warning);
  - `isRecording()`, `isReplaying()`, `stopReplay()`, read-only `frame` (-1 when idle),
    `checkpoint(name, value)`;
  - `InputReplay`: `isDone()`, `stop()`, read-only `frame`, `diverged` (nil or the R18 table);
    `Coroutine.waitFor(replay)` waits for it.
- **R28 Attract mode.** `play(..., {loop = true, stopOnRealInput = true})` loops and ends at the first
  real key, button or touch, which then reaches the game.

### Platform & backends

- **R29** Recording, replay, scripts, checkpoints and shots work on Windows, Linux, macOS, Android,
  iOS and the web, on every backend: they sit above sokol-gfx, and shots use RunControl's composite
  readback (players) and the screenshot path that waits for loads (editor).
- **R30** The web fetches `--input-script` URLs with the page's same-origin fetch before the start
  scene loads (as the content pack); a failed fetch exits 10 with the URL.

### Performance

- **R31** Idle: one relaxed atomic load per entry point call, no allocation, no per-frame work.
- **R32** Recording: at most 0.2 µs per event and 0.02 ms per frame of 50 events (desktop); records
  are 32 bytes plus pooled text; no allocation per event after the first chunk (64 KB chunks).
- **R33** Replay: at most 0.05 ms per frame applying 50 events (desktop), 0.2 ms (mid Android).
- **R34** A checkpoint over 2,000 transforms costs at most 0.15 ms on desktop and 0.6 ms on a mid
  Android device, once per `checkpoint_frames`.
- **R35** Parsing 100,000 events takes at most 150 ms on a desktop job (never on the main thread for
  files over 1 MB); the interpreter, recorder and hash add at most 64 KB to a stripped linux-x64
  release player (S34's size report).

### Editor UX (human)

- **R36 Record Input** button (red circle) on the main toolbar right of Play, Pause and Step, with a
  caret menu: Replay Input... (file dialog over the test root and `Recordings/Input/`), Capture
  Screenshots..., Open Recordings Folder, Record Input Settings.... States: off; armed (lit, tooltip
  "Records from the next Play"); recording ("REC 00:12 · 734 events", click = Stop Recording, Play
  goes on); disabled while playing unarmed (tooltip "Record Input starts with Play: stop, turn it on,
  press Play"). Commands `play.recordInput`, `play.replayInput`.
- **R37 Settings** (Preferences > Play Mode > Record Input): Frame Rate (60), Checkpoint Every
  (60 frames, 0 = off), Probes (up to 16 paths, empty = automatic).
- **R38 Capture Screenshots...** dialog: the input file, resolutions as checkboxes from the Game
  view's resolution list plus custom `WxH`, output folder (default
  `Recordings/Screenshots/<file stem>/`), Capture; runs R23 as a Recorder job with progress and
  Cancel, then opens the folder.
- **R39 Inspector** for a `.input.json` asset: kind, start, scene, frame rate, frames and game
  seconds, events by type, checkpoints, platform, truncated; buttons Replay and Capture
  Screenshots...; Open As Text. Read-only (edits are text edits).
- **R40 While replaying** the Game view shows a banner "Replaying <file> · frame N/M" (and the
  divergence when it happens); editor input to the game is ignored (R16).

### Agent UX (MCP, Lua, CLI)

- **R41** `play` gains `input`; `send_input` gains `file`, the steps of R4 and the space of R5;
  `record_game` gains `input`, `format: "shots"`, `resolutions`; `run_build` gains `input_script`,
  `resolutions`; `get_play_state` gains `input: {recording: {file, frame, events, truncated},
  replaying: {file, kind, frame, last_frame, events_left, comparable, compared, diverged_at_frame,
  differences}, shots}`. `run_command {command: "play.recordInput"}` toggles Record Input. No new
  tool.
- **R42** Errors name the fix (R6, R22, R23, R25 texts); `next` after `play {input}` (R21).
- **R43** Lua stub `engine/core/script/binding/stubs/InputRecording.lua` (hand-written), joined by
  `tools/lua_api`; `InputRecording` documented in `docs/mcp/guide/lua-api.md`.
- **R44** Guide: `docs/mcp/guide/input.md` gains "Recordings and input scripts" (format, steps,
  frames not ms, `wait until`, replay and divergence, shots, players, store screenshots); the agent
  guide gets no new line (input is reached through `send_input`, which agents already use), unless
  the eval of AC19 shows agents miss `file`.
- **R45** Human docs: `docs/input-recording.md` (record, replay, scripts, store screenshots, players,
  attract mode, divergence and its sources).

### Data & compatibility

- **R46** New: the `.input.json` format v1; workspace preferences of R37; nothing in scenes or
  project settings; no version bump. `.input.json` files under the asset folder are JSON data assets
  and export like other JSON (attract recordings ship).
- **R47** Existing `send_input`, `wait {input}` and `run_build input` calls behave as before (their
  MCP tests pass unchanged, AC20); `ms` stays wall-clock in live calls.
- **R48** A file of a newer version is refused (R6); a reader of version 1 ignores unknown keys.

## 6. Design constraints & decisions

### 6.1 File reference

```json
{
  "format": "jasperengine-input", "version": 1,
  "kind": "recording",
  "start": "play",
  "fixed_fps": 60,
  "space": "normalized",
  "seed": 3141592653,
  "scene": "Scenes/Level1.scene",
  "canvas": [1280, 720],
  "simulate_touch": false,
  "platform": "linux-x64",
  "engine_version": "…",
  "probes": ["Player", "Enemies/Bat (3)"],
  "truncated": false,
  "frames": 600,
  "events": [
    {"frame": 0,  "type": "key", "key": "right", "action": "down"},
    {"frame": 42, "type": "key", "key": "right", "action": "up"},
    {"frame": 43, "type": "touch", "action": "start", "pointer": 0, "x": 0.5, "y": 0.8125},
    {"frame": 47, "type": "touch", "action": "end", "pointer": 0, "x": 0.5, "y": 0.8125},
    {"frame": 60, "type": "gamepad", "action": "connect", "gamepad": 0, "name": "Xbox Controller"}
  ],
  "checkpoints": [
    {"frame": 60, "hash": "6c1f0e9a2b7d4c13", "probes": [[1.0029, 0.0, 3.5], [8.0, 1.25, -2.0]],
     "values": {"score": 10}}
  ]
}
```

A hand-written script (the store-screenshot run of R38):

```json
{"format": "jasperengine-input", "version": 1, "kind": "script", "fixed_fps": 60, "space": "safe_area",
 "events": [
   {"type": "wait", "until": "SceneManager.getActiveScene().name == 'Menu'", "frames": 600},
   {"type": "shot", "name": "01_menu"},
   {"type": "touch", "action": "tap", "target": "#play"},
   {"type": "wait", "until": "Player ~= nil and Player.grounded", "frames": 900},
   {"type": "touch", "action": "start", "pointer": 1, "x": 0.09, "y": 0.75},
   {"type": "wait", "frames": 180},
   {"type": "touch", "action": "end", "pointer": 1},
   {"type": "touch", "action": "swipe", "pointer": 2, "x": 0.7, "y": 0.4, "to": [0.55, 0.45], "steps": 150},
   {"type": "shot", "name": "02_walk"}
 ]}
```

Recorded types map to existing event objects: key `down`/`up` (+`mods`), `text` (+`per_unit`),
`ime`/`ime_commit`/`ime_cancel`, mouse `down`/`up`/`move`/`scroll` and `mouse_enter`/`mouse_leave`,
touch `start`/`move`/`end`/`cancel` (+`pointer`), gamepad `connect`/`disconnect`, `button`+`action`,
`axis`+`value`, `app` (`pause`, `resume`, `focus_lost`, `focus_gained`, `hide`, `show`,
`low_memory`, `orientation`), and interpreter-forced `input_action`/`action`/`axis`.

### 6.2 Engine API

```cpp
// engine/core/input/InputScript.h
namespace jasperengine::input {
    enum class ScriptKind : uint8_t { Recording, Script };
    struct ScriptHeader { ScriptKind kind; bool fromPlayStart; int fixedFps = 60; Space space; uint32_t seed = 0;
                          bool hasSeed = false; std::string scene, platform; Vector2 canvas; bool simulateTouch = false;
                          std::vector<std::string> probes; std::vector<Checkpoint> checkpoints; bool truncated = false; };
    struct ScriptAction { uint32_t frame; ActionType type; /* code, pointer, button, axis, mods, x, y, value,
                          textIndex, target index, untilIndex, capFrames, shotIndex */ };
    struct ParsedScript { ScriptHeader header; std::vector<ScriptAction> actions; std::vector<std::string> strings; };
    // false + error naming the event index and the fix (R6)
    bool parseScript(std::string_view json, ParsedScript& out, std::string& error);
    bool parseEvents(const Json& events, Space defaultSpace, int gapFrames, int fixedFps, ParsedScript& out, std::string& error);
    std::string writeScript(const ParsedScript& script);     // recordings

    struct PlayerHost {   // what differs between the editor and a player
        std::function<bool(const std::string& selector, const Json& document, Vector2& canvasPoint, std::string& error)> resolveTarget;
        std::function<bool(const std::string& luaCondition, bool& truthy, std::string& error)> evalCondition;
        std::function<void(const std::string& name, uint64_t frame)> shot;
        std::function<Vector2(Vector2 canvasPoint)> toEnginePoint;   // canvas → system* coordinates
    };
    class ScriptPlayer {
    public:
        ScriptPlayer(std::shared_ptr<const ParsedScript>, PlayerHost host, PlayOptions options);
        // before Input::beginFrame of each running frame: applies the actions due; false when done or failed
        bool beforeFrame(uint64_t frameIndex);
        void afterUpdate(uint64_t frameIndex);    // checkpoints, wait-until checks
        void releaseHeld();                        // at end, stop, error
        const ReplayReport& report() const;        // events left, comparable, diverged_at_frame, differences
        static bool isInjecting();                 // its own system* calls (real input filter, R16)
    };
}

// engine/core/input/InputRecorder.h
namespace jasperengine {
    class JASPERENGINE_API InputRecorder {
    public:
        static bool isActive() { return active.load(std::memory_order_relaxed); }
        static bool begin(const RecordOptions& options, std::string& error);   // fixed fps, seed, probes, maxBytes, fromPlayStart
        static void record(const RecordedEvent& event);                       // the system* sites
        static void recordForced(const RecordedEvent& event);                 // interpreter-forced actions
        static void checkpointValue(std::string_view name, const CheckpointValue& value);
        static void afterUpdate();                                            // checkpoints
        static std::shared_ptr<ParsedScript> end();                           // the recording; encoding is the caller's job
    private:
        static std::atomic<bool> active;
    };
    // engine/core/input/StateHash.h: the R17 hash and probes over running scenes
}
```

- The site is unconditional code (not behind a CMake option): replay and Lua recording ship in
  release players, and R31 keeps it at one load and branch.
- The real-input filter (R16) sits at the same sites: while a replay blocks real input and
  `ScriptPlayer::isInjecting()` is false, a device event other than a lifecycle event returns before
  dispatch.
- `send_input`'s `buildEventSteps` becomes a thin adapter: the editor builds a `ParsedScript` with
  `parseEvents` and runs a `ScriptPlayer` with the editor host; wall-clock `ms` in live calls stays
  as a step attribute the editor host honours (R3, R47).
- Holding the simulation until ready (R13, R16) uses `Engine::setSimulationHeld` in the editor and
  RunControl's ready frame in players, so frame 0 is the first update after `Engine ready:` on both.
- Encoding happens on a job; the JSON layout writes one event per line inside `events` so diffs of
  hand-edited scripts stay readable.

### 6.3 MCP changes

```text
play         + input?: string        "start: replay a .input.json from the ready frame."
send_input   + file?: string         "Run a script .input.json now."
             space enum + "safe_area"; event types + shot{name}, wait{until, frames}, gamepad action connect|disconnect
record_game  + input?: string        replay while recording (fps = the file's)
             format enum + "shots"   only the frames of shot steps
             + resolutions?: [string]  "WxH" each; one run per size
run_build    + input_script?: string engine-level input on any desktop (input: X11 only)
             + resolutions?: [string]
get_play_state  + input {recording, replaying, shots}
```

Budgets: `send_input` (limit 1,990, `tools/mcp/budget.json`) and `play` (1,709) are grandfathered
near their limits. Their new bytes are first found by trimming their descriptions; what remains raises
their `exceptions` by the measured bytes with this spec's reason and eval cases
(`guard_input_script_file`, `bad_input_ms_in_file`). `record_game` and `run_build` stay within 1,500.

| Situation | Text |
|---|---|
| recording given to `send_input` | "A recording replays from Play's start: play {action: \"start\", input: \"<path>\"}." |
| `record_game fps` differs | "This file runs at 60 fps (fixed_fps); record_game fps must be 60 or omitted." |
| target not found during a file | "Event 7 (touch tap): target \"#play\" matched no element of the running UI Documents (frame 312)." |
| wait until timed out | "Event 3 (wait): until timed out after 600 frames: Player ~= nil." |
| `view` in a file | "space \"view\" is the editor's window and cannot be stored: use normalized, canvas or safe_area." |

### 6.4 Store screenshots

The default resolutions offered by R38 come from the Game view's list (`editor/window/GameResolutions.cpp`);
nothing about store sizes is hard-coded. The run per resolution starts a new session (R16), so each
size replays from the same state; a script that branches on layout (`Screen.width`) is the game's
choice.

## 7. Acceptance criteria

The fixture is `tools/test-projects/input` (exists: extended) with a scene `Replay`: a
physics ball pushed by keys, a Lua `math.random` spawner, an HTML UI button `#play` that logs
`[gate] clicked`, and `tests/Input/menu.input.json` (a script: wait for the menu, shot, tap `#play`,
wait until clicked, shot). Red controls are named per criterion.

- **AC1 Round trip.** `InputScriptFormat.RoundTripsEveryEventType` (core gtest,
  `engine/tests/core/input/InputScriptTest.cpp`): every type and space parses, writes and parses to
  equal actions; `ms` converts at 30 and 60 fps (ceil). Red: a writer dropping `mods`.
- **AC2 Clean failure.** `InputScriptFormat.RejectsBadFilesNamingTheFix`: newer version, missing
  kind, decreasing frames (names the index), a stamped script, unknown type, `view` space, 64 MB and
  2 M caps, truncated JSON; each fails with its text and no actions. Fuzz:
  `InputScriptFormat.RandomBytesNeverCrash` (10,000 mutated files under ASan in the sanitizer run).
- **AC3 Parity.** `SendInputParity.EventsGiveTheSameSystemCalls` (editor gtest,
  `editor/tests/mcp/SendInputParityTest.cpp`): a table of events for every type through the editor
  adapter and through `ScriptPlayer` with a recording hook on `Engine::system*` gives identical
  (frame, call, arguments) traces. Red: the adapter's touch tap with `hold_frames + 1`.
- **AC4 Recorder sites.** `InputRecorder.RecordsEveryEntryPointOnce` (core gtest,
  `engine/tests/core/input/InputRecorderTest.cpp`): each `Engine::system*` input call while active
  gives one record with the right frame; moves coalesce per frame; repeats drop; forced actions
  only via `recordForced`. Red: a site removed from `systemGamepadAxisMove`.
- **AC5 Same window on replay.** `InputRecorder.ReplayHitsTheSameFrameEdges` (core gtest driving
  `Input::beginFrame` frames): a press and release recorded in one window are both seen by the same
  frame's `getKeyDown`/`getKeyUp` on replay; an event recorded during a paused frame lands on the
  next running frame both times. Red: stamping with `getGameFrames() + 1`.
- **AC6 Zero cost idle.** `InputRecorder.IdleCostsNothing`: 100,000 system calls with the recorder
  idle allocate 0 bytes (`AllocationCountScope`) and `InputRecorderBench.IdleSite` prints the cost
  per call (≤ 2 ns over the base, `perfSlack()`). Red: an idle path that builds the record first.
- **AC7 Record then replay matches.** `tools/mcp/tests/test_input_recording.py::
  test_record_then_replay_matches` (CI = True, scratch copy): `run_command play.recordInput`,
  `play start`, `send_input` keys for 300 frames, stop; the file has `seed`, events, 5 checkpoints;
  `play {input}` then `wait` from `next` gives `compared: 5`, no divergence, and a `shot` equal to the
  recorded run's (`diff_images` ok). Red: replay without the seed (the spawner diverges at the first
  checkpoint).
- **AC8 Divergence reported.** Same file, `test_divergence_names_the_probe`: after moving a wall in
  the scene, replay reports `diverged_at_frame` at the first checkpoint after the ball meets it and
  names the ball's probe with a distance; `stop_on_divergence` absent, the replay still finishes.
  Red: `afterUpdate` comparisons disabled (no divergence reported).
- **AC9 Cost of checkpoints.** `StateHashBench.Hash2000Transforms` ≤ 0.15 ms (`perfSlack()`) and
  `StateHash.QuantizesBelowAThousandth` (two scenes differing by 1e-4 hash equal, by 2e-3 differ).
- **AC10 send_input file and steps.** `test_send_input_file_steps`: the menu script via `send_input
  {file}` taps `#play` (Console has `[gate] clicked`), returns two shots; a recording is refused with
  the fixed text; `safe_area` points land inside `Screen.safeArea` under a simulated notch device
  (S56); `wait until` past its cap is isError naming the expression.
- **AC11 Desktop player, any OS.** `test_input_recording.py::test_desktop_player_runs_input_script`
  (`PREBUILT_PLAYERS = ["glcore-release"]`): `run_build {input_script, resolutions: ["800x600",
  "600x800"], headless: true}` exits 0, returns 2 × 2 shots of the right sizes and the `[input] done`
  line, with no X server use (run with `DISPLAY` unset). Red: the base tree (`input_script`
  unknown).
- **AC12 Web.** Gate `tools/web/gates/input.json` gains a run with the query
  `?input-script=tests/Input/menu.input.json&quit-after-input` expecting `[gate] clicked`,
  `[input] done` and `JasperEngine exit code 0` in order (after `WEB_PROJECT=input tools/build.sh
  web`). Red: the gate on a build without the flag (exit 2 line).
- **AC13 Android.** `tools/android-emulator-test.sh build/android-input/app-debug.apk --setprop
  "debug.jasperengine.args=--input-script tests/Input/menu.input.json --quit-after-input"
  --expect '[gate] clicked' --expect '[input] done'` (the `--setprop` value check widened as
  `project-test-runner.md` AC13 describes); run on a KVM server for the PR evidence.
- **AC14 record_game with input.** `test_record_game_replays_input`: two `record_game {input,
  format: "png", frames: 120}` runs give byte-identical PNG sequences (SHA-256 per frame);
  `format: "shots", resolutions: ["1280x720", "720x1280"]` writes `1280x720/01_menu.png`... with
  those sizes; a mismatched `fps` is refused. Red: replay started before the ready frame (the first
  frames differ between runs on a loaded machine).
- **AC15 Lua API.** `test_lua_input_recording`: `eval_lua` `InputRecording.start()`, `send_input`
  keys, `InputRecording.stop("data://r.input.json")` writes a `start: "now"` file in the session's
  data folder; `InputRecording.play(...)` returns a handle with `isDone()` false then true;
  `stopOnRealInput` ends a looping play at the first `send_input` key and that key reaches the game.
- **AC16 Caps and write failures.** `InputRecorder.StopsAtTheBufferCap` (a 64 KB cap: stops, warns
  with the frame, `truncated: true`) and editor gtest `InputRecordingWrite.FailedWriteKeepsTheBuffer`
  (S03's fault injector: the error names the file; a retry writes it).
- **AC17 Toolbar UI.** `test_record_input_toolbar`: `find_ui_item` finds Record Input; armed, Play
  starts recording (`get_play_state.input.recording`); Stop Recording leaves Play running; the file is
  in `Tests/Input/` when the project has a test root and `undo` removes it; screenshot of the
  recording state and the replay banner (PR evidence).
- **AC18 Comparability.** `InputReplay.OtherPlatformIsNotComparable` (core gtest): a recording with
  `platform: "android-arm64"` replayed on linux reports `comparable: false` and no divergence even
  with different hashes.
- **AC19 Docs and budgets.** `tool_budget.py --check`, `gen_tool_docs.py --check`,
  `test_tool_conventions.py`; eval cases `guard_input_script_file` (an agent asked to repeat a menu
  flow writes a script and runs it with `send_input {file}`) and `bad_input_ms_in_file` (an agent
  writing `ms` waits learns they become frames), each failing without its text;
  `tools/lua_api/tests/test_lua_api.py` with the stub.
- **AC20 No regressions.** `test_input.py`, `test_input_ui.py`, `test_html_ui_input.py`,
  `test_input_actions.py`, `test_scene_inputs.py` and `test_build.py`'s `run_build input` cases pass
  unchanged on the refactored `send_input`.

## 8. Dependencies & conflicts

- **Blocks** `project-test-runner.md`: `Test.input`, replay cases (its R9, AC16) and `shot` as test
  captures need P1 (format, interpreter, parity) and P2 (recorder, checkpoints). Land order: P1, then
  the test runner's core, then P2.
- **Related siblings**: `build-and-run-on-device.md` (its `run_build device` passes `--input-script`
  in `args`; pushing an input file to a device and fetching shots are its to add), `input-device-awareness.md` (recordings carry gamepad connect with the device name, so the
  game's last-used-device logic replays), `text-entry-gamepad-cursor-back-button.md` (IME and back
  button events are recorded as `ime*` and `app`/key events; its new entry points need recorder
  sites), `shipped-game-diagnostics.md` (may keep a ring of this recorder's records),
  `dev-console-for-agents.md` (a console command may start or stop a recording),
  `cancellable-prioritised-events.md` (recording sits before dispatch, so consumption order does not
  change what is recorded), `savegame.md` (replays should start on known data: the test runner's
  Clean profile).
- **Existing specs**: S22 (action injection, edges), S27/S62 (Recorder: shots and `resolutions`
  inherit its "no stand-in frames" rule), S30 (launch options, web query, Android property), S35
  (the sites beside `AudioMarkers`), S34 (player size report), S56 (device simulation for
  `safe_area` tests), S03 (AtomicFile writes).
- **Merge hotspots**: `engine/core/Engine.cpp` input entry points, `editor/mcp/tools/PlayTools.cpp`
  (`send_input`, `wait`, `play`, `get_play_state`; the test runner adds `play data`),
  `editor/mcp/tools/EditorToolsTools.cpp` (`record_game`), `editor/mcp/tools/BuildTools.cpp`
  (`run_build`), `engine/core/player/LaunchOptions.cpp` and `RunControl.h` (exit codes 8-11 shared
  with the test runner), `editor/window/MainToolbar.cpp`, `editor/recorder/*`,
  `tools/mcp/budget.json`, `docs/mcp/TOOLS.md`, `tools/android-emulator-test.sh`.

## 9. Risks & defaults

- **Users expect perfect replays.** Default: report, do not promise (R18-R20); checkpoints make a
  divergence visible at its first second instead of as a puzzling end state.
- **Refactoring `send_input` breaks agents.** Default: parity table (AC3) and the unchanged MCP suites
  (AC20) gate the move; live `ms` keeps wall-clock.
- **Hash too strict** (a cosmetic tween differs). Default: 1/1024 quantisation and per-recording
  `checkpoint_frames 0` to switch it off; probes and values show what actually differs.
- **Hash too weak** (logic diverged without moving a Transform). Default: game values
  (`InputRecording.checkpoint`) cover score and state machines.
- **Recordings grow large** (JSON). Default: 64 MB editor cap; a 10-minute session at 10 events a
  frame is about 15 MB; no compression in v1.
- **Real input mixing with a replay on devices.** Default: blocked unless `--input-allow-real` /
  `blockRealInput = false`.
- **Normalized points on another aspect.** Default: warn (`aspect_mismatch`); scripts prefer
  `target` selectors and `safe_area`.
- **Release players carry the interpreter.** Default: yes (≤ 64 KB, R35): attract modes and store
  screenshots run on the build that ships.

## Deviations

- **Writer signature.** `writeScript` is `bool writeScript(const ParsedScript&, std::string& out,
  std::string& error)`: the steps of a script's composite events (a drag's moves, a relative mouse
  move) have no single-event form, so writing them fails instead of producing a different file.
- **Recording length.** A recording stores `frames` (R1), the frames from `start()` to `stop()` (to
  the cap's frame when truncated), and replays that long; without it a replay ended on its last
  event, cutting the idle tail a recording was stopped after. An optional key, no version bump.
- **Merged steps.** `send_input`'s `app {action: "back"}` (Engine::systemBack, the result's `back`)
  and the gamepad cursor warp (`gamepad` with `x`, `y` or `target`, before its button or axis in the
  same frame) from text-entry-gamepad-cursor-back-button are interpreter steps, so files,
  `Test.input`, players and Lua take them too; `Test.input` resolves targets in the running UI
  Documents as Lua replays do.
- **Back in recordings.** `Engine::systemBack` is a recorder site: the Back button's and an agent's
  Back are recorded as `app {action: "back"}` (`source: "back_button"` for the button). An Escape or
  a gamepad B that is a Back is recorded once, as its key or button, which a replay turns into the
  same Back; the Escape the engine presses for a Back nobody takes is not recorded (the replayed Back
  presses it again).
- **Orientation in replays.** A recording's orientation turn replayed where no device can be
  simulated (Lua, players) is skipped with one warning; a script asking for a turn there fails. The
  recorder stores a turn when the orientation changes, not each `systemOrientationChanged` call.
- **Unmoved mouse moves.** The recorder drops a mouse move to where the last recorded mouse event
  left the pointer (the editor's Game view sends one each hovered frame); captured, every move is kept.
- **Recorder buffer.** `InputRecorder::end()` returns a `RecordedInput` (the swapped record chunks);
  `toScript()` turns it into a `ParsedScript` on a job, so Stop stays O(1) on the main thread (R15).
- **Parity (AC3).** The editor keeps no interpreter of its own: `send_input` parses with
  `parseEventList` (live rules) and runs a `ScriptPlayer`, polled once per editor frame. The parity
  test is the core test `InputRecorderTest.SendInputParityEventsGiveTheSameSystemCalls`: the live
  pacing and a script file run by `ScriptRunner` give the same recorded `Engine::system*` trace.
- **get_play_state.** The recording and replay state are merged into the existing `input` object
  (`input.recording`, `input.replaying`), which already held the game's input.
- **Commands.** Editor commands are in the `playmode.*` group (`playmode.playOrResume`): Record Input
  is `playmode.recordInput` and Replay Input... `playmode.replayInput`, neither with a default key.
- **Toolbar.** The Recorder's Start Recording (S27) already has the red circle right of Step; Record
  Input is a red-ringed circle (`circle-dot`) beside it with its caret menu, which also offers Retry
  Saving Recording while a write that failed waits.
- **Settings.** R37's settings are the project's (`.jasperengine/user/record-input.yaml`, beside the
  Recorder's), edited on Preferences > Play Mode > Record Input and applied with its OK, like every
  preference (no undo).
- **Writing.** The JSON is encoded on a job; the write itself is the undo step's (`WriteFilesCmd`
  through `ProjectFileWrites::writeUndoable`, an atomic write), so it runs on the main thread. A
  failed write is kept until a retry or the next recording: Play's stop is what writes, so dropping
  it at Play's end would lose every recording whose write failed then.
- **Editor replays' host.** Replays the editor starts (`play {input}`, Replay Input, the Inspector,
  Capture Screenshots) resolve `target` in the running UI Documents and `wait {until}` in the game's
  Lua state, as players and `Test.input` do (R8), not through `htmlui_mcp::elementCenter`. Their
  `shot` steps hold the game until two frames draw with nothing loading, then save the Game view.
  The seed goes in through Play Data's before-scripts hook at the session's start: the editor keeps
  one Lua state across sessions, so a seed waiting for a new state would never apply.
- **play {input} warnings.** `scene_override` and `aspect_mismatch` are entries of the start result's
  `warnings` (`{code, recorded_scene}`, `{code, recorded_canvas, canvas}`).
- **record_game {input}.** Play must be stopped: every run (one per resolution) is a new session.
  `format: "png"` or a movie records `frames` of the replay (default: all of it) and skips its `shot`
  steps; `format: "shots"` and `resolutions` need `input` (at most 20 sizes). The output defaults to
  `Recordings/Screenshots/<file stem>/` (shots) or `Recordings/Input/<file stem>/`; the sidecar is
  `<output>/input-capture.json` (per run: resolution, folder, files, the replay's `compared`,
  `comparable`, `diverged_at_frame`, `differences`, and the Recorder's sidecar for frames). The
  result lists `runs` and `files`.
- **run_build {input_script}.** The file is validated in the editor and passed by absolute path;
  its shots go to `Recordings/Input/<file stem>/player/` (`/<WxH>/` per resolution) and come back in
  `input_script {lines, shots, shots_folder}`; exit codes 10 and 11 are not crashes. `resolutions`
  (at most 8, not with `device`) launches once per size and returns `runs`, each run's images among
  the first 4. run_build runs this computer's player on Linux and macOS only: on Windows it was never
  implemented (it says so), so R24's Windows desktop is not covered here.
- **AC11 on macOS (Mac pass).** `test_desktop_player_runs_input_script`'s steps ran on an arm64 Mac
  with a macos-universal-metal-release runtime in place of the Linux glcore one (the test itself builds
  Linux runtimes only). The first run gave 1600x1200 shots for 800x600: a headless player's window
  was sized in points, so a Retina screen doubled its pixels. A headless player's window is now
  sized in backing pixels (`WindowMac::showHeadless`); the run then passed: both sizes exit 0 with
  `01_menu` and `02_game` at 800x600 and 600x800 and the `[input] done` line. A windowed run keeps
  `--resolution` in points.
- **Web input-script URL.** The page's `main()` fetches an `--input-script` that is a URL or a page
  path (`http(s)://`, `/`, `./`, `../`) with a synchronous same-origin `XMLHttpRequest` before the
  engine starts, so every web build has it (not only the prebuilt player's loader), and runs the copy
  from MEMFS; a failed fetch prints `[input] failed` with the URL and exits 10.
- **AC7.** A recording holds input, not shots: the replay is checked by its compared checkpoints (the
  state hash and a `math.random` value every frame) and the replayed presses, not by a shot.
- **The input test project.** It is a compiled project with one scene, so its Replay part is a menu
  in that scene, not a scene of its own: `lua/replay.lua` (required by `main.lua`) makes the UI
  Document `assets/ui/menu.html`, whose `#play` logs `[gate] clicked`, and a `math.random` spawner
  it checkpoints; `assets/tests/Input/menu.input.json` taps through it. It has no physics ball: AC7's
  MCP test builds a scene of its own. The document is made at the first update: `main.lua` runs
  before the game's init sets a scene, so `Engine.getScene()` is nil there (an object made with it
  is now a Lua error; it used to write through a null scene, and the failed `main.lua` then closed
  the Lua state under the engine, which hung the web page).
- **Web gates.** AC12's run is the gate `input-script` (`"project": "input"`, a second gate of the
  same build) rather than a run inside `input.json`, which keeps its pointer-lock run. R30 has
  `input-script-url` (the gate serves the menu script at `/scripts/menu.input.json`: browser_gate's
  new `"serve"`) and `input-script-url-missing` (a 404 exits 10 naming the URL before the engine
  starts: browser_gate's new `"no_ready"`).
- **Android.** `tools/android-emulator-test.sh` accepts `/` in `--setprop` values (a content path).
- A player's seed is applied when its Lua state opens after the engine starts.
- **R34 on box A.** `StateHashBench.Hash2000Transforms` took 0.22 ms on box A (a 2.4 GHz Haswell):
  `std::round` is a libm call there, ten a Transform, and each Transform was looked up by entity.
  `quantizeState` now rounds half away from zero itself (pinned against `std::round`), and the hash
  reads each Transform by its slot in entity order, skipping the hierarchy walk when no GameObject is
  inactive: 0.11-0.13 ms (six runs), the same hashes.
- **Input scripts are a module (S34 `inputscripts`), not in every release player.** R25 said every
  player, release included, takes `--input-script`, and "Release players carry the interpreter"
  defaulted to yes (at most 64 KB, R35). The integration review held that a feature a game does not
  use costs nothing in a mobile or web download, so the interpreter, the recorder, checkpoints and
  their Lua API compile to nothing without the module (`input/InputScriptSwitch.h`; the engine's
  input entry points test a constant, `core/input/InputScriptsOff.cpp` answers the few calls the
  engine makes). Development Builds and the full player keep it; a release Compile build or a
  ui/core/lite variant has it when the project's scripts name `InputRecording` or `InputReplay`
  (`ModuleUsage`, `ModuleUsage.DetectsInputScripts`). Without it `--input-script` logs `[input]
  failed` ("this build has no input scripts") and exits 10, and the two Lua names warn once and do
  nothing. Measured on release glcore players (`tools/build.sh players`, this branch before and after,
  the same image): lite3d `.text` 23,854,482 -> 23,600,946 bytes (-253,536; `.rodata` -9,888); the full
  player, which keeps the module, 40,666,306 -> 40,665,538.
