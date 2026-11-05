"""The Video Player component and MP4 through the platform's decoders, in Play, through MCP
(docs/superpowers/specs/2026-10-31-video-mp4-design.md §9). Linux: the system's FFmpeg (skipped
without libavcodec, libavformat and libswresample).

The clips are engine/tests/media/clips/ (tools/media/make_test_clips.sh): 160 x 90, 30 fps, 2 s; the
top-left, top-right and bottom-left quadrants show the base-4 digits of the frame number N (digit
d of quadrant q = (N / 4^q) % 4) as black, red, green or blue, the bottom-right one is white.

  render texture    a Video Player (Render Texture, no sound, not looping) shown by a full-canvas
                    UI Image through the link video|<id>: Play at a fixed 60 fps held between calls,
                    play() from Lua; the screenshot's quadrants give the frame get_runtime_entity
                    reports (+-1) and its time; onLoopPointReached once at the end; after stop()
                    the image keeps the last frame
  camera far plane  GameObject > Video > Video Player (Camera Background) in a 3D scene: the video
                    is the background of the main camera, behind a cube (the cube hidden: the
                    video's colours where it was)
  HTML <video>      <video src=clip.mp4 autoplay muted loop>: inspect_ui's media backend "ffmpeg",
                    playing, the frame of its currentTime on screen
  URL               a local HTTP server trickling the clips: the fast-start MP4 is prepared before
                    its download completes (progressive start), the moov-at-end one only after;
                    both in <userdata>/.cache/media/<sha256(url)>.mp4
  check --videos    the CLI's per-platform codec warnings, not-fast-start urls, missing clips

Runs in a new empty project (EDITOR create).
"""

from __future__ import annotations

import ctypes.util
import hashlib
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import harness
from harness import Editor, skip
from htmlui_pixels import Shot

EDITOR = {"create": True, "name": "Video Player"}
CI = True
TIMEOUT = 900

S: dict = {}

REPO = Path(__file__).resolve().parents[3]
CLIPS = REPO / "engine/tests/media/clips"
FAST = "h264_aac_160x90.mp4"
MOOV_END = "h264_aac_160x90_moov_end.mp4"
FPS = 30
FRAMES = 60

COLOURS = {0: (0, 0, 0), 1: (255, 0, 0), 2: (0, 255, 0), 3: (0, 0, 255)}
WHITE = (255, 255, 255)

PROBE = """---@class VideoProbe : ScriptInstance
local VideoProbe = { properties = {} }

function VideoProbe:init()
    videoEvents = {prepared = 0, started = 0, loops = 0, errors = {}}
    local vp = self.gameObject:getVideoPlayer()
    videoPlayer = vp
    vp.onPrepareCompleted = function(p) videoEvents.prepared = videoEvents.prepared + 1 end
    vp.onStarted = function(p) videoEvents.started = videoEvents.started + 1 end
    vp.onLoopPointReached = function(p) videoEvents.loops = videoEvents.loops + 1 end
    vp.onErrorReceived = function(p, message) table.insert(videoEvents.errors, message) end
end

return VideoProbe
"""

FILL = """---@class FillCanvas : ScriptInstance
local FillCanvas = { properties = {} }

function FillCanvas:init()
    local image = Image(self.scene, self.entity)
    image:setAnchorPoints(0, 0, 1, 1)
    image:setAnchorOffsets(0, 0, 0, 0)
end

return FillCanvas
"""

HTML = """<!DOCTYPE html>
<html>
<head><style>
    body { margin: 0; background-color: #303030; }
    #m { position: absolute; left: 0; top: 0; width: 320px; height: 180px; }
</style></head>
<body><video id="m" src="video/clip.mp4" autoplay muted loop></video></body>
</html>
"""


def need_ffmpeg() -> None:
    if not sys.platform.startswith("linux"):
        skip("the MP4 Play tests run on Linux (the system's FFmpeg); other platforms: their own decoders")
    missing = [lib for lib in ("avcodec", "avformat", "swresample") if not ctypes.util.find_library(lib)]
    if missing:
        skip(f"no FFmpeg libraries ({', '.join('lib' + m for m in missing)}): MP4 does not play on this Linux")


