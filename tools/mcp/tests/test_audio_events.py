"""Sound Events playing in the editor's Play (docs/specs/S37-sound-events.md R6-R11; AC3, AC4, AC5),
read through the voice lines of JASPERENGINE_AUDIO_TRACE on the null audio driver: picks equal to the
Python reference's under audio.seed, ranges, limits in their order, set_properties by path and GUID,
an unresolved event, clip variation repeating under its seed. The unit tests
(editor/tests/audio/SoundEventRuntimeTest.cpp) cover the sample boundary and each policy.
"""

import json

from audio_refs import TRACE_ENV, Event, EventState, voice_lines, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Events", "env": TRACE_ENV}
CI = True

SEED = 7
STATE = {}


async def lines_since(ed: Editor) -> list[dict]:
    return voice_lines(await ed.console_since(limit=500))


async def write_event(ed: Editor, name: str, **values) -> str:
    path = f"events/{name}.soundevent"
    clips = values.pop("clips", [{"clip": {"path": f"sounds/c{i}.wav"}} for i in range(4)])
    body = {"format": "jasperengine.soundevent", "version": 1, "mode": values.pop("mode", "shuffle"), "clips": clips, **values}
    await ed.call("write_file", path=path, content=json.dumps(body, indent=2))
    return path


async def play_scene(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    await ed.call("wait", frames=2)
    await ed.console_since(limit=500)


async def stop(ed: Editor):
    await ed.call("play", action="stop")


async def test_setup(ed: Editor):
    for i in range(4):
        write_tone(ed.project / f"sounds/c{i}.wav", seconds=0.2, frequency=400 + 200 * i)
    await ed.call("sync_files")
    await ed.call("set_project_settings", values={"audio.seed": SEED})
    ed.check_eq((await ed.call("get_project_settings", keys=["audio.seed"]))["settings"]["audio.seed"], SEED, "audio.seed")
    STATE["scene"] = (await ed.call("create_scene", name="Events", type="3D", path="scenes/Events.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=STATE["scene"], name="Source", type="audio_source")
    await ed.call("save_scene", scene=STATE["scene"])


async def test_set_properties_by_path_and_guid(ed: Editor):
    path = await write_event(ed, "steps")
    guid = (await ed.call("resolve_asset", asset=path))["guid"]
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"event": path}})
    got = await ed.call("get_entity", scene=STATE["scene"], entity="Source", components=["audiosource"], properties=["event"])
    ed.check_eq(got["components"]["audiosource"]["event"], path, "by path")
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"event": ""}})
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"event": guid}})
    got = await ed.call("get_entity", scene=STATE["scene"], entity="Source", components=["audiosource"], properties=["event"])
    ed.check_eq(got["components"]["audiosource"]["event"], path, "by GUID")
    await ed.check_error("set_properties", scene=STATE["scene"], entity="Source",
                         values={"audiosource": {"event": "sounds/c0.wav"}}, contains="edit_sound_event")

    # the schema and the flags' names
    types = await ed.call("list_component_types", component="audiosource")
    ed.check("playOn" in json.dumps(types) and "collision_3d" in json.dumps(types), f"{str(types)[:400]}")
    await ed.call("set_properties", scene=STATE["scene"], entity="Source",
                  values={"audiosource": {"playOn": ["ui_click", "collision_3d"], "minImpactSpeed": 1.5}})
    got = await ed.call("get_entity", scene=STATE["scene"], entity="Source", components=["audiosource"], properties=["playOn", "minImpactSpeed"])
    ed.check_eq(got["components"]["audiosource"]["playOn"], ["ui_click", "collision_3d"], "playOn names")
    await ed.check_error("set_properties", scene=STATE["scene"], entity="Source",
                         values={"audiosource": {"playOn": ["ui_tickle"]}}, contains="ui_click")
    await ed.call("save_scene", scene=STATE["scene"])
    text = (ed.project / "scenes/Events.scene").read_text()
    ed.check("ui_click" in text and "collision_3d" in text, f"the scene has names: {text}")
    await ed.call("set_properties", scene=STATE["scene"], entity="Source",
                  values={"audiosource": {"playOn": [], "minImpactSpeed": 0, "event": ""}})
    await ed.call("save_scene", scene=STATE["scene"])
    text = (ed.project / "scenes/Events.scene").read_text()
    for key in ("event", "playOn", "minImpactSpeed", "fullVolumeImpactSpeed", "pitchRandomSemitones", "volumeRandomDb"):
        ed.check(f"{key}:" not in text, f"{key} is left out at its default: {text}")


