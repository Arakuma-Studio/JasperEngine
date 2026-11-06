"""S27 Recorder for agents (docs/specs/S27-editor-tools.md AC21-AC26; jasper://docs/editor-tools).

A 2D scene of 640x360 points (canvas = output size) with a red square moved by a script at 60
points per game second. record_game:
- png 90 frames at 30 fps: exactly 90 PNGs of 640x360, the sidecar says 90, and the square in
  frame k sits k x 60/30 points from frame 0 (frame k is game time k/30);
- the same recording twice gives identical bytes (fixed timestep);
- Frame Interval [30, 59]: 30 frames named frame_0030..frame_0059, and Exit Play Mode stops Play;
- mp4 through FFmpeg (when the host has one with an H.264 encoder): ffprobe says h264, 90 frames,
  30 fps, 640x360, BT.709; mov is ProRes 422 HQ (yuv422p10le); with audio an AAC stream of 3 s;
- without FFmpeg (JASPER_FFMPEG=/nonexistent) a movie fails pointing at Preferences, png works;
- throughput: 1080p30 PNG keeps >= 10 recorded frames per wall second (perf_slack applies) and the
  queue never holds more than 8 frames;
- the editor_tools group is not listed until tool_groups enables it.
"""

import array
import json
import math
import shutil
import subprocess
from pathlib import Path

import harness
import pngdiff
from audio_refs import capture_sessions, read_float_wav, write_tone
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Recorder"}
TIMEOUT = 1800

