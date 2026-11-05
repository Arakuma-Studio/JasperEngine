"""S62 Recorder integrity (docs/specs/S62-recorder-integrity.md): a recording that reports success is
a good recording.

- A write that fails (JASPER_TEST_RECORDER_FAIL, or a real full disk on a small tmpfs) stops the
  recording: the job is failed and says what and why, Play goes on, the output path holds nothing,
  no temporary file stays, and what was recorded before is kept as <name>.partial.<ext> only when
  it decodes (AC1, AC2).
- The output appears under its name only once complete (R3).
- A free-space shortage warns with the estimate and does not refuse (R4).
- A normal recording is what it was: frames, audio length, the hash of its decoded audio (AC3).
- No recorded frame shows a stand-in (R5, AC4): a static scene whose script changes the look at
  game frame AT must record the old look before AT and the new one from AT on, with the frame's
  pipelines or textures forced to be pending (JASPER_ASYNC_SHADERS_DELAY_MS,
  JASPER_TEXTURE_LOAD_DELAY_MS) and without (a load that ends within the frame it started in).
"""

import hashlib
import json
import math
import os
import shutil
import stat
import struct
import subprocess
from pathlib import Path

import harness
import pngdiff
from harness import Editor

CI = True
TIMEOUT = 1800

WIDTH, HEIGHT = 320, 180
AT = 10              # the game frame the script changes the look at
FRAMES = 30

# The decoded PCM of test_normal_recording's MOV (no lossy codec, so no FFmpeg version in it), from
# the build before S62 (main at f9beac071): the recorder's rewrite did not touch a normal recording
REFERENCE_AUDIO_SHA256 = "408859a6ce060fa90c3699b5ff1a6b5b591250bc4dfd428f102bdd367b53c500"
REFERENCE_AUDIO_BYTES = 576000


def has_h264() -> bool:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg or not shutil.which("ffprobe"):
        return False
    encoders = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=60).stdout
    return any(name in encoders for name in ("libx264", "h264_nvenc", "h264_qsv", "h264_amf", "libopenh264"))


def need_ffmpeg() -> None:
    if not has_h264():
        harness.skip("no ffmpeg/ffprobe with an H.264 encoder on this host")


def write_tone(path: Path, seconds: float = 4.0, rate: int = 48000) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for i in range(int(seconds * rate)):
        value = int(round(0.5 * 32767 * math.sin(2 * math.pi * 1000.0 * i / rate)))
        frames += struct.pack("<hh", value, value)
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " +
                     struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16) + b"data" + struct.pack("<I", len(frames)) + bytes(frames))


async def small_scene(ed: Editor, audio: bool = False) -> None:
    await ed.call("tool_groups", enable=["editor_tools"])
    await ed.call("create_scene", name="Rec", type="2D", path="scenes/Rec")
    if audio:
        write_tone(ed.project / "tone.wav")
        await ed.call("create_entity", name="Tone", type="audio_source")
        await ed.call("set_properties", entity="Tone", values={"audiosource": {"filename": "tone.wav", "state": "playing"}})
    await ed.call("save_scene")


async def record_failed(ed: Editor, **args) -> tuple[str, dict]:
    """A record_game that must fail: its message and its structured result."""
    args.setdefault("timeout_seconds", 600)
    args.setdefault("width", 64)
    args.setdefault("height", 64)
    r = await ed.call_raw("record_game", args, timeout=900)
    ed.check(r.is_error, f"the job failed: {r.text[:800]}")
    return r.text, r.structured or {}


def leftovers(folder: Path, name: str) -> list[str]:
    """The temporary files of a recording of `name` still in `folder`."""
    return sorted(p.name for p in folder.iterdir() if p.name.startswith("." + name))


async def check_play_goes_on(ed: Editor) -> None:
    state = await ed.call("get_play_state")
    ed.check_eq(state.get("state"), "playing", f"Play goes on after the failure: {state}")
    await ed.call("play", action="stop")


def decoded_frames(path: Path) -> int:
    out = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames",
                          "-of", "csv=p=0", str(path)], capture_output=True, text=True, timeout=120)
    return int(out.stdout.strip() or 0)


# ---- R1, R2: failures at each write


