"""Audio mixer (spec docs/superpowers/specs/2026-10-31-audio-mixer-design.md) end to end: audio_mixer edit
builds the project's mixer (Master / Music / SFX, a low-pass and a high-pass with exposed cutoffs, a
remembered MusicVolume, two snapshots) in one undoable call, set_properties routes a looping source to
Music by name, and in Play the meters prove it is heard on a machine without a sound card (the engine
mixes without output): setFloat mutes it, a snapshot transition moves the exposed cutoff over time, the
remembered volume comes back on the next Play. Also get_play_state's audio block, the project settings
keys and the undo of an edit.
"""

import math
import struct

from harness import Editor

EDITOR = {"create": True, "name": "Audio Mixer"}
CI = True
TIMEOUT = 900

MIXER = "audio/Main.mixer"   # a new project's assets folder is the project folder


def write_tone(path, seconds=2.0, frequency=1000.0, amplitude=0.5, rate=44100):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = int(seconds * rate)
    data = b"".join(struct.pack("<h", int(round(amplitude * 32767 * math.sin(2 * math.pi * frequency * i / rate))))
                    for i in range(samples))
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    path.write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


def audio_props(entity: dict) -> dict:
    comps = entity["components"]
    return comps[next(k for k in comps if "audio" in k.lower())]


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def meter(ed: Editor, group: str) -> float:
    return (await lua(ed, f"return AudioMixer.getMeter('{group}')"))[0]


async def start(ed: Editor) -> None:
    await ed.call("play", action="start", auto_pause=False)
    await ed.call("wait", seconds=0.5)


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop")


