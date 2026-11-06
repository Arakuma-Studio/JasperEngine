"""Timeline audio over MCP (docs/specs/S40-timeline-audio.md AC6, AC11's MCP part): edit_animation's `audio`
on a clip (replace and merge by time and kind with events_mode, a sound by path or GUID, refusals that name
the fix, version 3 on disk, one project undo step) and get_animation's audio[] with each cue's length; on an
entity Animation the same items create audio and Mixer Snapshot actions (one scene undo step), get_animation
lists them as audio[] with their frame index and an automatic frame as long as its sound, and
remove_actions removes them as any action.
"""

import json

from audio_refs import write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Animation Audio MCP"}
CI = True
TIMEOUT = 600

CLIP = "anim/walk.anim"      # a new project's assets folder is the project folder
EVENT = "events/steps.soundevent"
STATE = {}


async def edit(ed: Editor, **args):
    r = await ed.call_raw("edit_animation", args)
    ed.check(not r.is_error, f"edit_animation {json.dumps(args)[:400]} failed: {r.text[:1500]}")
    return r.structured


async def refused(ed: Editor, args: dict, contains: str) -> str:
    r = await ed.call_raw("edit_animation", args)
    ed.check(r.is_error, f"edit_animation {json.dumps(args)[:300]} should fail, got: {r.text[:400]}")
    ed.check(contains.lower() in r.text.lower(), f"the error should say {contains!r}: {r.text[:500]}")
    return r.text


def on_disk(ed: Editor) -> dict:
    return json.loads((ed.project / CLIP).read_text())


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/step.wav", seconds=0.3, amplitude=0.5, frequency=600)
    write_tone(ed.project / "sounds/step2.wav", seconds=0.2, amplitude=0.5, frequency=700)
    await ed.call("sync_files")
    await ed.call("tool_groups", enable=["audio"])
    made = await ed.call("edit_sound_event", path=EVENT, create=True, values={"mode": "shuffle", "clips": ["sounds/step.wav", "sounds/step2.wav"]})
    STATE["event_guid"] = made["guid"]
    r = await ed.call_raw("audio_mixer", {"action": "edit", "ops": [{"op": "add_snapshot", "name": "Build"}]})
    ed.check(not r.is_error, f"a snapshot to move to: {r.text[:400]}")
    info = await ed.call("get_asset_info", path="sounds/step.wav")
    STATE["step_guid"] = info.get("guid")
    ed.check(STATE["step_guid"] and len(STATE["step_guid"]) == 32, f"the clip's GUID: {json.dumps(info)[:400]}")


async def test_edit_audio_cues(ed: Editor):
    made = await edit(ed, clip=CLIP, create={}, audio=[
        {"time": 0.25, "sound": EVENT, "spatial": True, "path": "Body/LeftFoot", "volume_db": -2, "name": "Left step"},
        {"time": 0.0, "source": "Body/Engine", "stop": "state_exit"},
        {"time": 1.0, "snapshot": "Build", "seconds": 0.5}])
    ed.check_eq([c["kind"] for c in made["audio"]], ["source", "sound", "snapshot"], f"sorted by time: {made['audio']}")
    body = on_disk(ed)
    ed.check_eq(body["version"], 3, "a clip with cues is version 3")
    ed.check_eq(body["audio"][1]["sound"]["guid"], STATE["event_guid"], f"the sound's GUID on disk: {body['audio']}")
    ed.check_eq(body["audio"][1]["sound"]["path"], EVENT, "asset-relative path")

    got = await ed.call("get_animation", clip=CLIP)
    sound = got["audio"][1]
    ed.check_eq(sound["sound"]["path"], EVENT, f"the resolved path: {sound}")
    ed.check(sound["length"] is not None and abs(sound["length"] - 0.3) < 0.01, f"an event's length is its longest clip: {sound}")
    ed.check_eq(got["audio"][2]["length"], 0.5, "a snapshot cue lasts its transition")

    # merge: same time and kind replaces, others are added; a clip by GUID
    merged = await edit(ed, clip=CLIP, events_mode="merge", audio=[
        {"time": 0.25, "sound": EVENT, "volume_db": -6},
        {"time": 0.75, "sound": STATE["step_guid"]}])
    ed.check_eq(len(merged["audio"]), 4, f"one replaced, one added: {merged['audio']}")
    ed.check_eq(merged["audio"][1]["volume_db"], -6, "the cue at 0.25 was replaced")
    ed.check_eq(merged["audio"][2]["sound"]["path"], "sounds/step.wav", "a GUID resolves to its clip")
    ed.check(abs(merged["audio"][2]["length"] - 0.3) < 0.01, f"a clip's length: {merged['audio'][2]}")

    # refusals name the fix and change nothing
    before = (ed.project / CLIP).read_text()
    await refused(ed, {"clip": CLIP, "audio": [{"time": 0.1, "sound": EVENT, "snapshot": "Build"}]}, "exactly one of")
    await refused(ed, {"clip": CLIP, "audio": [{"time": 0.1, "snapshot": "Nope"}]}, "snapshots: ")
    await refused(ed, {"clip": CLIP, "audio": [{"time": 0.1, "sound": EVENT, "volume_db": 40}]}, "from -60 to 12")
    await refused(ed, {"clip": CLIP, "audio": [{"time": 0.1, "sound": CLIP}]}, "not a sound")
    await refused(ed, {"clip": CLIP, "audio": [{"time": 0.1, "source": "Body//Engine"}]}, "binding path")
    await refused(ed, {"clip": CLIP, "audio": [{"time": 0.1, "sound": EVENT, "path": "Body"}]}, "spatial: true")
    ed.check_eq((ed.project / CLIP).read_text(), before, "a refused call writes nothing")

    # one project undo step per call
    await ed.call("undo", scope="project")
    ed.check_eq(len((await ed.call("get_animation", clip=CLIP))["audio"]), 3, "undo takes the merge back")
    await ed.call("redo", scope="project")
    ed.check_eq(len((await ed.call("get_animation", clip=CLIP))["audio"]), 4, "redo")

    # replace with [] clears them: the clip is version 1 again
    await edit(ed, clip=CLIP, audio=[])
    ed.check_eq(on_disk(ed)["version"], 1, "no cues: version 1")
    ed.check("audio" not in on_disk(ed), "no audio key without cues")