async def test_audio_write_failure_keeps_the_video():
    need_ffmpeg()
    async with Editor(create=True, name="Fail Audio", env={"JASPER_TEST_RECORDER_FAIL": "audio@20"}) as ed:
        await small_scene(ed, audio=True)
        out = ed.scratch.root / "movies"
        movie = out / "take.mp4"
        text, data = await record_failed(ed, frames=60, format="mp4", output=str(movie), audio=True)
        ed.check("disk full" in text and "MB free" in text, f"it says why: {text}")
        ed.check("audio" in text, f"it says what: {text}")
        ed.check("Play is still running" in text and "play action stop" in text, f"the text says what to do (all an error shows the model): {text}")
        ed.check_eq(data.get("failed_step"), "audio", f"{data}")
        ed.check(not movie.exists(), "no file at the output path")
        partial = out / "take.partial.mp4"
        ed.check(partial.is_file() and data.get("partial") == partial.as_posix(), f"the partial is kept and named: {data}")
        ed.check(partial.name in text and "no audio" in text, f"the message says it is kept, without audio: {text}")
        ed.check_eq(decoded_frames(partial), data.get("partial_frames"), "the partial decodes to the frames it reports")
        ed.check(data["partial_frames"] >= 20, f"it has the frames before the failure: {data}")
        ed.check_eq(leftovers(out, "take"), [], "no temporary file stays")
        await check_play_goes_on(ed)
        # the Console says the same
        console = json.dumps(await ed.call("read_console"))
        ed.check("Recording failed" in console and "disk full" in console, f"the Console says it: {console[-800:]}")


async def test_audio_header_failure():
    need_ffmpeg()
    async with Editor(create=True, name="Fail Header", env={"JASPER_TEST_RECORDER_FAIL": "audio-header"}) as ed:
        await small_scene(ed, audio=True)
        out = ed.scratch.root / "movies"
        movie = out / "take.mp4"
        text, data = await record_failed(ed, frames=10, format="mp4", output=str(movie), audio=True)
        ed.check_eq(data.get("failed_step"), "audio", f"{text}")
        ed.check(not movie.exists(), "no file at the output path")
        ed.check((out / "take.partial.mp4").is_file(), f"the video is kept: {text}")
        ed.check_eq(leftovers(out, "take"), [], "no temporary file stays")


async def test_video_write_failure_with_a_finished_encoder():
    need_ffmpeg()
    async with Editor(create=True, name="Fail Video", env={"JASPER_TEST_RECORDER_FAIL": "video@20"}) as ed:
        await small_scene(ed)
        out = ed.scratch.root / "movies"
        movie = out / "take.mov"
        text, data = await record_failed(ed, frames=200, format="mov", output=str(movie))
        ed.check("disk full" in text, f"it says why: {text}")
        ed.check_eq(data.get("failed_step"), "video", f"{data}")
        ed.check(not movie.exists(), "no file at the output path")
        partial = out / "take.partial.mov"
        ed.check(partial.is_file(), f"the frames encoded so far are kept: {text}")
        ed.check_eq(decoded_frames(partial), 20, "exactly the frames before the failure")
        ed.check_eq(leftovers(out, "take"), [], "no temporary file stays")
        await check_play_goes_on(ed)


async def test_video_failure_undecodable_is_not_kept():
    need_ffmpeg()
    async with Editor(create=True, name="Kill Encoder", env={"JASPER_TEST_RECORDER_FAIL": "encoder-kill@20"}) as ed:
        await small_scene(ed)
        out = ed.scratch.root / "movies"
        movie = out / "take.mp4"
        text, data = await record_failed(ed, frames=200, format="mp4", output=str(movie))
        ed.check_eq(data.get("failed_step"), "video", f"{data}")
        ed.check("partial" not in data, f"an MP4 without its index does not decode: nothing kept: {data}")
        ed.check("nothing" in text, f"the message says so: {text}")
        ed.check_eq(sorted(p.name for p in out.iterdir()), [], "the folder holds nothing: no movie, partial, sidecar or temporary")
        await check_play_goes_on(ed)


