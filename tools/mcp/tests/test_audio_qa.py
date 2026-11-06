"""audio_session check (docs/specs/S35-agent-audio.md R21-R25; AC10, AC11, AC12): each seeded failure FAILs or WARNs with
the right limited_by and at, its fixed version PASSes, and tools/mcp/audio_qa_ref.py agrees on what was measured. The
scenes are made here, each with what its check needs. There is no audio device under the harness: every Play captures.
"""

import json
import os
import shutil
import struct
import subprocess
from pathlib import Path

import audio_qa_ref as ref
import harness
from audio_refs import capture_sessions, read_float_wav, read_jsonl, silence, sine, write_tone, write_wav
from harness import Editor

EDITOR = {"create": True, "name": "Audio QA"}
CI = True
TIMEOUT = 1500

RATE = 48000
STATE = {}

DRIVER = """---@class Driver : ScriptInstance
local Driver = { properties = {} }

function Driver:init()
    self.frame = 0
    self.source = AudioSource(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function Driver:onUpdate()
    self.frame = self.frame + 1
BODY
end

return Driver
"""


def of(result: dict, check: str) -> dict:
    return next(c for c in result["checks"] if c["id"] == check)


async def scene_with(ed: Editor, name: str, sources: list[dict], body: str | None = None):
    """A 2D scene with these Audio Sources; body: Lua run each frame on the first one"""
    scene = (await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}.scene"))["scene"]["id"]
    for i, source in enumerate(sources):
        entity = f"Source{i}"
        await ed.call("create_entity", scene=scene, name=entity, type="audio_source")
        await ed.call("set_properties", scene=scene, entity=entity, values={"audiosource": source})
    if body is not None:
        await ed.call("create_script", kind="lua", name=f"Driver{name}", content=DRIVER.replace("BODY", body).replace("Driver", f"Driver{name}"), entity="Source0", scene=scene)
    await ed.call("save_scene", scene=scene)
    return scene


async def capture(ed: Editor, scene, frames: int, during=None) -> Path:
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
    return made[0]


async def check(ed: Editor, session: Path, *checks: str, **args) -> dict:
    return await ed.call("audio_session", action="check", session=session.name, checks=list(checks), **args)


def peaks(session: Path) -> list[float]:
    _, channels, samples = read_float_wav(session / "master.wav")
    return ref.peaks_per_ms(samples, channels, RATE)


