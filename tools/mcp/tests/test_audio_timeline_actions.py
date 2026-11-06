"""Audio and Mixer Snapshot actions of the legacy action timeline in Play (docs/specs/S40-timeline-audio.md AC8,
AC9): under S35's deterministic capture at fixed_fps 60, a "Footsteps" Animation with four audio actions starts
each voice on its sample (owner "main", cause "animation", on the sample of its `cue` line); a looping Animation
starts each once a pass (the target's rule restarted an ended action every frame); pausing and stopping the
Animation pause and stop its voice (reason cue_exit); speed 2 halves the times and doubles the pitch; an
auto-length frame lasts its sound; a standalone audio action fires onStop at its end; a Mixer Snapshot action
transitions at its start and reverts at its stop only while the snapshot is still its own, and its component
round-trips, is set from Lua and set_properties, and leaves an old scene unchanged.
"""

import json

from audio_refs import capture_sessions, read_jsonl, write_wav
from harness import Editor

EDITOR = {"create": True, "name": "Timeline Actions"}
CI = True
TIMEOUT = 1500

RATE = 48000
STEPS = [0.1, 0.35, 0.6, 0.85]

DRIVER = """---@class Driver : ScriptInstance
local Driver = { properties = {} }
function Driver:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end
function Driver:onUpdate()
    self.frame = self.frame + 1
    local animation = Action(self.scene, self.entity)
    if self.frame == 1 then
        Audio.mark("start")
        animation:start()
    end
    %s
end
return Driver
"""

STANDALONE = """---@class Solo : ScriptInstance
local Solo = { properties = {} }
function Solo:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, Action(self.scene, self.entity):getActionComponent().onStop, "onStopped")
end
function Solo:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 1 then Audio.mark("start"); Action(self.scene, self.entity):start() end
end
function Solo:onStopped()
    Audio.mark("standalone stopped")
end
return Solo
"""

SNAPSHOT_LUA = """---@class Snap : ScriptInstance
local Snap = { properties = {} }
function Snap:init()
    local snapshot = GameObject.find("Duck"):getAudioSnapshotComponent()
    snapshot.seconds = 0.25
    snapshot.revertSeconds = 0.1
    Log.print("snapshot " .. snapshot.snapshot .. " " .. tostring(snapshot.unscaled) .. " " .. tostring(snapshot.revert))
end
return Snap
"""


async def capture(ed: Editor, scene, frames: int) -> list:
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=frames)
    finally:
        await ed.call("play", action="stop")
    made = [p for p in capture_sessions(ed.project) if p not in before]
    ed.check_eq(len(made), 1, "one session")
    return read_jsonl(made[0] / "events.jsonl")[1:]


async def bare_action(ed: Editor, scene, name: str, component: str, values: dict) -> None:
    # an action entity without a Transform: an Animation's entity with its Animation component taken off
    await ed.call("create_entity", scene=scene, type="animation", name=name)
    await ed.call("remove_component", scene=scene, entity=name, component="animation")
    await ed.call("add_component", scene=scene, entity=name, component=component, values={component: values})


async def animation_scene(ed: Editor, name: str, frames: list, then: str = "", loop: bool = False, speed: float = 1.0) -> str:
    """frames: (start_time, duration or None, sound)"""
    scene = (await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="animation", name="Footsteps")
    await ed.call("set_properties", scene=scene, entity="Footsteps", values={"animation": {"loop": loop}, "action": {"speed": speed}})
    actions = []
    for i, (start, duration, sound) in enumerate(frames):
        await bare_action(ed, scene, f"Step{i}", "audiosource", {"filename": sound})
        item = {"action": f"Step{i}", "start_time": start}
        if duration:
            item["duration"] = duration
        actions.append(item)
    await ed.call("edit_animation", scene=scene, animation="Footsteps", add_actions=actions)
    await ed.call("create_script", kind="lua", name=f"Driver{name}", content=DRIVER % then, entity="Footsteps", scene=scene)
    await ed.call("save_scene", scene=scene)
    return scene


def mark(ed: Editor, lines: list, label: str) -> dict:
    found = [l for l in lines if l["kind"] == "marker" and l.get("label") == label]
    ed.check(found, f"mark {label!r}: {[l['kind'] for l in lines][:60]}")
    return found[0]