async def test_mux_failure_real_ffmpeg_exit():
    """A real non-zero exit of the second pass: a wrapper fails it, the first pass is the real FFmpeg."""
    need_ffmpeg()
    wrapper_dir = Path(os.environ.get("TMPDIR", "/tmp")) / f"recorder-wrapper-{os.getpid()}"
    wrapper_dir.mkdir(parents=True, exist_ok=True)
    wrapper = wrapper_dir / "ffmpeg"
    wrapper.write_text(f"""#!/bin/sh
for arg in "$@"; do
    case "$arg" in
        copy) echo "wrapper: mux refused" >&2; exit 3 ;;
    esac
done
exec {shutil.which('ffmpeg')} "$@"
""")
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IEXEC)
    try:
        async with Editor(create=True, name="Fail Mux", env={"JASPER_FFMPEG": str(wrapper)}) as ed:
            await small_scene(ed, audio=True)
            out = ed.scratch.root / "movies"
            movie = out / "take.mp4"
            text, data = await record_failed(ed, frames=10, format="mp4", output=str(movie), audio=True)
            ed.check_eq(data.get("failed_step"), "mux", f"{text}")
            ed.check("wrapper: mux refused" in text, f"FFmpeg's own words: {text}")
            ed.check(not movie.exists(), "no file at the output path")
            ed.check((out / "take.partial.mp4").is_file(), f"the video is kept: {text}")
            ed.check_eq(decoded_frames(out / "take.partial.mp4"), 10, "all its frames")
            ed.check_eq(leftovers(out, "take"), [], "no temporary file stays")
    finally:
        shutil.rmtree(wrapper_dir, ignore_errors=True)


async def test_image_sequence_failure():
    async with Editor(create=True, name="Fail PNG", env={"JASPER_TEST_RECORDER_FAIL": "png@12"}) as ed:
        await small_scene(ed)
        out = ed.scratch.root / "seq"
        text, data = await record_failed(ed, frames=100, format="png", output=str(out))
        ed.check_eq(data.get("failed_step"), "png", f"{text}")
        ed.check("disk full" in text, f"it says why: {text}")
        ed.check(not out.exists(), f"no file at the output path: {list(out.iterdir()) if out.exists() else ''}")
        partial = ed.scratch.root / "seq.partial"
        ed.check_eq(data.get("partial"), partial.as_posix(), f"{data}")
        names = sorted(p.name for p in partial.iterdir() if p.suffix == ".png")
        ed.check_eq(names, [f"frame_{k:04d}.png" for k in range(12)], "the frames before the failure, without a gap")
        for name in names:
            pngdiff.decode((partial / name).read_bytes())
        ed.check_eq(sorted(p.name for p in partial.iterdir() if p.name.startswith(".")), [], "no temporary file stays")
        await check_play_goes_on(ed)


async def test_failure_keeps_an_earlier_recording():
    """The chosen folder holds a recording already: a failed one leaves it as it was (R3)."""
    async with Editor(create=True, name="Keep PNG", env={"JASPER_TEST_RECORDER_FAIL": "png@3"}) as ed:
        await small_scene(ed)
        out = ed.scratch.root / "seq"
        out.mkdir()
        (out / "frame_0000.png").write_bytes(b"the earlier recording")
        await record_failed(ed, frames=10, format="png", output=str(out))
        ed.check_eq((out / "frame_0000.png").read_bytes(), b"the earlier recording", "the file is untouched")
        ed.check_eq(sorted(p.name for p in out.iterdir()), ["frame_0000.png"], "and nothing else appeared")


async def test_publish_failure():
    need_ffmpeg()
    async with Editor(create=True, name="Fail Publish", env={"JASPER_TEST_RECORDER_FAIL": "publish"}) as ed:
        await small_scene(ed)
        out = ed.scratch.root / "movies"
        movie = out / "take.mp4"
        text, data = await record_failed(ed, frames=5, format="mp4", output=str(movie))
        ed.check_eq(data.get("failed_step"), "publish", f"{text}")
        ed.check(not movie.exists(), "no file at the output path")
        ed.check_eq(leftovers(out, "take"), [], "no temporary file stays")


# ---- R3


