"""Audio cues of animation clips in Play (docs/specs/S40-timeline-audio.md AC3, AC4): under S35's deterministic
capture at fixed_fps 60, four footstep cues of an Animation Player land on their samples (a `cue` line, then a
`voice_start` owned "cue" with cause "animation" whose sample plus delay_samples is the cue's time, and the sound
in master.wav one latency later); speed 2 and time scale 0.5 move them as they should; a loop fires its cue once
a pass; an Animator's 1D blend tree fires only the leader's cues and a montage its clip's cues; a source cue
starts the hierarchy's AudioSource, whose audiosource.volume track then fades it; a state_exit cue stops with
reason cue_exit on a crossfade; a snapshot cue logs its mixer transition in its frame; an unresolved cue warns once.
"""

import json
import math

from audio_refs import capture_sessions, read_float_wav, read_jsonl, write_wav
from harness import Editor

EDITOR = {"create": True, "name": "Timeline Audio"}
CI = True
TIMEOUT = 1500

RATE = 48000
STEPS = [0.1, 0.35, 0.6, 0.85]
STATE = {}

SILENT_HANDLERS = """---@class Walker : ScriptInstance
local Walker = { properties = {} }
function Walker:onStart() end
return Walker
"""

TIME_SCALE = """---@class Slow : ScriptInstance
local Slow = { properties = {} }
function Slow:init() Engine.timeScale = 0.5 end
function Slow:onStart() end
return Slow
"""

CROSSFADE = """---@class Fader : ScriptInstance
local Fader = { properties = {} }
function Fader:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end
function Fader:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 20 then GameObject(self.scene, self.entity):getAnimationPlayer():crossFade("Idle", 0.1) end
end
function Fader:onStart() end
return Fader
"""


def anim(cues, wrap="once", length=1.0, tracks=None, events=None) -> str:
    return json.dumps({"format": "jasperengine.anim", "version": 3, "frame_rate": 60, "wrap": wrap, "length": length,
                       "tracks": tracks or [], "events": [{"time": 0, "function": "onStart"}] if events is None else events,
                       "audio": cues}, indent=2)


def steps(times, **extra) -> list:
    return [{"time": t, "sound": {"path": "sounds/step.wav"}, "name": f"step {i}", **extra} for i, t in enumerate(times)]


async def capture(ed: Editor, scene, frames: int, during=None) -> tuple:
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        if during:
            await during()
        await ed.call("wait", frames=frames)
    finally:
        await ed.call("play", action="stop")
    made = [p for p in capture_sessions(ed.project) if p not in before]
    ed.check_eq(len(made), 1, "one session")
    return made[0], read_jsonl(made[0] / "events.jsonl")[1:]


