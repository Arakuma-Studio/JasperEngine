"""S51 Pixel Fonts (docs/specs/S51-pixel-fonts.md) through MCP, on an owned project:

- AC7 test_blocks_uniform: a bitmap Text at x1, x2 and x3 draws every font pixel as a k x k block, and
  the x2 and x3 images are the x1 image scaled by whole numbers.
- AC8 test_world_snap: world text under a Pixel Perfect Camera (k = 3) moved 0.3 art px per step lands
  on whole art pixels and keeps its glyph shapes; test_ui_fractional_scale: UI text at a Canvas Scaler
  of 1.37 draws every font pixel as d = round(k * 1.37) device pixels.
- AC9 test_nearest: no filtered texel in bitmap text at x3 (two colours only), outline and shadow
  without a distance field.
- AC10 test_styles: outline 1/2 through 4/8 neighbours and shadows (1, 1) and (-2, 0) on a monochrome
  and a coloured font at x3; no back pixel covers a face pixel.
- AC12 test_text_effect: a Text Effect moves bitmap text (both layers) in Play.
- AC16 test_view_asset, AC17 test_asset_info and test_write_file: the three core tools on fonts.
- AC20 test_lua: BitmapFont.load, measure equals the drawn Text's size, Text.pixelScale, nil + error.
- AC23 test_hot_reload: an edited .bitmapfont and an edited sheet reach the Text.
- AC2 test_vector_text_unchanged: vector text renders as the reference the pre-S51 editor drew
  (tools/mcp/tests/golden/pixel_fonts/, written with JASPER_PIXEL_FONTS_CAPTURE=1 against that editor)
  and AllComponents re-saves byte for byte.
"""

from __future__ import annotations

import json
import os
import shutil

from harness import REPO, Editor
from htmlui_pixels import decode_png
import pngdiff
import ui_input

EDITOR = {"create": True, "name": "Pixel Fonts"}
CI = True
TIMEOUT = 900

S: dict = {}
FIXTURES = REPO / "engine/tests/core/fixtures/fonts"
GOLDEN = REPO / "tools/mcp/tests/golden/pixel_fonts"
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)


def pixels(png: bytes):
    """(width, height, rows of RGB tuples)"""
    w, h, channels, rows = decode_png(png)
    return w, h, [[tuple(row[x * channels:x * channels + 3]) for x in range(w)] for row in rows]


def ink_box(rows, background=BLACK, only=None):
    """the box of the pixels other than `background`, or (only) of that colour"""
    xs, ys = [], []
    for y, row in enumerate(rows):
        for x, p in enumerate(row):
            if (p == only) if only is not None else (p != background):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def outline_detached(rows, reach):
    """outline (red) pixels with no face (white) pixel within `reach` pixels"""
    h, w = len(rows), len(rows[0]) if rows else 0
    count = 0
    for y in range(h):
        for x in range(w):
            if rows[y][x] != (255, 0, 0):
                continue
            near = any(rows[yy][xx] == WHITE for yy in range(max(0, y - reach), min(h, y + reach + 1))
                       for xx in range(max(0, x - reach), min(w, x + reach + 1)))
            count += not near
    return count


def crop(rows, box):
    x0, y0, x1, y1 = box
    return [row[x0:x1] for row in rows[y0:y1]]