async def test_setup(ed: Editor):
    sounds = ed.project / "sounds"
    tone = sine(1000.0, 1.0, RATE, -6.0)
    write_wav(sounds / "on_time.wav", [tone], RATE)
    write_wav(sounds / "late.wav", [silence(0.12, RATE) + tone], RATE)
    write_wav(sounds / "music.wav", [sine(1000.0, 2.0, RATE, -6.0)], RATE)
    write_wav(sounds / "quiet_music.wav", [sine(1000.0, 2.0, RATE, -24.0)], RATE)
    write_wav(sounds / "sfx.wav", [sine(1000.0, 0.8, RATE, -6.0)], RATE)
    write_wav(sounds / "hot.wav", [sine(1000.0, 1.0, RATE, 0.0)], RATE)
    write_wav(sounds / "soft.wav", [sine(1000.0, 1.0, RATE, -20.0)], RATE)
    write_tone(sounds / "step_a.wav", seconds=0.1, frequency=500.0, rate=RATE)
    write_tone(sounds / "step_b.wav", seconds=0.1, frequency=700.0, rate=RATE)
    write_wav(sounds / "shifted.wav", [tone], RATE)
    await ed.call("sync_files")
    await ed.call("tool_groups", enable=["audio"])
    await ed.call("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "Music"}, {"op": "add_group", "name": "SFX"}])


async def test_sync_late_clip_and_its_fix(ed: Editor):
    late = await capture(ed, await scene_with(ed, "Late", [{"filename": "sounds/late.wav", "state": "playing"}]), 40)
    result = await check(ed, late, "sync")
    sync = of(result, "sync")
    ed.check_eq((sync["kind"], sync["status"]), ("content", "FAIL"), f"{sync}")
    ed.check("sounds/late.wav" in sync["limited_by"], f"{sync}")
    ed.check(len(sync["at"]) == 1 and sync["at"][0] < 0.05, f"{sync}")
    ed.check("trim_head_ms" in sync["fix_hint"], f"{sync}")
    ed.check_eq(result["verdict"], "FAIL", f"{result['verdict']}")
    onset = ref.onset_ms(peaks(late), 0, 3.0)
    ed.check(abs(sync["measured"] - onset) <= 2.0, f"AudioQa {sync['measured']} ms vs the oracle's {onset} ms")
    ed.check(abs(sync["measured"] - 120.0) <= 3.0, f"120 ms of head silence: {sync['measured']}")
    for key in ("id", "kind", "status", "round", "measured", "threshold", "threshold_source", "limited_by", "at", "fix_hint", "method"):
        ed.check(key in sync, f"{key} in {sync}")
    ed.check_eq(len(sync), 11, f"exactly these fields: {sorted(sync)}")
    ed.check((late / "check.json").is_file(), "check.json")
    ed.check_eq(json.loads((late / "check.json").read_text())["verdict"], "FAIL", "the file says the same")

    fixed = await capture(ed, await scene_with(ed, "OnTime", [{"filename": "sounds/on_time.wav", "state": "playing"}]), 40)
    ok = of(await check(ed, fixed, "sync"), "sync")
    ed.check_eq((ok["status"], ok["limited_by"], ok["at"]), ("PASS", None, []), f"{ok}")


async def test_thresholds_are_audited_and_the_third_fail_stops(ed: Editor):
    late = capture_sessions(ed.project)[-2]
    await ed.call("set_project_settings", values={"audio.qa.sync_tolerance_ms": 33})
    try:
        result = await check(ed, late, "sync")
        changed = result["thresholds"]["changed"]
        ed.check_eq(changed, [{"key": "audio.qa.sync_tolerance_ms", "value": 33, "default": 20}], f"{changed}")
        ed.check_eq(result["thresholds"]["values"]["audio.qa.head_silence_max_ms"], 15, f"{result['thresholds']['values']}")
    finally:
        await ed.call("set_project_settings", values={"audio.qa.sync_tolerance_ms": 20})
    ed.check(result["guidance"].endswith("Report these numbers; never claim a sound plays without a capture."), result["guidance"])
    # rounds: the two checks of the last test, and these
    rounds = [of(result, "sync")["round"]]
    for _ in range(2):
        result = await check(ed, late, "sync")
        rounds.append(of(result, "sync")["round"])
    ed.check_eq(rounds, sorted(rounds), f"they count up: {rounds}")
    ed.check(rounds[-1] > 2 and "stop fixing" in result["guidance"], f"{rounds}: {result['guidance']}")
    listen = result["listen"]
    ed.check(3 <= len(listen) <= 5, f"3 to 5 moments: {listen}")
    ed.check("sync" in listen[0]["why"], f"the first failure first: {listen}")


async def test_audibility_names_the_masking_group_and_band(ed: Editor):
    body = '    if self.frame == 60 then Audio.play("sounds/sfx.wav", { group = "SFX" }) end'
    hot = await capture(ed, await scene_with(ed, "HotMusic", [{"filename": "sounds/music.wav", "looping": True, "outputGroup": "Music", "state": "playing"}], body), 120)
    result = of(await check(ed, hot, "audibility"), "audibility")
    ed.check_eq(result["status"], "FAIL", f"{result}")
    ed.check("group Music" in result["limited_by"] and "1k" in result["limited_by"], f"{result}")
    ed.check(abs(result["at"][0] - 1.0) < 0.05, f"{result}")
    ed.check("Lower group Music" in result["fix_hint"], f"{result}")
    fixed = await capture(ed, await scene_with(ed, "QuietMusic", [{"filename": "sounds/quiet_music.wav", "looping": True, "outputGroup": "Music", "state": "playing"}], body), 120)
    ok = of(await check(ed, fixed, "audibility"), "audibility")
    ed.check_eq(ok["status"], "PASS", f"{ok}")
    ed.check(abs(ok["measured"] - 18.0) <= 2.0, f"-6 dB over -24 dB: {ok['measured']}")


async def test_coverage_click_with_no_sound_and_its_fix(ed: Editor):
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720, "graphics.scaling_mode": "letterbox"})

    async def menu(name: str, sounded: bool):
        scene = (await ed.call("create_scene", name=name, type="UI", path=f"scenes/{name}.scene"))["scene"]["id"]
        await ed.call("create_ui", scene=scene, root={"type": "button", "name": "Go", "layout": {"anchor": "top_left", "position": [100, 100], "size": [200, 100]}})
        if sounded:
            await ed.call("add_component", scene=scene, entity="Go", component="audiosource")
            await ed.call("set_properties", scene=scene, entity="Go", values={"audiosource": {"filename": "sounds/step_a.wav", "playOn": ["ui_click"]}})
        await ed.call("save_scene", scene=scene)
        return scene

    async def click():
        await ed.call("wait", frames=3)
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 200, "y": 150}], space="canvas", frames_after=3)

    silent = await capture(ed, await menu("Mute", False), 3, click)
    rule = [{"on": "ui_click", "entity": "Go"}]
    missed = of(await check(ed, silent, "coverage", expect=rule), "coverage")
    ed.check_eq(missed["status"], "FAIL", f"{missed}")
    ed.check_eq(len(missed["at"]), 1, f"{missed}")
    ed.check("no sound in time" in missed["limited_by"], f"{missed}")
    sounded = await capture(ed, await menu("Sounded", True), 3, click)
    ok = await check(ed, sounded, "coverage", "sync", expect=rule)
    ed.check_eq(of(ok, "coverage")["status"], "PASS", f"{ok}")
    ed.check_eq(of(ok, "sync")["status"], "PASS", f"{ok}")
    ed.check_eq(of(await check(ed, sounded, "coverage", expect=[{"on": "ui_click", "sound": "step_b"}]), "coverage")["status"], "FAIL", "another sound does not count")
    bad = await ed.call("audio_session", action="check", session=sounded.name, expect=[{"on": "ui_click", "within": 3}], expect_error=True)
    ed.check("within_ms" in bad, f"the error lists the keys: {bad}")