async def walker_scene(ed: Editor, name: str, clip: str, player: dict, script: str = SILENT_HANDLERS, script_name: str = "Walker"):
    scene = (await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="empty", name="Walker")
    await ed.call("add_component", scene=scene, entity="Walker", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": clip, "name": "Walk"}], "playAutomatically": True, **player}})
    await ed.call("create_script", kind="lua", name=f"{script_name}{name}", content=script, entity="Walker", scene=scene)
    await ed.call("save_scene", scene=scene)
    return scene


def start_sample(ed: Editor, lines: list) -> int:
    started = [l for l in lines if l["kind"] == "animation_event" and l.get("function") == "onStart"]
    ed.check(started, f"the clip's start event is the reference: {[l['kind'] for l in lines][:40]}")
    return started[0]["sample"]


def placed(voice: dict) -> int:
    return voice["sample"] + voice.get("delay_samples", 0)


async def test_setup(ed: Editor):
    # a step that starts at full scale (its first sample is its onset) and a long tone for the loops
    write_wav(ed.project / "sounds/step.wav", [[0.5] * 2400], RATE)
    write_wav(ed.project / "sounds/engine.wav", [[0.5 * math.sin(2 * math.pi * 440 * i / RATE) for i in range(RATE * 2)]], RATE)
    await ed.call("sync_files")


async def test_footsteps_land_on_their_samples(ed: Editor):
    await ed.call("write_file", path="anim/footsteps.anim", content=anim(steps(STEPS)))
    await ed.call("sync_files")
    scene = await walker_scene(ed, "Footsteps", "anim/footsteps.anim", {})
    await ed.console_since(limit=500)
    session, lines = await capture(ed, scene, 70)
    console = [e.get("message", "")[:300] for e in await ed.console_since(limit=200) if e.get("type") != "log"]
    s0 = start_sample(ed, lines)
    cues = [l for l in lines if l["kind"] == "cue"]
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq(len(cues), 4, f"one cue line a step: {cues} {console} {[l['kind'] for l in lines]}")
    ed.check_eq(len(voices), 4, f"one voice a step: {voices}")
    for i, (cue, voice) in enumerate(zip(cues, voices)):
        ed.check_eq((cue["timeline"], cue["clip"], cue["cue"], cue["name"], cue["cue_kind"], cue["player"], cue["state"]),
                    ("clip", "anim/footsteps.anim", i, f"step {i}", "sound", "Walker", "Walk"), f"{cue}")
        ed.check(lines.index(cue) < lines.index(voice), "the cue line comes before its voice")
        ed.check_eq((voice["owner"], voice["cause"], voice["clip"]), ("cue", "animation", "sounds/step.wav"), f"{voice}")
        ed.check_eq(cue["delay_samples"], voice.get("delay_samples", 0), f"the same placement: {cue} {voice}")
        ed.check_eq(placed(voice), s0 + round(STEPS[i] * RATE), f"step {i} on its sample (± 0): {voice}")
    # the sound itself: its first sample one latency after the placed sample
    rate, channels, samples = read_float_wav(session / "master.wav")
    left = samples[0::channels]
    first = next((i for i in range(s0, len(left)) if abs(left[i]) > 1e-4), None)
    ed.check_eq(first, placed(voices[0]) + voices[0]["latency_samples"], "the first step is heard where its line says")
    STATE["footsteps"] = scene


async def test_speed_and_time_scale_move_the_samples(ed: Editor):
    fast = await walker_scene(ed, "Fast", "anim/footsteps.anim", {"speed": 2.0})
    _, lines = await capture(ed, fast, 40)
    s0 = start_sample(ed, lines)
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq([placed(v) - s0 for v in voices], [round(t / 2 * RATE) for t in STEPS], "speed 2 halves the times")

    slow = await walker_scene(ed, "Slow", "anim/footsteps.anim", {}, script=TIME_SCALE, script_name="Slow")
    _, lines = await capture(ed, slow, 110)
    s0 = start_sample(ed, lines)
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq([placed(v) - s0 for v in voices], [round(t / 0.5 * RATE) for t in STEPS], "time scale 0.5 doubles them")


async def test_a_loop_fires_its_cue_once_a_pass(ed: Editor):
    await ed.call("write_file", path="anim/loop.anim", content=anim(steps([0.1]), wrap="loop", length=0.5))
    await ed.call("sync_files")
    scene = await walker_scene(ed, "Loop", "anim/loop.anim", {})
    _, lines = await capture(ed, scene, 72)
    s0 = start_sample(ed, lines)
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq([placed(v) - s0 for v in voices], [round((0.1 + 0.5 * k) * RATE) for k in range(3)], "0.1 s of each pass, once")


async def test_animator_fires_the_leader_and_montages_their_clip(ed: Editor):
    # a track on the root: the Animator's rig has a bone to bind
    bob = [{"path": "", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [0, 1, 0]]]}]
    await ed.call("write_file", path="anim/a.anim", content=anim(steps([0.2], name="A"), wrap="loop", length=1.0, events=[], tracks=bob))
    await ed.call("write_file", path="anim/b.anim", content=anim(steps([0.3], name="B"), wrap="loop", length=1.0, events=[], tracks=bob))
    await ed.call("write_file", path="anim/wave.anim", content=anim(steps([0.05], name="W"), length=0.5, events=[], tracks=bob))
    await ed.call("sync_files")
    await ed.call("edit_animator_controller", path="anim/walker.controller", create=True, ops=[
        {"op": "add_parameter", "name": "Speed", "type": "float", "default": 0.2},
        {"op": "add_state", "name": "Move", "default": True, "blend_tree": {
            "type": "simple_1d", "parameter": "Speed", "children": [{"clip": "anim/a.anim", "threshold": 0}, {"clip": "anim/b.anim", "threshold": 1}]}},
        {"op": "add_layer", "name": "Upper", "slot": "Upper", "weight": 1},
        {"op": "add_state", "layer": "Upper", "name": "Empty"}])
    await ed.call("edit_montage", path="anim/wave.montage", create=True, clip="anim/wave.anim", slot="Upper", rate=1,
                  sections=[{"name": "A", "start": 0}])
    scene = (await ed.call("create_scene", name="Animated", type="2D", path="scenes/Animated.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="empty", name="Walker")
    await ed.call("add_component", scene=scene, entity="Walker", component="animator", values={"animator": {"controller": "anim/walker.controller"}})
    await ed.call("save_scene", scene=scene)

    async def montage():
        await ed.call("wait", frames=20)
        await ed.call("control_animator", entity="Walker", scene=scene, montage={"path": "anim/wave.montage"})

    _, lines = await capture(ed, scene, 30, during=montage)
    names = [l["name"] for l in lines if l["kind"] == "cue"]
    if "A" not in names:
        state = await ed.call("get_animator", entity="Walker", scene=scene)
        ed.fail(f"the 1D tree fires its leader's cue (S21 R9): {names}; {json.dumps(state)[:2500]}")
    ed.check("B" not in names, f"the 1D tree fires only its leader's cue (S21 R9): {names}")
    ed.check("W" in names, f"the montage fires its clip's cue: {names}")
    montage_cue = next(l for l in lines if l["kind"] == "cue" and l["name"] == "W")
    ed.check(montage_cue["state"].endswith("wave.montage"), f"named by its montage: {montage_cue}")


async def test_source_cue_track_shaped_and_stopped_on_exit(ed: Editor):
    # the Engine child's AudioSource starts at 0 and its volume track fades it 1 -> 0.25 over the second
    # the track authored by edit_animation, the cue added to the file it wrote
    await ed.call("edit_animation", clip="anim/engine.anim", create={}, wrap="loop", frame_rate=60, length=1.0,
                  tracks=[{"path": "Engine", "channel": "property", "component": "audiosource", "property": "volume",
                           "keys": [{"time": 0, "value": 1.0}, {"time": 1.0, "value": 0.25}]}],
                  events=[{"time": 0, "function": "onStart"}])
    data = json.loads((ed.project / "anim/engine.anim").read_text())
    data["version"] = 3
    data["audio"] = [{"time": 0, "source": "Engine", "stop": "state_exit"}]
    await ed.call("write_file", path="anim/engine.anim", content=json.dumps(data, indent=2))
    await ed.call("write_file", path="anim/idle.anim", content=anim([], wrap="loop", length=1.0, events=[]))
    await ed.call("sync_files")
    scene = (await ed.call("create_scene", name="Engine", type="2D", path="scenes/Engine.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="empty", name="Walker")
    await ed.call("create_entity", scene=scene, type="empty", name="Engine", parent="Walker")
    await ed.call("add_component", scene=scene, entity="Engine", component="audiosource",
                  values={"audiosource": {"filename": "sounds/engine.wav", "looping": True}})
    await ed.call("add_component", scene=scene, entity="Walker", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "anim/engine.anim", "name": "Walk"}, {"clip": "anim/idle.anim", "name": "Idle"}],
                                              "defaultClip": "Walk", "playAutomatically": True}})
    await ed.call("create_script", kind="lua", name="Fader", content=CROSSFADE, entity="Walker", scene=scene)
    await ed.call("save_scene", scene=scene)
    session, lines = await capture(ed, scene, 40)
    starts = [l for l in lines if l["kind"] == "voice_start"]
    ed.check_eq(len(starts), 1, f"the source's voice: {starts}")
    ed.check_eq((starts[0]["entity"], starts[0]["owner"], starts[0]["cause"]), ("Walker/Engine", "main", "animation"), f"{starts[0]}")
    stops = [l for l in lines if l["kind"] == "voice_stop" and l.get("voice") == starts[0]["voice"]]
    ed.check(stops and stops[0]["reason"] == "cue_exit", f"stopped on the crossfade with reason cue_exit: {stops}")
    # the level follows the volume track: frame 3..15, 20 ms windows against 20 log10(v(t))
    rate, channels, samples = read_float_wav(session / "master.wav")
    left = samples[0::channels]
    s0 = starts[0]["sample"] + starts[0].get("delay_samples", 0) + starts[0]["latency_samples"]

    def level(t):
        a = s0 + int((t - 0.01) * rate)
        window = left[a:a + int(0.02 * rate)]
        return 10 * math.log10(sum(x * x for x in window) / len(window))

    reference = level(0.05)
    for t in (0.1, 0.15, 0.2, 0.25):
        expected = 20 * math.log10((1.0 - 0.75 * t) / (1.0 - 0.75 * 0.05))
        ed.check(abs((level(t) - reference) - expected) <= 0.5, f"at {t} s: {level(t) - reference:.2f} dB vs {expected:.2f} dB")


