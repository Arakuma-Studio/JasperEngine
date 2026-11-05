"""audio_session status, arm and report, and the play stop summary (docs/specs/S35-agent-audio.md R19, R20, R22; AC9).
Loudness against pyloudnorm on the same master.wav; the numbering against the image's metadata; the image decoded here.
"""

import pngdiff
from audio_refs import capture_sessions, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Session"}
CI = True
TIMEOUT = 900

STATE = {}

STEPS = """---@class Steps : ScriptInstance
local Steps = { properties = {} }

function Steps:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Steps:onUpdate()
    self.frame = self.frame + 1
    if self.frame % 60 == 0 then
        Audio.mark("step " .. self.frame)
        Audio.play("sounds/tone.wav", { group = "Music" })
    end
end

return Steps
"""


async def test_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("audio_session" not in default, "hidden until audio is enabled")
    await ed.call("tool_groups", enable=["audio"])
    ed.check("audio_session" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")


async def test_status_without_sessions(ed: Editor):
    status = await ed.call("audio_session", action="status")
    ed.check_eq(status["sessions"], [], "no sessions yet")
    ed.check_eq(status["capturing"], False, f"{status}")
    ed.check_eq(status["armed"], None, f"{status}")


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/tone.wav", seconds=0.6, frequency=1000.0, amplitude=0.5, rate=48000)
    await ed.call("sync_files")
    await ed.call("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "Music"}])
    STATE["scene"] = (await ed.call("create_scene", name="Steps", type="2D", path="scenes/Steps.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=STATE["scene"], type="empty", name="Stepper")
    await ed.call("create_script", kind="lua", name="Steps", content=STEPS, entity="Stepper", scene=STATE["scene"])
    await ed.call("save_scene", scene=STATE["scene"])


async def test_arm_false_then_no_session(ed: Editor):
    armed = await ed.call("audio_session", action="arm", capture=False)
    ed.check_eq(armed["armed"]["capture"], False, f"{armed}")
    ed.check_eq((await ed.call("audio_session", action="status"))["armed"]["capture"], False, "status says so")
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60)
    await ed.call("wait", frames=3)
    stopped = await ed.call("play", action="stop")
    ed.check_eq(capture_sessions(ed.project), [], "no session")
    ed.check("audio" not in stopped, f"no audio in the stop result: {stopped}")
    ed.check_eq((await ed.call("audio_session", action="status"))["armed"], None, "an arm is used once")


async def test_no_device_captures_and_stop_says_so(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60)
    await ed.call("wait", frames=5)
    live = await ed.call("audio_session", action="status")
    ed.check_eq((live["capturing"], live["output_mode"]), (True, "deterministic"), f"{live}")
    ed.check_eq(live["sessions"][0]["state"], "capturing", f"{live}")
    await ed.call("wait", frames=400)
    current = await ed.call("audio_session", action="report", session="current", image=False)
    ed.check_eq(current["capture"]["state"], "capturing", "a live session reads")
    ed.check(current["voices"]["started"] >= 1, f"{current['voices']}")
    r = await ed.call_raw("play", {"action": "stop"})
    audio = r.structured["audio"]
    for key in ("session", "duration_s", "integrated_lufs", "momentary_max", "true_peak_dbtp", "voices_started", "voices_peak", "markers", "warnings", "next"):
        ed.check(key in audio, f"{key} in {audio}")
    ed.check_eq(audio["next"], "audio_session report", f"{audio}")
    ed.check(audio["voices_started"] >= 6, f"{audio}")
    ed.check("Audio capture" in r.text, f"one text line: {r.text}")
    STATE["session"] = audio["session"]
    status = await ed.call("audio_session", action="status")
    ed.check_eq(status["sessions"][0]["session"], audio["session"], "newest first")
    ed.check_eq(status["sessions"][0]["state"], "complete", f"{status['sessions'][0]}")
    ed.check(status["sessions"][0]["markers"] >= 6, f"{status['sessions'][0]}")


async def test_report_numbers_and_loudness(ed: Editor):
    report = await ed.call("audio_session", action="report", image=False)
    ed.check_eq(report["capture"]["session"], STATE["session"], "last is the newest")
    ed.check_eq(report["voices"]["by_clip"][0]["clip"], "sounds/tone.wav", f"{report['voices']}")
    ed.check(report["groups"]["Music"]["peak_db"] > -12, f"{report['groups']}")
    ed.check(report["groups"]["Music"]["voices_peak"] >= 1, f"{report['groups']}")
    ids = [e["id"] for e in report["events"]]
    ed.check_eq(ids, [f"E{i + 1}" for i in range(len(ids))], "E<n> in order")
    mids = [m["id"] for m in report["markers"]]
    ed.check_eq(mids, [f"M{i + 1}" for i in range(len(mids))], "M<n> in order")
    ed.check("Report these numbers" in report["guidance"], f"{report['guidance']}")

    import audio_refs
    master = capture_sessions(ed.project)[-1] / "master.wav"
    reference = audio_refs.pyloudnorm_integrated(master)
    ed.check(abs(report["loudness"]["integrated_lufs"] - reference) <= 0.1, f"{report['loudness']['integrated_lufs']} vs pyloudnorm {reference}")

    limited = await ed.call("audio_session", action="report", image=False, events_limit=2)
    ed.check_eq(len(limited["events"]), 2, "events_limit")
    ed.check_eq(limited["events_truncated"], True, f"{limited['events_total']}")
    ranged = await ed.call("audio_session", action="report", image=False, start=2.0, end=4.0)
    ed.check(all(2.0 <= e["t"] < 4.0 for e in ranged["events"]), f"{ranged['events']}")
    ed.check(ranged["events"][0]["id"] != "E1", "a moment keeps its number whatever the range")
    by_path = await ed.call("audio_session", action="report", image=False, session=str(capture_sessions(ed.project)[-1]))
    ed.check_eq(by_path["capture"]["session"], STATE["session"], "a session directory reads too")
    missing = await ed.call("audio_session", action="report", session="20000101-000000-00", expect_error=True)
    ed.check(STATE["session"] in missing, f"the error names what there is: {missing}")


async def test_image_lanes(ed: Editor):
    width = 800
    r = await ed.call_raw("audio_session", {"action": "report", "max_width": width})
    ed.check(r.images, "an image")
    png, mime = r.images[0]
    ed.check_eq(mime, "image/png", "PNG")
    w, h, rgba = pngdiff.decode(png)
    meta = r.structured["image"]
    ed.check_eq((w, h), (width, meta["height"]), "the requested width")
    names = [lane["name"] for lane in meta["lanes"]]
    ed.check_eq(names, ["title", "waveform", "spectrogram", "loudness", "group:Master", "group:Music", "markers", "events", "time"], "the lanes")
    ed.check_eq(sum(lane["height"] for lane in meta["lanes"]), h, "they fill the image")
    for lane in meta["lanes"]:
        if lane["name"] == "title":
            continue
        rows = range(lane["y"], lane["y"] + lane["height"])
        first = rgba[lane["y"] * w * 4:lane["y"] * w * 4 + 3]
        different = sum(1 for y in rows for x in range(w) if rgba[(y * w + x) * 4:(y * w + x) * 4 + 3] != first)
        ed.check(different >= 0.02 * w * lane["height"], f"lane {lane['name']}: {different} of {w * lane['height']} pixels are not its background")
    ed.check(meta["marker_labels"] + meta["hidden_labels"] > 0, f"{meta}")
    ed.check(meta["event_labels"] > 0, f"{meta}")
    report = r.structured
    drawn = meta["event_labels"] + meta["marker_labels"] + meta["hidden_labels"]
    voice_lines = sum(1 for e in report["events"] if e["kind"] in ("voice_start", "voice_stop"))
    ed.check_eq(drawn, voice_lines + len(report["markers"]), "every numbered moment is on a lane or counted as hidden")


async def test_report_is_read_only(ed: Editor):
    tool = ed.tool("audio_session")
    ed.check(tool.input_schema["properties"]["expect"]["items"]["additionalProperties"] is False, "expect items are closed")
