"""Gameplay triggers (docs/specs/S37-sound-events.md R12; AC6): a UI click plays one voice per click,
a 2D bounce plays with a gain that grows with the impact and is gated below minImpactSpeed, a 3D drop
from h plays once per pair with Contact3D:getApproachSpeed() within 5 % of sqrt(2 g h). That a scene
without a flagged source has no audio subscription: SoundEventTriggerTest.NoFlagNoSubscription (the
signals' hasSubscribers is not in Lua).
"""

import math

from audio_refs import TRACE_ENV, voice_lines, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Triggers", "env": TRACE_ENV}
CI = True
TIMEOUT = 900

WATCH = """---@class ContactWatch : ScriptInstance
local ContactWatch = { properties = {} }

function ContactWatch:init()
    self.speeds = {}
    self.sensorSpeeds = {}
    RegisterEvent(self, self.scene:getPhysicsSystem().onContactAdded3D, "onAdded")
end

function ContactWatch:onAdded(a, b, contact)
    table.insert(self.speeds, contact:getApproachSpeed())
end

return ContactWatch
"""


async def test_ui_click_one_voice_per_click(ed: Editor):
    write_tone(ed.project / "sounds/hit.wav", seconds=0.1)
    await ed.call("sync_files")
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720,
                                                  "graphics.scaling_mode": "letterbox"})
    scene = (await ed.call("create_scene", name="Menu", type="UI", path="scenes/Menu.scene"))["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={"type": "button", "name": "Go", "layout": {"anchor": "top_left", "position": [100, 100], "size": [200, 100]}})
    await ed.call("add_component", scene=scene, entity="Go", component="audiosource")
    await ed.call("set_properties", scene=scene, entity="Go", values={"audiosource": {"filename": "sounds/hit.wav", "playOn": ["ui_click"]}})
    await ed.call("save_scene", scene=scene)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await ed.console_since(limit=500)
        for count in (1, 2):
            await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 200, "y": 150}], space="canvas", frames_after=3)
            lines = [l for l in voice_lines(await ed.console_since(limit=500)) if l["type"] == "voice_start"]
            ed.check_eq(len(lines), 1, f"click {count}: one voice: {lines}")
            ed.check_eq(lines[0]["trigger"], "ui_click", f"{lines}")
            ed.check_eq(lines[0]["cause"], "trigger", f"{lines}")
        # outside it: none
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 800, "y": 500}], space="canvas", frames_after=3)
        ed.check_eq(voice_lines(await ed.console_since(limit=500)), [], "a click elsewhere plays nothing")
    finally:
        await ed.call("play", action="stop")


async def bounce_scene(ed: Editor, heights: list[float], minimum: float, full: float) -> str:
    scene = (await ed.call("create_scene", name="Bounce", type="2d", path="scenes/Bounce.scene"))["scene"]["id"]
    floor = (await ed.call("create_entity", scene=scene, name="Floor", type="rigidbody2d", position=[0, 0, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=scene, entity=floor, body_type="static",
                  add=[{"shape": "box", "size": [4000, 20], "fit": False}])
    await ed.call("set_properties", scene=scene, entity=floor, values={"rigidbody2d": {"colliders[0].enableHitEvents": True}})
    for i, height in enumerate(heights):
        name = f"Ball{i}"
        ball = (await ed.call("create_entity", scene=scene, name=name, type="rigidbody2d", position=[-300 + 200 * i, height, 0]))["entity_id"]
        await ed.call("edit_colliders", scene=scene, entity=ball, body_type="dynamic", add=[{"shape": "circle", "radius": 10, "fit": False}])
        await ed.call("add_component", scene=scene, entity=ball, component="audiosource")
        await ed.call("set_properties", scene=scene, entity=ball, values={"audiosource": {
            "filename": "sounds/hit.wav", "playOn": ["collision_2d"], "minImpactSpeed": minimum, "fullVolumeImpactSpeed": full}})
    await ed.call("save_scene", scene=scene)
    return scene


async def test_bounce_2d_gain_grows_with_speed_and_a_gate(ed: Editor):
    # the balls land at different speeds (about 310, 610 and 1000 points a second at 64 points a
    # metre): the lowest is under the gate
    scene = await bounce_scene(ed, [100, 300, 800], 450.0, 100000.0)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.console_since(limit=500)
        await ed.call("wait", frames=180)
        lines = voice_lines(await ed.console_since(limit=500))
        starts = [l for l in lines if l["type"] == "voice_start"]
        gated = [l for l in lines if l.get("reason") == "impact_gate"]
        ed.check(gated, f"the slow ones are gated: {lines}")
        ed.check(all(l["impact_speed"] < 450 for l in gated), f"{gated}")
        ed.check(len(starts) >= 2, f"the fast ones play: {lines}")
        first_by_entity = {}
        for line in starts:
            first_by_entity.setdefault(line["entity"], line)
        ordered = sorted(first_by_entity.values(), key=lambda l: l["impact_speed"])
        for slower, faster in zip(ordered, ordered[1:]):
            ed.check(faster["gain"] >= slower["gain"], f"the gain grows with the speed: {ordered}")
        for line in starts:
            ed.check(abs(line["gain"] - min(1.0, line["impact_speed"] / 100000.0)) < 1e-3, f"gain = speed / full: {line}")
            ed.check(line["impact_speed"] >= 450, f"above the gate: {line}")
    finally:
        await ed.call("play", action="stop")


async def test_drop_3d_approach_speed_within_5_percent(ed: Editor):
    height = 5.0
    scene = (await ed.call("create_scene", name="Drop", type="3D", path="scenes/Drop.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="cube", name="Floor", position=[0, -0.5, 0], scale=[30, 1, 30])
    await ed.call("edit_colliders", scene=scene, entity="Floor", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=scene, type="cube", name="Box", position=[0, height + 0.5, 0])
    await ed.call("edit_colliders", scene=scene, entity="Box", body_type="dynamic", add=[{"shape": "box"}])
    await ed.call("add_component", scene=scene, entity="Box", component="audiosource")
    await ed.call("set_properties", scene=scene, entity="Box", values={"audiosource": {"filename": "sounds/hit.wav", "playOn": ["collision_3d"]}})
    await ed.call("create_entity", scene=scene, type="empty", name="Watch")
    await ed.call("create_script", kind="lua", name="ContactWatch", content=WATCH, entity="Watch", scene=scene)
    await ed.call("save_scene", scene=scene)

    await ed.call("play", action="start", scene=scene, fixed_fps=120, auto_pause=True)
    try:
        await ed.console_since(limit=500)
        await ed.call("wait", frames=240)
        speeds = (await ed.call("eval_lua", code="return script('Watch').speeds"))["values"][0]
        ed.check(speeds, "the contact arrived")
        expected = math.sqrt(2 * 9.81 * height)
        ed.check(abs(speeds[0] - expected) <= 0.05 * expected, f"approach speed {speeds[0]} vs {expected}")
        lines = [l for l in voice_lines(await ed.console_since(limit=500)) if l["type"] == "voice_start"]
        ed.check(lines, "a voice started")
        ed.check_eq(lines[0]["trigger"], "collision_3d", f"{lines}")
        ed.check(abs(lines[0]["impact_speed"] - expected) <= 0.05 * expected, f"{lines[0]}")
        first_sample = lines[0]["sample"]
        ed.check_eq(len([l for l in lines if l["sample"] == first_sample]), 1, f"one voice for the pair in its frame: {lines}")
    finally:
        await ed.call("play", action="stop")
