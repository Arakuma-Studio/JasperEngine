"""A prebuilt player hears as Play does (S30 R1, the audio block of game.json), and a --headless run's
game logic is a normal run's (S30 R17).

The same script, in a project with a mixer (a Music group at -12 dB) and non-default Project Settings
> Audio, runs in editor Play and in a real headless prebuilt player. It reports the groups, whether
its clip plays through Music and the Music meter: Play and the player must agree. Without the
project's mixer in game.json the player has only an implicit Master; with --headless muted, the
clip would not play and the meter would stay silent.

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (build_cache).
"""

import asyncio
import json
import math
import os
import re
import struct
import subprocess
from pathlib import Path

import build_cache
from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Prebuilt Audio", "env": build_cache.player_env("glcore", "release")}

SCRIPT = """local Probe = {}
function Probe:start()
    local sound = AudioSource(self.scene, self.entity)
    sound.outputGroup = "Music"
    sound:play()
    self.frames = 0
    RegisterEngineEvent(self, 'onUpdate')
end
function Probe:onUpdate(dt)
    self.frames = self.frames + 1
    if self.frames == 60 then
        local sound = AudioSource(self.scene, self.entity)
        print(string.format("audioprobe groups=%s playing=%s output=%s meter=%.1f",
            table.concat(AudioMixer.getGroupNames(), ","), tostring(sound:isPlaying()), sound.outputGroup,
            AudioMixer.getMeter("Music")))
    end
end
return Probe
"""

PROBE = re.compile(r"audioprobe groups=(\S+) playing=(\S+) output=(\S+) meter=(-?[\d.]+)")


def write_tone(path, seconds=4.0, frequency=1000.0, amplitude=0.5, rate=44100):
    samples = int(seconds * rate)
    data = b"".join(struct.pack("<h", int(round(amplitude * 32767 * math.sin(2 * math.pi * frequency * i / rate))))
                    for i in range(samples))
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    path.write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


def probe_of(lines):
    for line in lines:
        found = PROBE.search(line)
        if found:
            return found.group(1), found.group(2), found.group(3), float(found.group(4))
    return None


async def test_a_prebuilt_headless_player_hears_as_play_does(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real player (the run builds its prebuilt runtime once)")
    write_tone(ed.project / "music.wav")
    r = await ed.call_raw("audio_mixer", {"action": "edit", "ops": [
        {"op": "add_group", "name": "Music"},
        {"op": "set_group", "name": "Music", "volume": -12},
    ]})
    ed.check(not r.is_error, f"the project's mixer: {r.text[:300]}")
    await ed.call("set_project_settings", values={"audio.max_real_voices": 20, "audio.max_voices": 100, "audio.sample_rate": 48000})
    await ed.call("create_entity", name="Probe", type="audio_source")
    await ed.call("set_properties", entity="Probe", values={"audiosource": {"filename": "music.wav", "looping": True}})
    await ed.call("create_script", kind="lua", name="Probe", content=SCRIPT, entity="Probe")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])

    await ed.console_since(None)
    await ed.call("play", action="start", auto_pause=False, fixed_fps=60)
    try:
        heard = None
        console: list[str] = []
        for _ in range(40):
            await ed.call("wait", seconds=0.25)
            console += [e["message"] for e in await ed.console_since()]
            heard = probe_of(console)
            if heard:
                break
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    print(f"[audio] Play: {heard}")
    ed.check(heard, f"Play reports its audio: {console[-20:]}")
    ed.check_eq(heard[0], "Master,Music", "Play's groups")
    ed.check_eq(heard[1], "true", "it plays in Play")
    ed.check(heard[3] > -40, f"Music is heard in Play: {heard}")

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-prebuilt-audio"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    while job["status"] == "running":
        await asyncio.sleep(2)
        job = await ed.call("get_job", job_id=job["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    audio = json.loads((out / "game.json").read_text())["audio"]
    ed.check_eq((audio["maxRealVoices"], audio["maxVoices"], audio["sampleRate"]), (20, 100, 48000), "Project Settings > Audio")

    # the player itself, as a person runs it (run_build refuses a prebuilt player: build_project
    # records only what a compile produced)
    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--after-ready", "--quit-after", "120"],
        cwd=out, capture_output=True, text=True, timeout=600)
    log = run.stdout + run.stderr
    ed.check_eq(run.returncode, 0, f"the player quits after its frames: {log[-2000:]}")
    played = probe_of(log.splitlines())
    print(f"[audio] player: {played}")
    ed.check(played, f"the player reports its audio: {log[-2000:]}")
    ed.check_eq(played[:3], heard[:3], "the groups, the clip playing and its output group, as in Play")
    # the same tone through the same fader; a meter reads the last blocks' peak
    ed.check(abs(played[3] - heard[3]) <= 3.0, f"the Music meter, as in Play: {played[3]} against {heard[3]}")