async def test_output_appears_only_when_complete():
    """While the movie records its name does not exist: a temporary sibling holds it."""
    need_ffmpeg()
    async with Editor(create=True, name="Atomic") as ed:
        await small_scene(ed)
        out = ed.scratch.root / "movies"
        movie = out / "take.mp4"
        started = await ed.call("record_game", frames=100000, format="mp4", width=64, height=64, output=str(movie), wait=False)
        job = started["job_id"]
        for _ in range(100):
            if out.is_dir() and any(p.name.endswith(".je-tmp") and p.stat().st_size > 0 for p in out.iterdir()):
                break
            await ed.call("wait", frames=5, timeout_seconds=60)
        names = sorted(p.name for p in out.iterdir())
        ed.check(any(n.startswith(".take.mp4.") and n.endswith(".je-tmp") for n in names), f"a temporary sibling: {names}")
        ed.check(not movie.exists(), f"the movie's name is not there yet: {names}")
        await ed.call("cancel_job", job_id=job)
        for _ in range(200):
            state = await ed.call("get_job", job_id=job)
            if state.get("status") not in ("running", "pending"):
                break
            await ed.call("wait", frames=5, timeout_seconds=60)
        ed.check_eq(state.get("status"), "cancelled", f"{state}")
        ed.check(movie.is_file(), f"stopped: the movie is complete under its name: {sorted(p.name for p in out.iterdir())}")
        ed.check(decoded_frames(movie) > 0, "and it decodes")
        ed.check_eq(leftovers(out, "take"), [], "no temporary file stays")


# ---- R4


async def test_free_space_warning_does_not_refuse():
    async with Editor(create=True, name="Free Space", env={"JASPER_TEST_RECORDER_FAIL": "free=1"}) as ed:
        await small_scene(ed)
        out = ed.scratch.root / "seq"
        r = await ed.call("record_game", frames=200, format="png", width=640, height=360, output=str(out), timeout_seconds=600)
        ed.check_eq(r["frames"], 200, f"it did not refuse: {r}")
        warning = " ".join(r["warnings"])
        ed.check("needs about" in warning and "1 MB free" in warning, f"it warns with the estimate: {r['warnings']}")
        ed.check(r["estimated_bytes"] > 10 * 1024 * 1024 and r["free_bytes"] == 1024 * 1024, f"{r}")
        console = json.dumps(await ed.call("read_console"))
        ed.check("needs about" in console, f"the Console warns too: {console[-600:]}")
    async with Editor(create=True, name="Free Space Fits") as ed:
        await small_scene(ed)
        r = await ed.call("record_game", frames=2, format="png", width=64, height=64, output=str(ed.scratch.root / "seq"), timeout_seconds=600)
        ed.check(not any("needs about" in w for w in r["warnings"]), f"no warning when it fits: {r['warnings']}")


# ---- R6: nothing happens without the hook, and AC3


async def test_normal_recording():
    need_ffmpeg()
    async with Editor(create=True, name="Normal") as ed:
        # The reference is a recording's sound with no effect in the way, so this is a project as older
        # editors made them (S38 R9): a new project's Master limiter and linear output stage change the
        # samples on purpose. This also pins that such a project sounds exactly as it did.
        await ed.call("set_project_settings", values={"audio.mixer": "", "audio.output_stage": "roundoff",
                                                      "audio.ios_session_category": "default"})
        await small_scene(ed, audio=True)
        out = ed.scratch.root / "movies"
        movie = out / "take.mov"
        r = await ed.call("record_game", frames=90, fps=30, format="mov", width=64, height=64, output=str(movie), audio=True, timeout_seconds=600)
        ed.check_eq((r["frames"], decoded_frames(movie)), (90, 90), f"frame count: {r}")
        ed.check(Path(r["sidecar"]).is_file() and r["sidecar"].endswith("take.mov.recording.json"), f"the sidecar: {r}")
        ed.check(not r.get("warnings"), f"no warning: {r.get('warnings')}")
        pcm = subprocess.run(["ffmpeg", "-v", "error", "-i", str(movie), "-map", "0:a:0", "-f", "s16le", "-acodec", "pcm_s16le", "-"],
                             capture_output=True, timeout=120).stdout
        digest = hashlib.sha256(pcm).hexdigest()
        print(f"[recorder] decoded audio: {len(pcm)} bytes, sha256 {digest}")
        ed.check_eq(len(pcm), REFERENCE_AUDIO_BYTES, "3 s of 48 kHz 16-bit stereo")
        ed.check(any(b != 0 for b in pcm[:48000]), "the tone is in it")
        ed.check_eq(digest, REFERENCE_AUDIO_SHA256, "the decoded audio is the reference's")
        ed.check_eq(sorted(p.name for p in out.iterdir()), ["take.mov", "take.mov.recording.json"], "nothing else in the folder")
        # the same with MP4 and its AAC: the length within one AAC frame
        mp4 = out / "take.mp4"
        await ed.call("play", action="stop")
        await ed.call("record_game", frames=90, fps=30, format="mp4", width=64, height=64, output=str(mp4), audio=True, timeout_seconds=600)
        info = json.loads(subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(mp4)], capture_output=True, text=True, timeout=120).stdout)
        audio = [s for s in info["streams"] if s["codec_type"] == "audio"][0]
        ed.check(abs(float(audio["duration"]) - 3.0) <= 1024 / 48000 + 1e-3, f"3 s of AAC: {audio['duration']}")
        ed.check_eq(decoded_frames(mp4), 90, "90 frames")


