"""Golden images and pass counts of the S13 post FX (docs/specs/S13-post-fx.md AC13, AC14, AC15) on the
post-processing fixture (tools/fixtures/post-processing: the checker floor, the poles, the lit wall,
saturated objects, the HUD layer), played at a fixed 60 fps with the uber pass's time pinned
(set_post_processing_debug time_seconds), so grain, VHS and film damage hold still.

  test_goldens          one image per effect: vhs, film (Lens grain, damage, the Academy gate mask),
                        filmic (the Filmic grain), halation (its own chain) and halation_bloom (bloom's
                        levels), cube_log (an ACEScct .cube before the tone mapping), cube_display (an
                        sRGB .cube in the grade), and the presets film, vhs and mobile_adaptive (its
                        scale pinned at 1: the frame cost of a test machine must not move it). The Game
                        view's canvas (1280x720) downscaled to 640x360, compared with
                        tools/mcp/tests/golden/post_fx/<name>.png by pngdiff.compare(tolerance 24, at
                        most 1 % of the pixels); JASPER_POST_FX_UPDATE_GOLDEN=1 writes the references
  test_defaults_unchanged  every S13 field at its default: the same pixels (tolerance 0) as the
                        fixture's pre-S13 reference (golden/post_fx/defaults.png, captured on oe-port
                        before S13), and the frame stats' pass list of the frame has no S13 pass.
                        S14's mip chain (R4: linear-light Mitchell mips for the raw-load path too)
                        changed the mip-filtered texels of the checker floor and glow.png, so every
                        reference here was captured again (BF7); the old and new images differ only
                        on those textured surfaces (each golden's changed pixels lie inside the
                        defaults' changed area, every untextured pixel the same). Dynamic batching
                        (sprite-batching, 48cf20e75) moved 89 pixels of the additive glow and the
                        multiply quad by 1/255, so defaults.png was captured again then
  test_pass_counts      halation with bloom adds no pass to bloom's, halation alone adds 2 (its
                        prefilter and one level down), VHS, film damage and the grain types add none
                        to the uber pass alone (JASPERENGINE_FRAME_STATS=all)
  test_glitch           S48's Glitch (AC15): continuous bursts (rate 0) at three pinned times, each
                        compared with golden/post_fx/glitch_<t>.png; a pinned time gives one image (the
                        same time twice: identical), other times differ; it adds no pass to the uber
                        pass alone
"""

from __future__ import annotations

import os
import re
import time

import harness
import pngdiff
from harness import Editor

# read at editor start: this file runs in its own process
os.environ["JASPERENGINE_READY_SIGNAL"] = "1"
os.environ["JASPERENGINE_FRAME_STATS"] = "all"

CI = True
EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "post-processing"}
TIMEOUT = 1800

GOLDEN_DIR = harness.REPO / "tools" / "mcp" / "tests" / "golden" / "post_fx"
UPDATE_GOLDEN = os.environ.get("JASPER_POST_FX_UPDATE_GOLDEN") == "1"
GOLDEN_SIZE = (640, 360)
CHANNEL_TOLERANCE = 24
MAX_FRACTION = 0.01
TIME = 1.25     # the uber effects' pinned time, seconds

CASES = {
    "vhs": {"vhs_enabled": True},
    "film": {"film_grain_enabled": True, "film_grain_type": "lens", "film_grain_intensity": 0.35,
             "film_damage_enabled": True, "film_dust_amount": 6.0, "film_scratch_amount": 2.0, "film_hair_amount": 1.0,
             "film_gate_mask": "academy_137"},
    "filmic": {"film_grain_enabled": True, "film_grain_type": "filmic", "film_grain_intensity": 0.5, "film_grain_colored": True},
    "halation": {"halation_enabled": True, "halation_intensity": 1.5},
    "halation_bloom": {"halation_enabled": True, "halation_intensity": 1.5, "bloom_enabled": True, "bloom_intensity": 1.5,
                       "bloom_threshold": 0.8},
    "cube_log": {"tonemap": "aces", "color_grading_enabled": True, "lut_texture": "assets/luts/look_acescct.cube"},
    "cube_display": {"color_grading_enabled": True, "lut_texture": "assets/luts/look_display.cube"},
}
PRESETS = ["film", "vhs", "mobile_adaptive"]

LINE = re.compile(r"Frame stats \(frame \d+\): .*pass list: (.*)$")


def check(ed: Editor, cond, message: str):
    ed.check(cond, message)


def log_length(ed: Editor) -> int:
    log = ed.scratch.root / "editor.log"
    return len(log.read_text(errors="replace").splitlines()) if log.exists() else 0