def blocks_of(rows, box, k):
    """True when every pixel of the box equals the top-left pixel of its k x k block (blocks from the
    box's corner)"""
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        for x in range(x0, x1):
            if rows[y][x] != rows[y0 + (y - y0) // k * k][x0 + (x - x0) // k * k]:
                return False
    return True


def downsample(rows, box, k):
    x0, y0, x1, y1 = box
    return [[rows[y][x] for x in range(x0, x1, k)] for y in range(y0, y1, k)]


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        # (a test that made another scene left it selected: the Game view shows the selected scene)
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    fonts = ed.project / "assets/fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    for name in ("grid@6x9.png", "colored@6x9.png", "partial@6x9.png", "bm.fnt", "bm_0.png", "pixel8.ttf"):
        shutil.copy(FIXTURES / name, fonts / name)
    (fonts / "colored.bitmapfont").write_text(json.dumps({
        "format": "jasperengine.bitmapfont", "version": 1, "source": {"type": "grid", "image": "colored@6x9.png"}}))
    await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
    made = await ed.call("create_scene", name="Fonts", type="UI", path="scenes/Fonts")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0, 0, 0, 1]})
    await ed.call("create_ui", scene=S["scene"], root={
        "type": "text", "name": "Label", "text": "Hp 99!", "font": "builtin:pixel", "font_size": 9, "text_color": [1, 1, 1, 1],
        "layout": {"anchor": "top_left", "position": [20, 20]}})
    S["label"] = "Label"
    await ed.call("configure_game_view", resolution="320x180")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def label(ed: Editor, values: dict):
    await ed.call("set_properties", scene=S["scene"], entity=S["label"], values=values)
    await ui_input.frames(ed, 4)


SHOTS = REPO / "build" / "test-results" / "pixel-fonts"


async def shot(ed: Editor, name: str = ""):
    """the Game view once two screenshots agree (a new shader variant is made off the frame first);
    kept in build/test-results/pixel-fonts/<name>.png"""
    previous = None
    for _ in range(12):
        image = await ed.screenshot(view="game", max_width=0)
        if image == previous:
            break
        previous = image
        await ui_input.frames(ed, 4)
    if name:
        SHOTS.mkdir(parents=True, exist_ok=True)
        (SHOTS / f"{name}.png").write_bytes(previous)
    return pixels(previous)


async def test_blocks_uniform(ed: Editor):
    await setup(ed)
    images = {}
    for k in (1, 2, 3):
        await label(ed, {"text": {"fontSize": 9 * k, "outlineWidth": 0}})
        _, _, rows = await shot(ed, f"blocks-x{k}")
        box = ink_box(rows)
        ed.check(box is not None, f"x{k} draws")
        ed.check(blocks_of(rows, box, k), f"x{k}: every font pixel is a {k} x {k} block")
        images[k] = (rows, box)
    one = crop(*images[1])
    for k in (2, 3):
        rows, box = images[k]
        ed.check_eq(downsample(rows, box, k), one, f"x{k} is the x1 image scaled by {k}")
    # the scale the Text reports
    entity = await ed.call("get_entity", scene=S["scene"], entity=S["label"], components=["text"])
    ed.check_eq(entity["components"]["text"].get("fontSize"), 27)


async def test_nearest(ed: Editor):
    await setup(ed)
    await label(ed, {"text": {"fontSize": 27}})
    _, _, rows = await shot(ed)
    colours = {p for row in crop(rows, ink_box(rows)) for p in row}
    ed.check(colours <= {WHITE, BLACK}, f"sampled NEAREST: only the text colour and the background ({len(colours)} colours)")
    # an outline and a shadow keep it to whole colours: no distance-field blend
    await label(ed, {"text": {"outlineWidth": 1, "outlineColor": [1, 0, 0, 1], "shadowEnabled": True, "shadowOffset": [1, 1],
                              "shadowColor": [0, 0, 1, 1]}})
    _, _, rows = await shot(ed)
    colours = {p for row in crop(rows, ink_box(rows)) for p in row}
    ed.check(colours <= {WHITE, BLACK, (255, 0, 0), (0, 0, 255)}, f"pixel outline and shadow: {sorted(colours)[:6]}")
    await label(ed, {"text": {"outlineWidth": 0, "shadowEnabled": False}})


