"""HTML <video> through MCP in play mode (phase 6, p6-video, docs/html-ui/phase-6-video.md §5.4).

A UI scene with one document: a 640 x 360 <video controls> of the AV1 test card (320 x 180, 30 fps,
2 s, its frame index drawn as 8 bit blocks) and a <video> whose source is an MP4. Played with
fixed_fps 60 and auto_pause, so every frame is exactly 1/60 s and the video is frame-exact:

  inspect_ui        the media state (readyState, duration, size, paused) and the controls' rectangles
  send_input        a click on the controls' play button, then 30 frames: currentTime moved 0.5 s
                    and the Game view shows the frame of that time (the bit pattern read back);
                    Space pauses, ArrowRight seeks 5 s (clamped to the end), Home goes back to 0
  loop              from 1.9 s with loop set, 12 frames: wrapped to 0.1 s, no `ended`
  error             the MP4 source: MEDIA_ERR_SRC_NOT_SUPPORTED in inspect_ui; `check --ui` reports
                    HTMLUI-HTML-0027 on it
  import_files      a real MP4 (FFmpeg's built-in mpeg4 + aac) converted to AV1/Opus WebM by the
                    FFmpeg on PATH (skipped without one that has an AV1 encoder, like the LuaLS check
                    of the examples), the WebM test card copied as it is; and with JASPER_FFMPEG=""
                    (no FFmpeg) the MP4 copied unconverted with HTMLUI-IMP-0010 (a second editor)

Runs on a scratch copy of the fixture project (canvas 1280 x 720). Coordinates are canvas pixels,
which are document pixels here.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, skip
from htmlui_pixels import Shot

TIMEOUT = 900

S: dict = {}

REPO = Path(__file__).resolve().parents[3]
CLIPS = REPO / "engine/tests/htmlui/fixtures/video"
DOC = "assets/ui/video.html"
BAD = "assets/ui/badvideo.html"

HTML = """<!DOCTYPE html>
<html>
<head>
    <style>
        body { margin: 0; background-color: #202020; }
        #v { position: absolute; left: 0; top: 0; width: 640px; height: 360px; }
        #other { position: absolute; left: 700px; top: 0; width: 160px; height: 90px; }
    </style>
</head>
<body>
    <video id="v" src="video/testcard-noaudio.webm" controls></video>
</body>
</html>
"""

BAD_HTML = """<!DOCTYPE html>
<html>
<head><style>body { margin: 0; } video { width: 160px; height: 90px; }</style></head>
<body><video id="bad" src="video/clip.mp4"></video></body>
</html>
"""

# the test card's frame index: 8 blocks of 32 px from (16, 124), 36 px apart, green > 128 = 1 (the
# pattern VideoTestCommon.h reads); the card is drawn at 2x here
CARD_SCALE = 2.0


def card_index(shot: Shot) -> int:
    value = 0
    for bit in range(8):
        x = (16 + 36 * bit + 16) * CARD_SCALE
        y = (124 + 16) * CARD_SCALE
        value = (value << 1) | (1 if shot.px(x, y)[1] > 128 else 0)
    return value


def frame_of(seconds: float) -> int:
    return int(seconds * 30 + 1e-6)


async def lua(ed: Editor, code: str) -> list:
    return (await ed.call("eval_lua", code=code))["values"]


def video_lua(element: str = "v") -> str:
    return f"UIDocument(scene, entity('Video')):getElementById('{element}')"


async def media(ed: Editor, selector: str = "#v") -> dict:
    tree = await ed.call("inspect_ui", entity="Video", selector=selector, depth=0)
    ed.check(len(tree["elements"]) == 1, f"inspect_ui {selector}: {tree}")
    element = tree["elements"][0]
    ed.check("media" in element, f"inspect_ui {selector} has no media: {element}")
    return element["media"]


async def send(ed: Editor, *events, frames_after: int = 0):
    r = await ed.call_raw("send_input", {"events": list(events), "space": "canvas", "frames_after": frames_after})
    ed.check(not r.is_error, f"send_input {events}: {r.text[:300]}")


async def wait(ed: Editor, frames: int):
    await ed.call("wait", frames=frames)


async def shoot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-video-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def current_time(ed: Editor) -> float:
    return (await lua(ed, f"return {video_lua()}.currentTime"))[0]


def ffmpeg_with_av1() -> str | None:
    exe = shutil.which("ffmpeg")
    if not exe:
        return None
    out = subprocess.run([exe, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=60).stdout
    return exe if any(f" {name} " in out for name in ("libsvtav1", "libaom-av1", "librav1e")) else None


def make_mp4(ffmpeg: str, path: Path) -> None:
    """1 s of FFmpeg's test pattern and a tone, MPEG-4 Part 2 + AAC (encoders every FFmpeg has)."""
    r = subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=30:duration=1",
                        "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
                        "-c:v", "mpeg4", "-c:a", "aac", "-shortest", str(path)],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise AssertionError(f"ffmpeg could not make the MP4: {r.stderr[-1000:]}")