# ---- AC2: a real full disk


def small_tmpfs(ed: Editor) -> Path:
    """A small real tmpfs to fill: the container's /dev/shm (64 MB by default), else skip."""
    shm = Path("/dev/shm")
    if not shm.is_dir() or not os.access(shm, os.W_OK):
        harness.skip("no writable /dev/shm")
    space = shutil.disk_usage(shm)
    if space.total > 1024 * 1024 * 1024 or space.free < 8 * 1024 * 1024:
        harness.skip(f"/dev/shm is not a small empty tmpfs: {space}")
    folder = shm / f"recorder-{os.getpid()}"
    folder.mkdir()
    return folder


def fill(folder: Path, leave_mb: int) -> None:
    free = shutil.disk_usage(folder).free
    (folder / "ballast").write_bytes(b"\0" * (free - leave_mb * 1024 * 1024))


async def test_real_full_disk():
    need_ffmpeg()
    async with Editor(create=True, name="Full Disk") as ed:
        folder = small_tmpfs(ed)
        try:
            fill(folder, 2)
            await small_scene(ed, audio=True)
            out = folder / "movies"
            for name, fmt, audio in (("audio", "mp4", True), ("video", "mov", False)):
                movie = out / f"{name}.{fmt}"
                # (ProRes of noise-free frames: 2 MB fill within a few hundred frames at this size)
                text, data = await record_failed(ed, frames=20000, format=fmt, width=640, height=360, output=str(movie), audio=audio)
                print(f"[recorder] full disk, {name}: {text}")
                ed.check("disk full" in text and "MB free" in text, f"it says why: {text}")
                ed.check(not movie.exists(), f"no file at the output path: {sorted(p.name for p in out.iterdir())}")
                ed.check_eq(leftovers(out, name), [], "no temporary file stays")
                for kept in out.glob(f"{name}.partial*"):
                    if kept.suffix == fmt:
                        ed.check(decoded_frames(kept) > 0, f"a kept partial decodes: {kept}")
                await check_play_goes_on(ed)
            seq = out / "seq"
            text, data = await record_failed(ed, frames=20000, format="png", width=640, height=360, output=str(seq))
            print(f"[recorder] full disk, png: {text}")
            ed.check("disk full" in text, f"it says why: {text}")
            ed.check(not seq.exists(), "no file at the output path")
            await check_play_goes_on(ed)
        finally:
            shutil.rmtree(folder, ignore_errors=True)


# ---- R5: no stand-in frames

FLIP = """---@class Flip : ScriptInstance
local Flip = { properties = {} }

function Flip:init()
    self.mesh = Mesh(self.scene, self.entity)
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Flip:onUpdate()
    if self.frame == AT then
        CHANGE
    end
    self.frame = self.frame + 1
end

return Flip
"""
TEXTURE_CHANGE = 'self.mesh:setTexture("assets/textures/green.png")'
VARIANT_CHANGE = """local m = self.mesh:getMaterial()
        m.clearcoatEnabled = true
        m.clearcoatFactor = 1.0
        m.clearcoatRoughness = 0.02
        m.metallicFactor = 0.9
        self.mesh:setMaterial(m)"""


def write_image(ed: Editor, name: str, rgb: tuple) -> str:
    path = f"assets/textures/{name}.png"
    (ed.project / "assets" / "textures").mkdir(parents=True, exist_ok=True)
    (ed.project / path).write_bytes(pngdiff.encode(32, 32, bytes([*rgb, 255]) * 32 * 32))
    return path