def install_clip(ed: Editor, name: str, rel: str) -> None:
    target = ed.project / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(CLIPS / name, target)


async def lua(ed: Editor, code: str) -> list:
    return (await ed.call("eval_lua", code=code))["values"]


async def shoot(ed: Editor, label: str) -> Shot:
    raw = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    ed.check(not raw.is_error and raw.images, f"{label}: no screenshot: {raw.text[:300]}")
    png = raw.images[0][0]
    if ed.scratch:
        (ed.scratch.root / f"video-{label}.png").write_bytes(png)
    shot = Shot(png, raw.value)
    shot.canvas = (raw.value["canvas"]["width"], raw.value["canvas"]["height"])
    return shot


def nearest(pixel: tuple) -> tuple[str, float]:
    """The clip colour a pixel is (a digit or "white") and how far it is from it."""
    best, dist = "", 1e9
    for name, ref in [(str(d), c) for d, c in COLOURS.items()] + [("white", WHITE)]:
        d = sum((a - b) ** 2 for a, b in zip(pixel, ref)) ** 0.5
        if d < dist:
            best, dist = name, d
    return best, dist


def read_frame(ed: Editor, shot: Shot, x0: float, y0: float, w: float, h: float, label: str) -> int:
    """The frame number a picture of the clip at (x0, y0, w, h) (canvas px) shows."""
    centres = [(x0 + w * 0.25, y0 + h * 0.25), (x0 + w * 0.75, y0 + h * 0.25), (x0 + w * 0.25, y0 + h * 0.75)]
    n = 0
    for q, (cx, cy) in enumerate(centres):
        name, dist = nearest(shot.px(cx, cy))
        ed.check(name != "white" and dist < 90, f"{label}: quadrant {q} is a digit colour: {shot.px(cx, cy)}")
        n += int(name) * 4 ** q
    corner = shot.px(x0 + w * 0.75, y0 + h * 0.75)
    ed.check(nearest(corner)[0] == "white" and nearest(corner)[1] < 90, f"{label}: the bottom-right quadrant is white: {corner}")
    return n


async def status(ed: Editor, entity) -> dict:
    got = await ed.call("get_runtime_entity", entity=entity)
    ed.check("video" in got, f"get_runtime_entity has video: {sorted(got)}")
    return got["video"]


async def probe_script(ed: Editor, scene, entity) -> None:
    """The VideoProbe script on a player: made by the first test that needs it, attached after"""
    if not S.get("probe"):
        made = await ed.call("create_script", kind="lua", name="VideoProbe", content=PROBE, scene=scene, entity=entity)
        S["probe"] = made.get("path") or "VideoProbe.lua"
    else:
        await ed.call("attach_script", scene=scene, entity=entity, path=S["probe"])


# ---------------------------------------------------------------- render texture -> UI Image