async def test_setup_and_build_the_mixer(ed: Editor):
    write_tone(ed.project / "music.wav")
    await ed.call("create_entity", name="Music", type="audio_source")
    await ed.call("set_properties", entity="Music",
                  values={"audiosource": {"filename": "music.wav", "looping": True, "state": "playing"}})

    got = await ed.call_raw("audio_mixer", {"action": "get"})
    ed.check(not got.is_error and "implicit Master" in got.text, f"no mixer yet: {got.text[:300]}")

    r = await ed.call_raw("audio_mixer", {"action": "edit", "ops": [
        {"op": "add_group", "name": "Music"},
        {"op": "add_group", "name": "SFX"},
        {"op": "add_effect", "group": "Music", "type": "lowpass", "params": {"cutoff": 22000}},
        {"op": "add_effect", "group": "Music", "type": "highpass", "params": {"cutoff": 10}},
        {"op": "expose", "name": "MusicVolume", "group": "Music", "volume": True, "remember": True},
        {"op": "expose", "name": "lowPassCutoff", "group": "Music", "param": "cutoff", "effect": "lowpass"},
        {"op": "expose", "name": "highPassCutoff", "group": "Music", "param": "cutoff", "effect": "highpass"},
        {"op": "add_snapshot", "name": "Menu"},
        {"op": "add_snapshot", "name": "Gameplay", "values": {"lowPassCutoff": 525, "highPassCutoff": 430}},
        {"op": "set_start_snapshot", "name": "Menu"},
    ]})
    ed.check(not r.is_error, f"edit: {r.text[:500]}")
    data = r.value
    ed.check(data.get("created") is True, f"created the project mixer: {data}")
    ed.check_eq(data["mixer"], MIXER, "the default path")
    ed.check_eq([g["name"] for g in data["groups"]], ["Master", "Music", "SFX"], "groups")
    ed.check_eq([e["type"] for e in data["groups"][1]["effects"]], ["lowpass", "highpass"], "effects")
    ed.check_eq([e["name"] for e in data["exposed"]], ["MusicVolume", "lowPassCutoff", "highPassCutoff"], "exposed")
    ed.check_eq(data["start_snapshot"], "Menu", "start snapshot")
    ed.check((ed.project / MIXER).is_file(), "the file is written")

    settings = await ed.call("get_project_settings", keys=["audio.mixer", "audio.max_voices", "audio.sample_rate"])
    ed.check_eq(settings["settings"]["audio.mixer"], MIXER, "the project's mixer setting")
    ed.check_eq(settings["settings"]["audio.max_voices"], 256, "default max voices")
    ed.check_eq(settings["settings"]["audio.sample_rate"], "auto", "default sample rate")

    # mistakes are explained and change nothing
    await ed.check_error("audio_mixer", action="edit", ops=[{"op": "add_effect", "group": "Nope", "type": "lowpass"}],
                         contains="no group 'Nope'")
    await ed.check_error("audio_mixer", action="edit", ops=[{"op": "add_effect", "group": "Music", "type": "wobble"}],
                         contains="lowpass")
    await ed.check_error("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "Music"}], contains="exists")

    # the source by group name; it reports the name
    await ed.call("set_properties", entity="Music", values={"audiosource": {"outputGroup": "Music"}})
    entity = await ed.call("get_entity", entity="Music", components=["audiosource"])
    ed.check_eq(audio_props(entity)["outputGroup"], "Music", f"outputGroup by name: {entity['components']}")
    await ed.call("save_project", untitled_scene_dir="scenes")


async def test_meters_mute_and_transitions_in_play(ed: Editor):
    await start(ed)
    try:
        level = await meter(ed, "Music")
        ed.check(level > -40, f"Music is heard (no sound card needed): {level} dB")
        ed.check(await meter(ed, "SFX") < -79, "nothing plays through SFX")
        state = await ed.call("get_play_state")
        ed.check_in("Music", state["audio"]["mixer_meters"], f"the audio block: {state.get('audio')}")
        ed.check(state["audio"]["voices"] >= 1, f"voices: {state['audio']}")
        live = await ed.call("audio_mixer", action="get")
        music = [g for g in live["groups"] if g["name"] == "Music"][0]
        ed.check(music["meter_db"] > -40, f"live meters in get: {music}")

        # VolumeController: mute by the exposed volume
        ed.check_eq(await lua(ed, "return AudioMixer.setFloat('MusicVolume', -80)"), [True], "setFloat")
        await ed.call("wait", seconds=0.3)
        ed.check(await meter(ed, "Music") < -79, "muted")
        ed.check_eq(await lua(ed, "return AudioMixer.getFloat('MusicVolume')"), [-80], "getFloat")

        # MinigameSongs: the snapshot sweep moves the exposed cutoff over time
        ed.check_eq(await lua(ed, "return AudioMixer.getFloat('lowPassCutoff')"), [22000], "the start snapshot")
        await lua(ed, "AudioMixer.transitionTo('Gameplay', 1.0, {unscaled = true})")
        samples = []
        for _ in range(4):
            await ed.call("wait", seconds=0.2)
            samples.append((await lua(ed, "return AudioMixer.getFloat('lowPassCutoff')"))[0])
        await ed.call("wait", seconds=0.5)
        final = (await lua(ed, "return AudioMixer.getFloat('lowPassCutoff')"))[0]
        ed.check(all(a >= b for a, b in zip(samples, samples[1:])) and samples[0] < 22000, f"monotonic: {samples}")
        ed.check(abs(final - 525) < 1, f"reaches the snapshot: {final}")
        ed.check_eq(await lua(ed, "return AudioMixer.getCurrentSnapshot()"), ["Gameplay"], "current snapshot")
    finally:
        await stop(ed)


async def test_remembered_volume_comes_back_next_play(ed: Editor):
    await start(ed)
    try:
        ed.check_eq(await lua(ed, "return AudioMixer.getFloat('MusicVolume')"), [-80], "remembered from the last Play")
        ed.check_eq(await lua(ed, "return AudioMixer.getFloat('lowPassCutoff')"), [22000], "not remembered: the start snapshot")
        stored = await ed.call("user_settings", action="get", key="audio.mixer.MusicVolume")
        ed.check(stored.get("exists"), f"in UserSettings: {stored}")
        await lua(ed, "AudioMixer.clearFloat('MusicVolume')")
        await ed.call("wait", seconds=0.3)
        ed.check(await meter(ed, "Music") > -40, "heard again")
    finally:
        await stop(ed)
    stored = await ed.call("user_settings", action="get", key="audio.mixer.MusicVolume")
    ed.check(not stored.get("exists"), f"clearFloat forgot it: {stored}")


async def test_an_edit_is_one_undo_step(ed: Editor):
    before = (ed.project / MIXER).read_text()
    r = await ed.call("audio_mixer", action="edit", ops=[
        {"op": "set_group", "name": "Music", "volume": -6, "mute": True},
        {"op": "add_effect", "group": "SFX", "type": "limiter"},
    ])
    ed.check_eq([g.get("mute", False) for g in r["groups"]], [False, True, False], "mute set")
    ed.check((ed.project / MIXER).read_text() != before, "written")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / MIXER).read_text(), before, "undo restores the file")
    got = await ed.call("audio_mixer", action="get")
    ed.check_eq([len(g["effects"]) for g in got["groups"]], [0, 2, 0], "and the mixer the engine plays")


async def test_settings_keys(ed: Editor):
    await ed.call("set_project_settings", values={"audio.max_real_voices": 24, "audio.default_output_group": "SFX"})
    settings = await ed.call("get_project_settings", keys=["audio.max_real_voices", "audio.default_output_group"])
    ed.check_eq(settings["settings"]["audio.max_real_voices"], 24, "max real voices")
    ed.check_eq(settings["settings"]["audio.default_output_group"], "SFX", "default group")
    await ed.check_error("set_project_settings", values={"audio.default_output_group": "Nope"}, contains="no mixer group")
    await ed.check_error("set_project_settings", values={"audio.sample_rate": 12345}, contains="44100")
    # a new source plays through the default group
    await ed.call("create_entity", name="Click", type="audio_source")
    entity = await ed.call("get_entity", entity="Click", components=["audiosource"])
    ed.check_eq(audio_props(entity)["outputGroup"], "SFX", "the default output group")
