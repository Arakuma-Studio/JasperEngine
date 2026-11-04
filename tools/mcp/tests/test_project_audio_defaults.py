"""New projects ship at a sane level, old ones do not change (S38 R8-R10, R12, R13; AC5, AC6): what a
new project has on disk, the Master readouts agents read in Play, the ceiling holding on a hot signal,
the output stage applying, and existing projects keeping no mixer through a save.

S35's capture is not on main yet: the true peak and the loudness are the Master tap's, and
AudioMixerRuntimeTest.ANewProjectHoldsItsCeilingAtTheOutput proves in the unit tests that under the
linear stage they are what leaves the device.
"""

import json
import math
import os
import shutil
import struct
from pathlib import Path

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Audio Defaults"}
CI = True
TIMEOUT = 900

MIXER = "audio/Main.mixer"
FIXTURES = Path(__file__).resolve().parents[1].parent / "fixtures"


def write_tone(path, seconds=2.0, frequency=1000.0, amplitude=0.5, rate=48000):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = b"".join(struct.pack("<h", int(round(amplitude * 32767 * math.sin(2 * math.pi * frequency * i / rate))))
                    for i in range(int(seconds * rate)))
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    path.write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


async def test_a_new_project_has_the_mixer_and_the_keys(ed: Editor):
    await ed.call("save_project", untitled_scene_dir="scenes")
    mixer = json.loads((ed.project / MIXER).read_text())
    master = mixer["groups"][0]
    ed.check(len(master.pop("id")) == 8, f"an id: {master}")
    for effect in master["effects"]:
        ed.check(len(effect.pop("id")) == 8, f"an id: {effect}")
    ed.check_eq(mixer, {
        "format": "jasperengine.mixer", "version": 2, "updateMode": "scaled",
        "groups": [{"name": "Master", "parent": None, "volume": 0,
                    "effects": [{"type": "limiter",
                                 "params": {"threshold": -1, "release": 0.1, "lookahead": 1.5, "truePeak": 1}}]}],
    }, "the mixer of S38 R8")

    audio = yaml.safe_load((ed.project / "project.yaml").read_text())["audio"]
    ed.check_eq(audio, {"mixer": MIXER, "outputStage": "linear", "iosSessionCategory": "ambient"}, "project.yaml's audio node")

    settings = (await ed.call("get_project_settings", keys=[
        "audio.mixer", "audio.output_stage", "audio.ios_session_category", "audio.loudness_target_lufs",
        "audio.loudness_tolerance_lu", "audio.true_peak_ceiling_dbtp", "audio.max_voices"]))["settings"]
    ed.check_eq(settings["audio.output_stage"], "linear", "output stage")
    ed.check_eq(settings["audio.ios_session_category"], "ambient", "iOS session")
    ed.check_eq(settings["audio.loudness_target_lufs"], -18, "the target is the registry's default")
    ed.check_eq(settings["audio.loudness_tolerance_lu"], 2, "tolerance")
    ed.check_eq(settings["audio.true_peak_ceiling_dbtp"], -1, "ceiling")
    ed.check_eq(settings["audio.max_voices"], 256, "voices stay upstream's")

    described = await ed.call("audio_mixer", action="get")
    ed.check_eq(described["groups"][0]["latency_ms"], 1.5, "the Master limiter's latency")


async def test_settings_are_validated(ed: Editor):
    await ed.check_error("set_project_settings", values={"audio.output_stage": "loud"}, contains="roundoff")
    await ed.check_error("set_project_settings", values={"audio.ios_session_category": "record"}, contains="ambient")
    await ed.check_error("set_project_settings", values={"audio.loudness_target_lufs": -3}, contains="-40")
    await ed.check_error("set_project_settings", values={"audio.true_peak_ceiling_dbtp": 3}, contains="-12")
    await ed.check_error("set_project_settings", values={"audio.loudness_tolerance_lu": 0.1}, contains="0.5")


