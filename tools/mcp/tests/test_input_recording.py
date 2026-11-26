"""Input scripts and recordings as an agent sees them (docs/specs/input-recorder-and-input-scripts.md
AC7, AC10, AC11, AC14, AC15, AC17, R21): send_input {file} runs a script file's steps (wait until,
shot), a recording is refused there and replays from Play's start with play {input} (its shots into
Recordings/Input/<stem>/); the toolbar's Record Input records a Play into Tests/Input/ (one undo step)
and its replay matches every checkpoint; record_game replays a file (the same PNGs twice, shots per
resolution); a desktop player runs a script headless per resolution; Lua's InputRecording records and
replays.

Scene InputRec (3D) with one cube running KeyCounter: it moves with the frames and the presses, and
checkpoints a math.random roll every frame (a replay without the recording's seed diverges at the
first checkpoint). Play runs with a fixed 1/60 s step. The player test needs JASPER_SLOW_BUILD=1
(the run's shared glcore runtime, tools/mcp/build_cache.py).
"""

import asyncio
import hashlib
import json
import os
import struct
import time

import build_cache
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Input Recording CI"}
PREBUILT_PLAYERS = ["glcore-release"]

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "3600"))

COUNTER = """---@class KeyCounter : ScriptInstance
local KeyCounter = { properties = {} }

function KeyCounter:init()
    presses = 0
    frames = 0
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function KeyCounter:onUpdate()
    frames = frames + 1
    MenuReady = frames >= 30
    if Input.getKeyDown(Input.KEY_SPACE) then presses = presses + 1 end
    self.go.position = Vector3(frames * 0.01, 0, presses * 0.5)
    InputRecording.checkpoint("roll", math.random(1, 1000000))
    InputRecording.checkpoint("presses", presses)
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
    ent = await ed.call("create_entity", scene=S["scene"], type="cube", name="Counter")
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


def png_size(path) -> tuple[int, int]:
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])


async def disarm(ed: Editor) -> None:
    """Record Input off again: the tests share one editor."""
    state = await ed.call("get_play_state")
    if state.get("record_input", {}).get("armed"):
        await ed.call("run_command", id="playmode.recordInput")


async def wait_for_file(ed: Editor, path, what: str) -> None:
    deadline = time.monotonic() + 60
    while not path.is_file():
        ed.check(time.monotonic() < deadline, f"{what}: no {path}")
        await asyncio.sleep(0.2)


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
        recorded = await lua(ed, "return File.readText('data://r.input.json')")
        keys = [e for e in json.loads(recorded or "{}").get("events", []) if e.get("type") == "key"]
        ed.check_eq([(e["key"], e["action"]) for e in keys], [("space", "down"), ("space", "up")], f"the press was recorded: {recorded}")
        # asked in the same chunk: Play runs on between calls, past the few frames the recording lasts
        started = await lua(ed, "presses = 0; replay = InputRecording.play('data://r.input.json'); return replay:isDone()")
        ed.check_eq(started, False, "running")
        await ed.call("wait", until="replay:isDone()", frames=600)
        ed.check_eq(await lua(ed, "return presses"), 1, "the recorded press replayed")
    finally:
        await stop(ed)


async def test_play_input_shots(ed: Editor):
    """R21: play {input}'s shot steps are PNGs in Recordings/Input/<file stem>/, listed in input.shots."""
    await setup(ed)
    folder = ed.project / "Tests" / "Input"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "shots.input.json").write_text(script_file([
        {"type": "wait", "until": "MenuReady", "frames": 300},
        {"type": "shot", "name": "01_menu"},
        {"type": "key", "key": "space"},
        {"type": "wait", "frames": 5},
        {"type": "shot", "name": "02_after"},
    ]))
    started = await start(ed, input="Tests/Input/shots.input.json")
    try:
        await ed.call("wait", **started["next"]["args"])
        state = await ed.call("get_play_state")
        shots = state["input"].get("shots", [])
        ed.check_eq([s["name"] for s in shots], ["01_menu", "02_after"], f"shots: {state['input']}")
        for name in ("01_menu", "02_after"):
            png = ed.project / "Recordings" / "Input" / "shots" / f"{name}.png"
            ed.check(png.is_file(), f"the shot's PNG {png}")
        ed.check_eq(await lua(ed, "return presses"), 1, "the press between the shots")
    finally:
        await stop(ed)


