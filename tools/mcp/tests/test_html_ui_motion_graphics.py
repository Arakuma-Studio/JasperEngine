"""The portrait motion-graphics example at its public Lua/record_game boundary.

The new SVG/text modules must exist, procedural geometry must be seekable, and two six-second
1080x1350 recordings at 30 fps must have identical decoded pixels in all 180 frames. The trace
also measures the thirty seeded stars' Lua geometry update separately from SVG rasterization.
"""

from __future__ import annotations

import hashlib
import json
import shutil
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


async def test_motion_graphics_six_second_recordings_are_identical(ed: Editor):
    # Fail immediately on a player without the modules, before spending time on 360 frames.
    await start(ed)
    await ed.call("play", action="stop")
    before = (await ed.call("read_console", limit=1))["next_cursor"]
    takes = []
    for take in range(2):
        output = ed.scratch.root / f"motion-graphics-{take + 1}"
        result = await ed.call("record_game", frames=FRAMES, fps=FPS, format="png", width=WIDTH, height=HEIGHT,
                               output=str(output), exit_play=True, timeout_seconds=1500, timeout=1800)
        files = sorted(output.rglob("frame_*.png"))
        ed.check_eq(result["frames"], FRAMES, f"take {take + 1} completed: {result}")
        ed.check_eq(len(files), FRAMES, f"take {take + 1} contains every frame")
        hashes = [frame_hash(path) for path in files]
        ed.check(all((width, height) == (WIDTH, HEIGHT) for width, height, _ in hashes), "every frame is 1080x1350")
        ed.check(len({digest for _, _, digest in hashes}) > 100, "the six-second composition visibly animates")
        takes.append(hashes)
    ed.check_eq(takes[0], takes[1], "all 180 decoded frames are pixel-identical across two recordings")
    errors = await ed.call("read_console", since=before, types=["error", "warning"], limit=1000)
    problems = [entry["message"] for entry in errors["entries"]
                if entry.get("type") == "error" or "[HTML UI]" in entry.get("message", "") or "Lua" in entry.get("message", "")]
    ed.check_eq(problems, [], "no script or document warnings during either recording")

    ffmpeg = shutil.which("ffmpeg")
    ed.check(ffmpeg is not None, "FFmpeg is available to extract the visual review frames")
    samples = ed.scratch.root / "motion-graphics-samples"
    samples.mkdir()
    process = subprocess.run([ffmpeg, "-nostdin", "-y", "-v", "error", "-framerate", str(FPS), "-i",
                              str(ed.scratch.root / "motion-graphics-1/frame_%04d.png"), "-vf",
                              "select=eq(n\\,0)+eq(n\\,9)+eq(n\\,30)+eq(n\\,75)+eq(n\\,135)+eq(n\\,179)",
                              "-vsync", "vfr", str(samples / "sample_%02d.png")],
                             capture_output=True, text=True, timeout=180)
    ed.check_eq(process.returncode, 0, f"FFmpeg extracted frames 0, 9, 30, 75, 135, 179: {process.stderr}")
    ed.check_eq(len(list(samples.glob("sample_*.png"))), 6, "samples at 0, .3, 1, 2.5, 4.5, 5.9667 seconds")
    (ed.scratch.root / "motion-graphics-pixel-hashes.json").write_text(json.dumps(takes, indent=2))
    print(f"     [evidence] two 180-frame takes and six FFmpeg samples: {ed.scratch.root}", flush=True)


async def test_motion_graphics_procedural_cost_per_frame(ed: Editor):
    await start(ed)
    try:
        capture = await ed.call("capture_profile", frames=30, gpu=False,
                                save_path="captures/motion-graphics.jetrace.json", timeout=600)
        samples = []
        for frame in ("median", "worst"):
            profile = await ed.call("get_cpu_profile", capture_id=capture["capture_id"], frame=frame,
                                    filter="motion_graphics.procedural")
            rows = [row for row in profile["rows"] if row["name"] == "motion_graphics.procedural"]
            ed.check(rows and rows[0]["calls"] == 1, f"the procedural callback ran once in the {frame} frame: {profile}")
            samples.append({"frame": frame, "geometry_cpu_ms": rows[0]["total_ms"], "frame_cpu_ms": profile["cpu_ms"]})
        output = ed.scratch.root / "motion-graphics-cost.json"
        output.write_text(json.dumps({"stars": 30, "samples": samples, "capture": capture}, indent=2))
        print(f"     [bench] motion graphics, 30 stars: {json.dumps(samples)}; {output}", flush=True)
    finally:
        await ed.call("play", action="stop")