async def flip_scene(ed: Editor, change: str) -> None:
    """A box filling a still view; the script changes its look at game frame AT."""
    await ed.call("tool_groups", enable=["editor_tools"])
    await ed.call("set_project_settings", values={"graphics.canvas_width": WIDTH, "graphics.canvas_height": HEIGHT})
    scene = (await ed.call("create_scene", name="Flip", type="3D", path="scenes/Flip"))["scene"]["id"]
    red = write_image(ed, "red", (230, 30, 30))
    write_image(ed, "green", (30, 200, 30))
    await ed.call("sync_files")
    box = (await ed.call("create_entity", scene=scene, type="box", name="Box"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=box, values={"mesh": {"submeshes[0].material.baseColorTexture": red}})
    camera = (await ed.call("create_entity", scene=scene, type="camera", name="Camera"))["entity_id"]
    await ed.call("set_main_camera", scene=scene, entity=camera)
    await ed.call("set_transform", scene=scene, entity=camera, position=[0.4, 0.5, 1.6])
    await ed.call("set_properties", scene=scene, entity=camera, values={"camera": {"useTarget": True, "target": [0, 0, 0]}})
    await ed.call("create_script", kind="lua", name="Flip", content=FLIP.replace("AT", str(AT)).replace("CHANGE", change), scene=scene, entity=box)
    await ed.call("save_scene", scene=scene)


async def record_flip(ed: Editor, name: str) -> list:
    out = ed.scratch.root / name
    r = await ed.call("record_game", frames=FRAMES, fps=30, format="png", width=WIDTH, height=HEIGHT, output=str(out), timeout_seconds=600)
    ed.check_eq(r["frames"], FRAMES, f"{json.dumps(r)[:600]}")
    print(f"[recorder] {name}: {r['wallSeconds']:.2f} s, warnings {r['warnings']}")
    ed.check_eq(r["warnings"], [], "everything it waited for settled")
    return [pngdiff.decode(p.read_bytes()) for p in sorted(out.rglob("*.png"))]


def check_flip(ed: Editor, frames: list) -> None:
    """Frames before AT all alike, from AT on all alike, and the two looks differ: a frame recorded
    with a stand-in (the look before, or nothing) breaks it."""
    ed.check_eq(len(frames), FRAMES, "all frames")
    before, after = frames[0], frames[-1]
    ok, fraction, _ = pngdiff.compare(before, after, 8, 0.02)
    ed.check(not ok, f"the change shows in the last frame ({fraction:.3f} of the pixels differ)")
    wrong = []
    for k, frame in enumerate(frames):
        reference = before if k < AT else after
        ok, fraction, _ = pngdiff.compare(frame, reference, 2, 0.001)
        if not ok:
            wrong.append((k, round(fraction, 3)))
    ed.check_eq(wrong, [], f"every frame shows its look (frame, share of pixels that differ; the look changes at {AT})")


async def flip_test(env: dict, change: str) -> None:
    async with Editor(create=True, name="Flip", env=env) as ed:
        await flip_scene(ed, change)
        frames = await record_flip(ed, "run")
        for k in (0, AT - 1, AT, AT + 1, FRAMES - 1):
            (ed.scratch.root / f"frame{k}.png").write_bytes(pngdiff.encode(*frames[k]))
        check_flip(ed, frames)


async def test_texture_load_within_a_frame():
    await flip_test({}, TEXTURE_CHANGE)


async def test_texture_load_held():
    await flip_test({"JASPER_TEXTURE_LOAD_DELAY_MS": "800"}, TEXTURE_CHANGE)


async def test_variant_within_a_frame():
    await flip_test({}, VARIANT_CHANGE)


async def test_variant_pipelines_held():
    await flip_test({"JASPER_ASYNC_SHADERS_DELAY_MS": "400"}, VARIANT_CHANGE)


async def test_variant_synchronous_creation():
    await flip_test({"JASPER_ASYNC_SHADERS": "sync"}, VARIANT_CHANGE)


async def test_loading_past_the_limit_fails():
    """A load past the wait's limit fails the recording, naming it: it never records the stand-in."""
    env = {"JASPER_TEXTURE_LOAD_DELAY_MS": "20000", "JASPER_TEST_RECORDER_FAIL": "hold-limit=1500"}
    async with Editor(create=True, name="Past The Limit", env=env) as ed:
        await flip_scene(ed, TEXTURE_CHANGE)
        out = ed.scratch.root / "seq"
        text, data = await record_failed(ed, frames=FRAMES, fps=30, format="png", width=WIDTH, height=HEIGHT, output=str(out))
        ed.check_eq(data.get("failed_step"), "loading", f"{text}")
        ed.check("green.png" in text, f"it names what it waited for: {text}")
        ed.check(not out.exists(), "no file at the output path")
        await check_play_goes_on(ed)
