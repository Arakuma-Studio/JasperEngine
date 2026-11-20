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
`--input-allow-real`.

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
