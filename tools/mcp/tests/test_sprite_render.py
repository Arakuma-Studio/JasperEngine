"""render_sprite (docs/specs/S47-agent-sprite-drawing.md AC12, R27, R28): the sheet's integer
scale, nearest pixels, backdrops as rows, onion skin, the last-edit outline and the scene context."""

from __future__ import annotations

import base64
import time

from harness import Editor
from sprite_draw_util import decode_png, enable, images, ok

CI = True
EDITOR = {"create": True, "name": "SpriteRender", "xvfb": "harness"}
TIMEOUT = 600
PATH = "assets/sprites/hero.aseprite"
S: dict = {}


def px(image, x, y):
    w, _, data = image
    i = (y * w + x) * 4
    return tuple(data[i:i + 4])


async def render(ed: Editor, **args):
    data, imgs = await images(ed, "render_sprite", sprite=PATH, **args)
    return data, decode_png(imgs[0])


async def test_setup(ed: Editor):
    await enable(ed)
    await ok(ed, "create_sprite", path=PATH, size=[8, 8], frames=2, palette=["#ff0000", "#00ff00", "#0000ff"])
    r = await ok(ed, "edit_sprite", sprite=PATH, ops=[
        {"op": "pixel", "points": [[1, 1]], "color": 1, "frame": 1},
        {"op": "pixel", "points": [[6, 6]], "color": 2, "frame": 2},
    ])
    S["rects"] = r["changed"]["rects"]


async def test_scale_is_the_largest_that_fits(ed: Editor):
    data, image = await render(ed, max_width=200, labels=False)
    # 2 columns of 8 with 3 gaps of 2 px: (200 - 6) / 16 = 12
    ed.check(data["scale"] == 12, f"the largest integer scale: {data['scale']}")
    w, h, _ = image
    ed.check(w == 2 + 2 * (8 * 12 + 2) and w <= 200, f"the sheet fits: {w}x{h}")


async def test_every_pixel_is_a_block_of_one_colour(ed: Editor):
    data, image = await render(ed, max_width=512, labels=False)
    scale = data["scale"]
    panel = data["layout"][0]
    for y in range(8):
        for x in range(8):
            colours = {px(image, panel["x"] + x * scale + i, panel["y"] + y * scale + j) for i in range(scale) for j in range(scale)}
            ed.check(len(colours) == 1, f"pixel ({x},{y}) is one colour: {colours}")
    ed.check(px(image, panel["x"] + scale + 1, panel["y"] + scale + 1) == (255, 0, 0, 255), "frame 1's red pixel")


async def test_three_backdrops_give_three_rows(ed: Editor):
    data, image = await render(ed, max_width=512, labels=False, backdrops=["checker", "dark", "#00ffff"], frames=[1])
    ys = sorted({p["y"] for p in data["layout"]})
    ed.check(len(ys) == 3, f"three rows: {data['layout']}")
    third = next(p for p in data["layout"] if p["backdrop"] == "#00ffff")
    ed.check(px(image, third["x"], third["y"]) == (0, 255, 255, 255), f"the colour backdrop shows: {px(image, third['x'], third['y'])}")


async def test_onion_skin_has_the_given_opacity(ed: Editor):
    data, image = await render(ed, max_width=512, labels=False, frames=[2], onion={"before": 1, "opacity": 0.5}, backdrops=["#000000"])
    scale = data["scale"]
    panel = data["layout"][0]
    r, g, b, a = px(image, panel["x"] + scale + 1, panel["y"] + scale + 1)
    ed.check(a == 255 and g == 0 and b == 0 and abs(r - 127) <= 2, f"frame 1's red at 50%: {(r, g, b, a)}")


async def test_last_edit_outline_encloses_the_rects(ed: Editor):
    data, image = await render(ed, max_width=512, labels=False, frames=[1], highlight="last_edit")
    scale = data["scale"]
    panel = data["layout"][0]
    rect = next(r for r in S["rects"] if r["frame"] == 1)["rect"]
    left = panel["x"] + rect[0] * scale - 1
    top = panel["y"] + rect[1] * scale - 1
    right = panel["x"] + (rect[0] + rect[2]) * scale
    bottom = panel["y"] + (rect[1] + rect[3]) * scale
    magenta = (255, 0, 255, 255)
    for x in range(left, right + 1):
        ed.check(px(image, x, top) == magenta and px(image, x, bottom) == magenta, f"top/bottom of the frame at x={x}")
    for y in range(top, bottom + 1):
        ed.check(px(image, left, y) == magenta and px(image, right, y) == magenta, f"sides of the frame at y={y}")
    ed.check(px(image, left + 1, top + 1) != magenta, "the rect itself is not painted")
    ed.check(data["last_edit"]["revision"], f"last_edit: {data['last_edit']}")


async def test_grid_lines_and_labels(ed: Editor):
    data, image = await render(ed, max_width=256, labels=True, frames=[1])
    ed.check(data["scale"] >= 1, "renders with labels")
    plain, im2 = await render(ed, max_width=512, labels=False, grid=True, frames=[1])
    ed.check(plain["scale"] >= 6, f"grid needs scale 6: {plain['scale']}")


async def test_context_returns_the_view_around_the_game_object(ed: Editor):
    scenes = await ed.call("list_scenes")
    main = scenes["selected_scene_id"]
    created = await ed.call("set_sprite_frames", scene=main, create={"name": "Hero"}, sheet=PATH, frame=0)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        info = await ed.call("get_asset_info", path=PATH)
        if info.get("import_state") == "imported":
            break
        await ed.call("wait", seconds=0.2)
    await ed.call("frame_entity", entity=created["sprite"])
    await ed.call("wait", frames=5)
    data, imgs = await images(ed, "render_sprite", sprite=PATH, frames=[1], context={"entity": created["sprite"], "view": "scene"})
    ed.check(len(imgs) == 2, f"a second image: {len(imgs)}")
    w, h, pixels = decode_png(imgs[1])
    ed.check(w > 0 and h > 0, f"context image {w}x{h}")
    ed.check(data["context"]["view"] == "scene" and data["context"]["scale"] >= 1, f"{data.get('context')}")
    red = sum(1 for i in range(0, len(pixels), 4) if pixels[i] > 180 and pixels[i + 1] < 90 and pixels[i + 2] < 90)
    ed.check(red > 0, f"the sprite's red pixel shows in the view ({w}x{h}, context {data.get('context')})")