async def test_loudness_readouts(ed: Editor):
    write_tone(ed.project / "tone.wav")
    await ed.call("create_entity", name="Tone", type="audio_source")
    await ed.call("set_properties", entity="Tone",
                  values={"audiosource": {"filename": "tone.wav", "looping": True, "state": "playing"}})
    await ed.call("play", action="start", auto_pause=False)
    try:
        first = (await ed.call("audio_mixer", action="get"))["loudness"]
        ed.check_eq(first["target_lufs"], -18, "the target")
        await ed.call("wait", seconds=1.5)
        data = await ed.call("audio_mixer", action="get")
        loudness = data["loudness"]
        # a stereo 1 kHz tone reads as loud in LUFS as its peak in dBFS (EBU Tech 3341)
        level = data["groups"][0]["meter_db"]
        ed.check(level > -30, f"it is heard: {level}")
        ed.check(abs(loudness["momentary_lufs"] - level) < 0.3, f"LUFS-M {loudness['momentary_lufs']} against {level} dB")
        ed.check(abs(loudness["integrated_lufs"] - level) < 0.3, f"integrated {loudness['integrated_lufs']} against {level} dB")
        ed.check(loudness["short_term_lufs"] is None, f"under 3 s there is no short-term: {loudness}")
        ed.check(abs(loudness["true_peak_dbtp"] - level) < 0.3, f"true peak of a tone at its peak: {loudness}")
        ed.check_eq(loudness["gain_reduction_db"], 0, "under the ceiling nothing is reduced")

        state = await ed.call("get_play_state")
        short = state["audio"]["loudness"]
        ed.check_eq(set(short), {"momentary_lufs", "true_peak_dbtp"}, "get_play_state's results only")
        ed.check(abs(short["momentary_lufs"] - level) < 0.3, f"get_play_state's LUFS-M: {short}")
    finally:
        await ed.call("play", action="stop")


async def test_the_mixer_window_shows_the_readouts(ed: Editor):
    """Master's LUFS against the target and the limiter's gain reduction, in Play (evidence: the shot)."""
    await ed.call("play", action="start", auto_pause=False)
    try:
        await ed.call("run_command", id="window.audioMixer")
        await ed.call("wait", seconds=1.0)
        evidence = os.environ.get("JASPER_EVIDENCE_DIR")
        image = await ed.screenshot(view="editor", path=Path(evidence) / "s38-mixer-window.png" if evidence else None, max_width=0)
        ed.check(len(image) > 1000, f"an image of the editor: {len(image)} bytes")
    finally:
        await ed.call("play", action="stop")


async def test_the_ceiling_holds_on_a_hot_signal(ed: Editor):
    await ed.call("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "Hot"},
                                                     {"op": "set_group", "name": "Hot", "volume": 18}])
    await ed.call("set_properties", entity="Tone", values={"audiosource": {"outputGroup": "Hot"}})
    await ed.call("play", action="start", auto_pause=False)
    try:
        await ed.call("audio_mixer", action="get")
        await ed.call("wait", seconds=1.0)
        loudness = (await ed.call("audio_mixer", action="get"))["loudness"]
        ed.check(loudness["true_peak_dbtp"] <= -0.9, f"the ceiling: {loudness}")
        ed.check(loudness["true_peak_dbtp"] > -3, f"held at the ceiling, not under it: {loudness}")
        ed.check(loudness["gain_reduction_db"] > 3, f"the limiter works: {loudness}")
    finally:
        await ed.call("play", action="stop")


async def test_the_output_stage_applies(ed: Editor):
    for stage in ("roundoff", "linear"):
        await ed.call("set_project_settings", values={"audio.output_stage": stage})
        await ed.call("play", action="start", auto_pause=False)
        await ed.call("wait", seconds=0.3)
        await ed.call("play", action="stop")
        console = await ed.call("read_console", limit=200)
        lines = [e["message"] for e in console["entries"] if "Audio output stage" in e["message"]]
        ed.check(lines and stage in lines[-1], f"{stage}: {lines[-3:]}")


async def test_existing_projects_stay_as_they_are(ed: Editor):
    """Opened and saved, a project without the keys keeps no mixer, roundoff and the default session."""
    checked = 0
    for fixture in sorted(p for p in FIXTURES.iterdir() if (p / "project.yaml").is_file()):
        before = yaml.safe_load((fixture / "project.yaml").read_text())
        if "audio" in before:
            continue
        copy = ed.scratch.root / ("existing-" + fixture.name)
        shutil.copytree(fixture, copy)
        async with Editor(project=str(copy), label="existing") as other:
            settings = (await other.call("get_project_settings", keys=[
                "audio.mixer", "audio.output_stage", "audio.ios_session_category"]))["settings"]
            ed.check_eq(settings["audio.mixer"], "", f"{fixture.name}: no mixer")
            ed.check_eq(settings["audio.output_stage"], "roundoff", f"{fixture.name}: roundoff")
            ed.check_eq(settings["audio.ios_session_category"], "default", f"{fixture.name}: the default session")
            await other.call("save_project", untitled_scene_dir="scenes")
        after = yaml.safe_load((copy / "project.yaml").read_text())
        ed.check("audio" not in after, f"{fixture.name}: no audio node: {after.get('audio')}")
        ed.check(not list(copy.rglob("*.mixer")), f"{fixture.name}: no mixer file")
        checked += 1
    ed.check(checked >= 10, f"the fixture projects were found: {checked}")
