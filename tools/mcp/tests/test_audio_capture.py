"""Play's audio capture (docs/specs/S35-agent-audio.md R8, R10-R17; AC4, AC5, AC8). The editor here has
no device, so a Play captures on the deterministic driver: the same frames give the same bytes, every
voice is logged from its start to its stop, sessions are limited and a failure leaves the Play running.

The oracles are independent of the capture's code: frame numbers x 800 samples, the WAV's own samples
read here, and what the scene's scripts say from Lua.
"""

import hashlib
import math
import os
import signal
from pathlib import Path

from audio_refs import capture_sessions, read_float_wav, read_jsonl, read_session, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Capture"}
CI = True
TIMEOUT = 1200

FRAME = 800
STATE = {}

JUMP = """---@class Jump : ScriptInstance
local Jump = { properties = {} }

function Jump:init()
    Audio.mark("init " .. tostring(Audio.isCapturing()))
    Audio.play("sounds/init.wav")
    RegisterEngineEvent(self, "onKeyDown")
end

function Jump:onKeyDown(key, repeated, mods)
    Audio.mark("jump")
    Audio.play("sounds/jump.wav", { volume = 0.5 })
end

return Jump
"""


def digest(path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def editor_pid(project) -> int:
    """The editor that has this project open: no clean-up runs when it is killed."""
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                command = (entry / "cmdline").read_bytes()
            except OSError:
                continue
            if str(project).encode() in command and b"--project" in command or str(project).encode() in command and b"--create" in command:
                return int(entry.name)
    raise AssertionError(f"no editor has {project} open")


async def new_session(ed: Editor, before: list) -> "object":
    after = capture_sessions(ed.project)
    made = [p for p in after if p not in before]
    ed.check_eq(len(made), 1, f"one new session: {before} > {after}")
    return made[0]


async def play_jump(ed: Editor, frames_before_stop: int = 180) -> "object":
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=STATE["jump"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=29)
        await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=60)
        await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=frames_before_stop - 90)
    finally:
        await ed.call("play", action="stop")
    return await new_session(ed, before)


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/init.wav", seconds=0.2, frequency=440.0, rate=48000)
    write_tone(ed.project / "sounds/jump.wav", seconds=0.3, frequency=880.0)
    write_tone(ed.project / "sounds/long.wav", seconds=30.0, frequency=1000.0, amplitude=0.5, rate=48000)
    await ed.call("sync_files")
    STATE["jump"] = (await ed.call("create_scene", name="Jump", type="2D", path="scenes/Jump.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=STATE["jump"], type="empty", name="Player")
    await ed.call("create_script", kind="lua", name="Jump", content=JUMP, entity="Player", scene=STATE["jump"])
    await ed.call("save_scene", scene=STATE["jump"])


async def test_deterministic_twice(ed: Editor):
    first = await play_jump(ed)
    second = await play_jump(ed)
    ed.check(first != second, "two sessions")
    for name in ("master.wav", "events.jsonl", "levels.jsonl"):
        if name.endswith(".jsonl"):
            for a, b in zip((first / name).read_text().splitlines(), (second / name).read_text().splitlines()):
                ed.check_eq(a, b, f"{name}: the same line on both runs")
        ed.check_eq(digest(first / name), digest(second / name), f"{name} is the same bytes on both runs")
    one, two = read_session(first), read_session(second)
    ed.check(one["deterministic"], f"the session says so: {one['not_deterministic']}")
    ed.check_eq(one["fixed_fps"], 60, f"{one}")
    for key in set(one) | set(two):
        if key not in ("session", "started_utc", "ended_utc"):
            ed.check_eq(one.get(key), two.get(key), f"session.json {key}")
    ed.check_eq(one["state"], "complete", f"{one}")
    # the stops of a play at fixed frames: every frame is 800 samples
    ed.check_eq(one["samples"] % FRAME, 0, f"whole frames: {one['samples']}")
    ed.check_eq(one["frames"] * FRAME, one["samples"], f"{one}")
    STATE["session"] = first


async def test_session_lines(ed: Editor):
    directory = STATE["session"]
    lines = read_jsonl(directory / "events.jsonl")
    ed.check_eq(lines[0], {"format": "jasperengine.audio-events", "version": 1}, "the header")
    events = lines[1:]
    ed.check_eq((events[0]["kind"], events[0]["mode"], events[0]["sample"]), ("scene_load", "start", 0), f"first: {events[0]}")
    ed.check_eq([e["n"] for e in events], list(range(1, len(events) + 1)), "numbered from 1")
    samples = [e["sample"] for e in events]
    ed.check_eq(samples, sorted(samples), "sample never decreases")
    for line in events:
        for key in ("kind", "sample", "t", "frame", "game_time"):
            ed.check(key in line, f"{key} in {line}")
        ed.check(abs(line["t"] - line["sample"] / 48000) < 1e-5, f"{line}")

    starts = {e["voice"]: e for e in events if e["kind"] == "voice_start"}
    stops = {e["voice"]: e for e in events if e["kind"] == "voice_stop"}
    ed.check_eq(len(starts), 3, f"the init sound and two jumps: {starts}")
    ed.check_eq(set(starts), set(stops), "every voice_start has its voice_stop")

    init = next(e for e in starts.values() if e["clip"] == "sounds/init.wav")
    ed.check(init["sample"] <= FRAME, f"a script's init start is in the capture: {init}")
    ed.check_eq((init["owner"], init["group"], init["cause"]), ("free", "Master", "lua"), f"{init}")
    ed.check_eq(stops[init["voice"]]["reason"], "ended", f"{stops[init['voice']]}")
    ed.check(abs(stops[init["voice"]]["played_s"] - 0.2) < 2 / 60, f"it played its length: {stops[init['voice']]}")

    jumps = sorted((e for e in starts.values() if e["clip"] == "sounds/jump.wav"), key=lambda e: e["sample"])
    marks = [e for e in events if e["kind"] == "marker" and e["label"] == "jump"]
    ed.check_eq(len(marks), 2, f"{marks}")
    for mark, jump in zip(marks, jumps):
        ed.check_eq(mark["source"], "lua", f"{mark}")
        ed.check(0 <= jump["sample"] - mark["sample"] <= FRAME, f"the sound follows its mark: {mark} {jump}")
        ed.check(abs(jump["gain_db"] - 20 * math.log10(0.5)) < 0.05, f"{jump}")
    ed.check_eq(next(e for e in events if e["kind"] == "marker")["label"], "init true", "Audio.isCapturing in init")

    inputs = [e for e in events if e["kind"] == "input" and e["device"] == "key" and e["action"] == "down"]
    ed.check_eq(len(inputs), 2, f"send_input is marked: {inputs}")
    session = read_session(directory)
    ed.check_eq(session["counts"]["voice_start"], 3, f"{session['counts']}")
    ed.check_eq(session["files"]["master"], "master.wav", f"{session}")
    ed.check_eq(session["sample_rate"], 48000, f"{session}")
    ed.check_eq(session["driver"], "deterministic", f"{session}")

    rate, channels, samples = read_float_wav(directory / "master.wav")
    ed.check_eq((rate, channels), (48000, 2), "stereo at 48 kHz")
    ed.check_eq(len(samples) // 2, session["captured_samples"], "the WAV holds what was captured")
    latency = init["latency_samples"]
    ed.check_eq(latency, session["latency_samples"], "through Master, the session's")
    onset = next(i // 2 for i, v in enumerate(samples) if v != 0.0)
    # (a sine's first sample is 0: its second is the first that shows)
    ed.check_eq(onset, init["sample"] + latency + 1, f"the first sound is at its stamp + latency_samples ({latency})")


async def test_stop_says_scene(ed: Editor):
    scene = (await ed.call("create_scene", name="Loop", type="2D", path="scenes/Loop.scene"))["scene"]["id"]
    STATE["loop"] = scene
    await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
    await ed.call("set_properties", scene=scene, entity="Source", values={"audiosource": {"filename": "sounds/long.wav", "looping": True, "state": "playing"}})
    await ed.call("save_scene", scene=scene)
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=scene, fixed_fps=60)
    await ed.call("wait", frames=10)
    await ed.call("play", action="stop")
    events = read_jsonl(await new_session(ed, before) / "events.jsonl")[1:]
    start = next(e for e in events if e["kind"] == "voice_start")
    stop = next(e for e in events if e["kind"] == "voice_stop")
    ed.check_eq((start["owner"], start["entity"], start["cause"], start["looping"]), ("main", "Source", "play_on_awake", True), f"{start}")
    ed.check_eq((stop["reason"], stop["voice"]), ("scene", start["voice"]), f"{stop}")


async def test_truncates_and_keeps_events(ed: Editor):
    await ed.call("set_project_settings", values={"audio.capture_max_seconds": 2})
    try:
        before = capture_sessions(ed.project)
        await ed.call("play", action="start", scene=STATE["jump"], fixed_fps=60)
        await ed.call("wait", frames=150)
        await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=2)
        await ed.call("play", action="stop")
        directory = await new_session(ed, before)
    finally:
        await ed.call("set_project_settings", values={"audio.capture_max_seconds": 120})
    session = read_session(directory)
    _, _, samples = read_float_wav(directory / "master.wav")
    ed.check_eq(len(samples) // 2, 96000, "master.wav stops at 2 s")
    ed.check_eq((session["truncated"], session["captured_samples"], session["max_seconds"]), (True, 96000, 2), f"{session}")
    ed.check(session["samples"] > 96000, f"the clock goes on: {session['samples']}")
    late = [e for e in read_jsonl(directory / "events.jsonl")[1:] if e["kind"] == "marker" and e["label"] == "jump"]
    ed.check(late and late[0]["sample"] > 96000, f"events keep being logged: {late}")
    levels = read_jsonl(directory / "levels.jsonl")[1:]
    ed.check(levels[-1]["sample"] <= 96000, f"the levels stop growing: {levels[-1]['sample']}")


async def test_keep_limits_sessions(ed: Editor):
    await ed.call("set_project_settings", values={"audio.capture_keep": 2})
    try:
        for _ in range(3):
            await ed.call("play", action="start", scene=STATE["jump"], fixed_fps=60)
            await ed.call("wait", frames=2)
            await ed.call("play", action="stop")
        ed.check_eq(len(capture_sessions(ed.project)), 2, "two sessions are left")
        # only names of sessions go
        stray = ed.project / ".jasperengine/audio/captures/notes"
        stray.mkdir()
        await ed.call("play", action="start", scene=STATE["jump"], fixed_fps=60)
        await ed.call("wait", frames=2)
        await ed.call("play", action="stop")
        ed.check(stray.is_dir(), "a folder that is not a session stays")
        stray.rmdir()
    finally:
        await ed.call("set_project_settings", values={"audio.capture_keep": 3})


async def test_settings_round_trip(ed: Editor):
    values = (await ed.call("get_project_settings", keys=["audio.capture_max_seconds", "audio.capture_keep", "audio.capture_stems"]))["settings"]
    ed.check_eq(values, {"audio.capture_max_seconds": 120, "audio.capture_keep": 3, "audio.capture_stems": False}, "the defaults")
    await ed.call("save_project")
    text = (ed.project / "project.yaml").read_text()
    ed.check("capture" not in text, "a default is not written")


async def test_cannot_write_plays_on():
    async with Editor(create=True, name="Capture Fails") as ed:
        scene = (await ed.call("create_scene", name="Fails", type="2D", path="scenes/Fails.scene"))["scene"]["id"]
        await ed.call("save_scene", scene=scene)
        # a file where the sessions' folder goes: this fails for root too
        blocked = ed.project / ".jasperengine/audio"
        blocked.parent.mkdir(exist_ok=True)
        blocked.write_text("not a folder")
        started = await ed.call("play", action="start", scene=scene, fixed_fps=60)
        ed.check("cannot create" in started.get("audio_capture_error", ""), f"the error reaches play start: {started}")
        await ed.call("wait", frames=3)
        state = await ed.call("get_play_state")
        ed.check_eq(state["state"], "playing", "Play goes on")
        errors = await ed.console_since(limit=200)
        ed.check(any("Audio capture" in e.get("message", "") for e in errors), f"and the Console: {errors}")
        await ed.call("play", action="stop")


async def test_crash_leaves_a_readable_session():
    async with Editor(create=True, name="Capture Crash") as ed:
        write_tone(ed.project / "sounds/long.wav", seconds=10.0, rate=48000)
        await ed.call("sync_files")
        scene = (await ed.call("create_scene", name="Crash", type="2D", path="scenes/Crash.scene"))["scene"]["id"]
        await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
        await ed.call("set_properties", scene=scene, entity="Source", values={"audiosource": {"filename": "sounds/long.wav", "state": "playing"}})
        await ed.call("save_scene", scene=scene)
        await ed.call("play", action="start", scene=scene, fixed_fps=60)
        await ed.call("wait", frames=150)
        os.kill(editor_pid(ed.project), signal.SIGKILL)
        directory = capture_sessions(ed.project)[0]
        ed.check_eq(read_session(directory)["state"], "capturing", "it reads as not finished")
        temporary = [p for p in directory.iterdir() if p.name.startswith(".master.wav.")]
        ed.check_eq(len(temporary), 1, f"master.wav's temporary sibling: {list(directory.iterdir())}")
        rate, channels, samples = read_float_wav(temporary[0])
        ed.check_eq((rate, channels), (48000, 2), "a valid header")
        ed.check(len(samples) // 2 >= 48000, f"at least the first flushed second: {len(samples) // 2}")
        ed.check(max(samples) > 0.1, "with the sound in it")
        events = read_jsonl(directory / "events.jsonl")
        ed.check(any(e.get("kind") == "voice_start" for e in events), "and the lines so far")


async def test_levels_and_stems():
    async with Editor(create=True, name="Capture Levels") as ed:
        await ed.call("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "Music"}])
        write_tone(ed.project / "sounds/tone.wav", seconds=3.0, frequency=1000.0, amplitude=0.5, rate=48000)
        await ed.call("sync_files")
        scene = (await ed.call("create_scene", name="Tone", type="2D", path="scenes/Tone.scene"))["scene"]["id"]
        await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
        await ed.call("set_properties", scene=scene, entity="Source", values={"audiosource": {
            "filename": "sounds/tone.wav", "looping": True, "outputGroup": "Music", "state": "playing"}})
        await ed.call("save_scene", scene=scene)

        async def capture(fader_db: float = 0.0, stems: bool = False):
            await ed.call("set_project_settings", values={"audio.capture_stems": stems})
            before = capture_sessions(ed.project)
            await ed.call("play", action="start", scene=scene, fixed_fps=60)
            if fader_db:
                await ed.call("eval_lua", code=f'AudioMixer.getGroup("Music").volume = {fader_db}')
            await ed.call("wait", frames=60)
            await ed.call("play", action="stop")
            return await new_session(ed, before)

        def settled(directory):
            lines = read_jsonl(directory / "levels.jsonl")
            ed.check_eq(lines[0]["format"], "jasperengine.audio-levels", "the header")
            for i, line in enumerate(lines[1:], 1):
                ed.check_eq(line["sample"], i * 4800, "every 100 ms of audio")
            return lines[-1]

        plain = await capture()
        line = settled(plain)
        _, _, samples = read_float_wav(plain / "master.wav")
        tail = samples[-2 * 9600:]
        peak_db = 20 * math.log10(max(abs(v) for v in tail))
        band = line["groups"]["Music"]["bands_db"][5]
        ed.check(abs(band - line["groups"]["Music"]["peak_db"]) < 0.5, f"a 1 kHz tone is its 1 kHz band: {line['groups']['Music']}")
        ed.check(abs(line["master"]["peak_db"] - peak_db) < 0.1, f"master peak {line['master']['peak_db']} vs the WAV's {peak_db}")
        ed.check(abs(line["master"]["bands_db"][5] - peak_db) < 0.5, f"master's 1k band {line['master']['bands_db']} vs {peak_db}")
        ed.check(all(b < band - 10 for i, b in enumerate(line["groups"]["Music"]["bands_db"]) if abs(i - 5) >= 2),
                 f"nothing far from 1 kHz: {line['groups']['Music']['bands_db']}")
        voices = line["voices"]
        ed.check_eq(len(voices), 1, f"one voice: {line}")

        down = await capture(fader_db=-6.0)
        lowered = settled(down)["groups"]["Music"]
        moved = lowered["bands_db"][5] - band
        ed.check(abs(moved + 6.0) <= 0.2, f"fader -6 dB moves it by {moved}")

        stem = await capture(stems=True)
        session = read_session(stem)
        ed.check("groups/Music.wav" in session["files"]["stems"], f"{session['files']}")
        _, _, music = read_float_wav(stem / "groups/Music.wav")
        _, _, master = read_float_wav(stem / "master.wav")
        ed.check_eq(len(music), len(master), "as long as the master")
        n = len(master)
        mean_a, mean_b = sum(music) / n, sum(master) / n
        cov = sum((a - mean_a) * (b - mean_b) for a, b in zip(music, master))
        norm = math.sqrt(sum((a - mean_a) ** 2 for a in music) * sum((b - mean_b) ** 2 for b in master))
        ed.check(cov / norm > 0.999, f"the stem is the group's signal: correlation {cov / norm}")
        await ed.call("set_project_settings", values={"audio.capture_stems": False})
