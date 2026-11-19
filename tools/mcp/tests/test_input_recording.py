"""Input scripts and recordings as an agent sees them (docs/specs/input-recorder-and-input-scripts.md
AC10, AC15, R21): send_input {file} runs a script file's steps (wait until, shot), a recording is
refused there and replays from Play's start with play {input}; Lua's InputRecording records and
replays.

Scene InputRec (3D) with one GameObject running KeyCounter. Play runs with a fixed 1/60 s step.
"""

import json

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Input Recording CI"}

TIMEOUT = 900

COUNTER = """---@class KeyCounter : ScriptInstance
local KeyCounter = { properties = {} }

function KeyCounter:init()
    presses = 0
    frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

function KeyCounter:onUpdate()
    frames = frames + 1
    MenuReady = frames >= 30
    if Input.getKeyDown(Input.KEY_SPACE) then presses = presses + 1 end
end

return KeyCounter
"""

S: dict = {}


def script_file(events: list, **header) -> str:
    return json.dumps({"format": "jasperengine-input", "version": 1, "kind": "script", **header, "events": events})


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="InputRec", type="3D", path="scenes/InputRec")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Counter")
    await ed.call("create_script", kind="lua", name="KeyCounter", content=COUNTER, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def start(ed: Editor, **extra) -> dict:
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, **extra)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    return started


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop", fixed_fps=0)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"][0]


async def test_send_input_file_steps(ed: Editor):
    """A script file: wait until the menu is ready, shot, press space; its shots come back."""
    await setup(ed)
    folder = ed.project / "Tests" / "Input"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "menu.input.json").write_text(script_file([
        {"type": "wait", "until": "MenuReady", "frames": 300},
        {"type": "shot", "name": "01_menu"},
        {"type": "key", "key": "space", "hold_ms": 50},
    ]))
    await start(ed)
    try:
        await ed.call("eval_lua", code="presses = 0")
        result = await ed.call("send_input", file="Tests/Input/menu.input.json")
        ed.check_eq(await lua(ed, "return presses"), 1, f"the press reached the game: {result}")
        shots = result.get("shots", [])
        ed.check_eq([s["name"] for s in shots], ["01_menu"], f"shots: {shots}")
        ed.check((ed.project / "Recordings" / "Input" / "menu" / "01_menu.png").is_file(), "the shot's PNG")
        # a wait until past its cap fails naming the condition
        (folder / "never.input.json").write_text(script_file([{"type": "wait", "until": "NeverTrue == true", "frames": 10}]))
        failed = await ed.call("send_input", file="Tests/Input/never.input.json", expect_error=True)
        ed.check("until timed out after 10 frames: NeverTrue == true" in failed, f"cap: {failed}")
        # a recording replays from Play's start
        (folder / "rec.input.json").write_text(json.dumps({"format": "jasperengine-input", "version": 1, "kind": "recording",
                                                           "events": [{"frame": 0, "type": "key", "key": "a", "action": "down"}]}))
        refused = await ed.call("send_input", file="Tests/Input/rec.input.json", expect_error=True)
        ed.check("A recording replays from Play's start: play {action: \"start\", input: \"Tests/Input/rec.input.json\"}" in refused,
                 f"refusal: {refused}")
        # ms in a file are frames; view cannot be stored
        (folder / "view.input.json").write_text(script_file([{"type": "touch", "x": 1, "y": 1, "space": "view"}]))
        view = await ed.call("send_input", file="Tests/Input/view.input.json", expect_error=True)
        ed.check("cannot be stored" in view, f"view: {view}")
    finally:
        await stop(ed)


async def test_play_replays_a_recording(ed: Editor):
    """play {input}: a recording replays from the first frame the scene runs; next says how to wait for it."""
    await setup(ed)
    folder = ed.project / "Tests" / "Input"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "presses.input.json").write_text(json.dumps({
        "format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play", "fixed_fps": 60,
        "events": [{"frame": 10, "type": "key", "key": "space", "action": "down"},
                   {"frame": 12, "type": "key", "key": "space", "action": "up"},
                   {"frame": 40, "type": "key", "key": "space", "action": "down"},
                   {"frame": 41, "type": "key", "key": "space", "action": "up"}]}))
    started = await start(ed, input="Tests/Input/presses.input.json")
    try:
        nxt = started.get("next", {})
        ed.check_eq(nxt.get("tool"), "wait", f"next: {started}")
        await ed.call("wait", **nxt["args"])
        ed.check_eq(await lua(ed, "return presses"), 2, "both presses replayed")
        ed.check_eq(await lua(ed, "return InputRecording.isReplaying()"), False, "the replay ended")
        state = await ed.call("get_play_state")
        ed.check_eq(state["input"]["replaying"]["kind"], "recording", f"input: {state['input']}")
    finally:
        await stop(ed)


async def test_lua_input_recording(ed: Editor):
    """InputRecording.start, send_input keys, stop into data://; play returns a handle that ends."""
    await start(ed)
    try:
        ed.check_eq(await lua(ed, "return InputRecording.start()"), True, "start")
        await ed.call("send_input", events=[{"type": "key", "key": "space"}, {"type": "wait", "frames": 5}])
        path = await lua(ed, "return InputRecording.stop('data://r.input.json')")
        ed.check_eq(path, "data://r.input.json", "stop returns the path")
        await ed.call("wait", frames=10)
        # asked in the same chunk: Play runs on between calls, past the few frames the recording lasts
        started = await lua(ed, "presses = 0; replay = InputRecording.play('data://r.input.json'); return replay:isDone()")
        ed.check_eq(started, False, "running")
        await ed.call("wait", until="replay:isDone()", frames=600)
        ed.check_eq(await lua(ed, "return presses"), 1, "the recorded press replayed")
    finally:
        await stop(ed)