async def test_entity_mode_audio_actions(ed: Editor):
    hero = (await ed.call("create_entity", type="cube", name="Hero"))["entity_id"]
    made = await edit(ed, target=hero, audio=[
        {"time": 0.1, "sound": "sounds/step.wav"},
        {"time": 0.5, "sound": EVENT, "duration": 0.2, "lane": 3},
        {"time": 1.0, "snapshot": "Build", "seconds": 0.25}])
    animation = made["animation"]["id"]
    STATE["animation"] = animation
    audio = made["audio"]
    ed.check_eq([a["index"] for a in audio], [0, 1, 2], f"frame indices: {audio}")
    kinds = [made["actions"][a["index"]]["kind"] for a in audio]
    ed.check_eq(kinds, ["audio", "sound_event", "mixer_snapshot"], f"the action kinds: {made['actions']}")
    ed.check(abs(audio[0]["effective_duration"] - 0.3) < 0.01, f"an automatic frame lasts its sound: {audio[0]}")
    ed.check(abs(audio[1]["duration"] - 0.2) < 1e-6, f"an explicit duration: {audio[1]}")
    ed.check_eq(audio[1]["lane"], 3, "its lane")
    ed.check_eq(audio[2]["snapshot"], "Build", "the snapshot action")
    ed.check(abs(audio[2]["effective_duration"] - 0.25) < 1e-4, f"a snapshot action lasts its transition: {audio[2]}")
    ed.check_eq(audio[0]["sound"]["path"], "sounds/step.wav", "the clip")

    got = await ed.call("get_animation", entity=animation)
    ed.check_eq(len(got.get("audio", [])), 3, f"get_animation lists them: {json.dumps(got)[:600]}")

    # one scene undo step creates all three
    await ed.call("undo", scope="scene")
    got = await ed.call_raw("get_animation", {"entity": animation})
    ed.check(got.is_error or not got.structured.get("audio"), f"undo removed the animation and its actions: {got.text[:300]}")
    await ed.call("redo", scope="scene")
    ed.check_eq(len((await ed.call("get_animation", entity=animation))["audio"]), 3, "redo")

    # remove_actions removes them as any action, one step
    removed = await edit(ed, animation=animation, remove_actions=[0, 2])
    ed.check_eq([a["snapshot"] if "snapshot" in a else a["sound"]["path"] for a in removed.get("audio", [])], [EVENT],
                f"only the event's action is left: {removed.get('audio')}")
    await ed.call("undo", scope="scene")
    ed.check_eq(len((await ed.call("get_animation", entity=animation))["audio"]), 3, "undo brings them back")

    await refused(ed, {"animation": animation, "audio": [{"time": 0.2, "source": "Body/Engine"}]}, "audio actions")
    await refused(ed, {"animation": animation, "audio": [{"time": 0.2, "sound": EVENT, "stop": "state_exit"}]}, "duration")
