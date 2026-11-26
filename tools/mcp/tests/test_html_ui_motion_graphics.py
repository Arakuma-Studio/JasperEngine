"""The portrait motion-graphics example at its public Lua/record_game boundary.

The new SVG/text modules must exist, procedural geometry must be seekable, and two six-second
1080x1350 recordings at 30 fps must have identical decoded pixels in all 180 frames. The trace
also measures the thirty seeded stars' Lua geometry update separately from SVG rasterization.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import subprocess
from pathlib import Path

import pngdiff
from harness import Editor, REPO

CI = True
EDITOR = {"create": True, "name": "Motion Graphics"}
TIMEOUT = 1800

WIDTH, HEIGHT, FPS, FRAMES = 1080, 1350, 30, 180
LIBRARY = REPO / "docs/mcp/guide/html-ui-examples"
ENTRY = next(entry for entry in json.loads((LIBRARY / "examples.json").read_text())["examples"]
             if entry["name"] == "motion_graphics")
S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("editor") is ed:
        return S
    S.clear()
    await ed.call("tool_groups", enable=["editor_tools"])
    await ed.call("set_project_settings", values={"graphics.canvas_width": WIDTH, "graphics.canvas_height": HEIGHT,
                                                   "graphics.scaling_mode": "letterbox"})
    made = await ed.call("create_scene", name="MotionGraphics", type="2D", path="scenes/MotionGraphics")
    scene = made["scene"]["id"]
    created = await ed.call("create_ui_document", scene=scene, name="MotionGraphics",
                            html="assets/ui/motion_graphics.html", template="motion_graphics", script=True)
    counts = created["document"]["diagnostics_count"]
    ed.check_eq((counts["error"], counts["warning"]), (0, 0), f"the composition loads cleanly: {created['diagnostics']}")
    await ed.call("save_scene", scene=scene)
    S.update(editor=ed, scene=scene, entity=created["entity"]["id"])
    return S


async def start(ed: Editor) -> dict:
    state = await setup(ed)
    await ed.call("play", action="start", scene=state["scene"], fixed_fps=FPS, auto_pause=True)
    api = await ed.call("eval_lua", code="return type(anime.splitText), type(anime.createDrawable), "
                        "type(anime.morphTo), type(anime.createMotionPath)")
    ed.check_eq(api["values"], ["function"] * 4, "the text and SVG modules are available in Lua")
    return state


def ui_lua() -> str:
    return 'GameObject(scene, entity("MotionGraphics")):getScript("MotionGraphicsUI")'


async def test_motion_graphics_exact_procedural_seek(ed: Editor):
    await start(ed)
    try:
        result = await ed.call("eval_lua", code=f"""
            local ui = {ui_lua()}
            assert(ui and ui.timeline and ui.split, "motion_graphics script initialized")
            ui.timeline:pause()
            ui.timeline:seek(1234)
            local first = ui.starPath:getAttribute("d")
            local logo = ui.doc:getElementById("logo")
            local firstFill = logo:getAttribute("fill")
            ui.timeline:seek(2500)
            local drawCompleteFill = logo:getAttribute("fill")
            ui.timeline:seek(4321)
            local other = ui.starPath:getAttribute("d")
            local filled = logo:getAttribute("fill")
            ui.timeline:seek(1234)
            return first == ui.starPath:getAttribute("d"), first ~= other,
                   firstFill == drawCompleteFill and firstFill == logo:getAttribute("fill"), firstFill ~= filled,
                   #ui.stars, #ui.split.chars, #ui.split.words, #ui.split.lines
        """)
        same, moves, fill_replays, fills, stars, chars, words, lines = result["values"]
        ed.check(same and moves, "procedural paths depend on exact timeline time and seed, even after a backwards seek")
        ed.check(fill_replays and fills, "the logo fills after its stroke completes and restores its fill on a backwards seek")
        ed.check_eq(stars, 30, "thirty stars from the fixed seed")
        ed.check(chars >= 18 and words == 3 and lines >= 1, f"the headline is split into animatable units: {result['values']}")
        checked = await ed.call("assert_ui", entity="MotionGraphics", checks=ENTRY["checks"], viewports=ENTRY["viewports"])
        ed.check(checked["passed"], f"the portrait layout checks: {checked}")
    finally:
        await ed.call("play", action="stop")


def frame_hash(path: Path) -> tuple[int, int, str]:
    width, height, rgba = pngdiff.decode(path.read_bytes())
    return width, height, hashlib.sha256(rgba).hexdigest()


def recording_hashes(ffmpeg: str, files: list[Path]) -> list[tuple[int, int, str]]:
    """SHA256 of every decoded frame's tightly packed RGBA8 bytes, in file order."""
    process = subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-threads", "2", "-filter_threads", "2",
                              "-framerate", str(FPS), "-start_number", "0", "-i",
                              str(files[0].parent / "frame_%04d.png"), "-pix_fmt", "rgba", "-c:v", "rawvideo",
                              "-threads", "2", "-f", "framehash", "-hash", "sha256", "-"],
                             capture_output=True, text=True, timeout=180)
    if process.returncode:
        raise AssertionError(f"FFmpeg could not decode the recording: {process.stderr}")
    records = [line.split(",") for line in process.stdout.splitlines() if line and not line.startswith("#")]
    if len(records) != len(files):
        raise AssertionError(f"FFmpeg decoded {len(records)} of {len(files)} frames")
    hashes = []
    for path, record in zip(files, records):
        with path.open("rb") as image:
            header = image.read(24)
        if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
            raise AssertionError(f"The recorded frame is not a PNG: {path}")
        width, height = struct.unpack(">II", header[16:24])
        if len(record) != 6 or int(record[4]) != width * height * 4:
            raise AssertionError(f"FFmpeg did not hash the complete RGBA8 frame: {path}: {record}")
        digest = record[5].strip()
        if len(digest) != 64 or len(bytes.fromhex(digest)) != 32:
            raise AssertionError(f"Invalid SHA256 frame hash: {path}: {digest}")
        hashes.append((width, height, digest))
    return hashes


