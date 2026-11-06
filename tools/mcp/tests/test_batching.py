# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Dynamic Batching through MCP (docs/superpowers/specs/2026-11-04-sprite-batching-design.md, T25-T27).

A 2D scene of 30 sprites in three hierarchy blocks: 10 red, 10 blue over them, 10 red over the
blue (overlapping, so the last block cannot move before the blue one):
- get_render_stats returns `batching` (3 batches, 27 draws saved, 2 texture breaks) and, with
  batch_breaks, the first breaks with the entities involved;
- get_gpu_passes include_draws marks the batch draws with batch_items;
- graphics.dynamic_batching (get/set_project_settings) turns it off: 30 draws alone, the same
  image; Engine.dynamicBatching (eval_lua) reads the same switch.
"""

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Batching CI"}

TIMEOUT = 600

S: dict = {}

RED = "assets/batching/red.png"
BLUE = "assets/batching/blue.png"


async def stats(ed: Editor, **args) -> dict:
    # the game's frames while it plays (the Game view of Edit mode redraws only when it changes)
    await ed.call("wait", frames=10)
    return await ed.call("get_render_stats", view="game", **args)


async def play(ed: Editor):
    await ed.call("play", action="start", scene=S["scene"])


async def stop(ed: Editor):
    await ed.call("play", action="stop")


async def test_sprites_batch_and_report_breaks(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "generate_placeholder_art", "get_render_stats")
    await ed.call("generate_placeholder_art", path=RED, width=16, height=16, shape="rect", color="#ff2020")
    await ed.call("generate_placeholder_art", path=BLUE, width=16, height=16, shape="rect", color="#2040ff")
    made = await ed.call("create_scene", name="Batching", type="2D")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    # a 2D scene's world is its canvas in pixels, y up from the bottom left: the rows across its middle
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 320})
    canvas = shot.value.get("canvas") if isinstance(shot.value, dict) else None
    ed.check(canvas and canvas.get("width") and canvas.get("height"), f"the screenshot names the canvas: {shot.text[:300]}")
    step = min(70.0, (canvas["width"] - 100) / 10.0)
    left = canvas["width"] / 2 - 4.5 * step - 12
    for block, texture, offset in ((0, RED, 0), (1, BLUE, 12), (2, RED, 24)):
        for i in range(10):
            await ed.call("create_entity", scene=S["scene"], type="sprite", name=f"{'Red' if texture == RED else 'Blue'} {block}.{i}",
                          select=False, position=[left + i * step + offset, canvas["height"] / 2 + offset - 20, 0],
                          values={"sprite": {"width": 40, "height": 40}, "mesh": {"submeshes[0].material.baseColorTexture": texture}})

    await play(ed)
    try:
        await batched_frame(ed)
    finally:
        await stop(ed)


async def batched_frame(ed: Editor):
    got = await stats(ed, batch_breaks=10)
    batching = got.get("batching", {})
    ed.check(batching.get("enabled") is True, f"batching on by default: {batching}")
    ed.check_eq(batching.get("batches"), 3, f"three runs: {batching}")
    ed.check_eq(batching.get("saved_by_batching"), 27, f"27 draws saved: {batching}")
    ed.check(batching.get("breaks", {}).get("texture", 0) >= 2, f"texture breaks: {batching}")
    breaks = batching.get("batch_breaks", [])
    ed.check(breaks and breaks[0]["reason"] == "texture" and breaks[0]["entity_name"].startswith("Blue")
             and breaks[0].get("previous_name", "").startswith("Red"), f"the first break: {breaks}")
    S["on"] = pngdiff.decode(await ed.screenshot(view="game", max_width=320))

    passes = await ed.call("get_gpu_passes", view="game", include_draws=True, max_draws=500)
    items = [draw.get("batch_items", 0) for draw in passes.get("draws", [])]
    ed.check(10 in items, f"a draw of 10 objects: {items}")


async def test_toggle_is_visual_neutral(ed: Editor):
    described = await ed.call("get_project_settings", keys=["graphics.dynamic_batching"], describe=True)
    ed.check("graphics.dynamic_batching" in str(described), f"the setting is listed: {described}")
    await ed.call("set_project_settings", values={"graphics.dynamic_batching": False})
    await play(ed)
    try:
        got = await stats(ed)
        batching = got.get("batching", {})
        ed.check(batching.get("enabled") is False and batching.get("batches", 0) == 0, f"off: {batching}")
        off = pngdiff.decode(await ed.screenshot(view="game", max_width=320))
    finally:
        await stop(ed)
    same, fraction, _ = pngdiff.compare(S["on"], off, channel_tolerance=1, max_fraction=0.0)
    ed.check(same, f"the same image batched and alone: {fraction:.4%} of the pixels differ")
    await ed.call("set_project_settings", values={"graphics.dynamic_batching": True})
    await play(ed)
    try:
        got = await stats(ed)
        ed.check_eq(got.get("batching", {}).get("batches"), 3, "on again")
    finally:
        await stop(ed)


async def test_lua_switch(ed: Editor):
    value = await ed.call("eval_lua", code="return Engine.dynamicBatching", allow_edit_mode=True)
    ed.check(value["values"] == [True], f"Engine.dynamicBatching reads the setting: {value}")
