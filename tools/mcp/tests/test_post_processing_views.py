"""Post-processing in the editor's views (shader library phase 4): the Game view draws the scene's
post-processing, the Scene view does not (unless its display setting turns it on). Both the
in-shader colour (monochrome: a T0 effect) and the uber pass (CRT: a T1 effect) are checked."""

from __future__ import annotations

from harness import Editor
from htmlui_pixels import decode_png, image_difference

EDITOR = {"create": True, "name": "Post Processing Views"}
TIMEOUT = 600


async def shot(ed: Editor, view: str) -> bytes:
    await ed.call("wait", frames=20)
    r = await ed.call_raw("screenshot", {"view": view}, timeout=300)
    ed.check(not r.is_error and r.images, f"no {view} screenshot: {r.text[:300]}")
    return r.images[0][0]


def saturation(png: bytes) -> float:
    """Mean of max(rgb) - min(rgb), sampled every 4 px."""
    w, h, c, rows = decode_png(png)
    total = 0
    count = 0
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            px = rows[y][x * c:x * c + 3]
            total += max(px) - min(px)
            count += 1
    return total / max(count, 1)


async def test_post_processing_in_views(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_scene_settings", "screenshot", "wait")
    await ed.call("create_scene", name="PostViews", type="3d")
    await ed.call("create_entity", name="Ball", type="sphere")

    game_before = await shot(ed, "game")
    scene_before = await shot(ed, "scene")
    ed.check(saturation(game_before) > 8.0, f"the default scene has colour (saturation {saturation(game_before):.1f})")

    # T0: grayscale in the shaders
    await ed.call("set_scene_settings", settings={"post_processing.monochrome_enabled": True})
    game_mono = await shot(ed, "game")
    scene_mono = await shot(ed, "scene")
    ed.check(saturation(game_mono) < 1.5, f"the Game view is grey (saturation {saturation(game_mono):.1f})")
    scene_change = image_difference(scene_before, scene_mono)
    ed.check(scene_change < 0.5, f"the Scene view keeps the colours (difference {scene_change:.2f}, saturation "
             f"{saturation(scene_before):.1f} -> {saturation(scene_mono):.1f})")

    # T1: the uber pass (CRT) on top
    await ed.call("set_scene_settings", settings={"post_processing.crt_enabled": True, "post_processing.crt_curvature": 1.0})
    game_crt = await shot(ed, "game")
    scene_crt = await shot(ed, "scene")
    ed.check(image_difference(game_mono, game_crt) > 2.0, "the Game view shows the CRT")
    ed.check(image_difference(scene_before, scene_crt) < 0.5, "the Scene view shows no uber pass")

    # off again: the Game view as before
    await ed.call("set_scene_settings", settings={"post_processing": None})
    game_after = await shot(ed, "game")
    ed.check(image_difference(game_before, game_after) < 0.5, "post-processing off draws what was drawn before")
