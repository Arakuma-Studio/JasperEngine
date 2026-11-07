"""visualize_terrain: the Scene view's terrain overlays and framing (T1 R65, R71, R90; AC50, AC57)."""

import pngdiff
from harness import Editor

EDITOR = {"create": True, "name": "Terrain Visualize"}
CI = True


async def hilly(ed: Editor):
    sid = (await ed.call("create_scene", name="Hills", type="3d"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=sid, name="Hills", type="terrain"))["entity_id"]
    await ed.call("edit_terrain", scene=sid, entity=terrain, create="test_patch", map_resolution=65, ops=[
        {"op": "set_height", "height": 2},
        {"op": "set_height", "height": 12, "shape": {"type": "circle", "center": [0, 0], "radius": 18, "falloff": "linear"}},
        {"op": "paint_layer", "layer": 1, "shape": {"type": "rect", "min": [-32, -32], "max": [0, 32]}}])
    return sid, terrain


def image_of(result) -> bytes:
    assert result.images, f"no image in the result: {result.text}"
    return result.images[0][0]


async def test_overlays_change_pixels(ed: Editor):
    sid, terrain = await hilly(ed)
    plain = await ed.call_raw("visualize_terrain", {"scene": sid, "entity": terrain, "overlay": "none", "framing": "top", "max_width": 256})
    ed.check(not plain.is_error, plain.text)
    _, _, base = pngdiff.decode(image_of(plain))
    for overlay in ("height", "slope", "contours", "layer_weights"):
        r = await ed.call_raw("visualize_terrain", {"scene": sid, "entity": terrain, "overlay": overlay, "framing": "keep", "max_width": 256})
        ed.check(not r.is_error, r.text)
        _, _, pixels = pngdiff.decode(image_of(r))
        changed = sum(1 for a, b in zip(pixels[0::4], base[0::4]) if abs(a - b) > 8)
        ed.check(changed > len(base) // 4 // 20, f"{overlay} changed {changed} pixels of the view")
        ed.check(len(r.value["legend"]) > 0, f"{overlay} has a legend: {r.value}")
    await ed.call("visualize_terrain", scene=sid, entity=terrain, overlay="none", screenshot=False)
    message = await ed.check_error("visualize_terrain", scene=sid, entity=terrain, overlay="steepness")
    ed.check("slope" in message, f"the overlays are listed: {message}")


async def test_framing_modes(ed: Editor):
    sid, terrain = await hilly(ed)
    top = await ed.call("visualize_terrain", scene=sid, entity=terrain, overlay="height", framing="top", screenshot=False)
    position, target = top["camera"]["position"], top["camera"]["target"]
    ed.check(abs(position[0] - target[0]) < 0.01 and abs(position[2] - target[2]) < 0.01 and position[1] > target[1] + 30,
             f"top looks straight down from above: {top['camera']}")
    orbit = await ed.call("visualize_terrain", scene=sid, entity=terrain, framing="orbit", yaw=0, pitch=30, screenshot=False)
    p, t = orbit["camera"]["position"], orbit["camera"]["target"]
    ed.check(abs(p[0] - t[0]) < 0.01 and p[2] > t[2] and p[1] > t[1], f"yaw 0 looks from +Z, pitch 30 from above: {orbit['camera']}")
    fit = await ed.call("visualize_terrain", scene=sid, entity=terrain, framing="fit", screenshot=False)
    ed.check(fit["camera"]["target"] == t, f"fit keeps the direction and the centre: {fit['camera']}")
    kept = await ed.call("visualize_terrain", scene=sid, entity=terrain, framing="keep", screenshot=False)
    ed.check_eq(kept["camera"]["position"], fit["camera"]["position"], "keep leaves the camera")