def pass_lists(ed: Editor, start: int) -> list[list[str]]:
    """The pass names of each frame line since `start` (the frame's own view)."""
    log = ed.scratch.root / "editor.log"
    lines = log.read_text(errors="replace").splitlines()[start:] if log.exists() else []
    out = []
    for line in lines:
        m = LINE.search(line)
        if m:
            # "name WxH ..." entries separated by commas
            out.append([re.sub(r" \d+x\d+.*$", "", item.strip()) for item in m.group(1).split(",") if item.strip()])
    return out


async def capture(ed: Editor, label: str):
    """The Game view's canvas, downscaled to the golden size."""
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    check(ed, not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"{label}.png").write_bytes(png)
    rect = shot.value["canvas_rect_in_image"]
    canvas = pngdiff.crop(pngdiff.decode(png), round(rect["x"]), round(rect["y"]), round(rect["width"]), round(rect["height"]))
    check(ed, canvas[0] >= GOLDEN_SIZE[0] and canvas[1] >= GOLDEN_SIZE[1], f"{label}: canvas {canvas[0]}x{canvas[1]}")
    return pngdiff.downscale(canvas, *GOLDEN_SIZE)


def compare_golden(ed: Editor, name: str, image, tolerance: int = CHANNEL_TOLERANCE, max_fraction: float = MAX_FRACTION):
    path = GOLDEN_DIR / f"{name}.png"
    # the defaults' reference comes from the editor before S13: an S13 editor never rewrites it
    if UPDATE_GOLDEN and not (name == "defaults" and path.is_file() and ed.has_tool("set_post_processing_debug")):
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        path.write_bytes(pngdiff.encode(image))
        # a copy under build/ (ignored by git, so a sync of the worktree never removes it)
        copy = harness.REPO / "build" / "post_fx_golden" / path.name
        copy.parent.mkdir(parents=True, exist_ok=True)
        copy.write_bytes(path.read_bytes())
        print(f"     [golden] wrote {path.relative_to(harness.REPO)}: look at it")
        return
    check(ed, path.is_file(), f"no golden {path} (JASPER_POST_FX_UPDATE_GOLDEN=1 writes it)")
    ok, fraction, diff = pngdiff.compare(image, pngdiff.load(path), tolerance, max_fraction)
    if not ok:
        # also under build/ (the scratch directory goes with the container that ran the test)
        for folder in (ed.scratch.root, harness.REPO / "build" / "test-results" / "post_fx"):
            folder.mkdir(parents=True, exist_ok=True)
            (folder / f"{name}.actual.png").write_bytes(pngdiff.encode(image))
            (folder / f"{name}.diff.png").write_bytes(diff or b"")
        ed.keep = True
    check(ed, ok, f"{name}: {fraction * 100:.3f}% of the pixels differ from the golden (at most {max_fraction * 100:.2f}%, "
                  f"tolerance {tolerance}): {ed.scratch.root / (name + '.actual.png')}")


async def start(ed: Editor) -> int:
    ed.require_tools("play", "wait", "screenshot", "configure_game_view", "set_scene_settings")
    await ed.call("configure_game_view", aspect="project", scale=1, maximize_on_play=True)
    played = await ed.call("play", action="start", fixed_fps=60)
    sid = played.get("scene", {}).get("id") if isinstance(played.get("scene"), dict) else None
    if sid is None:
        sid = (await ed.call("list_scenes"))["selected_scene_id"]
    await ed.call("wait", frames=60)
    # (a pre-S13 editor, which made the defaults reference, has no debug pins: nothing animates there)
    if ed.has_tool("set_post_processing_debug"):
        await ed.call("set_post_processing_debug", scene=sid, time_seconds=TIME)
    return sid


async def show(ed: Editor, sid: int, settings: dict | None = None, preset: str | None = None) -> None:
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})
    if preset:
        await ed.call("apply_post_processing_preset", scene=sid, preset=preset)
    if settings:
        await ed.call("set_scene_settings", scene=sid, settings={"post_processing": settings})
    # the variant builds, the chain settles
    deadline = time.time() + 120
    while True:
        await ed.call("wait", frames=10)
        if not (settings or preset) or not ed.has_tool("get_post_processing_status"):
            break
        status = await ed.call("get_post_processing_status", scene=sid)
        if status["uber"] or status["tail_color"] or time.time() > deadline:
            break


async def test_goldens(ed: Editor):
    ed.require_tools("set_post_processing_debug", "apply_post_processing_preset", "get_post_processing_status")
    sid = await start(ed)
    try:
        for name, settings in CASES.items():
            await show(ed, sid, settings)
            compare_golden(ed, name, await capture(ed, name))
        for preset in PRESETS:
            if preset == "mobile_adaptive":
                await ed.call("set_post_processing_debug", scene=sid, force_render_scale=1.0)
            await show(ed, sid, preset=preset)
            compare_golden(ed, f"preset_{preset}", await capture(ed, f"preset_{preset}"))
            if preset == "mobile_adaptive":
                await ed.call("set_post_processing_debug", scene=sid, force_render_scale=None)
    finally:
        await ed.call("play", action="stop", keep_changes="none")


