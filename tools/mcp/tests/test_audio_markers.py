"""Gameplay markers of a capture (docs/specs/S35-agent-audio.md R14; AC7): a click and the voice its
subscriber starts are on one sample, a bounce and a drop say how fast they hit, spawns and destroys name
their prefab, an animation event its function; and a Play with no capture marks nothing.
"""

import math

from audio_refs import capture_sessions, read_jsonl, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Markers"}
CI = True
TIMEOUT = 900

SPAWNER = """---@class Spawner : ScriptInstance
local Spawner = { properties = {} }

function Spawner:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Spawner:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 3 then
        self.coin = GameObject.instantiate("prefabs/Coin.prefab")
    elseif self.frame == 6 then
        GameObject.destroy(self.coin)
    end
end

return Spawner
"""


async def capture_of(ed: Editor, scene, frames: int, fps: int = 60, during=None) -> list[dict]:
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=scene, fixed_fps=fps, auto_pause=True)
    try:
        if during:
            await during()
        await ed.call("wait", frames=frames)
    finally:
        await ed.call("play", action="stop")
    made = [p for p in capture_sessions(ed.project) if p not in before]
    ed.check_eq(len(made), 1, "one session")
    return read_jsonl(made[0] / "events.jsonl")[1:]


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/hit.wav", seconds=0.1)
    await ed.call("sync_files")


async def test_click_and_its_voice_are_on_one_sample(ed: Editor):
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720,
                                                  "graphics.scaling_mode": "letterbox"})
    scene = (await ed.call("create_scene", name="Menu", type="UI", path="scenes/Menu.scene"))["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={"type": "button", "name": "Go", "layout": {"anchor": "top_left", "position": [100, 100], "size": [200, 100]}})
    await ed.call("add_component", scene=scene, entity="Go", component="audiosource")
    await ed.call("set_properties", scene=scene, entity="Go", values={"audiosource": {"filename": "sounds/hit.wav", "playOn": ["ui_click"]}})
    await ed.call("save_scene", scene=scene)

    async def click():
        await ed.call("wait", frames=3)
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 200, "y": 150}], space="canvas", frames_after=3)

    events = await capture_of(ed, scene, 3, during=click)
    clicks = [e for e in events if e["kind"] == "ui_click"]
    voices = [e for e in events if e["kind"] == "voice_start"]
    ed.check_eq(len(clicks), 1, f"{events}")
    ed.check_eq(len(voices), 1, f"{events}")
    ed.check(events.index(clicks[0]) < events.index(voices[0]), "the marker is written before the voice it starts")
    ed.check_eq(clicks[0]["sample"], voices[0]["sample"], f"on one sample: {clicks[0]} {voices[0]}")
    ed.check_eq((clicks[0]["entity"], clicks[0]["x"], clicks[0]["y"]), ("Go", 200, 150), f"{clicks[0]}")
    ed.check([e for e in events if e["kind"] == "ui_press"], f"and its press: {[e['kind'] for e in events]}")
    ed.check([e for e in events if e["kind"] == "ui_hover"], f"and its hover: {[e['kind'] for e in events]}")
    ed.check_eq(clicks[0]["label"], "ui_click Go", f"{clicks[0]}")


