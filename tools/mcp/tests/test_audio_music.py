"""The music system (docs/specs/S39-music.md AC1-AC6) on Play's deterministic captures: a playlist's samples
against audio_refs.music_sequence, a track that survives a scene load and stops at Play stop, routed to
audio.music_group and ducked there, held by the app's and the listener's pause; switches with their crossfade
curves, on the bar with a snapshot, never three voices; stingers on their beat and bar; the beat callbacks in the
frame whose mix holds each beat; the music lines in the report and the silence check.

The oracles are independent of the engine: the WAVs written here, audio_refs' grid and sequence written from the
spec's text, Goertzel levels of master.wav, and what the scripts mark from Lua.
"""

import json
import math

import audio_refs as ref
from audio_refs import capture_sessions, read_float_wav, read_jsonl, write_import, write_tone, write_wav
from harness import Editor

EDITOR = {"create": True, "name": "Music"}
CI = True
TIMEOUT = 1800

RATE = 48000
FRAME = 800          # 60 fps fixed
FIRST_BEAT_MS = 12.4
STATE = {}

DRIVER = """---@class MusicDriver : ScriptInstance
local MusicDriver = { properties = {} }

function MusicDriver:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end

function MusicDriver:onUpdate()
    self.frame = self.frame + 1
    local f = self.frame
%s
end

return MusicDriver
"""


def lines_of(session, kind=None):
    lines = read_jsonl(session / "events.jsonl")[1:]
    return [l for l in lines if kind is None or l["kind"] == kind]


def music_lines(session, action=None):
    return [l for l in lines_of(session, "music") if action is None or l["action"] == action]


def music_voices(session):
    return [l for l in lines_of(session, "voice_start") if l["owner"] == "music"]


async def scene_with(ed: Editor, name: str, body: str) -> int:
    scene = (await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="empty", name="Driver")
    await ed.call("create_script", kind="lua", name=f"{name}Driver", content=DRIVER % body, entity="Driver", scene=scene)
    await ed.call("save_scene", scene=scene)
    return scene


async def capture(ed: Editor, scene: int, frames: int, during=None):
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


def window(samples, start: int, end: int, channel: int = 0):
    return [samples[i * 2 + channel] for i in range(max(start, 0), end)]


async def test_setup(ed: Editor):
    music = ed.project / "music"
    write_tone(music / "menu.wav", seconds=8.0, frequency=440.0, amplitude=0.25, rate=RATE)
    write_tone(music / "game.wav", seconds=8.0, frequency=660.0, amplitude=0.25, rate=RATE)
    write_tone(music / "free.wav", seconds=4.0, frequency=550.0, amplitude=0.25, rate=RATE)
    write_tone(music / "sting.wav", seconds=0.25, frequency=1500.0, amplitude=0.25, rate=RATE)
    write_tone(music / "sfx.wav", seconds=1.0, frequency=2000.0, amplitude=0.25, rate=RATE)
    # the playlist's sections: triangles exact in float32, loop regions of whole periods
    for name, period, frames in (("s0", 64, 9600), ("s1", 32, 4800), ("s2", 128, 12800)):
        write_wav(music / f"{name}.wav", [ref.triangle(period, frames)], RATE, encoding=ref.FLOAT32)
    write_import(music / "menu.wav", f"music: {{bpm: 120, first_beat_ms: {FIRST_BEAT_MS}}}")
    write_import(music / "game.wav", "music: {bpm: 120}")
    write_import(music / "s0.wav", "loop_start_ms: 33.3333\nloop_end_ms: 166.6667")
    write_import(music / "s1.wav", "trim_head_ms: 6.6667\ntrim_tail_ms: 3.3333")
    write_import(music / "s2.wav", "loop_start_ms: 26.6667\nloop_end_ms: 133.3333")
    await ed.call("sync_files")
    playlist = {"format": "jasperengine.soundevent", "version": 1, "mode": "sequence", "type": "music",
                "clips": [{"clip": {"path": "music/s0.wav"}, "loops": 2}, {"clip": {"path": "music/s1.wav"}},
                          {"clip": {"path": "music/s2.wav"}, "loops": 0}]}
    await ed.call("write_file", path="music/playlist.soundevent", content=json.dumps(playlist, indent=2))
    layer = dict(playlist, mode="layer")
    await ed.call("write_file", path="music/layer.soundevent", content=json.dumps(layer, indent=2))
    await ed.call("sync_files")
    await ed.call("tool_groups", enable=["audio"])
    await ed.call("audio_mixer", action="edit", ops=[
        {"op": "add_group", "name": "Music"}, {"op": "add_group", "name": "SFX"},
        {"op": "add_effect", "group": "Music", "type": "duck", "params": {"sidechainGroup": "SFX", "level": 0.25, "attack": 0.01, "release": 0.05}},
        {"op": "expose", "name": "SfxVolume", "group": "SFX", "volume": True},
        {"op": "add_snapshot", "name": "Calm"},
        {"op": "add_snapshot", "name": "Combat", "values": {"SfxVolume": -6}},
        {"op": "set_start_snapshot", "name": "Calm"}])
    await ed.call("set_project_settings", values={"audio.music_group": "Music"})
    STATE["other"] = await scene_with(ed, "MusicOther", "")


