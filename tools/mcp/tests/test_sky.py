"""S94 F16 (docs/specs/S94-rendering-anim-gaps.md AC16): in Play, Lua's Skybox.visible = false and
then an inactive sky entity each leave the Game view showing the scene's clear colour; visible and
active again, the sky is back. setVisible is the method form."""

from __future__ import annotations

import harness
from harness import Editor
from htmlui_pixels import decode_png

CI = True
EDITOR = {"create": True, "name": "Sky"}
TIMEOUT = 600

CLEAR = (0, 0, 255)


def center(png: bytes) -> tuple[int, int, int]:
    width, height, channels, rows = decode_png(png)
    row = rows[height // 2]
    x = (width // 2) * channels
    return row[x], row[x + 1], row[x + 2]


def near(pixel, color, tolerance=6) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(pixel, color))


async def test_skybox_visible_and_inactive(ed: Editor):
    ed.require_tools("create_scene", "find_entities", "set_properties", "set_scene_settings", "play", "eval_lua", "screenshot", "wait")
    scene = (await ed.call("create_scene", name="SkyVisible", type="3D"))["scene"]["id"]
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0, 0, 1, 1]})
    skies = harness.find_entities(await ed.call("find_entities", scene=scene, component="skybox"))
    if not skies:
        made = await ed.call("create_entity", scene=scene, type="skybox", name="Sky", select=False)
        sky = made["entity_id"]
    else:
        sky = harness.entity_id_of(skies[0])

    await ed.call("play", action="start")
    try:
        await ed.call("wait", frames=10)
        shown = center(await ed.screenshot("game"))
        ed.check(not near(shown, CLEAR, 30), f"the sky draws over the clear colour: {shown}")

        out = await ed.call("eval_lua", scene=scene, code=f"local s = object({sky}, 'Skybox') s.visible = false return s.visible")
        ed.check_eq(out["values"][0], False, "Skybox.visible reads back")
        await ed.call("wait", frames=4)
        shown = center(await ed.screenshot("game"))
        ed.check(near(shown, CLEAR), f"a hidden sky shows the clear colour: {shown}")

        await ed.call("eval_lua", scene=scene, code=f"local s = object({sky}, 'Skybox') s:setVisible(true) s:setActive(false)")
        await ed.call("wait", frames=4)
        shown = center(await ed.screenshot("game"))
        ed.check(near(shown, CLEAR), f"an inactive sky shows the clear colour: {shown}")

        await ed.call("eval_lua", scene=scene, code=f"object({sky}, 'Skybox'):setActive(true)")
        await ed.call("wait", frames=4)
        shown = center(await ed.screenshot("game"))
        ed.check(not near(shown, CLEAR, 30), f"active and visible, the sky is back: {shown}")
    finally:
        await ed.call("play", action="stop")
    errors = [m for m in (await ed.console_since("last", limit=200)) if m.get("level") == "error"]
    ed.check(not errors, f"no errors: {errors[:3]}")
