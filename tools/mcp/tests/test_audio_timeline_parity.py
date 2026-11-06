"""One engine path for both timelines (docs/specs/S40-timeline-audio.md AC10, R26): the same four footsteps of a
Sound Event placed as clip cues on an Animation Player and as audio actions on a legacy Animation, with the same
seed and fixed_fps, start on the same samples with the same gains, pitches and picks, and master.wav holds the
same bytes from the start of the walk.
"""

import hashlib
import json

from audio_refs import capture_sessions, read_jsonl, write_wav
from harness import Editor

EDITOR = {"create": True, "name": "Timeline Parity"}
CI = True
TIMEOUT = 900

RATE = 48000
STEPS = [0.1, 0.35, 0.6, 0.85]
EVENT = "events/step.soundevent"

CLIP_DRIVER = """---@class ClipDriver : ScriptInstance
local ClipDriver = { properties = {} }
function ClipDriver:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end
function ClipDriver:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 1 then
        Audio.mark("start")
        GameObject(self.scene, self.entity):getAnimationPlayer():play("Walk")
    end
end
return ClipDriver
"""

ACTION_DRIVER = """---@class ActionDriver : ScriptInstance
local ActionDriver = { properties = {} }
function ActionDriver:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end
function ActionDriver:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 1 then
        Audio.mark("start")
        Action(self.scene, self.entity):start()
    end
end
return ActionDriver
"""


async def capture(ed: Editor, scene) -> tuple:
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=75)
    finally:
        await ed.call("play", action="stop")
    made = [p for p in capture_sessions(ed.project) if p not in before]
    ed.check_eq(len(made), 1, "one session")
    lines = read_jsonl(made[0] / "events.jsonl")[1:]
    start = next(l for l in lines if l["kind"] == "marker" and l.get("label") == "start")["sample"]
    voices = [l for l in lines if l["kind"] == "voice_start"]
    return made[0], start, voices


def walk(session, start: int) -> str:
    """SHA-1 of master.wav's bytes from the walk's start, one second of it"""
    data = (session / "master.wav").read_bytes()
    at = data.find(b"data") + 8
    frame = 2 * 4
    return hashlib.sha1(data[at + start * frame:at + (start + RATE) * frame]).hexdigest()


async def test_both_timelines_give_the_same_samples(ed: Editor):
    for i in range(3):
        write_wav(ed.project / f"sounds/step{i}.wav", [[(0.2 + 0.1 * i) * (1 if (k // 40) % 2 else -1) for k in range(3000 + 500 * i)]], RATE)
    await ed.call("sync_files")
    event = {"format": "jasperengine.soundevent", "version": 1, "mode": "random",
             "clips": [{"clip": {"path": f"sounds/step{i}.wav"}} for i in range(3)],
             "pitch_semitones": [-2, 2], "volume_db": [-6, 0]}
    await ed.call("write_file", path=EVENT, content=json.dumps(event, indent=2))
    clip = {"format": "jasperengine.anim", "version": 3, "frame_rate": 60, "wrap": "once", "length": 1.0, "tracks": [], "events": [],
            "audio": [{"time": t, "sound": {"path": EVENT}} for t in STEPS]}
    await ed.call("write_file", path="anim/walk.anim", content=json.dumps(clip, indent=2))
    await ed.call("sync_files")

    clips = (await ed.call("create_scene", name="Clips", type="2D", path="scenes/Clips.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=clips, type="empty", name="Walker")
    await ed.call("add_component", scene=clips, entity="Walker", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "anim/walk.anim", "name": "Walk"}], "playAutomatically": False}})
    await ed.call("create_script", kind="lua", name="ClipDriver", content=CLIP_DRIVER, entity="Walker", scene=clips)
    await ed.call("save_scene", scene=clips)

    actions = (await ed.call("create_scene", name="Actions", type="2D", path="scenes/Actions.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=actions, type="animation", name="Footsteps")
    frames = []
    for i, t in enumerate(STEPS):
        await ed.call("create_entity", scene=actions, type="animation", name=f"Step{i}")
        await ed.call("remove_component", scene=actions, entity=f"Step{i}", component="animation")
        await ed.call("add_component", scene=actions, entity=f"Step{i}", component="audiosource", values={"audiosource": {"event": EVENT}})
        frames.append({"action": f"Step{i}", "start_time": t})
    await ed.call("edit_animation", scene=actions, animation="Footsteps", add_actions=frames)
    await ed.call("create_script", kind="lua", name="ActionDriver", content=ACTION_DRIVER, entity="Footsteps", scene=actions)
    await ed.call("save_scene", scene=actions)

    clip_session, clip_start, clip_voices = await capture(ed, clips)
    action_session, action_start, action_voices = await capture(ed, actions)
    ed.check_eq(len(clip_voices), 4, f"{clip_voices}")
    ed.check_eq(len(action_voices), 4, f"{action_voices}")

    def summary(voices, start):
        return [(v["sample"] + v.get("delay_samples", 0) - start, v["gain"], v["pitch"], v.get("variation"), v["event_path"]) for v in voices]

    ed.check_eq(summary(clip_voices, clip_start), summary(action_voices, action_start),
                "the same placement, gain, pitch and pick on both timelines")
    ed.check_eq([s[0] for s in summary(clip_voices, clip_start)], [round(t * RATE) for t in STEPS], "on the footsteps' samples")
    ed.check(len({v.get("variation") for v in clip_voices}) > 1, f"the event really picks: {clip_voices}")
    ed.check_eq(clip_start, action_start, "both walks start in the same frame")
    ed.check_eq(walk(clip_session, clip_start), walk(action_session, action_start), "master.wav: the same bytes over the walk")