def write_session(ed: Editor, name: str, seconds: float, dbfs: float, extra: list | None = None) -> Path:
    """A session on disk with a tone of this level and a line every 100 ms: a master hotter than the engine's clipper lets
    through, S38's limiter lines, 60 s of audio without playing them"""
    root = ed.project / ".jasperengine/audio/captures" / name
    root.mkdir(parents=True)
    tone = sine(1000.0, seconds, RATE, dbfs)
    data = b"".join(struct.pack("<ff", value, value) for value in tone)
    (root / "master.wav").write_bytes(b"RIFF" + struct.pack("<I", 44 + 12 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, RATE, RATE * 8, 8, 32) +
                                      b"fact" + struct.pack("<II", 4, len(tone)) + b"data" + struct.pack("<I", len(data)) + data)
    events = [{"format": "jasperengine.audio-events", "version": 1}]
    levels = [{"format": "jasperengine.audio-levels", "version": 1}]
    bands = [dbfs] * 10
    for i in range(int(seconds * 10)):
        sample = i * 4800
        events.append({"n": i + 1, "kind": "marker", "sample": sample, "t": sample / RATE, "frame": i, "game_time": sample / RATE, "label": "m", "source": "lua"})
        levels.append({"sample": sample + 4800, "t": (sample + 4800) / RATE, "master": {"peak_db": dbfs, "rms_db": dbfs, "bands_db": bands},
                       "groups": {"Master": {"peak_db": dbfs, "rms_db": dbfs, "bands_db": bands}}, "voices": []})
    events += extra or []
    (root / "events.jsonl").write_text("".join(json.dumps(line) + "\n" for line in events))
    (root / "levels.jsonl").write_text("".join(json.dumps(line) + "\n" for line in levels))
    (root / "session.json").write_text(json.dumps({"format": "jasperengine.audio-capture", "version": 1, "session": name, "state": "complete", "scene": "Written", "sample_rate": RATE, "channels": 2,
                                                   "samples": len(tone), "duration_s": seconds, "captured_samples": len(tone), "files": {"master": "master.wav", "events": "events.jsonl", "levels": "levels.jsonl", "stems": []}}))
    return root


async def test_true_peak(ed: Editor):
    # the engine's round-off clipper holds two voices at full scale under the ceiling: what the mixer guarantees
    hot = await capture(ed, await scene_with(ed, "Hot", [{"filename": "sounds/hot.wav", "state": "playing"}, {"filename": "sounds/hot.wav", "state": "playing"}]), 30)
    result = await check(ed, hot, "true_peak", "clipping")
    peak = of(result, "true_peak")
    ed.check_eq((peak["kind"], peak["status"]), ("mixer", "PASS"), f"{peak}")
    _, _, samples = read_float_wav(hot / "master.wav")
    ed.check(peak["measured"] >= ref.true_peak_dbtp_lower_bound(samples) - 0.01, f"a true peak is never under the sample peak: {peak['measured']}")
    ed.check_eq(of(result, "clipping")["measured"], ref.clipped_samples(samples), "the clipped samples, counted here")
    # a hotter master than that (a session from before a clipper, a stem read as one)
    over = write_session(ed, "20200101-000000-01", 1.0, 0.0)
    result = await check(ed, over, "true_peak", "clipping")
    ed.check_eq(of(result, "true_peak")["status"], "FAIL", f"{of(result, 'true_peak')}")
    ed.check("ceiling" in of(result, "true_peak")["limited_by"], f"{of(result, 'true_peak')}")
    ed.check_eq(of(result, "clipping")["status"], "FAIL", f"{of(result, 'clipping')}")
    _, _, samples = read_float_wav(over / "master.wav")
    ed.check_eq(of(result, "clipping")["measured"], ref.clipped_samples(samples), "the clipped samples, counted here")
    # S38's limiter lines: under the ceiling, but too hot
    limited = write_session(ed, "20200101-000000-02", 1.0, -6.0, [{"n": 99, "kind": "limiter", "sample": 24000, "t": 0.5, "frame": 30, "game_time": 0.5, "gain_reduction_db": 8.0}])
    warned = of(await check(ed, limited, "true_peak"), "true_peak")
    ed.check_eq(warned["status"], "WARN", f"{warned}")
    ed.check_eq(warned["threshold_source"], "audio.qa.limiter_warn_db", f"{warned}")
    ed.check("too hot" in warned["limited_by"] and warned["at"] == [0.5], f"{warned}")