async def test_record_then_replay_matches(ed: Editor):
    """AC7: Record Input armed, Play records from Play's start (seed, events, a checkpoint every 60
    frames); play {input} replays it with every checkpoint compared and none diverged."""
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    await ed.call("run_command", id="playmode.recordInput")
    try:
        ed.check((await ed.call("get_play_state")).get("record_input", {}).get("armed"), "armed")
        await ed.call("play", action="start", scene=s["scene"])
        state = await ed.call("get_play_state")
        recording = state["input"].get("recording", {})
        ed.check("file" in recording, f"recording: {state['input']}")
        for _ in range(5):
            await ed.call("send_input", events=[{"type": "key", "key": "space"}, {"type": "wait", "frames": 59}])
        await ed.call("play", action="stop")
        file = ed.project / recording["file"]
        await wait_for_file(ed, file, "Play's stop writes the recording")
    finally:
        await disarm(ed)
    data = json.loads(file.read_text())
    ed.check_eq((data["kind"], data["start"]), ("recording", "play"), f"header: {list(data)}")
    ed.check(isinstance(data.get("seed"), int), f"seed: {data.get('seed')}")
    keys = [e for e in data["events"] if e["type"] == "key"]
    ed.check_eq(len(keys), 10, f"5 presses down and up: {keys}")
    ed.check(len(data.get("checkpoints", [])) >= 5, f"a checkpoint every 60 frames: {data.get('checkpoints')}")

    started = await start(ed, input=recording["file"])
    try:
        await ed.call("wait", **started["next"]["args"])
        replaying = (await ed.call("get_play_state"))["input"]["replaying"]
        ed.check_eq(replaying["compared"], len(data["checkpoints"]), f"every checkpoint compared: {replaying}")
        ed.check_eq(replaying["diverged_at_frame"], None, f"no divergence: {replaying}")
        ed.check_eq(await lua(ed, "return presses"), 5, "the presses replayed")
    finally:
        await stop(ed)


async def test_record_input_toolbar(ed: Editor):
    """AC17: the toolbar's Record Input; armed, Play records into the test root's Input/; Stop
    Recording leaves Play running; undo removes the file."""
    await setup(ed)
    (ed.project / "Tests").mkdir(exist_ok=True)
    (ed.project / "Tests" / "tests.yaml").write_text("version: 1\n")
    items = (await ed.call("find_ui_item", label="##Record Input")).get("items", [])
    ed.check(items and items[0].get("enabled", True), f"the toolbar button: {items}")
    await ed.call("run_command", id="playmode.recordInput")
    try:
        await start(ed)
        state = await ed.call("get_play_state")
        recording = state["input"].get("recording", {})
        ed.check(recording.get("file", "").startswith("Tests/Input/"), f"into the test root: {recording}")
        await ed.call("send_input", events=[{"type": "key", "key": "space"}, {"type": "wait", "frames": 10}])
        await ed.call("run_command", id="playmode.recordInput")
        state = await ed.call("get_play_state")
        ed.check_eq(state["state"], "playing", "Stop Recording leaves Play running")
        ed.check("recording" not in state["input"], f"stopped: {state['input']}")
        file = ed.project / recording["file"]
        await wait_for_file(ed, file, "Stop Recording writes the recording")
        ed.check_eq((await ed.call("get_play_state"))["record_input"].get("last_file"), recording["file"], "last_file")
        await ed.call("undo", scope="project")
        ed.check(not file.exists(), "undo removes the recording")
    finally:
        await stop(ed)
        await disarm(ed)
        (ed.project / "Tests" / "tests.yaml").unlink(missing_ok=True)