async def test_render_texture_to_image(ed: Editor):
    need_ffmpeg()
    ed.require_tools("create_entity", "set_properties", "play", "wait", "eval_lua", "get_runtime_entity", "screenshot")
    install_clip(ed, FAST, "assets/video/clip.mp4")
    await ed.call("sync_files")
    info = await ed.call("get_asset_info", path="assets/video/clip.mp4")
    ed.check_eq(info.get("video", {}).get("container"), "mp4", f"get_asset_info probes the MP4: {info}")
    ed.check_eq(info.get("fast_start"), True, f"fast start: {info}")
    ed.check_eq(info.get("playable_on", {}).get("windows"), "yes", f"the platform matrix: {info}")
    ed.check_eq(info.get("playable_on", {}).get("linux"), "needs_ffmpeg", f"the platform matrix: {info}")

    made = await ed.call("create_scene", name="VideoMain", type="2D", path="scenes/VideoMain")
    scene = made["scene"]["id"]
    player = (await ed.call("create_entity", scene=scene, type="video_player", name="Player"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=player, values={"videoplayer": {
        "clip": "assets/video/clip.mp4", "playOnAwake": False, "isLooping": False, "audioOutputMode": "none"}})
    got = await ed.call("get_entity", scene=scene, entity=player, components=["videoplayer"])
    vp = got["components"]["videoplayer"]
    ed.check_eq((vp["renderMode"], vp["clip"], vp["audioOutputMode"]), ("render_texture", "assets/video/clip.mp4", "none"),
                f"the component: {vp}")
    screen = (await ed.call("create_entity", scene=scene, type="image", name="Screen"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=screen, values={"ui": {"texture": f"video|{player}"}})
    got = await ed.call("get_entity", scene=scene, entity=screen, components=["ui"], properties=["texture"])
    ed.check_eq(got["components"]["ui"]["texture"]["video"]["id"], player, "the Image shows the player's texture")
    await probe_script(ed, scene, player)
    await ed.call("create_script", kind="lua", name="FillCanvas", content=FILL, scene=scene, entity=screen)
    await ed.call("save_scene", scene=scene)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]

    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        ed.check_eq(await lua(ed, "return videoPlayer.isPrepared, videoPlayer.isPlaying"), [False, False],
                    "playOnAwake off: nothing opened")
        await lua(ed, "videoPlayer:play()")
        waited = await ed.call("wait", **{"until": "videoEvents.started > 0", "timeout_seconds": 60})
        ed.check_eq(waited.get("reason"), "condition", f"onStarted: {waited}")
        ed.check_eq(await lua(ed, "return videoEvents.prepared, #videoEvents.errors"), [1, 0], "onPrepareCompleted once, no error")
        st = await status(ed, player)
        ed.check_eq((st["backend"], st["videoCodec"], st["width"], st["height"], st["frameCount"]), ("ffmpeg", "H.264", 160, 90, FRAMES),
                    f"the status: {st}")
        ed.check(abs(st["frameRate"] - FPS) < 1e-3 and abs(st["length"] - 2.0) < 0.05, f"the rate and length: {st}")

        # 30 game frames of 1/60 s: about 0.5 s in; the picture is the frame reported
        await ed.call("wait", frames=30)
        st = await status(ed, player)
        shot = await shoot(ed, "rt-mid")
        w, h = shot.canvas
        seen = read_frame(ed, shot, 0, 0, w, h, "mid-play")
        ed.check(st["playing"] and abs(seen - st["frame"]) <= 1, f"the screen shows frame {seen}, the player reports {st}")
        ed.check(abs(st["frame"] - int(st["time"] * FPS + 1e-6)) <= 1, f"the frame of the time: {st}")
        ed.check(0.35 <= st["time"] <= 0.65, f"half a second in: {st}")

        # to the end: onLoopPointReached once, not playing, the last frame on screen
        waited = await ed.call("wait", **{"until": "videoEvents.loops > 0", "timeout_seconds": 60})
        ed.check_eq(waited.get("reason"), "condition", f"onLoopPointReached: {waited}")
        await ed.call("wait", frames=40)
        ed.check_eq(await lua(ed, "return videoEvents.loops, videoPlayer.isPlaying"), [1, False],
                    "onLoopPointReached once for a clip that does not loop, then stopped playing")
        shot = await shoot(ed, "rt-end")
        ed.check(read_frame(ed, shot, 0, 0, w, h, "the end") >= FRAMES - 2, "the last frame stays on screen")

        # play again, stop() mid-way: the texture keeps the frame shown
        await lua(ed, "videoPlayer:play()")
        await ed.call("wait", frames=20)
        before = read_frame(ed, await shoot(ed, "rt-before-stop"), 0, 0, w, h, "before stop")
        await lua(ed, "videoPlayer:stop()")
        await ed.call("wait", frames=5)
        ed.check_eq(await lua(ed, "return videoPlayer.isPlaying, videoPlayer.isPrepared, videoPlayer.time"), [False, False, 0],
                    "stop(): time 0, released")
        after = read_frame(ed, await shoot(ed, "rt-after-stop"), 0, 0, w, h, "after stop")
        ed.check_eq(after, before, "after stop() the image keeps the last frame")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"no errors: {errors}")


# ---------------------------------------------------------------- camera far plane