async def test_silence_gap_in_music_and_its_fix(ed: Editor):
    gap = await capture(ed, await scene_with(ed, "Gap", [{"filename": "sounds/music.wav", "outputGroup": "Music", "state": "playing"}]), 300)
    result = of(await check(ed, gap, "silence"), "silence")
    ed.check_eq(result["status"], "FAIL", f"{result}")
    ed.check(abs(result["at"][0] - 2.0) < 0.1, f"{result}")
    oracle = ref.longest_gap_seconds(peaks(gap), 0)
    ed.check(abs(result["measured"] - oracle) <= 0.02, f"{result['measured']} vs the oracle's {oracle}")
    looped = await capture(ed, await scene_with(ed, "NoGap", [{"filename": "sounds/music.wav", "looping": True, "outputGroup": "Music", "state": "playing"}]), 300)
    ed.check_eq(of(await check(ed, looped, "silence"), "silence")["status"], "PASS", "looping")
    none = capture_sessions(ed.project)[0]
    ed.check_eq(of(await check(ed, none, "silence"), "silence")["status"], "SKIP", "no music")


async def test_repetition_and_its_fix(ed: Editor):
    same = '    if self.frame % 20 == 0 and self.frame <= 80 then self.source:playOneShot("sounds/step_a.wav") end'
    rows = await capture(ed, await scene_with(ed, "Same", [{"filename": "sounds/step_a.wav"}], same), 100)
    result = of(await check(ed, rows, "repetition"), "repetition")
    ed.check_eq(result["status"], "WARN", f"{result}")
    ed.check("sounds/step_a.wav" in result["limited_by"], f"{result}")
    starts = [line for line in read_jsonl(rows / "events.jsonl")[1:] if line["kind"] == "voice_start"]
    ed.check_eq(result["measured"], ref.most_repeats(starts, 4.0), "the plays in a row, counted here")
    varied = '    if self.frame % 20 == 0 and self.frame <= 80 then self.source:playOneShot(self.frame % 40 == 0 and "sounds/step_a.wav" or "sounds/step_b.wav") end'
    ok = await capture(ed, await scene_with(ed, "Varied", [{"filename": "sounds/step_a.wav"}], varied), 100)
    ed.check_eq(of(await check(ed, ok, "repetition"), "repetition")["status"], "PASS", "two clips in turn")


async def test_mixer_passes_its_own_checks(ed: Editor):
    await ed.call("audio_mixer", action="edit", ops=[
        {"op": "add_effect", "group": "Master", "type": "limiter", "params": {"threshold": -3}},
        {"op": "add_effect", "group": "Music", "type": "duck", "params": {"sidechainGroup": "SFX", "level": 0.25, "attack": 0.01, "release": 0.05}}])
    try:
        body = '    if self.frame == 90 then Audio.play("sounds/sfx.wav", { group = "SFX" }) end'
        session = await capture(ed, await scene_with(ed, "Guarantees", [{"filename": "sounds/music.wav", "looping": True, "outputGroup": "Music", "state": "playing"},
                                                                         {"filename": "sounds/hot.wav", "state": "playing"}], body), 240)
        result = await ed.call("audio_session", action="check", session=session.name)
        ed.check_eq([c["id"] for c in result["checks"]], ["loudness", "true_peak", "clipping", "coverage", "sync", "audibility", "voice_budget", "ducking", "silence", "load_delay", "repetition", "regression"], "always in this order")
        for item in result["checks"]:
            if item["kind"] == "mixer":
                ed.check_eq(item["status"], "PASS", f"{item}")
        ducking = of(result, "ducking")
        ed.check_eq(ducking["status"], "PASS", f"{ducking}")
        ed.check(ducking["measured"] >= 9.0, f"level 0.25 is 12 dB: {ducking}")
        ed.check_eq(of(result, "regression")["status"], "SKIP", "no baseline")
        limiter = [line for line in read_jsonl(session / "events.jsonl")[1:] if line["kind"] == "limiter"]
        ed.check(limiter and limiter[0]["group"] == "Master" and limiter[0]["gain_reduction_db"] > 0, f"the limiter's lines: {limiter[:2]}")
        ed.check_eq(of(result, "loudness")["status"], "SKIP", "under audio.qa.loudness_min_seconds of gated audio")
    finally:
        await ed.call("audio_mixer", action="edit", ops=[{"op": "remove_effect", "group": "Master", "type": "limiter"}, {"op": "remove_effect", "group": "Music", "type": "duck"}])