async def test_motion_graphics_six_second_recordings_are_identical(ed: Editor):
    # Fail immediately on a player without the modules, before spending time on 360 frames.
    await start(ed)
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    before = (await ed.call("read_console", limit=1))["next_cursor"]
    ffmpeg = shutil.which("ffmpeg")
    ed.check(ffmpeg is not None, "FFmpeg is available to decode and review every recorded frame")
    takes = []
    for take in range(2):
        output = ed.scratch.root / f"motion-graphics-{take + 1}"
        result = await ed.call("record_game", frames=FRAMES, fps=FPS, format="png", width=WIDTH, height=HEIGHT,
                               output=str(output), exit_play=True, timeout_seconds=1500, timeout=1800)
        files = sorted(output.rglob("frame_*.png"))
        ed.check_eq(result["frames"], FRAMES, f"take {take + 1} completed: {result}")
        ed.check_eq(len(files), FRAMES, f"take {take + 1} contains every frame")
        if take == 0:
            region = pngdiff.crop(pngdiff.decode(files[0].read_bytes()), 90, 245, 850, 300)[2]
            bright = sum(1 for pixel in range(0, len(region), 4) if min(region[pixel:pixel + 3]) > 200)
            ed.check_eq(bright, 0, "frame zero has no visible pale headline pixels before the masked reveal")
        hashes = recording_hashes(ffmpeg, files)
        for index in (0, FRAMES // 2, FRAMES - 1):
            ed.check_eq(hashes[index], frame_hash(files[index]),
                        f"FFmpeg RGBA8 hash matches the independent PNG decoder: take {take + 1}, frame {index}")
        ed.check(all((width, height) == (WIDTH, HEIGHT) for width, height, _ in hashes), "every frame is 1080x1350")
        ed.check(len({digest for _, _, digest in hashes}) > 100, "the six-second composition visibly animates")
        takes.append(hashes)
    ed.check_eq(takes[0], takes[1], "all 180 decoded frames are pixel-identical across two recordings")
    errors = await ed.call("read_console", since=before, types=["error", "warning"], limit=1000)
    problems = [entry["message"] for entry in errors["entries"]
                if entry.get("type") == "error" or "[HTML UI]" in entry.get("message", "") or "Lua" in entry.get("message", "")]
    ed.check_eq(problems, [], "no script or document warnings during either recording")

    samples = ed.scratch.root / "motion-graphics-samples"
    samples.mkdir()
    process = subprocess.run([ffmpeg, "-nostdin", "-y", "-v", "error", "-framerate", str(FPS), "-i",
                              str(ed.scratch.root / "motion-graphics-1/frame_%04d.png"), "-vf",
                              "select=eq(n\\,0)+eq(n\\,9)+eq(n\\,30)+eq(n\\,75)+eq(n\\,135)+eq(n\\,179)",
                              "-vsync", "vfr", str(samples / "sample_%02d.png")],
                             capture_output=True, text=True, timeout=180)
    ed.check_eq(process.returncode, 0, f"FFmpeg extracted frames 0, 9, 30, 75, 135, 179: {process.stderr}")
    ed.check_eq(len(list(samples.glob("sample_*.png"))), 6, "samples at 0, .3, 1, 2.5, 4.5, 5.9667 seconds")
    (ed.scratch.root / "motion-graphics-pixel-hashes.json").write_text(json.dumps({
        "algorithm": "sha256", "pixel_format": "RGBA8", "bytes_per_frame": WIDTH * HEIGHT * 4,
        "independent_png_decoder_frames": [0, FRAMES // 2, FRAMES - 1], "takes": takes,
    }, indent=2))
    print(f"     [evidence] two 180-frame takes and six FFmpeg samples: {ed.scratch.root}", flush=True)


async def test_motion_graphics_procedural_cost_per_frame(ed: Editor):
    await start(ed)
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    output = ed.scratch.root / "motion-graphics-profile"
    recording = await ed.call("record_game", frames=64, fps=FPS, format="png", width=WIDTH, height=HEIGHT,
                              output=str(output), exit_play=True, wait=False)
    job_id = recording["job_id"]
    try:
        # Profile the fixed recording clock, so the six-second timeline stays active even
        # when thirty software-rendered frames take more than six wall-clock seconds.
        for _ in range(120):
            if output.is_dir() and any(path.stat().st_size > 0 for path in output.glob(".frame_*.je-tmp")):
                break
            state = await ed.call("get_job", job_id=job_id)
            ed.check_eq(state["status"], "running", f"the recording is still arming: {state}")
            await ed.call("wait", frames=1, timeout_seconds=60)
        else:
            ed.fail("the profiling recording did not capture its first frame")

        capture = await ed.call("capture_profile", frames=30, gpu=False,
                                save_path="captures/motion-graphics.jetrace.json", timeout=600)
        samples = []
        for frame in ("median", "worst"):
            profile = await ed.call("get_cpu_profile", capture_id=capture["capture_id"], frame=frame,
                                    filter="motion_graphics.procedural")
            rows = [row for row in profile["rows"] if row["name"] == "motion_graphics.procedural"]
            ed.check(rows and rows[0]["calls"] == 1, f"the procedural callback ran once in the {frame} frame: {profile}")
            samples.append({"frame": frame, "geometry_cpu_ms": rows[0]["total_ms"], "frame_cpu_ms": profile["cpu_ms"]})
        for _ in range(120):
            state = await ed.call("get_job", job_id=job_id)
            if state["status"] != "running":
                break
            await ed.call("wait", frames=2, timeout_seconds=60)
        ed.check_eq(state["status"], "succeeded", f"the profiling recording completed: {state}")
        ed.check_eq(state["result"]["frames"], 64, "the profiling workload captured every fixed-time frame")
        output = ed.scratch.root / "motion-graphics-cost.json"
        output.write_text(json.dumps({"stars": 30, "samples": samples, "capture": capture,
                                      "recording": state}, indent=2))
        print(f"     [bench] motion graphics, 30 stars: {json.dumps(samples)}; {output}", flush=True)
    finally:
        state = await ed.call("get_job", job_id=job_id)
        if state["status"] == "running":
            await ed.call("cancel_job", job_id=job_id)
        await ed.call("play", action="stop")