def probe_streams(path: Path) -> list[str]:
    exe = shutil.which("ffprobe")
    if not exe:
        return []
    r = subprocess.run([exe, "-v", "error", "-show_entries", "stream=codec_name", "-of", "csv=p=0", str(path)],
                       capture_output=True, text=True, timeout=60)
    return [line.strip() for line in r.stdout.splitlines() if line.strip()]


# -- tests ---------------------------------------------------------------------------------------


async def setup_and_play(ed: Editor):
    """The clip imported (a playable WebM is copied as it is), the document in a UI scene, playing
    with fixed_fps 60 and auto_pause."""
    await ed.console_since(None, limit=1)
    r = await ed.call("import_files", sources=[str(CLIPS / "testcard-noaudio.webm"), str(CLIPS / "poster.png")],
                      to_dir="assets/ui/video")
    ed.check("videos" not in r, f"a playable WebM is not converted: {r}")
    ed.check((ed.project / "assets/ui/video/testcard-noaudio.webm").read_bytes() == (CLIPS / "testcard-noaudio.webm").read_bytes(),
             "the WebM is copied byte for byte")
    await ed.call("write_file", path=DOC, content=HTML)
    made = await ed.call("create_scene", name="HtmlUiVideo", type="UI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Video")
    await ed.call("add_component", scene=S["scene"], entity=ent["entity_id"], component="UIDocument",
                  values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiVideo")
    started = await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    ed.check(started, "play started")
    await wait(ed, 3)
    errors = [e.get("message", "") for e in await ed.console_since(types=["error", "warning"], limit=1000)
              if "[HTML UI]" in e.get("message", "")]
    ed.check(not errors, f"no HTML UI error or warning: {errors}")


async def test_inspect_media_and_controls(ed: Editor):
    await setup_and_play(ed)
    m = await media(ed)
    ed.check_eq((m["readyState"], m["videoWidth"], m["videoHeight"], m["paused"], m["ended"]), (4, 320, 180, True, False),
                f"the loaded clip: {m}")
    ed.check(abs(m["duration"] - 2.0) < 1e-3, f"duration {m['duration']}")
    ed.check(m["currentSrc"].endswith("video/testcard-noaudio.webm"), m["currentSrc"])
    ed.check_eq((m["currentTime"], m["frame"]), (0, 0), "at the start, frame 0 shown")
    controls = m.get("controls")
    ed.check(controls and controls["visible"], f"paused: the controls are shown: {m}")
    S["play"] = controls["play"]
    shot = await shoot(ed, "start")
    ed.check_eq(card_index(shot), 0, "the Game view shows frame 0")


async def test_click_play_then_30_frames(ed: Editor):
    """A click on the play button starts it; 30 frames of 1/60 s later currentTime moved 0.5 s and
    the frame of that time is on screen."""
    x, y, w, h = S["play"]
    await send(ed, {"type": "mouse", "action": "click", "x": x + w / 2, "y": y + h / 2}, frames_after=1)
    m = await media(ed)
    ed.check(not m["paused"], f"playing after the click: {m}")
    t0 = m["currentTime"]
    await wait(ed, 30)
    m = await media(ed)
    ed.check(abs(m["currentTime"] - t0 - 0.5) < 1e-6, f"30 frames at 60 fps: {t0} -> {m['currentTime']}")
    ed.check_eq(m["frame"], frame_of(m["currentTime"]), f"the frame of {m['currentTime']} s")
    ed.check_eq(m["droppedFrames"], 0, "frame-exact: nothing dropped")
    shot = await shoot(ed, "playing")
    ed.check_eq(card_index(shot), frame_of(m["currentTime"]), "the Game view shows that frame")


async def test_keyboard(ed: Editor):
    """Space pauses (the video focused), ArrowRight seeks 5 s (clamped to the end), Home goes to 0."""
    await lua(ed, f"{video_lua()}:focus()")
    await send(ed, {"type": "key", "key": "space"}, frames_after=2)
    m = await media(ed)
    ed.check(m["paused"], f"Space paused: {m}")
    paused_at = m["currentTime"]
    await wait(ed, 10)
    ed.check_eq((await media(ed))["currentTime"], paused_at, "paused: the time stays")
    await send(ed, {"type": "key", "key": "right"}, frames_after=3)
    m = await media(ed)
    ed.check(abs(m["currentTime"] - 2.0) < 1e-6 and not m["seeking"], f"ArrowRight: +5 s clamped to the end: {m}")
    await send(ed, {"type": "key", "key": "home"}, frames_after=3)
    m = await media(ed)
    ed.check_eq((m["currentTime"], m["frame"]), (0, 0), f"Home: back to the start: {m}")
    shot = await shoot(ed, "home")
    ed.check_eq(card_index(shot), 0, "frame 0 again")


async def test_loop_wraps(ed: Editor):
    """loop: from 1.9 s, 12 frames (0.2 s) later it is 0.2 s past the start again, playing; the wrap
    is a seek (`seeking`, `seeked`), not an `ended`."""
    await lua(ed, f"""
        local v = {video_lua()}
        _G.videoEvents = {{}}
        for _, t in ipairs({{"ended", "seeked", "pause"}}) do
            v:addEventListener(t, function(e) table.insert(_G.videoEvents, e.type) end)
        end
        v.loop = true
        v.currentTime = 1.9
    """)
    await wait(ed, 2)
    seen = (await lua(ed, "return table.concat(_G.videoEvents, ',')"))[0]
    ed.check_eq(seen, "seeked", "the seek")
    await lua(ed, f"{video_lua()}:play()")
    await wait(ed, 1)
    t0 = await current_time(ed)
    await wait(ed, 12)
    t1 = await current_time(ed)
    duration = (await media(ed))["duration"]
    expected = (t0 + 0.2) % duration
    ed.check(t0 + 0.2 > duration and abs(t1 - expected) < 1e-6, f"wrapped: {t0} + 0.2 s -> {t1} (expected {expected})")
    events, paused = await lua(ed, f"return table.concat(_G.videoEvents, ','), {video_lua()}.paused")
    ed.check_eq((events, paused), ("seeked,seeked", False), "the seek, then the wrap's seeked; no ended, no pause")
    m = await media(ed)
    ed.check_eq(m["frame"], frame_of(m["currentTime"]), f"the frame after the wrap: {m}")
    await lua(ed, f"{video_lua()}:pause()")


async def test_unsupported_source(ed: Editor):
    """An MP4 source: HTMLUI-HTML-0027 from check --ui; MEDIA_ERR_SRC_NOT_SUPPORTED on the element."""
    try:
        await check_reports_the_mp4(ed)
        await other_video_fails(ed)
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def check_reports_the_mp4(ed: Editor):
    shutil.copy(CLIPS / "testcard-vp9.webm", ed.project / "assets/ui/video/clip.mp4")
    await ed.call("write_file", path=BAD, content=BAD_HTML)
    await ed.call("sync_files")
    # `check --ui` on a copy of the fixture with the page and the file (not the project the editor has open)
    tmp = Path(tempfile.mkdtemp(prefix="jasper-videocheck-"))
    try:
        project = tmp / "project"
        shutil.copytree(harness.default_fixture(), project, symlinks=True,
                        ignore=lambda d, names: {".git"} & set(names) | ({"build"} & set(names) if Path(d).name == ".jasperengine" else set()))
        (project / "assets/ui/video").mkdir(parents=True, exist_ok=True)
        shutil.copy(CLIPS / "testcard-vp9.webm", project / "assets/ui/video/clip.mp4")
        (project / BAD).write_text(BAD_HTML, encoding="utf-8")
        proc = subprocess.run(harness.editor_command() + ["check", "--project", str(project), "--files", BAD, "--format", "json"],
                              cwd=str(project), capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        raise AssertionError(f"check --format json: {proc.stdout[-1500:]} {proc.stderr[-1500:]}")
    named = [f for f in data["findings"] if f["code"] == "HTMLUI-HTML-0027" and "clip.mp4" in f["message"]]
    ed.check(named and named[0]["line"] == 4, f"check --ui names the MP4 source, at its line: {data['findings']}")
    # and, as an error, that the <video> has nothing it can play
    ed.check(any(f["code"] == "HTMLUI-HTML-0027" and f["severity"] == "error" for f in data["findings"]), f"{data['findings']}")


async def other_video_fails(ed: Editor):
    await lua(ed, f"""
        local doc = UIDocument(scene, entity('Video'))
        local holder = doc:createElement('div')
        holder.innerHTML = '<video id="other" src="video/clip.mp4"></video>'
        doc.body:appendChild(holder)
    """)
    await wait(ed, 3)
    m = await media(ed, "#other")
    ed.check(m.get("error") and m["error"]["code"] == 4, f"MEDIA_ERR_SRC_NOT_SUPPORTED: {m}")
    ed.check_eq((m["networkState"], m["readyState"]), (3, 0), "NETWORK_NO_SOURCE, HAVE_NOTHING")


async def test_import_converts_an_mp4(ed: Editor):
    """import_files converts an MP4 into AV1 + Opus WebM with the FFmpeg on PATH; the result plays."""
    ffmpeg = ffmpeg_with_av1()
    if not ffmpeg:
        skip("no FFmpeg with an AV1 encoder (libsvtav1, libaom-av1, librav1e) on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "intro.mp4"
        make_mp4(ffmpeg, source)
        r = await ed.call("import_files", sources=[str(source)], to_dir="assets/ui/video", timeout=600)
    videos = r.get("videos", [])
    ed.check(len(videos) == 1 and videos[0]["converted"], f"converted: {r}")
    ed.check_eq(videos[0]["file"], "assets/ui/video/intro.webm", "the output")
    ed.check(videos[0]["encoder"] in ("libsvtav1", "libaom-av1", "librav1e"), videos[0])
    ed.check(not (ed.project / "assets/ui/video/intro.mp4").exists(), "the original is not copied")
    out = ed.project / "assets/ui/video/intro.webm"
    ed.check(out.read_bytes()[:4] == b"\x1a\x45\xdf\xa3", "a Matroska/WebM file")
    streams = probe_streams(out)
    if streams:
        ed.check_eq(sorted(streams), ["av1", "opus"], "AV1 video and Opus audio")
    # it plays: a document with it, played one second
    await ed.call("write_file", path="assets/ui/intro.html",
                  content='<html><body style="margin: 0"><video id="i" src="video/intro.webm" autoplay muted loop></video></body></html>')
    made = await ed.call("create_scene", name="HtmlUiVideoIntro", type="UI")
    ent = await ed.call("create_entity", scene=made["scene"]["id"], type="empty", name="Intro")
    await ed.call("add_component", scene=made["scene"]["id"], entity=ent["entity_id"], component="UIDocument",
                  values={"uidocument": {"filename": "assets/ui/intro.html"}})
    await ed.call("save_scene", scene=made["scene"]["id"], path="scenes/HtmlUiVideoIntro")
    await ed.call("play", action="start", scene=made["scene"]["id"], fixed_fps=60, auto_pause=True)
    try:
        await wait(ed, 3)
        first = (await ed.call("inspect_ui", entity="Intro", selector="#i", depth=0))["elements"][0]["media"]
        await wait(ed, 12)
        m = (await ed.call("inspect_ui", entity="Intro", selector="#i", depth=0))["elements"][0]["media"]
        ed.check_eq((m["videoWidth"], m["videoHeight"], m["readyState"]), (160, 90, 4), f"the converted clip plays: {m}")
        ed.check(m.get("error") is None and not m["paused"], f"playing: {m}")
        moved = (m["currentTime"] - first["currentTime"]) % m["duration"]
        ed.check(abs(moved - 0.2) < 1e-6, f"12 frames of 1/60 s: {first['currentTime']} -> {m['currentTime']}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_import_without_ffmpeg():
    """JASPER_FFMPEG set to nothing: the MP4 is copied unconverted with HTMLUI-IMP-0010, which
    gives the command to run."""
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "clip.mp4"
        shutil.copy(CLIPS / "testcard-vp9.webm", source)
        saved = os.environ.get("JASPER_FFMPEG")
        os.environ["JASPER_FFMPEG"] = ""
        try:
            async with Editor() as ed:
                r = await ed.call("import_files", sources=[str(source)], to_dir="assets/ui/video")
                videos = r.get("videos", [])
                ed.check(len(videos) == 1 and not videos[0]["converted"], f"not converted: {r}")
                ed.check_eq(videos[0]["file"], "assets/ui/video/clip.mp4", "copied as it is")
                ed.check((ed.project / "assets/ui/video/clip.mp4").exists(), "the original is in the project")
                diagnostics = r.get("diagnostics", [])
                ed.check(len(diagnostics) == 1 and diagnostics[0]["code"] == "HTMLUI-IMP-0010", f"HTMLUI-IMP-0010: {r}")
                ed.check("ffmpeg" in diagnostics[0]["suggestion"] and "libopus" in diagnostics[0]["suggestion"],
                         f"the command to run: {diagnostics[0]}")
                ed.check("JASPER_FFMPEG" in diagnostics[0]["message"], f"says why: {diagnostics[0]}")
        finally:
            if saved is None:
                os.environ.pop("JASPER_FFMPEG", None)
            else:
                os.environ["JASPER_FFMPEG"] = saved
