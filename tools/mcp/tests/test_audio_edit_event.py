"""edit_sound_event (docs/specs/S37-sound-events.md R20; AC10): create, values, null reset, match_levels,
audition with picks equal to the next Play picks under the same seed, problems for a missing clip and an
unknown group, one undo step per call, the tool hidden until the audio group is enabled, and
get_asset_info's block.
"""

import json

from audio_refs import TRACE_ENV, Event, EventState, voice_lines, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Edit Event", "env": TRACE_ENV}
CI = True
TIMEOUT = 600

EVENT = "events/steps.soundevent"
SEED = 11
STATE = {}


def on_disk(ed: Editor) -> dict:
    return json.loads((ed.project / EVENT).read_text())


async def test_the_group_hides_it_until_enabled(ed: Editor):
    listed = [t.name for t in await ed.tools(groups=None)]
    ed.check("edit_sound_event" not in listed, "not in a new session's list")
    await ed.call("tool_groups", enable=["audio"])
    ed.check_in("edit_sound_event", [t.name for t in await ed.tools()], "the audio group lists it")


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/loud.wav", seconds=0.6, amplitude=0.8, frequency=800)
    write_tone(ed.project / "sounds/quiet.wav", seconds=0.6, amplitude=0.05, frequency=500)
    write_tone(ed.project / "sounds/mid.wav", seconds=0.6, amplitude=0.3, frequency=300)
    await ed.call("sync_files")
    await ed.call("set_project_settings", values={"audio.seed": SEED})


async def test_create_and_refuse_a_second_create(ed: Editor):
    made = await ed.call("edit_sound_event", path=EVENT, create=True, values={
        "mode": "random", "clips": ["sounds/loud.wav", {"clip": "sounds/quiet.wav", "weight": 2}, "sounds/mid.wav"],
        "pitch_semitones": [-1, 1], "volume_db": [-3, 0]})
    STATE["guid"] = made["guid"]
    ed.check(len(made["guid"]) == 32, f"its GUID: {made}")
    ed.check((ed.project / (EVENT + ".meta")).is_file(), "the .meta")
    ed.check(made["guid"] not in (ed.project / EVENT).read_text(), "no GUID in the file")
    body = on_disk(ed)
    ed.check_eq(body["format"], "jasperengine.soundevent", "format")
    ed.check_eq(body["clips"][1]["weight"], 2, "the weight")
    ed.check(body["clips"][0]["clip"]["guid"], f"a clip's GUID: {body}")
    await ed.check_error("edit_sound_event", path=EVENT, create=True, contains="without `create`")
    await ed.check_error("edit_sound_event", path="events/nothing.soundevent", contains="create: true")


async def test_values_null_reset_and_errors(ed: Editor):
    await ed.call("edit_sound_event", path=EVENT, values={"cooldown_ms": 40, "max_instances": 3, "steal": "newest"})
    body = on_disk(ed)
    ed.check((body["cooldown_ms"], body["max_instances"], body["steal"]) == (40, 3, "newest"), f"{body}")
    await ed.call("edit_sound_event", path=EVENT, values={"cooldown_ms": None, "steal": None, "max_instances": None})
    body = on_disk(ed)
    ed.check(not {"cooldown_ms", "steal", "max_instances"} & set(body), f"null resets: {body}")
    await ed.check_error("edit_sound_event", path=EVENT, values={"nonsense": 1}, contains="Keys:")
    await ed.check_error("edit_sound_event", path=EVENT, values={"priority": 999}, contains="priority")
    await ed.check_error("edit_sound_event", path=EVENT, values={"mode": "loud"}, contains="shuffle")


async def test_one_undo_step_per_call(ed: Editor):
    before = (ed.project / EVENT).read_text()
    await ed.call("edit_sound_event", path=EVENT, values={"loop": True, "priority": 10, "cooldown_ms": 5})
    ed.check((ed.project / EVENT).read_text() != before, "written")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / EVENT).read_text(), before, "one undo puts it all back")
    # nothing to change: no step
    same = await ed.call_raw("edit_sound_event", {"path": EVENT, "values": {"loop": False}})
    ed.check("Unchanged" in same.text, f"{same.text}")
    ed.check_eq((ed.project / EVENT).read_text(), before, "unchanged")


async def test_problems(ed: Editor):
    bad = "events/bad.soundevent"
    made = await ed.call("edit_sound_event", path=bad, create=True, values={
        "mode": "shuffle", "clips": ["sounds/gone.wav", "sounds/mid.wav"], "output_group": "Nowhere"})
    text = json.dumps(made["problems"])
    ed.check("missing clip sounds/gone.wav" in text, f"a missing clip: {text}")
    ed.check("Nowhere" in text, f"an unknown group: {text}")
    info = await ed.call("get_asset_info", path=bad)
    ed.check_eq(info["sound_event"]["mode"], "shuffle", f"{info}")
    ed.check("missing clip" in json.dumps(info["sound_event"]["problems"]), f"{info}")

    spread = await ed.call("edit_sound_event", path=EVENT)
    ed.check("loudness spread" in json.dumps(spread["problems"]), f"the loudness spread: {spread['problems']}")


async def test_match_levels_brings_the_clips_within_1_lu(ed: Editor):
    matched = await ed.call("edit_sound_event", path=EVENT, match_levels=True)
    ed.check("loudness spread" not in json.dumps(matched["problems"]), f"the spread is gone: {matched['problems']}")
    volumes = [c["volume_db"] for c in matched["values"]["clips"]]
    ed.check(volumes[1] > 0 > volumes[0], f"the quiet up, the loud down: {volumes}")
    await ed.call("undo", scope="project")
    await ed.check_error("edit_sound_event", path="events/layered.soundevent", create=True, match_levels=True,
                         values={"mode": "layer", "clips": ["sounds/mid.wav"]}, contains="layer")
    ed.check(not (ed.project / "events/layered.soundevent").exists(), "a refused match writes nothing")


async def test_audition_equals_the_next_play_picks(ed: Editor):
    audition = await ed.call("edit_sound_event", path=EVENT, audition=8)
    renditions = audition["renditions"]
    ed.check_eq(len(renditions), 8, f"{audition}")
    ed.check_eq(audition["seed"], SEED, "audio.seed")
    ed.check(all(r["lufs"] < 0 and r["peak_dbtp"] <= 0.5 for r in renditions), f"{renditions}")
    picks = [r["picks"][0]["variation"] for r in renditions]

    reference = EventState(Event.from_file(on_disk(ed), STATE["guid"]), SEED).plays_n(8)
    ed.check_eq(picks, [p.clips[0] for p in reference], "the Python reference")

    scene = (await ed.call("create_scene", name="E", type="3D", path="scenes/E.scene"))["scene"]["id"]
    await ed.call("save_scene", scene=scene)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await ed.console_since(limit=500)
        for _ in range(8):
            await ed.call("eval_lua", code=f"Audio.play('{EVENT}'); return true")
            await ed.call("wait", frames=1)
        played = [l["variation"] for l in voice_lines(await ed.console_since(limit=500)) if l["type"] == "voice_start"]
        ed.check_eq(played, picks, "the next 8 Play picks")

        # during Play the next plays use an edit
        await ed.call("edit_sound_event", path=EVENT, values={"mode": "sequence"})
        await ed.console_since(limit=500)
        await ed.call("eval_lua", code=f"Audio.play('{EVENT}'); return true")
        line = voice_lines(await ed.console_since(limit=500))[-1]
        ed.check_eq(line["variation"], 0, f"the new mode's first: {line}")
    finally:
        await ed.call("play", action="stop")