async def test_picks_equal_the_python_reference_under_the_seed(ed: Editor):
    path = await write_event(ed, "ranged", mode="random", pitch_semitones=[-2, 2], volume_db=[-6, 0], delay_ms=[0, 20])
    guid = (await ed.call("resolve_asset", asset=path))["guid"]
    await play_scene(ed)
    try:
        for _ in range(12):
            await ed.call("eval_lua", code=f"Audio.play('{path}'); return true")
            await ed.call("wait", frames=1)
        lines = [l for l in await lines_since(ed) if l["type"] == "voice_start"]
        ed.check_eq(len(lines), 12, f"{lines}")
        state = EventState(Event.from_file(json.loads((ed.project / path).read_text()), guid), SEED)
        for line, expected in zip(lines, state.plays_n(12)):
            ed.check_eq(line["event"], guid, "the GUID on the line")
            ed.check_eq(line["event_path"], path, "its path")
            ed.check_eq(line["variation"], expected.clips[0], f"pick of play {expected.index}")
            ed.check(abs(line["pitch"] - 2.0 ** (expected.pitch / 12.0)) < 1e-4, f"pitch of play {expected.index}: {line} {expected}")
            ed.check(abs(line["gain"] - 10.0 ** (expected.volume_db / 20.0)) < 1e-4, f"gain of play {expected.index}: {line} {expected}")
    finally:
        await stop(ed)


async def test_shuffle_over_four_clips_eight_plays(ed: Editor):
    path = await write_event(ed, "bag")
    await play_scene(ed)
    try:
        for _ in range(8):
            await ed.call("eval_lua", code=f"Audio.play('{path}'); return true")
            await ed.call("wait", frames=1)
        picks = [l["variation"] for l in await lines_since(ed) if l["type"] == "voice_start"]
        ed.check_eq(sorted(picks), [0, 0, 1, 1, 2, 2, 3, 3], f"each twice: {picks}")
        ed.check(all(a != b for a, b in zip(picks, picks[1:])), f"none back to back: {picks}")
    finally:
        await stop(ed)


async def test_max_instances_steals_before_the_start(ed: Editor):
    path = await write_event(ed, "limited", mode="sequence", max_instances=2,
                             clips=[{"clip": {"path": "sounds/c0.wav"}}])
    await play_scene(ed)
    try:
        for _ in range(3):
            await ed.call("eval_lua", code=f"Audio.play('{path}'); return true")
        lines = await lines_since(ed)
        kinds = [(l["type"], l.get("reason", "")) for l in lines]
        ed.check_eq(kinds, [("voice_start", ""), ("voice_start", ""), ("voice_stop", "stolen_instance"), ("voice_start", "")],
                    f"the steal comes before the third's start: {lines}")
        ed.check_eq(lines[2]["by_instance"], lines[3]["instance"], "by the new instance")
    finally:
        await stop(ed)


async def test_limits_compose(ed: Editor):
    """AC4: max_instances 4, a source's maxVoices 3, audio.max_voices 2: the budget acts first here, then the cap, in D3's order"""
    path = await write_event(ed, "composed", mode="sequence", max_instances=4)
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"maxVoices": 3}})
    await ed.call("set_project_settings", values={"audio.max_voices": 2})
    await play_scene(ed)
    try:
        await ed.call("eval_lua", code=f"local s = object('Source', 'AudioSource'); for i = 1, 5 do s:playOneShot('{path}') end; return true")
        await ed.call("wait", frames=2)
        lines = await lines_since(ed)
        stops = [l["reason"] for l in lines if l["type"] == "voice_stop"]
        ed.check(len([l for l in lines if l["type"] == "voice_start"]) == 5, f"every play starts a voice: {lines}")
        ed.check("stolen_budget" in stops, f"the global budget of 2 stole: {lines}")
        ed.check(all(r in ("stolen_budget", "stolen_source") for r in stops), f"{stops}")
        voices = (await ed.call("eval_lua", code="return AudioSystem.getVoiceCount()"))["values"][0]
        ed.check(voices <= 2, f"the budget holds: {voices}")
    finally:
        await stop(ed)
        await ed.call("set_project_settings", values={"audio.max_voices": 256})
        await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"maxVoices": 8}})