async def test_snapshot_cue_and_an_unresolved_cue(ed: Editor):
    await ed.call("tool_groups", enable=["audio"])
    await ed.call("audio_mixer", action="edit", ops=[{"op": "add_snapshot", "name": "Build"}])
    await ed.call("write_file", path="anim/build.anim", content=anim(
        [{"time": 0.25, "snapshot": "Build", "seconds": 0.5}, {"time": 0.3, "sound": {"path": "sounds/missing.wav"}},
         {"time": 0.4, "source": "Nobody"}], wrap="loop", length=0.5))
    await ed.call("sync_files")
    scene = await walker_scene(ed, "Build", "anim/build.anim", {})
    await ed.console_since(limit=500)
    _, lines = await capture(ed, scene, 70)
    console = [e.get("message", "") for e in await ed.console_since(limit=500)]
    s0 = start_sample(ed, lines)
    mixer = [l for l in lines if l["kind"] == "mixer" and l.get("action") == "transition"]
    ed.check(mixer, f"the snapshot cue transitions: {[l['kind'] for l in lines]}")
    ed.check_eq((mixer[0]["snapshot"], mixer[0].get("cause")), ("Build", "animation"), f"{mixer[0]}")
    # in its frame: the update that reaches 0.25 s (frame 14 of 800 samples, 15 when the float sum lands just short)
    ed.check(mixer[0]["sample"] - s0 in (14 * 800, 15 * 800), f"in the cue's frame: {mixer[0]}")
    missing = [m for m in console if "missing.wav" in m and "does not resolve" in m]
    nobody = [m for m in console if "Nobody binds nothing" in m]
    ed.check_eq(len(missing), 1, f"one warning for the missing sound over two passes: {missing}")
    ed.check_eq(len(nobody), 1, f"one warning for the path that binds nothing: {nobody}")
    ed.check("cue 2 of anim/build.anim" in nobody[0], f"names the cue and the clip: {nobody[0]}")