async def test_camera_far_plane(ed: Editor):
    need_ffmpeg()
    install_clip(ed, FAST, "assets/video/clip.mp4")
    await ed.call("sync_files")
    made = await ed.call("create_scene", name="VideoBackground", type="3D", path="scenes/VideoBackground")
    scene = made["scene"]["id"]
    camera = (await ed.call("create_entity", scene=scene, type="camera", name="View", position=[0, 0, 5]))["entity_id"]
    await ed.call("set_main_camera", scene=scene, entity=camera)
    await ed.call("create_entity", scene=scene, type="cube", name="Cube", position=[0, 0, 0])
    await ed.call("create_entity", scene=scene, type="directional_light", name="Sun", rotation={"euler": [-40, 30, 0]})
    background = (await ed.call("create_entity", scene=scene, type="video_background", name="Background"))["entity_id"]
    got = (await ed.call("get_entity", scene=scene, entity=background, components=["videoplayer"]))["components"]["videoplayer"]
    ed.check_eq((got["renderMode"], got["aspectRatio"], got["isLooping"], got["playOnAwake"]),
                ("camera_far_plane", "fit_outside", True, True), f"the camera background's settings: {got}")
    await ed.call("set_properties", scene=scene, entity=background, values={"videoplayer": {
        "clip": "assets/video/clip.mp4", "audioOutputMode": "none"}})
    await probe_script(ed, scene, background)
    await ed.call("save_scene", scene=scene)

    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        waited = await ed.call("wait", **{"until": "videoEvents.started > 0", "timeout_seconds": 60})
        ed.check_eq(waited.get("reason"), "condition", f"playOnAwake started it: {waited}")
        await ed.call("wait", frames=20)
        await lua(ed, "videoPlayer:pause()")
        await ed.call("wait", frames=3)
        st = await status(ed, background)
        ed.check(st["paused"] and not st["playing"], f"paused: {st}")
        shown = await shoot(ed, "far-cube")
        w, h = shown.canvas
        # the canvas is 16:9 like the clip: fit_outside fills it exactly; the corners are the video
        corners = [(w * 0.08, h * 0.08), (w * 0.92, h * 0.08), (w * 0.08, h * 0.92), (w * 0.92, h * 0.92)]
        digits = [nearest(shown.px(*c)) for c in corners]
        ed.check(all(d < 90 for _, d in digits), f"the corners are the video's colours: {[shown.px(*c) for c in corners]}")
        ed.check_eq(digits[3][0], "white", "the bottom-right corner is the white quadrant")
        n = sum(int(digits[q][0]) * 4 ** q for q in range(3) if digits[q][0] != "white")
        ed.check(abs(n - st["frame"]) <= 1, f"the background shows frame {n}, the player reports {st}")
        near = [(w * 0.47, h * 0.47), (w * 0.53, h * 0.47), (w * 0.47, h * 0.53), (w * 0.53, h * 0.53)]

        await lua(ed, "object('Cube'):setActive(false)")
        await ed.call("wait", frames=3)
        hidden = await shoot(ed, "far-no-cube")
        for q, point in enumerate(near):
            ed.check_eq(nearest(hidden.px(*point))[0], digits[q][0], f"without the cube the video is at the centre too ({q})")
        moved = max(sum(abs(a - b) for a, b in zip(shown.px(*p), hidden.px(*p))) for p in near)
        ed.check(moved > 60, f"the cube was drawn over the video ({[shown.px(*p) for p in near]})")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


# ---------------------------------------------------------------- HTML <video>

async def test_html_video_mp4(ed: Editor):
    need_ffmpeg()
    install_clip(ed, FAST, "assets/ui/video/clip.mp4")
    (ed.project / "assets/ui/mp4.html").write_text(HTML, encoding="utf-8")
    await ed.call("sync_files")
    made = await ed.call("create_scene", name="VideoHtml", type="UI")
    scene = made["scene"]["id"]
    r = await ed.call("create_ui_document", scene=scene, html="assets/ui/mp4.html", name="Page")
    page = r["entity"]["id"]
    await ed.call("save_scene", scene=scene, path="scenes/VideoHtml")
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        deadline = time.monotonic() + 60
        while True:
            await ed.call("wait", frames=5)
            m = (await ed.call("inspect_ui", scene=scene, entity=page, selector="#m", depth=0))["elements"][0]["media"]
            if m.get("readyState", 0) >= 4 or m.get("error") or time.monotonic() > deadline:
                break
        ed.check(m.get("error") is None, f"no error: {m}")
        ed.check_eq((m.get("backend"), m["videoWidth"], m["videoHeight"], m["readyState"]), ("ffmpeg", 160, 90, 4), f"the media: {m}")
        ed.check(not m["paused"] and abs(m["duration"] - 2.0) < 0.05, f"playing, 2 s: {m}")
        await ed.call("wait", frames=12)
        m = (await ed.call("inspect_ui", scene=scene, entity=page, selector="#m", depth=0))["elements"][0]["media"]
        seen = read_frame(ed, await shoot(ed, "html"), 0, 0, 320, 180, "<video>")
        expected = int(m["currentTime"] * FPS + 1e-6)
        ed.check(abs(seen - expected) <= 1, f"the <video> shows frame {seen} at currentTime {m['currentTime']}")
        backend = await lua(ed, "return UIDocument(scene, entity('Page')):getElementById('m').videoBackend")
        ed.check_eq(backend, ["ffmpeg"], "element.videoBackend")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


