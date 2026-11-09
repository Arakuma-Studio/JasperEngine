"""capture_contact_sheet (docs/specs/S48-pixel-fx.md R27, R28, R36, AC22, AC23; editor/mcp/tools/PixelFxTools.cpp,
editor/pixelart/fx/, editor/pixelart/lint/EffectLints.cpp) on a new project with pixels per unit 16 and the assets of
tools/fixtures/pixel-fx copied in:

- the pixel_art group hides the tool until tool_groups enables it;
- AC22: an isolated sheet of an 8x8 sprite: the grid (columns x rows per backdrop band), integer scale (every scaled
  art pixel one colour), labels, backdrop rows; two identical calls give the same PNG bytes; the automatic scale is the
  largest that fits max_width; output: true writes the PNG as one project undo step and undo removes it; scene mode
  shows the scene through its camera, stopped and in Play (the paused game stepped, still paused after);
- AC23: the lints on the fixtures, each red one with its fixed twin: a leftover particle at the end (one_shot_end
  WARN), a burst spilling past the margin (spill WARN with its spans), a loop that jumps (loop_seam WARN), a one-frame
  blink (flicker WARN), an unseeded random script in Play (determinism FAIL, verdict needs_person), and a still sprite
  where nothing warns (verdict clean); the checks are the verdict tools' rows (S35 / S91 shape);
- R36: 16 frames of a 64x64 target at scale 4 within 2 s, with every lint within 5 s (reported, harness.over_budget).
"""

from __future__ import annotations

import shutil
import time

import pngdiff
from harness import REPO, Editor, over_budget

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "ContactSheet"}

FIXTURE = REPO / "tools" / "fixtures" / "pixel-fx"
HERO_PNG = "assets/sprites/hero.png"
BIG_PNG = "assets/sprites/big.png"
M = "submeshes[0].material."
S: dict = {}

DARK = (0x15, 0x13, 0x1D, 255)
LIGHT = (0xEB, 0xE8, 0xF0, 255)


def hero_pixels(size: int) -> bytes:
    """An opaque two-colour sprite: a red border around a green middle"""
    out = bytearray()
    for y in range(size):
        for x in range(size):
            edge = x in (0, size - 1) or y in (0, size - 1)
            out += bytes((200, 40, 40, 255)) if edge else bytes((40, 180, 60, 255))
    return bytes(out)


def pixel(rgba: bytes, width: int, x: int, y: int) -> tuple[int, int, int, int]:
    i = (y * width + x) * 4
    return tuple(rgba[i:i + 4])


async def sheet(ed: Editor, **args):
    r = await ed.call_raw("capture_contact_sheet", args, timeout=300)
    ed.check(not r.is_error, f"capture_contact_sheet failed: {r.text}")
    ed.check(r.images, "the sheet comes back as an image")
    png = r.images[0][0]
    width, height, rgba = pngdiff.decode(png)
    return r.value, png, width, height, rgba


def check_of(ed: Editor, value: dict, lint_id: str) -> dict:
    rows = [row for row in value.get("checks", []) if row["id"] == lint_id]
    ed.check(len(rows) == 1, f"one {lint_id} row: {value.get('checks')}")
    return rows[0]


async def sprite(ed: Editor, name: str, texture: str, size: int, at=(0.0, 0.0)) -> int:
    made = await ed.call("create_entity", scene=S["scene"], type="sprite", name=name, position=[at[0], at[1], 0],
                         values={"mesh": {M + "baseColorTexture": {"path": texture, "min_filter": "nearest", "mag_filter": "nearest"}},
                                 "sprite": {"pivotPreset": "center", "width": size, "height": size}})
    return made["entity_id"]