async def test_cue_follows_a_moved_sound(ed: Editor):
    # R3: the cue holds the sound's GUID; a move (one undo step) leaves it resolving to the new place
    write_wav(ed.project / "sounds/door.wav", [[0.5] * 2400], RATE)
    await ed.call("sync_files")
    await ed.call("edit_animation", clip="anim/door.anim", create={}, wrap="once", length=0.5, frame_rate=60,
                  events=[{"time": 0, "function": "onStart"}], audio=[{"time": 0.1, "sound": "sounds/door.wav", "name": "door"}])
    body = json.loads((ed.project / "anim/door.anim").read_text())
    ed.check(body["audio"][0]["sound"].get("guid"), f"the GUID is stored: {body['audio']}")
    await ed.call("create_folder", path="sounds/moved")
    await ed.call("move_asset", path="sounds/door.wav", to="sounds/moved/door.wav")
    got = await ed.call("get_animation", clip="anim/door.anim")
    ed.check_eq(got["audio"][0]["sound"]["path"], "sounds/moved/door.wav", f"resolved by its GUID: {got['audio']}")
    scene = await walker_scene(ed, "Door", "anim/door.anim", {})
    _, lines = await capture(ed, scene, 20)
    voices = [l for l in lines if l["kind"] == "voice_start"]
    ed.check(voices and voices[0]["clip"] == "sounds/moved/door.wav", f"Play plays the moved file: {voices}")
    await ed.call("undo", scope="project")
    got = await ed.call("get_animation", clip="anim/door.anim")
    ed.check_eq(got["audio"][0]["sound"]["path"], "sounds/door.wav", f"undo moves it back, one step: {got['audio']}")