async def test_record_game_replays_input(ed: Editor):
    """AC14: two record_game {input, png} runs give the same PNGs frame by frame; shots per
    resolution land in <output>/<WxH>/ at that size; another fps is refused naming the file's."""
    await setup(ed)
    folder = ed.project / "Tests" / "Input"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "rec120.input.json").write_text(json.dumps({
        "format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play", "fixed_fps": 60, "seed": 4242,
        "scene": "scenes/InputRec.scene", "frames": 120,
        "events": [{"frame": 10, "type": "key", "key": "space", "action": "down"},
                   {"frame": 12, "type": "key", "key": "space", "action": "up"},
                   {"frame": 70, "type": "key", "key": "space", "action": "down"},
                   {"frame": 71, "type": "key", "key": "space", "action": "up"}]}))
    hashes = []
    for run in range(2):
        result = await ed.call("record_game", input="Tests/Input/rec120.input.json", format="png", frames=120,
                               output=f"Recordings/replay{run}", timeout_seconds=600, timeout=900)
        files = result.get("files", [])
        ed.check_eq(len(files), 120, f"run {run}: {result.get('runs')}")
        hashes.append([hashlib.sha256(open(f, "rb").read()).hexdigest() for f in files])
    differ = [i for i, (a, b) in enumerate(zip(*hashes)) if a != b]
    ed.check(not differ, f"frames that differ between the runs: {differ[:10]}")

    (folder / "store.input.json").write_text(script_file([
        {"type": "wait", "frames": 30}, {"type": "shot", "name": "01_menu"},
        {"type": "key", "key": "space"}, {"type": "wait", "frames": 10}, {"type": "shot", "name": "02_game"}]))
    result = await ed.call("record_game", input="Tests/Input/store.input.json", format="shots", resolutions=["1280x720", "720x1280"],
                           output="Recordings/Store", timeout_seconds=600, timeout=900)
    for size, (w, h) in (("1280x720", (1280, 720)), ("720x1280", (720, 1280))):
        for name in ("01_menu", "02_game"):
            png = ed.project / "Recordings" / "Store" / size / f"{name}.png"
            ed.check(png.is_file(), f"{png}: {result}")
            ed.check_eq(png_size(png), (w, h), f"{png}")
    ed.check((ed.project / "Recordings" / "Store" / "input-capture.json").is_file(), "the sidecar")
    refused = await ed.call("record_game", input="Tests/Input/rec120.input.json", fps=30, frames=10, expect_error=True)
    ed.check("This file runs at 60 fps (fixed_fps); record_game fps must be 60 or omitted." in refused, f"fps: {refused}")


async def test_desktop_player_runs_input_script():
    """AC11: run_build {input_script, resolutions, headless} on a release prebuilt player, with no
    DISPLAY: each size exits 0 with its two shots at that size and the [input] done line."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run prebuilt players (the run builds its glcore runtimes once)")
    async with Editor(create=True, name="Input Script Player", env=build_cache.player_env("glcore", "release")) as ed:
        await build_cache.player_runtime(ed, "glcore", "release")
        await setup(ed)
        saved = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
        job = await ed.call("build_project", mode="desktop", output_dir=str(ed.scratch.root / "export"), method="prebuilt",
                            timeout_seconds=600)
        deadline = time.monotonic() + TIMEOUT
        while job.get("status") == "running" and time.monotonic() < deadline:
            await asyncio.sleep(3)
            job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
        ed.check_eq(job["status"], "succeeded", f"{job}")
        folder = ed.project / "Tests" / "Input"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "player.input.json").write_text(script_file([
            {"type": "wait", "until": "MenuReady", "frames": 300}, {"type": "shot", "name": "01_menu"},
            {"type": "key", "key": "space"}, {"type": "wait", "frames": 5}, {"type": "shot", "name": "02_game"}]))
        result = await ed.call("run_build", input_script="Tests/Input/player.input.json", resolutions=["800x600", "600x800"],
                               headless=True, timeout=900)
        runs = result.get("runs", [])
        ed.check_eq([r["resolution"] for r in runs], ["800x600", "600x800"], f"{result}")
        for run, size in zip(runs, ((800, 600), (600, 800))):
            ed.check_eq(run.get("exit_code"), 0, f"{run.get('log')}")
            shots = run["input_script"]["shots"]
            ed.check_eq([s["name"] for s in shots], ["01_menu", "02_game"], f"{run['input_script']}")
            for shot in shots:
                ed.check_eq((shot["width"], shot["height"]), size, f"{shot}")
            ed.check(any(line.startswith("[input] done") for line in run["input_script"]["lines"]), f"{run['input_script']['lines']}")