async def test_styles(ed: Editor):
    await setup(ed)
    for font, colored in (("builtin:pixel", False), ("assets/fonts/colored.bitmapfont", True)):
        await label(ed, {"text": {"font": font, "fontSize": 27, "outlineWidth": 0, "shadowEnabled": False}})
        tag = "colored" if colored else "mono"
        _, _, plain = await shot(ed, f"styles-{tag}-plain")
        faces = {(x, y) for y, row in enumerate(plain) for x, p in enumerate(row) if p != BLACK}
        ed.check(len(faces) > 0, f"{font} draws")
        counts = {}
        for width, neighbors in ((1, 4), (1, 8), (2, 4), (2, 8)):
            await label(ed, {"text": {"outlineWidth": width, "outlineNeighbors": neighbors, "outlineColor": [1, 0, 0, 1]}})
            _, _, rows = await shot(ed, f"styles-{tag}-outline-{width}-{neighbors}")
            covered = [p for p in faces if rows[p[1]][p[0]] != plain[p[1]][p[0]]]
            ed.check(not covered, f"{font} outline {width}/{neighbors}: no outline pixel over a face ({len(covered)})")
            counts[(width, neighbors)] = sum(1 for row in rows for p in row if p == (255, 0, 0))
            ed.check(blocks_of(rows, ink_box(rows), 3), f"{font} outline {width}/{neighbors}: whole font pixels")
        ed.check(counts[(1, 8)] > counts[(1, 4)] and counts[(2, 4)] > counts[(1, 4)], f"{font}: wider and 8-neighbour outlines grow: {counts}")
        for offset in ([1, 1], [-2, 0]):
            await label(ed, {"text": {"outlineWidth": 0, "shadowEnabled": True, "shadowOffset": offset, "shadowColor": [0, 0, 1, 1]}})
            _, _, rows = await shot(ed, f"styles-{tag}-shadow-{offset[0]}-{offset[1]}")
            covered = [p for p in faces if rows[p[1]][p[0]] != plain[p[1]][p[0]]]
            ed.check(not covered, f"{font} shadow {offset}: no shadow pixel over a face ({len(covered)})")
            blue = sum(1 for row in rows for p in row if p == (0, 0, 255))
            ed.check(blue > 0, f"{font} shadow {offset} draws")
        await label(ed, {"text": {"shadowEnabled": False, "font": "builtin:pixel"}})
        if colored:
            colours = {p for p in (plain[y][x] for x, y in faces)}
            ed.check(len(colours) >= 2, f"the coloured font keeps its colours: {sorted(colours)[:4]}")