# AC1 (R1-R3): the playlist's samples are audio_refs.music_sequence's; the keys survive a round trip
async def test_playlist_samples_match_the_oracle(ed: Editor):
    scene = await scene_with(ed, "Playlist", """    if f == 1 then Audio.playMusic("music/playlist.soundevent", { group = "Master" }) end
    if f == 40 then Audio.advanceMusic() end""")
    await ed.call("set_project_settings", values={"audio.output_stage": "linear"})
    try:
        session = await capture(ed, scene, 90)
    finally:
        await ed.call("set_project_settings", values={"audio.output_stage": "roundoff"})
    starts = music_voices(session)
    ed.check_eq(len(starts), 1, f"one voice for the track: {starts}")
    sections = music_lines(session, "section")
    ed.check_eq([(s["section"], s["clip"]) for s in sections], [(1, "music/s0.wav"), (2, "music/s1.wav"), (3, "music/s2.wav")], f"{sections}")
    ed.check_eq([s["starts_at_sample"] - sections[0]["starts_at_sample"] for s in sections], [0, 16000, 20320], "each at the frame after the last")
    # advanced at frame 40: in the third section's pass that holds that sample
    advance = music_lines(session, "advance")[0]["sample"] - sections[2]["starts_at_sample"]
    passes = 1 if advance < 6400 else 2 + (advance - 6400) // 5120
    files = {n: [ref.triangle(p, f)] for n, p, f in (("s0", 64, 9600), ("s1", 32, 4800), ("s2", 128, 12800))}
    expected = ref.music_sequence([ref.MusicSection(files["s0"], 0, 9600, 1600, 8000, 2),
                                   ref.MusicSection(files["s1"], 320, 4640, 320, 4640, 1),
                                   ref.MusicSection(files["s2"], 0, 12800, 1280, 6400, passes)])[0]
    # a linear output: master.wav is the track times the mixer's constant gain, sample for sample
    _, channels, samples = read_float_wav(session / "master.wav")
    first = next(i for i in range(len(samples) // channels) if samples[i * channels] != 0.0)
    ed.check(0 <= first - starts[0]["sample"] <= 4, f"the voice's first sample a few frames after its stamp: {first} {starts[0]}")
    got = window(samples, first, first + len(expected))
    gain = got[0] / expected[0]
    mismatched = [i for i, (a, b) in enumerate(zip(got, expected)) if abs(a - gain * b) > 1e-6]
    ed.check(not mismatched and len(got) == len(expected), f"sample for sample: first differences at {mismatched[:3]}")
    ed.check_eq(len(music_lines(session, "end")), 1, "the playlist ends after its last section")

    event = json.loads((ed.project / "music/playlist.soundevent").read_text())
    await ed.call("edit_sound_event", path="music/playlist.soundevent", values={"avoid_repeat": False})
    after = json.loads((ed.project / "music/playlist.soundevent").read_text())
    ed.check_eq([c.get("loops", 1) for c in after["clips"]], [c.get("loops", 1) for c in event["clips"]], "loops survive an edit")
    ed.check("loops" not in after["clips"][1], "loops 1 is not written")


async def test_a_layer_event_is_refused(ed: Editor):
    await ed.console_since(None)
    await ed.call("play", action="start", scene=STATE["other"], fixed_fps=60)
    try:
        result = await ed.call("eval_lua", code='return Audio.playMusic("music/layer.soundevent"), Audio.playMusic("music/layer.soundevent")')
        ed.check_eq(result["results"] if "results" in result else result.get("result"), [False, False], f"{result}")
    finally:
        await ed.call("play", action="stop")
    warned = [e for e in await ed.console_since() if "layer event cannot play as music" in e["message"]]
    ed.check_eq(len(warned), 1, f"one warning: {warned}")


# AC2 (R4, R6): survives a scene load, stops at Play stop (reason scene); edit mode leaves no voice in a capture;
# routed to audio.music_group and ducked there; held by the app's pause and the listener's
async def test_survives_a_scene_load_and_stops_with_play(ed: Editor):
    await ed.call("eval_lua", code='Audio.playMusic("music/free.wav")')
    scene = await scene_with(ed, "Load", """    if f == 1 then Audio.playMusic("music/menu.wav") end
    if f == 50 then Audio.playStinger("music/sting.wav", "now") end
    if f == 51 then SceneManager.loadScene("MusicOther") end""")
    session = await capture(ed, scene, 120)
    voices = music_voices(session)
    ed.check_eq([v["clip"] for v in voices], ["music/menu.wav", "music/sting.wav"], f"no edit-mode voice in the capture: {voices}")
    stops = {l["voice"]: l for l in lines_of(session, "voice_stop")}
    loads = lines_of(session, "scene_load")
    ed.check(loads, "the scene loaded")
    track = stops.get(voices[0]["voice"])
    ed.check(track and track["reason"] == "scene" and track["sample"] > loads[0]["sample"], f"stopped by Play stop, not the load: {track}")
    sting = stops.get(voices[1]["voice"])
    ed.check(sting is None or sting["reason"] in ("ended", "scene"), f"the stinger is not cut by the load: {sting}")


async def test_music_group_ducking_and_pauses(ed: Editor):
    scene = await scene_with(ed, "Duck", """    if f == 1 then Audio.playMusic("music/menu.wav") end
    if f == 60 then Audio.play("music/sfx.wav", { group = "SFX" }) end
    if f == 150 then AudioSystem.pauseAll() end
    if f == 180 then AudioSystem.resumeAll() end
    if f == 210 then AudioListener.pause = true end
    if f == 240 then AudioListener.pause = false; Audio.playMusic("music/game.wav", { ignoreListenerPause = true }) end
    if f == 270 then AudioListener.pause = true end""")
    session = await capture(ed, scene, 300)
    levels = read_jsonl(session / "levels.jsonl")[1:]

    def music_db(t0, t1):
        rows = [l["groups"]["Music"]["rms_db"] for l in levels if t0 <= l["t"] <= t1]
        return sum(rows) / len(rows)

    before, ducked = music_db(0.3, 0.9), music_db(1.15, 1.8)
    ed.check(before > -40, f"the music plays through the Music group: {before}")
    ed.check(before - ducked >= 9.0, f"the duck on Music lowers it by ~12 dB while SFX plays: {before} -> {ducked}")
    ed.check(music_db(2.6, 2.95) < -60, "the app's pause holds it")
    ed.check(music_db(3.1, 3.45) > -40, "and it resumes")
    ed.check(music_db(3.6, 3.95) < -60, "AudioListener.pause holds it")
    ed.check(music_db(4.6, 4.95) > -40, "unless ignoreListenerPause")


# AC3 (R8-R10): an immediate 2 s crossfade follows cos/sin within 0.3 dB; sync bar lands on the bar's sample;
# the snapshot's transition is in the frame that mixes the switch; a third switch keeps two voices
async def test_crossfade_follows_equal_power_curves(ed: Editor):
    scene = await scene_with(ed, "Crossfade", """    if f == 1 then Audio.playMusic("music/menu.wav", { group = "Master" }) end
    if f == 30 then Audio.playMusic("music/game.wav", { crossfade = 2, group = "Master" }) end""")
    session = await capture(ed, scene, 180)
    play = music_lines(session, "play")[1]
    ed.check_eq((play["from"], play["fade_s"], play["sync"]), ("music/menu.wav", 2, "now"), f"{play}")
    _, channels, samples = read_float_wav(session / "master.wav")
    mono = samples[0::channels]
    switch = play["boundary_sample"]
    full = ref.goertzel_amplitude(mono[switch - 9600:switch - 4800], 440.0, RATE)
    worst = 0.0
    for k in range(1, 18):
        a, b = switch + k * 4800, switch + (k + 1) * 4800
        t = (k + 0.5) * 0.1 / 2.0
        out_db = ref.amplitude_to_db(ref.goertzel_amplitude(mono[a:b], 440.0, RATE) / full)
        in_db = ref.amplitude_to_db(ref.goertzel_amplitude(mono[a:b], 660.0, RATE) / full)
        worst = max(worst, abs(out_db - ref.amplitude_to_db(math.cos(math.pi / 2 * t))), abs(in_db - ref.amplitude_to_db(math.sin(math.pi / 2 * t))))
    ed.check(worst <= 0.3, f"cos/sin within 0.3 dB: worst {worst:.3f} dB")
    ed.check_eq(len(music_lines(session, "crossfade_end")), 1, "the crossfade ends")


async def test_bar_switch_with_a_snapshot_and_a_third_switch(ed: Editor):
    scene = await scene_with(ed, "Bar", """    if f == 1 then Audio.playMusic("music/menu.wav") end
    if f == 40 then Audio.playMusic("music/game.wav", { sync = "bar", snapshot = "Combat", crossfade = 0.5 }) end
    if f == 200 then Audio.playMusic("music/free.wav", { crossfade = 1 }) end
    if f == 210 then Audio.playMusic("music/menu.wav", { crossfade = 1 }) end""")
    session = await capture(ed, scene, 300)
    plays = music_lines(session, "play")
    voices = music_voices(session)
    bar = plays[1]
    incoming = next(v for v in voices if v["clip"] == "music/game.wav")
    ed.check_eq(bar["boundary_sample"], incoming["sample"] + incoming.get("delay_frames", 0), f"on the bar's sample: {bar} {incoming}")
    section = music_lines(session, "section")[0]
    position = bar["sample"] - section["starts_at_sample"]
    first = FIRST_BEAT_MS * RATE / 1000.0
    expected = ref.music_boundary(position, "bar", 120.0, 4, round(first), RATE)
    ed.check(abs(bar["boundary_sample"] - section["starts_at_sample"] - expected) <= 1, f"the next bar from the grid: {bar} {expected}")
    transitions = [l for l in lines_of(session, "mixer") if l.get("action") == "transition" and l.get("cause") == "music"]
    ed.check_eq(len(transitions), 1, f"{transitions}")
    m = transitions[0]["sample"]
    ed.check(m <= bar["boundary_sample"] < m + FRAME, f"in the frame whose mix holds the switch: {m} {bar['boundary_sample']}")
    # never three music voices at once
    events = sorted([(v["sample"] + v.get("delay_frames", 0), 1) for v in voices if v["owner"] == "music"] +
                    [(s["sample"], -1) for s in lines_of(session, "voice_stop") if s["voice"] in {v["voice"] for v in voices}])
    alive = peak = 0
    for _, step in events:
        alive += step
        peak = max(peak, alive)
    ed.check(peak <= 2, f"two voices at most: {peak}")


# AC4 (R12): beat and bar stingers on their samples at 120 bpm with first_beat_ms 12.4; without tempo: now, warned once
async def test_stinger(ed: Editor):
    scene = await scene_with(ed, "Stinger", """    if f == 1 then Audio.playMusic("music/menu.wav") end
    if f == 37 then Audio.playStinger("music/sting.wav", "beat") end
    if f == 41 then Audio.playStinger("music/sting.wav", "bar") end
    if f == 150 then Audio.playMusic("music/free.wav") end
    if f == 160 then Audio.playStinger("music/sting.wav") end
    if f == 170 then Audio.playStinger("music/sting.wav") end""")
    await ed.console_since(None)
    session = await capture(ed, scene, 200)
    section = music_lines(session, "section")[0]
    stingers = music_lines(session, "stinger")
    voices = [v for v in music_voices(session) if v["clip"] == "music/sting.wav"]
    ed.check_eq([s["sync"] for s in stingers], ["beat", "bar", "now", "now"], f"{stingers}")
    first = round(FIRST_BEAT_MS * RATE / 1000.0)
    for line, voice in zip(stingers[:2], voices[:2]):
        ed.check_eq(line["boundary_sample"], voice["sample"] + voice.get("delay_frames", 0), f"{line} {voice}")
        position = line["sample"] - section["starts_at_sample"]
        expected = ref.music_boundary(position, line["sync"], 120.0, 4, first, RATE)
        ed.check(abs(line["boundary_sample"] - section["starts_at_sample"] - expected) <= 1, f"{line['sync']}: {line} {expected}")
    ed.check_eq([v.get("delay_frames", 0) for v in voices[2:]], [0, 0], "without tempo it plays now")
    warned = [e for e in await ed.console_since() if "the music has no tempo" in e["message"]]
    ed.check_eq(len(warned), 1, f"one warning: {warned}")


# AC5 (R13, R14): 8 s at 120 bpm, 4/4: 16 beats in order with bars 1-4, each in the frame whose mix holds it;
# getMusicPosition agrees with the grid; get_play_state shows the music
async def test_callbacks(ed: Editor):
    scene = await scene_with(ed, "Beats", """    if f == 1 then
        Audio.playMusic("music/game.wav")
        Audio.onMusicBeat:add("marks", function(bar, beat) Audio.mark("beat " .. bar .. " " .. beat) end)
        Audio.onMusicSection:add("marks", function(section, clip) Audio.mark("section " .. section .. " " .. clip) end)
    end
    if f == 100 then
        local p = Audio.getMusicPosition()
        Audio.mark(string.format("position %d %d %d %.6f", p.bar, p.beat, p.section, p.beatFraction))
    end""")

    async def during():
        await ed.call("wait", frames=60)
        state = await ed.call("get_play_state")
        music = state["audio"]["music"]
        ed.check_eq((music["track"], music["section"], music["bpm"]), ("music/game.wav", 1, 120), f"{music}")
    session = await capture(ed, scene, 420, during)
    section = music_lines(session, "section")[0]
    marks = [l for l in lines_of(session, "marker")]
    beats = [m for m in marks if m["label"].startswith("beat ")]
    ed.check_eq([m["label"] for m in beats], [f"beat {k // 4 + 1} {k % 4 + 1}" for k in range(16)], "16 beats in order")
    grid = ref.beat_frames(120.0, 0, 0, RATE, 8 * RATE)
    for mark, frame in zip(beats, grid):
        sample = section["starts_at_sample"] + frame
        ed.check(mark["sample"] <= sample < mark["sample"] + FRAME, f"in the frame that mixes it: {mark} {sample}")
    ed.check([m for m in marks if m["label"] == "section 1 music/game.wav"], "the section's callback")
    position = next(m for m in marks if m["label"].startswith("position "))
    _, bar, beat, number, fraction = position["label"].split()
    beats_in = (position["sample"] - section["starts_at_sample"]) / (RATE * 0.5)
    ed.check_eq((int(bar), int(beat), int(number)), (int(beats_in) // 4 + 1, int(beats_in) % 4 + 1, 1), f"{position}")
    ed.check(abs(float(fraction) - (beats_in - int(beats_in))) < 1e-3, f"{position} {beats_in}")
    STATE["beats"] = session


# AC6 (R15): the music lines are in the report; the silence check counts music voices
async def test_report_and_silence(ed: Editor):
    report = await ed.call("audio_session", action="report", image=False)
    actions = [l["action"] for l in report.get("music", [])]
    ed.check("play" in actions and "section" in actions, f"the report lists the music lines: {actions}")
    result = await ed.call("audio_session", action="check", checks=["silence"])
    silence = next(c for c in result["checks"] if c["id"] == "silence")
    ed.check(silence["status"] != "FAIL", f"music counts as sound: {silence}")