def placed(voice: dict) -> int:
    return voice["sample"] + voice.get("delay_samples", 0)


async def test_setup(ed: Editor):
    write_wav(ed.project / "sounds/step.wav", [[0.5] * 2400], RATE)
    write_wav(ed.project / "sounds/quarter.wav", [[0.5] * (RATE // 4)], RATE)
    write_wav(ed.project / "sounds/long.wav", [[0.25] * (RATE * 2)], RATE)
    await ed.call("sync_files")


async def test_footsteps_land_on_their_samples(ed: Editor):
    scene = await animation_scene(ed, "Footsteps", [(t, None, "sounds/step.wav") for t in STEPS])
    lines = await capture(ed, scene, 70)
    s0 = mark(ed, lines, "start")["sample"]
    voices = [l for l in lines if l["kind"] == "voice_start"]
    cues = [l for l in lines if l["kind"] == "cue"]
    ed.check_eq(len(voices), 4, f"{voices}")
    ed.check_eq(len(cues), 4, f"{cues}")
    for i, (cue, voice) in enumerate(zip(cues, voices)):
        ed.check_eq((cue["timeline"], cue["cue"], cue["cue_kind"], cue["player"]), ("actions", i, "source", "Footsteps"), f"{cue}")
        ed.check_eq((voice["owner"], voice["cause"], voice["entity"]), ("main", "animation", f"Step{i}"), f"{voice}")
        ed.check_eq(voice["sample"], cue["sample"], f"on the sample of its cue line: {cue} {voice}")
        ed.check_eq(placed(voice) - s0, round(STEPS[i] * RATE), f"step {i} on its sample (± 0): {voice}")


async def test_a_loop_starts_each_once_a_pass(ed: Editor):
    scene = await animation_scene(ed, "Loop", [(0.1, 0.4, "sounds/step.wav")], loop=True)
    lines = await capture(ed, scene, 72)
    s0 = mark(ed, lines, "start")["sample"]
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq([placed(v) - s0 for v in voices], [round((0.1 + 0.5 * k) * RATE) for k in range(3)],
                "once a pass at 0.1 s, not again every frame after it ended")


async def test_an_auto_length_frame_lasts_its_sound(ed: Editor):
    scene = await animation_scene(ed, "Auto", [(0.0, None, "sounds/quarter.wav")], loop=True)
    lines = await capture(ed, scene, 50)
    s0 = mark(ed, lines, "start")["sample"]
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq([placed(v) - s0 for v in voices][:3], [0, RATE // 4, RATE // 2], "the loop is the sound's 0.25 s")
    got = await ed.call("get_animation", scene=scene, entity="Footsteps")
    ed.check("0.25" in json.dumps(got), f"get_animation reports the frame's length: {json.dumps(got)[:800]}")


async def test_speed_two_halves_the_times(ed: Editor):
    scene = await animation_scene(ed, "Fast", [(t, None, "sounds/step.wav") for t in STEPS], speed=2.0)
    lines = await capture(ed, scene, 40)
    s0 = mark(ed, lines, "start")["sample"]
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq([placed(v) - s0 for v in voices], [round(t / 2 * RATE) for t in STEPS], "speed 2 halves the times")
    ed.check(all(abs(v["pitch"] - 2.0) < 1e-6 for v in voices), f"and the voice plays at the animation's speed: {voices}")


async def test_pause_and_stop_follow_the_animation(ed: Editor):
    then = """if self.frame == 10 then animation:pause() end
    if self.frame == 20 then animation:start() end
    if self.frame == 30 then animation:stop() end"""
    scene = await animation_scene(ed, "Paused", [(0.0, None, "sounds/long.wav")], then=then)
    lines = await capture(ed, scene, 40)
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq(len(voices), 1, f"a resume does not start it again: {voices}")
    voice = voices[0]["voice"]
    kinds = [(l["kind"], l.get("reason")) for l in lines if l.get("voice") == voice and l["kind"] != "voice_start"]
    ed.check_eq(kinds, [("voice_pause", None), ("voice_resume", None), ("voice_stop", "cue_exit")], f"{kinds}")


async def test_a_standalone_audio_action_stops_at_its_end(ed: Editor):
    scene = (await ed.call("create_scene", name="Solo", type="2D", path="scenes/Solo.scene"))["scene"]["id"]
    await bare_action(ed, scene, "Solo", "audiosource", {"filename": "sounds/quarter.wav"})
    await ed.call("create_script", kind="lua", name="Solo", content=STANDALONE, entity="Solo", scene=scene)
    await ed.call("save_scene", scene=scene)
    lines = await capture(ed, scene, 40)
    s0 = mark(ed, lines, "start")["sample"]
    stopped = mark(ed, lines, "standalone stopped")["sample"]
    ed.check(RATE // 4 <= stopped - s0 <= RATE // 4 + 3 * 800, f"onStop when its sound ended: {stopped - s0}")


async def test_snapshot_action(ed: Editor):
    await ed.call("tool_groups", enable=["audio"])
    await ed.call("audio_mixer", action="edit", ops=[{"op": "add_snapshot", "name": "Calm"}, {"op": "add_snapshot", "name": "Duck"},
                                                     {"op": "add_snapshot", "name": "Boss"}, {"op": "set_start_snapshot", "name": "Calm"}])
    scene = (await ed.call("create_scene", name="Snap", type="2D", path="scenes/Snap.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="animation", name="Footsteps")
    await bare_action(ed, scene, "Duck", "audiosnapshot", {"snapshot": "Duck"})
    await bare_action(ed, scene, "Duck2", "audiosnapshot", {"snapshot": "Duck"})
    await ed.call("edit_animation", scene=scene, animation="Footsteps", add_actions=[{"action": "Duck", "start_time": 0.1, "duration": 0.2},
                                                                                {"action": "Duck2", "start_time": 0.5, "duration": 0.1}])
    # Boss takes over during the second one: its stop leaves the mixer on Boss
    then = 'if self.frame == 35 then AudioMixer.transitionTo("Boss", 0) end'
    await ed.call("create_script", kind="lua", name="DriverSnap", content=DRIVER % then, entity="Footsteps", scene=scene)
    await ed.call("create_script", kind="lua", name="Snap", content=SNAPSHOT_LUA, entity="Footsteps", scene=scene)
    await ed.call("save_scene", scene=scene)
    await ed.console_since(limit=500)
    lines = await capture(ed, scene, 50)
    console = " ".join(e.get("message", "") for e in await ed.console_since(limit=500))
    ed.check("snapshot Duck false true" in console, f"read from Lua: {console[-600:]}")
    mixer = [(l["snapshot"], l.get("cause"), l["seconds"]) for l in lines if l["kind"] == "mixer" and l.get("action") == "transition"]
    ed.check_eq(mixer, [("Duck", "animation", 0.25), ("Calm", "animation", 0.1), ("Duck", "animation", 0), ("Boss", None, 0)],
                f"transitions at the start, reverts at the stop while it is its own (Lua set seconds and revertSeconds): {mixer}")

    # the component round-trips, set_properties sets it, an old scene re-saves unchanged
    await ed.call("set_properties", scene=scene, entity="Duck", values={"audiosnapshot": {"seconds": 1.5, "unscaled": True, "revert": False}})
    got = await ed.call("get_entity", scene=scene, entity="Duck", components=["audiosnapshot"])
    ed.check('"seconds": 1.5' in json.dumps(got) and '"revert": false' in json.dumps(got), f"{json.dumps(got)[:600]}")
    await ed.call("save_scene", scene=scene)
    text = (ed.project / "scenes/Snap.scene").read_text()
    ed.check("audiosnapshot:" in text and "revertSeconds" not in text, f"OmitIfDefault keys: {text[text.find('audiosnapshot'):][:300]}")
    await ed.call("save_scene", scene=scene)
    ed.check_eq((ed.project / "scenes/Snap.scene").read_text(), text, "saving again changes nothing")
    old = (ed.project / "scenes/Footsteps.scene").read_text()
    footsteps = (await ed.call("open_scene", scene="scenes/Footsteps.scene"))["scene"]["id"]
    await ed.call("save_scene", scene=footsteps)
    ed.check_eq((ed.project / "scenes/Footsteps.scene").read_text(), old, "a scene without the component re-saves unchanged")