async def test_defaults_unchanged(ed: Editor):
    sid = await start(ed)
    try:
        await show(ed, sid)
        mark = log_length(ed)
        await ed.call("wait", frames=10)
        image = await capture(ed, "defaults")
        compare_golden(ed, "defaults", image, tolerance=0, max_fraction=0.0)
        lists = pass_lists(ed, mark)
        check(ed, lists, "frame stats lines")
        for passes in lists:
            check(ed, not any(p in ("uber", "halation prefilter", "halation down") for p in passes),
                  f"defaults: no uber pass, no S13 pass: {passes}")
    finally:
        await ed.call("play", action="stop", keep_changes="none")


async def test_pass_counts(ed: Editor):
    sid = await start(ed)

    async def passes(settings: dict) -> list[str]:
        await show(ed, sid, settings)
        mark = log_length(ed)
        await ed.call("wait", frames=10)
        lists = []
        deadline = time.time() + 30
        while len(lists) < 3 and time.time() < deadline:
            lists = pass_lists(ed, mark)
            if len(lists) < 3:
                await ed.call("wait", frames=5)
        check(ed, lists, f"frame stats for {settings}")
        return lists[-1]

    try:
        uber_only = await passes({"fxaa_enabled": True})
        for name, settings in (("vhs", {"vhs_enabled": True}), ("film damage", {"film_damage_enabled": True}),
                               ("lens grain", {"film_grain_enabled": True, "film_grain_type": "lens"}),
                               ("filmic grain", {"film_grain_enabled": True, "film_grain_type": "filmic"})):
            got = await passes(settings)
            check(ed, len(got) == len(uber_only) and "uber" in got, f"{name} adds no pass: {got} (uber alone: {uber_only})")

        bloom = await passes({"bloom_enabled": True})
        both = await passes({"bloom_enabled": True, "halation_enabled": True})
        check(ed, both == bloom, f"halation with bloom adds no pass: {both} (bloom: {bloom})")
        alone = await passes({"halation_enabled": True})
        check(ed, len(alone) == len(uber_only) + 2 and "halation prefilter" in alone and "halation down" in alone,
              f"halation alone adds 2 passes: {alone} (uber alone: {uber_only})")
        print(f"uber alone {uber_only}; bloom {bloom}; halation {alone}")
    finally:
        await ed.call("play", action="stop", keep_changes="none")


GLITCH = {"glitch_enabled": True, "glitch_burst_rate": 0.0, "glitch_seed": 7, "glitch_tears": 6, "glitch_tear_shift": 24.0,
          "glitch_rgb_split": 4.0, "glitch_interlace": 1.0}
GLITCH_TIMES = (0.5, 1.25, 2.0)


async def test_glitch(ed: Editor):
    ed.require_tools("set_post_processing_debug", "get_post_processing_status")
    sid = await start(ed)
    try:
        uber_only_mark = log_length(ed)
        await show(ed, sid, {"fxaa_enabled": True})
        await ed.call("wait", frames=10)
        uber_only = pass_lists(ed, uber_only_mark)
        await show(ed, sid, GLITCH)
        status = await ed.call("get_post_processing_status", scene=sid)
        check(ed, "Glitch" in status["uber_features"], f"the uber variant has Glitch: {status['uber_features']}")
        images = {}
        for t in GLITCH_TIMES:
            await ed.call("set_post_processing_debug", scene=sid, time_seconds=t)
            await ed.call("wait", frames=4)
            images[t] = await capture(ed, f"glitch_{t}")
            compare_golden(ed, f"glitch_{t}", images[t], tolerance=0, max_fraction=0.0)
        await ed.call("set_post_processing_debug", scene=sid, time_seconds=GLITCH_TIMES[0])
        await ed.call("wait", frames=4)
        again = await capture(ed, "glitch_again")
        check(ed, again[2] == images[GLITCH_TIMES[0]][2], "a pinned time gives one image")
        check(ed, images[GLITCH_TIMES[0]][2] != images[GLITCH_TIMES[1]][2], "another time, another corruption")
        mark = log_length(ed)
        await ed.call("wait", frames=10)
        lists = pass_lists(ed, mark)
        check(ed, lists and uber_only and len(lists[-1]) == len(uber_only[-1]) and "uber" in lists[-1],
              f"Glitch adds no pass: {lists[-1] if lists else None} (uber alone: {uber_only[-1] if uber_only else None})")
    finally:
        await ed.call("set_post_processing_debug", scene=sid, time_seconds=TIME)
        await ed.call("play", action="stop", keep_changes="none")