async def test_bounce_2d_has_a_speed(ed: Editor):
    scene = (await ed.call("create_scene", name="Bounce", type="2d", path="scenes/Bounce.scene"))["scene"]["id"]
    floor = (await ed.call("create_entity", scene=scene, name="Floor", type="rigidbody2d", position=[0, 0, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=scene, entity=floor, body_type="static", add=[{"shape": "box", "size": [4000, 20], "fit": False}])
    await ed.call("set_properties", scene=scene, entity=floor, values={"rigidbody2d": {"colliders[0].enableHitEvents": True}})
    ball = (await ed.call("create_entity", scene=scene, name="Ball", type="rigidbody2d", position=[0, 400, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=scene, entity=ball, body_type="dynamic", add=[{"shape": "circle", "radius": 10, "fit": False}])
    await ed.call("set_properties", scene=scene, entity=ball, values={"rigidbody2d": {"colliders[0].enableHitEvents": True}})
    await ed.call("save_scene", scene=scene)
    events = await capture_of(ed, scene, 180, fps=120)
    hits = [e for e in events if e["kind"] == "collision_2d"]
    ed.check(hits, f"a collision: {[e['kind'] for e in events]}")
    ed.check(hits[0].get("speed", 0) > 0, f"with its approach speed: {hits[0]}")
    ed.check({hits[0]["entity"], hits[0]["other"]} == {"Ball", "Floor"}, f"{hits[0]}")
    ed.check(hits[0]["label"].startswith("collision_2d "), f"{hits[0]}")


async def test_drop_3d_one_marker_per_pair_with_its_speed(ed: Editor):
    height = 2.0
    scene = (await ed.call("create_scene", name="Drop", type="3D", path="scenes/Drop.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="cube", name="Floor", position=[0, -0.5, 0], scale=[30, 1, 30])
    await ed.call("edit_colliders", scene=scene, entity="Floor", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=scene, type="cube", name="Box", position=[0, height + 0.5, 0])
    await ed.call("edit_colliders", scene=scene, entity="Box", body_type="dynamic", add=[{"shape": "box"}])
    await ed.call("save_scene", scene=scene)
    events = await capture_of(ed, scene, 240, fps=120)
    hits = [e for e in events if e["kind"] == "collision_3d"]
    ed.check(hits, f"a collision: {[e['kind'] for e in events]}")
    expected = math.sqrt(2 * 9.81 * height)
    ed.check(abs(hits[0]["speed"] - expected) <= 0.05 * expected, f"speed {hits[0]['speed']} vs sqrt(2gh) = {expected}")
    per_step = [e["frame"] for e in hits]
    ed.check_eq(len(per_step), len(set(per_step)), f"one per pair per step: {hits}")


async def test_spawn_and_destroy_name_their_prefab(ed: Editor):
    scene = (await ed.call("create_scene", name="Spawn", type="2D", path="scenes/Spawn.scene"))["scene"]["id"]
    coin = (await ed.call("create_entity", scene=scene, type="empty", name="Coin"))["entity_id"]
    await ed.call("create_prefab", scene=scene, entity=coin, path="prefabs/Coin.prefab")
    made = (await ed.call("get_prefab_info", prefab="prefabs/Coin.prefab"))
    # (the instance stays in the scene: only prefabs of the build load by path)
    await ed.call("create_entity", scene=scene, type="empty", name="Spawner")
    await ed.call("create_script", kind="lua", name="Spawner", content=SPAWNER, entity="Spawner", scene=scene)
    await ed.call("save_scene", scene=scene)
    await ed.console_since(limit=500)
    events = await capture_of(ed, scene, 12)
    spawns = [e for e in events if e["kind"] == "spawn"]
    destroys = [e for e in events if e["kind"] == "destroy"]
    console = [e.get("message", "")[:200] for e in await ed.console_since(limit=100)]
    ed.check_eq(len(spawns), 1, f"{[e['kind'] for e in events]} {console}")
    ed.check_eq(spawns[0]["prefab"], "prefabs/Coin.prefab", f"{spawns[0]}")
    ed.check(spawns[0]["guid"] and spawns[0]["guid"] in str(made), f"{spawns[0]} {made}")
    ed.check_eq(len(destroys), 1, f"{[e['kind'] for e in events]}")
    ed.check_eq(destroys[0]["entity"], spawns[0]["entity"], f"{destroys[0]}")
    ed.check(spawns[0]["sample"] < destroys[0]["sample"], "in order")


async def test_animation_event_carries_its_function(ed: Editor):
    scene = (await ed.call("create_scene", name="Anim", type="3D", path="scenes/Anim.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="cube", name="Bob")
    clip = "assets/anim/bob.anim"
    await ed.call("edit_animation", clip=clip, create={}, wrap="loop", frame_rate=30,
                  tracks=[{"path": "", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.5, "value": [0, 2, 0]}]}],
                  events=[{"time": 0.1, "function": "onBob"}])
    await ed.call("add_component", scene=scene, entity="Bob", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": clip, "name": "Bob", "wrap": "loop"}], "playAutomatically": True}})
    await ed.call("save_scene", scene=scene)
    events = await capture_of(ed, scene, 30)
    fired = [e for e in events if e["kind"] == "animation_event"]
    ed.check(fired, f"it fired: {[e['kind'] for e in events]}")
    ed.check_eq(fired[0]["function"], "onBob", f"{fired[0]}")
    ed.check_eq(fired[0]["entity"], "Bob", f"{fired[0]}")


async def test_no_capture_marks_nothing():
    async with Editor(create=True, name="No Capture", env={"JASPERENGINE_AUDIO_BACKEND": "nosound"}) as ed:
        scene = (await ed.call("create_scene", name="Off", type="2D", path="scenes/Off.scene"))["scene"]["id"]
        await ed.call("save_scene", scene=scene)
        await ed.call("play", action="start", scene=scene, fixed_fps=60)
        await ed.call("wait", frames=3)
        values = (await ed.call("eval_lua", code='Audio.mark("nothing"); return Audio.isCapturing()'))["values"]
        await ed.call("play", action="stop")
        ed.check_eq(values, [False], "not capturing")
        ed.check_eq(capture_sessions(ed.project), [], "and no session")