# ---------------------------------------------------------------- URL, the media cache

class ClipServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), ClipHandler)
        self.files = {FAST: (CLIPS / FAST).read_bytes(), MOOV_END: (CLIPS / MOOV_END).read_bytes()}
        self.done: dict[str, float] = {}
        self.chunk, self.delay = 1024, 0.3


class ClipHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        server: ClipServer = self.server  # type: ignore[assignment]
        name = self.path.split("?")[0].lstrip("/")
        data = server.files.get(name)
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            for i in range(0, len(data), server.chunk):
                self.wfile.write(data[i:i + server.chunk])
                self.wfile.flush()
                if i + server.chunk < len(data):
                    time.sleep(server.delay)
            server.done[self.path] = time.monotonic()
        except OSError:
            pass


def cache_file(ed: Editor, url: str) -> Path:
    return ed.project / ".jasperengine/user/rundata/.cache/media" / (hashlib.sha256(url.encode()).hexdigest() + ".mp4")


async def test_url_progressive_start(ed: Editor):
    need_ffmpeg()
    server = ClipServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    stamp = int(time.time() * 1000)
    try:
        made = await ed.call("create_scene", name="VideoUrl", type="2D", path="scenes/VideoUrl")
        scene = made["scene"]["id"]
        player = (await ed.call("create_entity", scene=scene, type="video_player", name="UrlPlayer"))["entity_id"]
        await ed.call("set_properties", scene=scene, entity=player, values={"videoplayer": {
            "source": "url", "playOnAwake": False, "audioOutputMode": "none"}})
        await probe_script(ed, scene, player)
        await ed.call("save_scene", scene=scene)
        await ed.call("play", action="start", scene=scene, fixed_fps=0, auto_pause=False)
        try:
            # fast start: prepared while the server is still sending
            fast = f"{base}/{FAST}?run={stamp}"
            await lua(ed, f"videoEvents.prepared = 0; videoPlayer.url = '{fast}'; videoPlayer:prepare()")
            waited = await ed.call("wait", **{"until": "videoEvents.prepared > 0 or #videoEvents.errors > 0", "timeout_seconds": 60})
            prepared_at = time.monotonic()
            ed.check_eq(await lua(ed, "return #videoEvents.errors"), [0], f"no error: {await lua(ed, 'return videoEvents.errors')}")
            ed.check_eq(waited.get("reason"), "condition", f"prepared: {waited}")
            sent = server.done.get(f"/{FAST}?run={stamp}")
            ed.check(sent is None or sent > prepared_at, f"prepared before the download completed (progressive start; sent at {sent}, prepared at {prepared_at})")
            st = await status(ed, player)
            ed.check_eq((st["backend"], st["frameCount"]), ("ffmpeg", FRAMES), f"the URL's status: {st}")
            await lua(ed, "videoPlayer:play()")
            waited = await ed.call("wait", **{"until": "videoEvents.started > 0", "timeout_seconds": 30})
            ed.check_eq(waited.get("reason"), "condition", f"started while downloading: {waited}")
            deadline = time.monotonic() + 30
            while f"/{FAST}?run={stamp}" not in server.done and time.monotonic() < deadline:
                await ed.call("wait", seconds=0.5)
            await ed.call("wait", seconds=1.0)
            ed.check_eq(cache_file(ed, fast).read_bytes() if cache_file(ed, fast).exists() else None, server.files[FAST],
                        "the media cache holds the whole file")

            # moov at the end: nothing to play until the download completed
            late = f"{base}/{MOOV_END}?run={stamp}"
            await lua(ed, f"videoEvents.prepared = 0; videoPlayer.url = '{late}'; videoPlayer:prepare()")
            waited = await ed.call("wait", **{"until": "videoEvents.prepared > 0 or #videoEvents.errors > 0", "timeout_seconds": 60})
            prepared_at = time.monotonic()
            ed.check_eq(await lua(ed, "return #videoEvents.errors"), [0], f"no error: {await lua(ed, 'return videoEvents.errors')}")
            sent = server.done.get(f"/{MOOV_END}?run={stamp}")
            ed.check(sent is not None and sent <= prepared_at, f"the moov-at-end MP4 prepared only after its download (sent at {sent}, prepared at {prepared_at})")
            ed.check_eq((await status(ed, player))["frameCount"], FRAMES, "and it plays")
            ed.check(cache_file(ed, late).exists(), "cached too")
        finally:
            await ed.call("play", action="stop")
    finally:
        server.shutdown()
        server.server_close()