async def test_baseline_and_regression(ed: Editor):
    scene = await scene_with(ed, "Shift", [{"filename": "sounds/shifted.wav", "state": "playing"}])
    first = await capture(ed, scene, 40)
    saved = await check(ed, first, "sync", save_baseline="shift")
    ed.check_eq(saved["baseline_saved"], "baselines/audio/shift.json", f"{saved.get('baseline_saved')}")
    file = ed.project / "baselines/audio/shift.json"
    baseline = json.loads(file.read_text())
    ed.check_eq((baseline["format"], baseline["version"]), ("jasperengine.audio-baseline", 1), f"{baseline}")
    await ed.call("undo", scope="project")
    ed.check(not file.exists(), "undo removes it")
    await ed.call("redo", scope="project")
    ed.check(file.exists(), "redo writes it")
    same = await capture(ed, scene, 40)
    ok = of(await check(ed, same, "regression", baseline="shift"), "regression")
    ed.check_eq(ok["status"], "PASS", f"{ok}")
    ed.check_eq(ok["measured"]["timing_ms"], 0, f"the same build, the same samples: {ok}")
    by_session = of(await check(ed, same, "regression", baseline=first.name), "regression")
    ed.check_eq(by_session["status"], "PASS", f"a session compares too: {by_session}")
    write_wav(ed.project / "sounds/shifted.wav", [silence(0.02, RATE) + sine(1000.0, 1.0, RATE, -6.0)], RATE)
    await ed.call("sync_files")
    moved = await capture(ed, scene, 40)
    bad = of(await check(ed, moved, "regression", baseline="shift"), "regression")
    ed.check_eq(bad["status"], "FAIL", f"{bad}")
    ed.check(abs(bad["measured"]["timing_ms"] - 20.0) <= 2.0, f"{bad}")
    ed.check("sounds/shifted.wav" in bad["limited_by"], f"{bad}")


async def test_fail_on_is_an_error_naming_the_fix(ed: Editor):
    scene = await scene_with(ed, "LateAgain", [{"filename": "sounds/late.wav", "state": "playing"}])
    session = await capture(ed, scene, 40)
    r = await ed.call_raw("audio_session", {"action": "check", "session": session.name, "checks": ["sync"], "fail_on": "fail"})
    ed.check(r.is_error, "a verdict at fail_on is an error")
    ed.check("trim_head_ms" in r.text, f"the fix is in the error's text: {r.text}")
    ok = await ed.call_raw("audio_session", {"action": "check", "session": session.name, "checks": ["load_delay"], "fail_on": "warn"})
    ed.check(not ok.is_error, f"{ok.text}")

    # the CLI: exit 1, and 0 on the fixed scene; on a copy, this editor holds the project
    env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY")}
    env["HOME"] = str(ed.scratch.home)
    copy = ed.scratch.root / "cli-copy"
    shutil.copytree(ed.project, copy, ignore=shutil.ignore_patterns("*.lock", "lock", "build"))

    def cli(args: dict) -> subprocess.CompletedProcess:
        return subprocess.run(harness.editor_command() + ["call", "--project", str(copy), "audio_session", json.dumps(args)], env=env, cwd=str(ed.scratch.root),
                              capture_output=True, text=True, timeout=600)

    failing = cli({"action": "check", "session": session.name, "checks": ["sync"], "fail_on": "fail"})
    ed.check_eq(failing.returncode, 1, f"{failing.stdout[-500:]} {failing.stderr[-500:]}")
    passing = cli({"action": "check", "session": session.name, "checks": ["load_delay"], "fail_on": "fail"})
    ed.check_eq(passing.returncode, 0, f"{passing.stdout[-500:]} {passing.stderr[-500:]}")