SPEED = 60
MOVER = """---@class Mover : ScriptInstance
local Mover = { properties = {} }

function Mover:init()
    self.go = GameObject(self.scene, self.entity)
    self.time = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Mover:onUpdate()
    -- the game time since this Play started (fixed 1/fps steps while recording)
    self.go.position = Vector3(SPEED_START + self.time * SPEED, 180, 0)
    self.time = self.time + Engine.deltatime
end

return Mover
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("set_project_settings", values={"graphics.canvas_width": 640, "graphics.canvas_height": 360,
                                                   "graphics.scaling_mode": "letterbox"})
    made = await ed.call("create_scene", name="Rec", type="2D", path="scenes/Rec")
    S["scene"] = made["scene"]["id"]
    square = (await ed.call("create_entity", scene=S["scene"], type="sprite", name="Square", position=[0, 180, 0],
                            values={"sprite": {"width": 32, "height": 32},
                                    "mesh": {"submeshes[0].material.baseColorFactor": [1, 0, 0, 1]}}))["entity_id"]
    await ed.call("create_script", kind="lua", name="Mover", content=MOVER.replace("SPEED_START", "0").replace("SPEED", str(SPEED)), scene=S["scene"], entity=square)
    await ed.call("save_scene", scene=S["scene"])
    return S


async def record(ed: Editor, **args) -> dict:
    args.setdefault("timeout_seconds", 900)
    r = await ed.call("record_game", **args)
    state = await ed.call("get_play_state")
    if state.get("state") not in (None, "stopped"):
        await ed.call("play", action="stop")
    return r


def red_centre(png: bytes) -> tuple[float, int, int]:
    width, height, rgba = pngdiff.decode(png)
    xs = []
    for y in range(height):
        row = y * width * 4
        for x in range(width):
            i = row + x * 4
            if rgba[i] > 200 and rgba[i + 1] < 60 and rgba[i + 2] < 60:
                xs.append(x)
    return (sum(xs) / len(xs) if xs else -1.0), width, height


async def test_tool_group(ed: Editor):
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("record_game" not in listed, "editor_tools is not listed by default")
    ed.check("list_project_templates" in listed and "import_project" in listed, "the hub tools are core")
    await ed.call("tool_groups", enable=["editor_tools"])
    listed = {t.name for t in await ed.tools(groups=None)}
    for name in ("record_game", "sample_view_color", "edit_swatches", "list_curve_presets", "evaluate_curve"):
        ed.check(name in listed, f"{name} listed once enabled")


async def test_png_sequence(ed: Editor):
    await setup(ed)
    out = ed.scratch.root / "rec-png"
    r = await record(ed, frames=90, fps=30, format="png", width=640, height=360, output=str(out))
    ed.check_eq(r["frames"], 90, f"{json.dumps(r)[:600]}")
    files = sorted(out.rglob("*.png"))
    ed.check_eq(len(files), 90, "90 PNGs")
    ed.check_eq(len(r["files"]), 90)
    ed.check_eq(files[0].name, "frame_0000.png")
    ed.check_eq(files[-1].name, "frame_0089.png")
    sidecar = json.loads(Path(r["sidecar"]).read_text())
    ed.check_eq((sidecar["frames"], sidecar["fps"], sidecar["width"], sidecar["height"]), (90, 30, 640, 360))
    ed.check(sidecar["maxQueued"] <= 8, f"the queue held at most 8: {sidecar}")
    centres = {}
    for k in (0, 15, 45, 89):
        c, w, h = red_centre(files[k].read_bytes())
        ed.check((w, h) == (640, 360), f"frame {k} is 640x360: {w}x{h}")
        ed.check(c >= 0, f"frame {k} shows the square")
        centres[k] = c
    for k in (15, 45, 89):
        expected = k * SPEED / 30
        ed.check(abs((centres[k] - centres[0]) - expected) <= 1.5,
                 f"frame {k} is game time {k}/30: the square moved {centres[k] - centres[0]:.2f}, expected {expected:.2f}")
    S["first"] = [f.read_bytes() for f in files]


async def test_deterministic(ed: Editor):
    await setup(ed)
    if "first" not in S:
        await test_png_sequence(ed)
    out = ed.scratch.root / "rec-png-again"
    r = await record(ed, frames=90, fps=30, format="png", width=640, height=360, output=str(out))
    files = sorted(out.rglob("*.png"))
    ed.check_eq(len(files), 90, f"{r}")
    same = sum(1 for a, b in zip(S["first"], (f.read_bytes() for f in files)) if a == b)
    ed.check_eq(same, 90, "two recordings at a fixed fps are byte-identical")


async def test_frame_interval(ed: Editor):
    await setup(ed)
    out = ed.scratch.root / "rec-interval"
    try:
        r = await ed.call("record_game", frames=30, start_frame=30, fps=30, format="png", width=640, height=360,
                          output=str(out), exit_play=True, timeout_seconds=900)
        names = sorted(f.name for f in out.rglob("*.png"))
        ed.check_eq(len(names), 30, f"{r}")
        ed.check_eq((names[0], names[-1]), ("frame_0030.png", "frame_0059.png"))
        ed.check_eq(r["startFrame"], 30)
        state = await ed.call("get_play_state")
        ed.check_eq(state["state"], "stopped", f"Exit Play Mode stopped Play: {state}")
    finally:
        state = await ed.call("get_play_state")
        if state["state"] != "stopped":
            await ed.call("play", action="stop")


def ffprobe(path: Path) -> dict:
    out = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-count_frames", "-of", "json", str(path)],
                         capture_output=True, text=True, timeout=120)
    return json.loads(out.stdout or "{}")


def has_h264() -> bool:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg or not shutil.which("ffprobe"):
        return False
    encoders = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=60).stdout
    return any(name in encoders for name in ("libx264", "h264_nvenc", "h264_qsv", "h264_amf", "libopenh264"))


async def test_mp4(ed: Editor):
    if not has_h264():
        harness.skip("no ffmpeg/ffprobe with an H.264 encoder on this host")
    await setup(ed)
    movie = ed.scratch.root / "rec.mp4"
    r = await record(ed, frames=90, fps=30, format="mp4", width=640, height=360, output=str(movie))
    ed.check(movie.is_file() and r["movie"].endswith("rec.mp4"), f"{r}")
    streams = ffprobe(movie)["streams"]
    video = [s for s in streams if s["codec_type"] == "video"][0]
    ed.check_eq(video["codec_name"], "h264")
    ed.check_eq((video["width"], video["height"]), (640, 360))
    ed.check_eq(int(video["nb_read_frames"]), 90)
    ed.check_eq(video["r_frame_rate"], "30/1")
    ed.check_eq(video.get("color_space"), "bt709", f"BT.709 tagged: {video}")
    ed.check(Path(str(movie) + ".recording.json").is_file(), "the sidecar")


async def test_mov(ed: Editor):
    if not has_h264():
        harness.skip("no ffmpeg/ffprobe with an H.264 encoder on this host")
    await setup(ed)
    movie = ed.scratch.root / "rec.mov"
    r = await record(ed, frames=30, fps=30, format="mov", width=640, height=360, output=str(movie))
    ed.check(movie.is_file(), f"{r}")
    video = [s for s in ffprobe(movie)["streams"] if s["codec_type"] == "video"][0]
    ed.check_eq((video["codec_name"], video["pix_fmt"], int(video["nb_read_frames"])), ("prores", "yuv422p10le", 30),
                f"ProRes 422 HQ: {video}")


async def test_mp4_audio(ed: Editor):
    if not has_h264():
        harness.skip("no ffmpeg/ffprobe with an H.264 encoder on this host")
    await setup(ed)
    movie = ed.scratch.root / "rec-audio.mp4"
    await record(ed, frames=90, fps=30, format="mp4", width=640, height=360, output=str(movie), audio=True)
    streams = ffprobe(movie)["streams"]
    audio = [s for s in streams if s["codec_type"] == "audio"]
    ed.check(audio and audio[0]["codec_name"] == "aac", f"an AAC stream: {streams}")
    duration = float(audio[0]["duration"])
    ed.check(abs(duration - 3.0) <= 1024 / 48000 + 1e-3, f"3 s of audio (one AAC frame of slack): {duration}")


async def test_movie_audio_matches_capture():
    """S35 AC6: one tap: a movie's PCM audio is the capture's master.wav, quantised the same way, frame
    for frame; the mixer's effect is in it (MOV: its audio is PCM, an MP4's AAC is lossy)."""
    if not has_h264():
        harness.skip("no ffmpeg/ffprobe with an H.264 encoder on this host")
    async with Editor(create=True, name="Recorder Capture") as ed:
        await ed.call("tool_groups", enable=["editor_tools"])
        await ed.call("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "Music"}])
        await ed.call("audio_mixer", action="edit", ops=[{"op": "add_effect", "group": "Music", "type": "lowpass", "params": {"cutoff": 200}}])
        write_tone(ed.project / "sounds/tone.wav", seconds=2.0, frequency=4000.0, amplitude=0.5, rate=48000)
        await ed.call("sync_files")
        made = await ed.call("create_scene", name="Rec", type="2D", path="scenes/Rec")
        scene = made["scene"]["id"]
        await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
        await ed.call("set_properties", scene=scene, entity="Source", values={"audiosource": {
            "filename": "sounds/tone.wav", "looping": True, "outputGroup": "Music", "state": "playing"}})
        await ed.call("save_scene", scene=scene)

        movie = ed.scratch.root / "rec-capture.mov"
        await ed.call("record_game", frames=60, fps=30, format="mov", width=64, height=64, output=str(movie), audio=True, timeout_seconds=900)
        if (await ed.call("get_play_state"))["state"] != "stopped":
            await ed.call("play", action="stop")
        pcm = ed.scratch.root / "rec-capture.pcm"
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(movie), "-f", "s16le", "-acodec", "copy", str(pcm)], check=True)
        movie_audio = array.array("h")
        movie_audio.frombytes(pcm.read_bytes())
        ed.check_eq(len(movie_audio) // 2, 60 * 1600, "2 s of audio at 30 fps: 1600 frames a frame")

        session = capture_sessions(ed.project)[-1]
        _, channels, master = read_float_wav(session / "master.wav")
        ed.check_eq(channels, 2, "stereo")
        ed.check(len(master) >= len(movie_audio), f"the capture has them all: {len(master)} of {len(movie_audio)}")

        def quantised(x: float) -> int:
            scaled = max(-1.0, min(1.0, x)) * 32767.0
            return int(math.floor(scaled + 0.5)) if scaled >= 0 else int(math.ceil(scaled - 0.5))

        different = next((i for i, v in enumerate(movie_audio) if v != quantised(master[i])), -1)
        ed.check_eq(different, -1, "the movie's samples are the capture's")
        peak = max(abs(v) for v in movie_audio[len(movie_audio) // 2:]) / 32767.0
        ed.check(0.0 < peak < 0.05, f"the group's low-pass is in the movie: a 4 kHz tone reads {peak}")


async def test_no_ffmpeg():
    async with Editor(create=True, name="No FFmpeg", env={"JASPER_FFMPEG": "/nonexistent"}) as ed:
        await ed.call("tool_groups", enable=["editor_tools"])
        await ed.call("create_scene", name="Rec", type="2D", path="scenes/Rec")
        msg = await ed.call("record_game", frames=5, format="mp4", width=64, height=64, expect_error=True)
        ed.check("Preferences" in msg and "png" in msg, f"the movie fails with the hint: {msg}")
        out = ed.scratch.root / "png"
        r = await ed.call("record_game", frames=5, format="png", width=64, height=64, output=str(out), timeout_seconds=600)
        ed.check_eq(r["frames"], 5, "png needs no FFmpeg")


async def test_throughput(ed: Editor):
    await setup(ed)
    out = ed.scratch.root / "rec-1080"
    r = await record(ed, frames=60, fps=30, format="png", width=1920, height=1080, output=str(out))
    ed.check_eq(r["frames"], 60)
    fps = r["frames"] / max(r["wallSeconds"], 1e-6)
    ed.check(r["maxQueued"] <= 8, f"queue <= 8: {r['maxQueued']}")
    print(f"[recorder] 1080p PNG: {fps:.1f} recorded frames per wall second (max queued {r['maxQueued']})")
    # R37's budget is enforced on the benchmark run (harness.perf_slack), printed otherwise
    if harness.perf_slack() > 0:
        ed.check(fps * harness.perf_slack() >= 10, f"1080p PNG >= 10 frames per wall second: {fps:.1f}")
    if has_h264():
        movie = ed.scratch.root / "rec-1080.mp4"
        r = await record(ed, frames=60, fps=30, format="mp4", width=1920, height=1080, output=str(movie))
        fps = r["frames"] / max(r["wallSeconds"], 1e-6)
        print(f"[recorder] 1080p H.264: {fps:.1f} recorded frames per wall second (max queued {r['maxQueued']})")
        ed.check(r["frames"] == 60 and r["maxQueued"] <= 8, f"{r}")
        if harness.perf_slack() > 0:
            ed.check(fps * harness.perf_slack() >= 20, f"1080p H.264 >= 20 frames per wall second: {fps:.1f}")
