"""S42 AC4 (docs/specs/S42-pixel-art-mode.md R3): Project Settings > Graphics > Default Filter Mode is a
runtime setting. With graphics.default_filter_mode point, a Sprite a Lua script makes in Play and
gives a texture (Sprite:setTexture) samples it NEAREST; in the default (bilinear) project the same
script reads LINEAR (the red control). The generated main of a compiled game sets it
(Engine::setDefaultTextureFilter), the prebuilt player's config carries it, and Lua reads it. Saved
references keep their own filters: after Play and Stop with point, a scene saves as it did before.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "Pixel Art Runtime"}

SPAWN = """
local s = Sprite(scene)
s:setTexture("sprites/hero.png")
local t = s:getMaterial(0).baseColorTexture
return t.minFilter == TextureFilter.NEAREST, t.magFilter == TextureFilter.NEAREST,
       t.minFilter == TextureFilter.LINEAR, Engine.getDefaultTextureFilter() == TextureFilter.NEAREST
"""


async def spawn_filters(ed: Editor, scene: int) -> list:
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=10)
        return (await ed.call("eval_lua", code=SPAWN))["values"]
    finally:
        await ed.call("play", action="stop")


async def test_default_filter_mode_reaches_runtime_textures(ed: Editor):
    await ed.call("get_project_summary")
    await ed.call("generate_placeholder_art", path="sprites/hero.png", width=16, height=16, color="#ff0000")
    made = await ed.call("create_scene", name="Runtime", type="2D", path="scenes/Runtime")
    scene = made["scene"]["id"]
    await ed.call("save_scene", scene=scene)

    linear = await spawn_filters(ed, scene)
    ed.check(linear[2] is True and linear[0] is False, f"the default project: a runtime texture is LINEAR (red control): {linear}")

    await ed.call("set_project_settings", values={"graphics.default_filter_mode": "point"})
    got = await ed.call("get_project_settings", keys=["graphics.default_filter_mode"])
    ed.check("point" in str(got), f"the setting reads back: {got}")
    nearest = await spawn_filters(ed, scene)
    ed.check(nearest[0] is True and nearest[1] is True and nearest[3] is True,
             f"Default Filter Mode point: Sprite:setTexture in Play is NEAREST/NEAREST: {nearest}")

    # edit mode keeps bilinear for what the editor makes; Play's value is restored at stop
    ed.check("defaultFilterMode: point" in (ed.project / "project.yaml").read_text(), "written to project.yaml (graphics)")
    # the compiled game's generated main carries it
    for _ in range(60):
        generated = [p for p in ed.project.rglob("*.cpp") if "setDefaultTextureFilter" in p.read_text(errors="replace")]
        if generated:
            break
        await ed.call("wait", frames=10)
    ed.check(generated, "the generated main contains Engine::setDefaultTextureFilter(TextureFilter::NEAREST)")

    await ed.call("set_project_settings", values={"graphics.default_filter_mode": "bilinear"})
    ed.check("defaultFilterMode" not in (ed.project / "project.yaml").read_text(), "bilinear (the default) is not written")


async def test_stop_leaves_edit_data(ed: Editor):
    """a Text's font atlas sampler is saved without its default keys: Stop under point must not read it NEAREST"""
    await ed.call("set_project_settings", values={"graphics.default_filter_mode": "point"})
    made = await ed.call("create_scene", name="Stop", type="2D", path="scenes/Stop")
    scene = made["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="text", name="Label", values={"text": {"text": "HELLO"}})
    await ed.call("save_scene", scene=scene)
    path = next(p for p in ed.project.rglob("Stop.scene"))
    saved = path.read_text()
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=10)
    finally:
        await ed.call("play", action="stop")
    await ed.call("save_scene", scene=scene)
    after = path.read_text()
    # Play may add the shader keys it compiled; the samplers are what Stop must not touch
    def samplers(text: str) -> list[str]:
        return [line.strip() for line in text.splitlines() if "Filter" in line or "wrap" in line]
    ed.check("nearest" not in after, f"no sampler turned NEAREST by Stop: {samplers(after)}")
    ed.check_eq(samplers(after), samplers(saved), "the samplers save as before Play")
    await ed.call("set_project_settings", values={"graphics.default_filter_mode": "bilinear"})
