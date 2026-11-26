"""S94 P3 (docs/specs/S94-rendering-anim-gaps.md AC16, AC19, AC20): in Play, Lua's Skybox.visible =
false and then an inactive sky entity each leave the Game view showing the scene's clear colour;
visible and active again, the sky is back (setVisible is the method form). The vignette's Radii
falloff and the LUT's Flip Green through set_scene_settings: one undo step each, and saved."""

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


async def test_vignette_radii_and_lut_flip(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene", "undo")
    sid = (await ed.call("create_scene", name="VignetteRadii", type="3D"))["scene"]["id"]
    detail = await ed.call("get_scene_settings", scene=sid, names=["post_processing.vignette_falloff"], detail=True)
    ed.check_eq(detail["schema"]["post_processing.vignette_falloff"]["options"], ["smoothness", "radii"])

    settings = {"post_processing.vignette_enabled": True, "post_processing.vignette_falloff": "radii",
                "post_processing.vignette_start": 0.4, "post_processing.vignette_end": 1.1,
                "post_processing.lut_flip_green": True}
    for name, value in settings.items():
        out = await ed.call("set_scene_settings", scene=sid, settings={name: value})
        ed.check_eq(out["settings"][name], value, name)
    await ed.call("save_scene", scene=sid, path="scenes/VignetteRadii.scene")
    text = (ed.project / "scenes/VignetteRadii.scene").read_text()
    for key in ("vignetteFalloff", "vignetteStart", "vignetteEnd", "lutFlipGreen"):
        ed.check(key in text, f"{key} saved")
    await ed.call("undo")
    got = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check("lut_flip_green" not in got, f"undo: the last field only ({sorted(got)})")
    ed.check_eq(got.get("vignette_falloff"), "radii")