async def test_an_unresolved_event_plays_nothing_with_one_warning(ed: Editor):
    path = await write_event(ed, "gone", mode="sequence", clips=[{"clip": {"path": "sounds/c0.wav"}}])
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"event": path}})
    await ed.call("save_scene", scene=STATE["scene"])
    (ed.project / path).unlink()
    await ed.call("sync_files")
    await play_scene(ed)
    try:
        await ed.call("eval_lua", code="local s = object('Source', 'AudioSource'); s:play(); return true")
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="local s = object('Source', 'AudioSource'); s:play(); return s:isPlaying()")
        await ed.call("wait", frames=2)
        entries = await ed.console_since(limit=500)
        lines = voice_lines(entries)
        ed.check_eq([l["type"] for l in lines], ["voice_reject", "voice_reject"], f"{lines}")
        ed.check_eq(lines[0]["reason"], "missing_event", f"{lines}")
        warnings = [e for e in entries if "does not resolve" in e.get("message", "")]
        ed.check_eq(len(warnings), 1, f"one warning: {warnings}")
    finally:
        await stop(ed)
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"event": ""}})


async def test_clip_variation_in_range_and_repeating(ed: Editor):
    await ed.call("set_properties", scene=STATE["scene"], entity="Source",
                  values={"audiosource": {"filename": "sounds/c0.wav", "pitchRandomSemitones": 3, "volumeRandomDb": 6}})
    await ed.call("save_scene", scene=STATE["scene"])

    async def run_once():
        await play_scene(ed)
        try:
            await ed.call("eval_lua", code="local s = object('Source', 'AudioSource'); for i = 1, 6 do s:playOneShot('sounds/c1.wav') end; return true")
            await ed.call("wait", frames=2)
            return [(l["pitch"], l["gain"]) for l in await lines_since(ed) if l["type"] == "voice_start"]
        finally:
            await stop(ed)

    first = await run_once()
    ed.check_eq(len(first), 6, f"{first}")
    for pitch, gain in first:
        ed.check(2 ** (-3 / 12) - 1e-3 <= pitch <= 2 ** (3 / 12) + 1e-3, f"pitch {pitch}")
        ed.check(10 ** (-6 / 20) - 1e-3 <= gain <= 1 + 1e-3, f"gain {gain} is never louder")
    ed.check(len({p for p, _ in first}) > 1, f"it varies: {first}")
    second = await run_once()
    ed.check_eq(second, first, "the same seed gives the same draws")


async def test_a_scene_resaves_byte_identical(ed: Editor):
    """AC5: the new fields are left out at their defaults, so a scene saved before them saves the same"""
    scene = STATE["scene"]
    await ed.call("set_properties", scene=scene, entity="Source",
                  values={"audiosource": {"filename": "", "pitchRandomSemitones": 0, "volumeRandomDb": 0}})
    await ed.call("save_scene", scene=scene)
    before = (ed.project / "scenes/Events.scene").read_bytes()
    for key in (b"event", b"playOn", b"minImpactSpeed", b"fullVolumeImpactSpeed", b"pitchRandomSemitones", b"volumeRandomDb"):
        ed.check(key + b":" not in before, f"{key} is written at its default")
    await ed.call("close_scene", scene=scene, unsaved="discard")
    STATE["scene"] = (await ed.call("open_scene", scene="scenes/Events.scene"))["scene"]["id"]
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"volume": 0.5}})
    await ed.call("set_properties", scene=STATE["scene"], entity="Source", values={"audiosource": {"volume": 1}})
    await ed.call("save_scene", scene=STATE["scene"])
    ed.check_eq((ed.project / "scenes/Events.scene").read_bytes(), before, "byte-identical")