# ---------------------------------------------------------------- check --videos

async def test_cli_check_videos(ed: Editor):
    """`jasperengine-editor check --videos`: a VP9 WebM clip warns for the platforms without VP9
    (Apple), a moov-at-end MP4 played as a url is reported as not fast start, a missing clip is
    named; the H.264 clips of the other scenes give nothing. Plain text names the Video Players."""
    install_clip(ed, "vp9_opus_160x90.webm", "assets/video/vp9.webm")
    install_clip(ed, MOOV_END, "assets/video/late.mp4")
    await ed.call("sync_files")
    made = await ed.call("create_scene", name="VideoCheck", type="2D", path="scenes/VideoCheck")
    scene = made["scene"]["id"]
    for name, values in (("Vp9", {"clip": "assets/video/vp9.webm"}),
                         ("Late", {"source": "url", "url": "assets/video/late.mp4"}),
                         ("Gone", {"clip": "assets/video/gone.mp4"})):
        made = await ed.call("create_entity", scene=scene, type="video_player", name=name)
        await ed.call("set_properties", scene=scene, entity=made["entity_id"], values={"videoplayer": values})
    await ed.call("save_scene", scene=scene)
    await ed.call("save_project")

    cmd = harness.editor_command() + ["check", "--project", str(ed.project), "--videos", "--format", "json"]
    r = subprocess.run(cmd, cwd=str(ed.project), capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    try:
        data = json.loads(r.stdout)
    except json.JSONDecodeError:
        raise AssertionError(f"check --videos --format json (exit {r.returncode}): {r.stdout[-1500:]} {r.stderr[-1500:]}")
    findings = [f for f in data["findings"] if f.get("tool") == "videos"]
    quoted = lambda m: (re.search(r"'(assets/[^']+)'", m) or re.search("'([^']*)'", m)).group(1)
    codes = sorted((f.get("code", ""), quoted(f["message"])) for f in findings)
    unsupported = [m for c, m in codes if c == "video-codec-unsupported-on-target"]
    ed.check(unsupported and set(unsupported) == {"assets/video/vp9.webm"}, f"only the VP9 clip warns: {findings}")
    ed.check(any(f["code"] == "video-codec-unsupported-on-target" and "macOS" in f["message"] for f in findings) and
             any(f["code"] == "video-codec-unsupported-on-target" and "iOS" in f["message"] for f in findings),
             f"for the Apple platforms: {findings}")
    ed.check(("video-not-faststart-url", "assets/video/late.mp4") in codes, f"the moov-at-end url: {findings}")
    ed.check(("video-missing", "assets/video/gone.mp4") in codes, f"the missing clip: {findings}")
    ed.check(data["checked"].get("videos", 0) >= 3, f"the Video Players counted: {data['checked']}")

    r = subprocess.run(harness.editor_command() + ["check", "--project", str(ed.project), "--videos"], cwd=str(ed.project),
                       capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    ed.check("Video Player" in r.stderr and "video-not-faststart-url" in r.stdout, f"the text summary: {r.stdout[-800:]} {r.stderr[-800:]}")