async def test_world_snap(ed: Editor):
    await setup(ed)
    made = await ed.call("create_scene", name="World", type="2D", path="scenes/World")
    scene = made["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="camera", name="Main Camera")
    await ed.call("set_main_camera", scene=scene, entity="Main Camera")
    await ed.call("add_component", scene=scene, entity="Main Camera", component="pixelperfectcamera")
    await ed.call("set_properties", scene=scene, entity="Main Camera",
                  values={"pixelperfectcamera": {"referenceWidth": 320, "referenceHeight": 180}})
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0, 0, 0, 1]})
    await ed.call("create_entity", scene=scene, type="text", name="Sign", position=[10, 10, 0])
    await ed.call("set_properties", scene=scene, entity="Sign",
                  values={"text": {"text": "Sign 7", "font": "builtin:pixel", "fontSize": 9}, "ui": {"color": [1, 1, 1, 1]}})
    await ed.call("configure_game_view", resolution="960x540")
    shapes = None
    lefts = []
    try:
        for step in range(10):
            await ed.call("set_properties", scene=scene, entity="Sign", values={"transform": {"position": [10 + 0.3 * step, 10, 0]}})
            _, _, rows = await shot(ed)
            box = ink_box(rows)
            ed.check(box is not None, f"step {step} draws")
            # whole art pixels: 3 x 3 screen pixels each, on the base grid of the 960 x 540 viewport
            ed.check(box[0] % 3 == 0 and box[1] % 3 == 0, f"step {step}: on whole art pixels {box}")
            ed.check(blocks_of(rows, box, 3), f"step {step}: every art pixel 3 x 3")
            shape = downsample(rows, box, 3)
            if shapes is None:
                shapes = shape
            ed.check_eq(shape, shapes, f"step {step}: the glyphs keep their shapes")
            lefts.append(box[0] // 3)
        ed.check(lefts[-1] - lefts[0] in (2, 3), f"it moved 2.7 art px in whole steps: {lefts}")
        # R8 rotation: the camera's S41 policy. Steps90 (the default) draws 30 degrees as 0 and 80 as
        # 90, on whole art pixels; Rotate On Grid keeps the angle
        await ed.call("set_properties", scene=scene, entity="Sign", values={"transform": {"position": [10, 10, 0]}})

        def turned(shape):
            return [list(row) for row in zip(*shape[::-1])]

        for angle, wanted in ((30, shapes), (80, None)):
            await ed.call("set_properties", scene=scene, entity="Sign", values={"transform": {"rotation": {"euler": [0, 0, angle]}}})
            _, _, rows = await shot(ed)
            box = ink_box(rows)
            ed.check(box is not None and blocks_of(rows, box, 3), f"{angle} degrees: whole 3 x 3 art pixels {box}")
            shape = downsample(rows, box, 3)
            if wanted is not None:
                ed.check_eq(shape, wanted, f"{angle} degrees draws as 0 (Steps90)")
            else:
                quarter = turned(shapes)
                ed.check(shape in (quarter, turned(turned(quarter))), f"{angle} degrees draws as a quarter turn (Steps90)")
        await ed.call("set_properties", scene=scene, entity="Main Camera", values={"pixelperfectcamera": {"rotationMode": "on_grid"}})
        await ed.call("set_properties", scene=scene, entity="Sign", values={"transform": {"rotation": {"euler": [0, 0, 30]}}})
        _, _, rows = await shot(ed)
        box = ink_box(rows)
        ed.check(box is not None and (box[3] - box[1]) > len(shapes) * 3, f"Rotate On Grid keeps 30 degrees: {box}")
    finally:
        await ed.call("configure_game_view", resolution="320x180")


async def test_ui_fractional_scale(ed: Editor):
    await setup(ed)
    await ed.call("set_scene_settings", scene=S["scene"], settings={"canvas_scaler": {"mode": "constant_pixel_size", "scale_factor": 1.37}})
    try:
        for k in (1, 2):
            await label(ed, {"text": {"fontSize": 9 * k}})
            _, _, rows = await shot(ed, f"ui-1.37-x{k}")
            box = ink_box(rows)
            d = max(1, round(k * 1.37))
            ed.check(blocks_of(rows, box, d), f"x{k} at 1.37: every font pixel {d} x {d} device pixels")
            ed.check(((box[2] - box[0]) % d, (box[3] - box[1]) % d) == (0, 0), f"x{k}: whole font pixels {box}")
    finally:
        await ed.call("set_scene_settings", scene=S["scene"], settings={"canvas_scaler": {"mode": "follow_game_canvas", "scale_factor": 1}})
        await label(ed, {"text": {"fontSize": 9}})


async def test_text_effect(ed: Editor):
    s = await setup(ed)
    await label(ed, {"text": {"fontSize": 18, "outlineWidth": 1, "outlineColor": [1, 0, 0, 1]}})
    await ed.call("add_component", scene=s["scene"], entity=s["label"], component="texteffect")
    await ed.call("save_scene", scene=s["scene"])
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("eval_lua", code="local fx = TextEffect(scene, scene:findEntity('Label')) "
                                       "fx:setLayer(1, {type = TextEffectType.Wave, amplitude = 0.3, duration = 1}) fx:play()")
        await ed.call("wait", frames=10)
        _, _, a = await shot(ed)
        await ed.call("wait", frames=20)
        _, _, b = await shot(ed)
        ed.check(a != b, "the effect moves the glyphs")
        for rows in (a, b):
            colours = {p for row in rows for p in row}
            ed.check(WHITE in colours and (255, 0, 0) in colours, "faces and outlines both draw while the effect plays")
            # both layers of a glyph move together: every outline pixel stays within its 1 font px
            # (2 screen px at x2) of a face pixel, however far the wave moved the glyph
            detached = outline_detached(rows, 2)
            ed.check(detached == 0, f"{detached} outline pixels left their faces behind")
    finally:
        await ed.call("play", action="stop")
        await ed.call("remove_component", scene=s["scene"], entity=s["label"], component="texteffect")
    # visibleCharacters hides both layers of the hidden glyphs
    try:
        _, _, full = await shot(ed)
        await label(ed, {"text": {"visibleCharacters": 2}})
        _, _, two = await shot(ed)
        whole, part = ink_box(full), ink_box(two)
        ed.check(part is not None and part[2] < whole[2], f"2 visible characters draw less: {whole} -> {part}")
        faces = ink_box(two, background=None, only=WHITE)
        outlines = ink_box(two, background=None, only=(255, 0, 0))
        ed.check(faces and outlines and outlines[2] <= faces[2] + 2, f"no outline past the visible faces: {faces} {outlines}")
        ed.check(outline_detached(two, 2) == 0, "the visible glyphs keep their outlines")
    finally:
        await label(ed, {"text": {"outlineWidth": 0, "fontSize": 9, "visibleCharacters": -1}})


async def test_preview_font(ed: Editor):
    """AC16 (R26, deviation 14: preview_font in pixel_art): the glyph grid, a string at a scale with
    the missing characters, a TTF smooth, the size kept within max_size; view_asset names it"""
    await setup(ed)
    (ed.project / "assets/fonts/hud.bitmapfont").write_text(json.dumps({
        "format": "jasperengine.bitmapfont", "version": 1, "source": {"type": "grid", "image": "grid@6x9.png"}}))
    grid = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont"})
    ed.check(not grid.is_error and grid.images, grid.text)
    ed.check_eq(grid.structured["font"]["kind"], "bitmap")
    ed.check_eq(grid.structured["font"]["glyphs"], 95)
    text = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont", "text": "ABé", "scale": 2})
    ed.check(not text.is_error and text.images, text.text)
    font = text.structured["font"]
    ed.check_eq(font["scale"], 2)
    ed.check_eq(font["missing"], ["é"])
    w, h, rows = pixels(text.images[0][0])
    ed.check((w, h) == (font["width"], font["height"]), f"the image is the reported size: {w}x{h}")
    ed.check(any(p == WHITE for row in rows for p in row), "white text")
    builtin = await ed.call("preview_font", path="builtin:pixel", text="Hello")
    ed.check_eq(builtin["font"]["native_size"], 9)
    vector = await ed.call_raw("preview_font", {"path": "assets/fonts/pixel8.ttf", "text": "Smooth"})
    ed.check(not vector.is_error and vector.images, vector.text)
    ed.check_eq(vector.structured["font"]["kind"], "vector")
    long = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont", "text": "x" * 300})
    ed.check(long.is_error and "256" in long.text, long.text)
    # an explicit scale never makes an image past max_size: the error names the largest that fits
    huge = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont", "text": "x" * 256, "scale": 16})
    ed.check(huge.is_error and "max_size" in huge.text and not huge.images, huge.text)
    fitting = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont", "text": "HP 99", "scale": 16, "max_size": 256})
    ed.check(fitting.is_error and "largest scale that fits is" in fitting.text, fitting.text)
    largest = int(fitting.text.split("largest scale that fits is ")[1].split(" ")[0])
    ok = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont", "text": "HP 99", "scale": largest, "max_size": 256})
    ed.check(not ok.is_error and max(ok.structured["font"]["width"], ok.structured["font"]["height"]) <= 256, ok.text)
    big_grid = await ed.call_raw("preview_font", {"path": "assets/fonts/hud.bitmapfont", "scale": 16})
    ed.check(not big_grid.is_error and "shown" in big_grid.text, big_grid.text)
    gw, gh, _ = pixels(big_grid.images[0][0])
    ed.check(max(gw, gh) <= 512, f"the grid at x16 keeps to max_size: {gw}x{gh}")
    # view_asset sends fonts to preview_font
    viewed = await ed.call_raw("view_asset", {"path": "assets/fonts/hud.bitmapfont"})
    ed.check(viewed.is_error and "preview_font" in viewed.text, viewed.text)


async def test_asset_info(ed: Editor):
    await setup(ed)
    info = await ed.call("get_asset_info", path="assets/fonts/bm.fnt")
    ed.check_eq(info["type"], "font")
    ed.check_eq(info["font"]["native_size"], 9)
    ed.check_eq(info["font"]["baseline"], 7)
    ed.check(["A", "V", -1] in info["font"]["kerning_pairs"], f"kerning pairs: {info['font']['kerning_pairs']}")
    ed.check_eq(info["pixel_report"]["antialiased"], False)
    (ed.project / "assets/fonts/sloppy.bitmapfont").write_text(json.dumps({
        "format": "jasperengine.bitmapfont", "source": {"type": "grid", "image": "partial@6x9.png"}, "glyphs": {"i": {"advance": 2}}}))
    sloppy = await ed.call("get_asset_info", path="assets/fonts/sloppy.bitmapfont")
    ed.check(sloppy["pixel_report"]["antialiased"] is True and sloppy["pixel_report"]["partial_alpha_share"] > 0, f"{sloppy['pixel_report']}")
    ed.check_eq(sloppy["overrides"], {"i": {"advance": 2}})
    ed.check_eq(sloppy["source"]["type"], "grid")
    vector = await ed.call("get_asset_info", path="assets/fonts/pixel8.ttf")
    ed.check_eq(vector["font"]["kind"], "vector")
    ed.check_eq(vector["font"]["pixel_grid_detected"], True)
    ed.check_eq(vector["fix"], "create a .bitmapfont with source truetype")
    (ed.project / "assets/fonts/broken.bitmapfont").write_text(json.dumps({"format": "jasperengine.bitmapfont", "source": {"type": "grid"}}))
    broken = await ed.call("get_asset_info", path="assets/fonts/broken.bitmapfont")
    ed.check(broken["errors"] and broken["errors"][0]["field"] == "source.image", f"typed error with its field: {broken.get('errors')}")


async def test_write_file(ed: Editor):
    await setup(ed)
    good = await ed.call("write_file", path="assets/fonts/written.bitmapfont", content=json.dumps({
        "format": "jasperengine.bitmapfont", "version": 1, "source": {"type": "grid", "image": "grid@6x9.png"}}))
    ed.check_eq(good["font"]["glyphs"], 95)
    ed.check_eq(good["font"]["native_size"], 9)
    bad = await ed.call_raw("write_file", {"path": "assets/fonts/overflow.bitmapfont", "content": json.dumps({
        "format": "jasperengine.bitmapfont", "source": {"type": "grid", "image": "grid@6x9.png", "cell": [6, 10]}})})
    ed.check(not bad.is_error, "an invalid font is still written")
    ed.check((ed.project / "assets/fonts/overflow.bitmapfont").exists(), "on disk")
    ed.check_eq(bad.structured["errors"][0]["kind"], "font_chars_overflow")
    ed.check("95 chars but 80 cells" in bad.text, bad.text)


LUA = """
local hud = BitmapFont.load("assets/fonts/bm.fnt")
local missing, err = BitmapFont.load("assets/fonts/nowhere.bitmapfont")
local label = Text(scene, scene:findEntity("Label"))
label.font = "builtin:pixel"
label.fontSize = 18
label.text = "SCORE 120"
local pixel = BitmapFont.load("builtin:pixel")
local w, h = pixel:measure("SCORE 120", 2)
label.outlineNeighbors = 4
return {hud.nativeSize, hud.lineHeight, hud.baseline, hud.glyphCount, hud:hasGlyph("A"), hud:hasGlyph("\\u{e9}"),
        hud:advance("A"), missing == nil, err ~= nil, w, h, label.outlineNeighbors}
"""


async def test_lua(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        values = (await ed.call("eval_lua", code=LUA))["values"][0]
        ed.check_eq(values[:4], [9, 10, 7, 95])
        ed.check(values[4] is True and values[5] is False, f"hasGlyph: {values}")
        ed.check_eq(values[6], 4)
        ed.check(values[7] is True and values[8] is True, f"a bad path is nil, err: {values}")
        ed.check_eq(values[11], 4)
        await ed.call("wait", frames=3)
        drawn = (await ed.call("eval_lua", code="local t = Text(scene, scene:findEntity('Label')) "
                                                "return {t.pixelScale, t.effectiveFontSize, t.width, t.height}"))["values"][0]
        ed.check_eq(drawn[:2], [2, 18])
        ed.check_eq(drawn[2:], values[9:11], "measure is the drawn Text's size")
    finally:
        await ed.call("play", action="stop")


async def test_hot_reload(ed: Editor):
    await setup(ed)
    path = ed.project / "assets/fonts/live.bitmapfont"
    path.write_text(json.dumps({"format": "jasperengine.bitmapfont", "source": {"type": "grid", "image": "grid@6x9.png"}}))
    await label(ed, {"text": {"font": "assets/fonts/live.bitmapfont", "fontSize": 18, "text": "AAAA"}})
    _, _, before = await shot(ed)
    box = ink_box(before)
    # a wider spacing on disk: the Text is wider within a frame of the watcher's event
    path.write_text(json.dumps({"format": "jasperengine.bitmapfont", "source": {"type": "grid", "image": "grid@6x9.png"}, "spacing": 3}))
    await ed.call("sync_files", full=False)
    await ed.call("wait", frames=3)
    _, _, after = await shot(ed)
    wider = ink_box(after)
    ed.check(wider[2] - wider[0] == box[2] - box[0] + 3 * 2 * 2, f"spacing 3 reached the Text: {box} -> {wider}")
    # the sheet itself: an empty sheet draws nothing
    sheet = ed.project / "assets/fonts/grid@6x9.png"
    original = sheet.read_bytes()
    try:
        sheet.write_bytes(pngdiff.encode(96, 54, bytes(96 * 54 * 4)))
        await ed.call("sync_files", full=False)
        await ed.call("wait", frames=3)
        _, _, blank = await shot(ed)
        ed.check(ink_box(blank) is None, "the edited sheet reached the Text")
    finally:
        sheet.write_bytes(original)
        await ed.call("sync_files", full=False)
    # a .fnt used directly (AC23): 'A' advancing 2 font pixels more reaches the Text
    fnt = ed.project / "assets/fonts/bm.fnt"
    fnt_original = fnt.read_text()
    try:
        await label(ed, {"text": {"font": "assets/fonts/bm.fnt", "fontSize": 18, "text": "AAAA"}})
        _, _, before = await shot(ed)
        box = ink_box(before)
        fnt.write_text(fnt_original.replace("char id=65 x=7 y=19 width=2 height=6 xoffset=1 yoffset=1 xadvance=4",
                                            "char id=65 x=7 y=19 width=2 height=6 xoffset=1 yoffset=1 xadvance=6"))
        await ed.call("sync_files", full=False)
        await ed.call("wait", frames=3)
        _, _, after = await shot(ed)
        wider = ink_box(after)
        ed.check(wider[2] - wider[0] == box[2] - box[0] + 3 * 2 * 2, f"the edited .fnt reached the Text: {box} -> {wider}")
    finally:
        fnt.write_text(fnt_original)
        await ed.call("sync_files", full=False)
        await label(ed, {"text": {"font": "builtin:pixel", "fontSize": 9, "text": "Hp 99!"}})


async def test_vector_text_unchanged(ed: Editor):
    """AC2: vector text draws as the pre-S51 editor drew it (the references), and the fixture with Text
    re-saves byte for byte (no bitmap font here: the pre-S51 editor runs it to capture the references)"""
    capture = os.environ.get("JASPER_PIXEL_FONTS_CAPTURE") == "1"
    await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
    made = await ed.call("create_scene", name="Vector", type="UI", path="scenes/Vector")
    scene = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0, 0, 0, 1]})
    await ed.call("create_ui", scene=scene, root={
        "type": "text", "name": "Plain", "text": "Vector text 0123", "font_size": 20, "text_color": [1, 1, 1, 1],
        "layout": {"anchor": "top_left", "position": [10, 10]}})
    await ed.call("create_ui", scene=scene, root={
        "type": "text", "name": "Styled", "text": "Outline & shadow", "font_size": 24, "text_color": [1, 1, 0, 1],
        "outline": {"width": 2, "color": [1, 0, 0, 1]}, "shadow": {"offset": [2, 2], "color": [0, 0, 1, 1], "softness": 2},
        "layout": {"anchor": "top_left", "position": [10, 60]}})
    # AllComponents' Text (tools/fixtures/reflection-parity, the only fixture Text with a TTF; its
    # fonts/Parity.ttf is not in the fixture, so it draws in the default font, as here)
    await ed.call("create_ui", scene=scene, root={
        "type": "text", "name": "Parity", "text": "text: [1, 2]", "font_size": 20, "text_color": [1, 1, 1, 1],
        "layout": {"anchor": "top_left", "position": [10, 110]}})
    await ed.call("set_properties", scene=scene, entity="Parity", values={"text": {
        "maxTextSize": 100, "fixedWidth": True, "fixedHeight": True, "pivotBaseline": True, "pivotCentered": True}})
    await ed.call("configure_game_view", resolution="320x180")
    image = await ed.screenshot(view="game", max_width=0)
    reference = GOLDEN / "vector_text.png"
    if capture:
        GOLDEN.mkdir(parents=True, exist_ok=True)
        reference.write_bytes(image)
    else:
        ed.check(reference.exists(), f"the reference {reference} (captured with the pre-S51 editor)")
        _, _, got = pixels(image)
        _, _, want = pixels(reference.read_bytes())
        differing = sum(1 for a, b in zip(got, want) for p, q in zip(a, b) if p != q)
        ed.check(len(got) == len(want) and differing == 0, f"vector text pixel-identical to the reference: {differing} differ")
    # tools/fixtures/reflection-parity/AllComponents.scene (its Text a TTF): re-saved byte for byte
    src = REPO / "tools/fixtures/reflection-parity"
    copy = ed.project / "parity"
    if copy.exists():
        shutil.rmtree(copy)
    shutil.copytree(src, copy)
    opened = await ed.call("open_scene", scene="parity/AllComponents.scene")
    parity = opened["scene"]["id"]
    await ed.call("save_scene", scene=parity, path="parity/AllComponents.resaved.scene")
    after = (copy / "AllComponents.resaved.scene").read_bytes()
    if capture:
        (GOLDEN / "AllComponents.resaved.scene").write_bytes(after)
        return
    SHOTS.mkdir(parents=True, exist_ok=True)
    (SHOTS / "AllComponents.resaved.scene").write_bytes(after)
    # (a save to a new path is a new scene file: its own guid line)
    def body(data: bytes) -> list[bytes]:
        return [line for line in data.splitlines() if not line.startswith(b"guid: ")]
    ed.check(body(after) == body((GOLDEN / "AllComponents.resaved.scene").read_bytes()),
             "AllComponents re-saves as the pre-S51 editor re-saved it")