async def test_hidden_until_enabled(ed: Editor):
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_contact_sheet" not in listed, "pixel_art is hidden by default")
    await ed.call("tool_groups", enable=["pixel_art"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_contact_sheet" in listed, "listed once tool_groups enables pixel_art")


async def test_setup(ed: Editor):
    await ed.call("get_project_summary")
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets", "graphics.pixels_per_unit": 16,
                                                   "graphics.canvas_width": 320, "graphics.canvas_height": 180})
    for folder in ("fx", "anim", "scripts"):
        shutil.copytree(FIXTURE / folder, ed.project / "assets" / folder, dirs_exist_ok=True)
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    (ed.project / HERO_PNG).write_bytes(pngdiff.encode(8, 8, hero_pixels(8)))
    (ed.project / BIG_PNG).write_bytes(pngdiff.encode(64, 64, hero_pixels(64)))
    await ed.call("sync_files")
    await ed.call("set_texture_import_settings", paths=[HERO_PNG, BIG_PNG], settings={"quality": "none", "mipmaps": False})
    S["scene"] = (await ed.call("create_scene", name="Sheets", type="2D"))["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.1, 0.2, 0.3, 1.0]})
    S["hero"] = await sprite(ed, "Hero", HERO_PNG, 8)
    await ed.call("select_scene", scene=S["scene"])


async def test_grid_scale_labels_and_backdrops(ed: Editor):
    value, png, width, height, rgba = await sheet(ed, target={"entity": S["hero"]}, frames=6, columns=3, scale=4, margin=4,
                                                  backdrops=["dark", "light"])
    # the frame: the 8x8 sprite and twice the margin each side
    ed.check_eq(value["cell"], [24, 24])
    ed.check_eq(value["scale"], 4)
    ed.check_eq(value["columns"], 3)
    ed.check_eq(value["backdrops"], ["dark", "light"])
    ed.check_eq([f["index"] for f in value["frames"]], list(range(6)))
    ed.check_eq([f["t"] for f in value["frames"]], [0, 0.2, 0.4, 0.6, 0.8, 1.0], "one-shot times from 0 to the duration")
    ed.check_eq(value["frames"][0]["bbox"], [8, 8, 8, 8], "the sprite's drawn box, in art pixels of the frame")
    ed.check_eq(value["bounds"], [8, 8, 8, 8])
    # PoseContactSheet geometry: a 14 px title strip per band, cells of 96 px with a 14 px label strip, 1 px separators
    cell, label, title = 96, 14, 14
    band = title + 1 + 2 * (cell + label) + 1
    ed.check_eq((width, height), (3 * cell + 2, 2 * band + 1))
    ed.check_eq(value["size"], [width, height])
    for b, colour in ((0, DARK), (1, LIGHT)):
        top = b * (band + 1) + title + 1
        ed.check_eq(pixel(rgba, width, 0, top), colour, f"band {b} is drawn on its backdrop")
        # the sprite's art pixel (8, 8) is red and covers a whole 4x4 block of the sheet
        block = {pixel(rgba, width, 32 + dx, top + 32 + dy) for dx in range(4) for dy in range(4)}
        ed.check_eq(block, {(200, 40, 40, 255)}, "an art pixel is one 4x4 block")
        # integer scale: every 4x4 block of the cell is one colour
        uneven = 0
        for by in range(24):
            for bx in range(24):
                colours = {pixel(rgba, width, bx * 4 + dx, top + by * 4 + dy) for dx in range(4) for dy in range(4)}
                uneven += len(colours) != 1
        ed.check_eq(uneven, 0, "every art pixel is a whole block")
        # the label strip holds white text
        white = sum(pixel(rgba, width, x, top + cell + y) == (255, 255, 255, 255) for x in range(cell) for y in range(label))
        ed.check(white > 20, f"the label strip of frame 0 has text ({white} px)")
    ed.check("output: true" in value.get("next", ""), f"a preview says how to write: {value.get('next')}")
    S["png"] = png


async def test_identical_calls_give_identical_bytes(ed: Editor):
    _, png, _, _, _ = await sheet(ed, target={"entity": S["hero"]}, frames=6, columns=3, scale=4, margin=4, backdrops=["dark", "light"])
    ed.check(png == S["png"], "the same arguments give the same PNG bytes")


async def test_auto_scale_fits_max_width(ed: Editor):
    value, _, width, _, _ = await sheet(ed, target={"entity": S["hero"]}, frames=4, margin=4, max_width=300)
    # 4 frames of 24 px: 4 * 24 * 3 + 3 = 291 <= 300 < 4 * 24 * 4 + 3
    ed.check_eq(value["scale"], 3)
    ed.check_eq(width, 291)
    ed.check_eq(value["backdrops"], ["checker"], "the default backdrop")


async def test_output_writes_one_undo_step(ed: Editor):
    path = "assets/sheets/hero.png"
    before = {e["id"] for e in (await ed.call("get_undo_history", scope="project", limit=500))["entries"]}
    value, png, _, _, _ = await sheet(ed, target={"entity": S["hero"]}, frames=6, columns=3, scale=4, margin=4,
                                      backdrops=["dark", "light"], output=True, path=path)
    written = ed.project / path
    ed.check(written.is_file(), "the PNG is written")
    ed.check(written.read_bytes() == png, "the file is the returned image")
    added = [e for e in (await ed.call("get_undo_history", scope="project", limit=500))["entries"] if e["id"] not in before]
    ed.check_eq(len(added), 1, f"one undo step: {[e['name'] for e in added]}")
    ed.check_eq(value.get("undo", {}).get("id"), added[0]["id"], "the result names its undo step")
    await ed.call("undo", scope="project")
    ed.check(not written.exists(), "one undo removes the PNG")


async def test_errors_name_the_fix(ed: Editor):
    bad = await ed.call_raw("capture_contact_sheet", {"target": {"entity": S["hero"]}, "backdrops": ["grass"]})
    ed.check(bad.is_error and "grass" in bad.text and "checker" in bad.text, f"an unknown backdrop lists the names: {bad.text}")
    bad = await ed.call_raw("capture_contact_sheet", {"target": {"entity": S["hero"]}, "output": True})
    ed.check(bad.is_error and "path" in bad.text, f"output needs path: {bad.text}")
    bad = await ed.call_raw("capture_contact_sheet", {"target": {"entity": S["hero"]}, "lint": ["jaggies"]})
    ed.check(bad.is_error and "spill" in bad.text, f"an unknown lint lists the ids: {bad.text}")
    bad = await ed.call_raw("capture_contact_sheet", {"target": {"particles": "assets/fx/none.particles"}})
    ed.check(bad.is_error and "create_particle_effect" in bad.text, f"a missing effect names how to make one: {bad.text}")
    bad = await ed.call_raw("capture_contact_sheet", {"target": {"particles": "assets/fx/burst_clean.particles"}, "mode": "scene"})
    ed.check(bad.is_error and "isolated" in bad.text, f"an asset has no scene: {bad.text}")


async def test_scene_mode_stopped(ed: Editor):
    value, _, width, height, rgba = await sheet(ed, target={"entity": S["hero"]}, mode="scene", frames=2, scale=1, labels=False)
    # the 320x180 canvas at most 480 wide: whole
    ed.check_eq(value["cell"], [320, 180])
    ed.check_eq(value["backdrops"], ["scene"], "one band: the view itself")
    ed.check_eq(value["mode"], "scene")
    corner = pixel(rgba, width, 0, 15)
    ed.check(abs(corner[0] - 26) <= 3 and abs(corner[2] - 77) <= 3, f"the scene's background colour fills the view: {corner}")


# ------------------------------------------------------------------ AC23: the lints and their fixed twins


async def test_one_shot_end_and_spill(ed: Editor):
    clean = await sheet(ed, target={"particles": "assets/fx/burst_clean.particles"}, frames=8, lint=True, margin=24)
    value = clean[0]
    ed.check_eq(value["duration"], 1, "the effect's own duration")
    ed.check_eq(value["loop"], False)
    ed.check_eq(check_of(ed, value, "one_shot_end")["status"], "PASS")
    ed.check_eq(check_of(ed, value, "spill")["status"], "PASS")
    leftover = (await sheet(ed, target={"particles": "assets/fx/burst_leftover.particles"}, frames=8, lint=True))[0]
    row = check_of(ed, leftover, "one_shot_end")
    ed.check_eq(row["status"], "WARN", f"a particle left at t = duration: {row}")
    ed.check(row["measured"] > 0 and row["fix_hint"], f"measured and fix_hint: {row}")
    spilled = (await sheet(ed, target={"particles": "assets/fx/burst_spill.particles"}, frames=8, lint=["spill"]))[0]
    row = check_of(ed, spilled, "spill")
    ed.check_eq(row["status"], "WARN", f"a burst flying 38 px from its emitter spills past 24: {row}")
    ed.check("spans" in row["limited_by"] and "past the margin" in row["limited_by"], f"the message gives the spans: {row}")
    ed.check(row["spots"] and row["at"], f"spots and times: {row}")
    ed.check_eq([r["id"] for r in spilled["checks"]], ["spill"], "lint names run just those")


async def test_loop_seam(ed: Editor):
    jump = await sprite(ed, "Jumper", HERO_PNG, 8, at=(-4.0, 0.0))
    smooth = await sprite(ed, "Glider", HERO_PNG, 8, at=(-4.0, 3.0))
    red = (await sheet(ed, target={"entity": jump, "animation": "assets/anim/slide_jump.anim"}, frames=8, lint=["loop_seam"]))[0]
    ed.check_eq(red["loop"], True, "a looping clip loops")
    row = check_of(ed, red, "loop_seam")
    ed.check_eq(row["status"], "WARN", f"the slide jumps back 14 px: {row}")
    fixed = (await sheet(ed, target={"entity": smooth, "animation": "assets/anim/slide_pingpong.anim"}, frames=8, lint=["loop_seam"]))[0]
    ed.check_eq(check_of(ed, fixed, "loop_seam")["status"], "PASS")


async def test_flicker(ed: Editor):
    blinker = await sprite(ed, "Blinker", HERO_PNG, 8, at=(4.0, 0.0))
    red = (await sheet(ed, target={"entity": blinker, "animation": "assets/anim/blink_one.anim"}, frames=8, lint=["flicker"]))[0]
    row = check_of(ed, red, "flicker")
    ed.check_eq(row["status"], "WARN", f"gone for one frame of 8: {row}")
    ed.check(0.99 <= row["measured"] <= 1.0, f"every drawn pixel blinks: {row}")
    fixed = (await sheet(ed, target={"entity": blinker, "animation": "assets/anim/blink_two.anim"}, frames=8, lint=["flicker"]))[0]
    ed.check_eq(check_of(ed, fixed, "flicker")["status"], "PASS", "gone for two frames is no flicker")


async def test_clean_fixture(ed: Editor):
    value = (await sheet(ed, target={"entity": S["hero"]}, frames=8, loop=True, lint=True))[0]
    statuses = {row["id"]: row["status"] for row in value["checks"]}
    ed.check(all(s in ("PASS", "SKIP") for s in statuses.values()), f"nothing warns on a still sprite: {statuses}")
    ed.check_eq(value["verdict"], "clean")
    ed.check_eq(statuses["determinism"], "PASS", "two fresh isolated captures agree")
    ed.check_eq(statuses["spill"], "PASS")
    ed.check_eq(statuses["transition_edges"], "SKIP")
    for row in value["checks"]:
        for key in ("id", "kind", "status", "round", "measured", "threshold", "threshold_source", "limited_by", "at",
                    "fix_hint", "method", "count", "spots"):
            ed.check(key in row, f"{row['id']} has {key} (the verdict tools' row): {row}")
    ed.check("thresholds" in value and value["thresholds"]["changed"] == [], f"the thresholds used: {value.get('thresholds')}")


async def test_determinism_in_play(ed: Editor):
    # a 2D scene's camera works in points: a 128 px sprite at 16 per unit is 8 points wide, and the scripts
    # move it by up to 24 points
    red = await sprite(ed, "Random", HERO_PNG, 128, at=(0.0, -3.0))
    await ed.call("create_script", kind="lua", name="RandomStart", content=(FIXTURE / "scripts" / "RandomStart.lua").read_text(), entity=red)
    await ed.call("save_scene", scene=S["scene"], path="assets/scenes/Sheets")
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        value = (await sheet(ed, target={"entity": "Random"}, mode="scene", frames=3, duration=0.2, lint=["determinism"]))[0]
        row = check_of(ed, value, "determinism")
        ed.check_eq(row["status"], "FAIL", f"an unseeded random start differs between two fresh loads: {row}")
        ed.check_eq(value["verdict"], "needs_person")
        ed.check(value.get("next"), "the guidance comes in next")
        state = await ed.call("get_play_state")
        ed.check(state.get("time", {}).get("held", True), f"the game is still paused after the capture: {state}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("delete_entity", scene=S["scene"], entity=red)
    fixed = await sprite(ed, "Seeded", HERO_PNG, 128, at=(0.0, -3.0))
    await ed.call("create_script", kind="lua", name="SeededStart", content=(FIXTURE / "scripts" / "SeededStart.lua").read_text(), entity=fixed)
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        value = (await sheet(ed, target={"entity": "Seeded"}, mode="scene", frames=3, duration=0.2, lint=["determinism"]))[0]
        ed.check_eq(check_of(ed, value, "determinism")["status"], "PASS", "seeded at every start: the fresh loads agree")
        ed.check_eq(len(value["frames"]), 3)
    finally:
        await ed.call("play", action="stop")


async def test_budget(ed: Editor):
    big = await sprite(ed, "Big", BIG_PNG, 64, at=(0.0, 8.0))
    start = time.monotonic()
    value, _, _, _, _ = await sheet(ed, target={"entity": big}, frames=16, scale=4, margin=0, max_width=8192)
    plain = time.monotonic() - start
    ed.check_eq(value["cell"], [64, 64])
    start = time.monotonic()
    await sheet(ed, target={"entity": big}, frames=16, scale=4, margin=0, max_width=8192, lint=True)
    linted = time.monotonic() - start
    over = over_budget("16 frames of 64x64 at scale 4", {"sheet": plain}, 2.0)
    over.update(over_budget("the same with every lint", {"linted": linted}, 5.0))
    ed.check(not over, f"over budget: {over}")
