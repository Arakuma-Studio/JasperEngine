"""S42 AC5 (docs/specs/S42-pixel-art-mode.md R4, R7): a texture's import Filter Mode and Pixels Per Unit
are what an assignment copies. create_entity of a sprite with a pixelArt texture gives its reference
nearest/nearest and the import's Pixels Per Unit (32 in a PPU-16 project), and one undo removes the
sprite; a texture whose Filter Mode is bilinear gives LINEAR; a value that names filters keeps them;
get_texture_import_settings reports the resolved Pixel Art fields, and set_texture_import_settings
takes pixel_art, filter_mode, pixels_per_unit and alpha_clip (snake_case, free-form).
"""

from __future__ import annotations

from harness import Editor

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "Pixel Art Import"}
M = "submeshes[0].material."


async def texture_of(ed: Editor, scene: int, entity: int) -> dict:
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["mesh", "sprite"])
    value = got["components"]["mesh"].get(M + "baseColorTexture") or got["components"]["mesh"]["submeshes"][0]["material"]["baseColorTexture"]
    return {"texture": value, "sprite": got["components"]["sprite"]}


async def test_assignment_copies_import_defaults(ed: Editor):
    await ed.call("tool_groups", enable=["assets_pipeline"])
    await ed.call("set_project_settings", values={"graphics.pixels_per_unit": 16})
    for name in ("hero", "soft"):
        await ed.call("generate_placeholder_art", path=f"sprites/{name}.png", width=16, height=16, color="#ff8800")
    await ed.call("set_texture_import_settings", paths=["sprites/hero.png"], settings={"pixel_art": True, "pixels_per_unit": 32})
    await ed.call("set_texture_import_settings", paths=["sprites/soft.png"], settings={"filter_mode": "bilinear"})
    info = await ed.call("get_texture_import_settings", path="sprites/hero.png")
    ed.check_eq(info["pixel_art"], {"enabled": True, "filter_mode": "point", "pixels_per_unit": 32}, "the resolved Pixel Art fields")
    ed.check(info["resolved"]["desktop"]["quality"] == "none" and info["resolved"]["desktop"]["mipmaps"] is False,
             f"Pixel Art resolves lossless and unmipmapped: {info['resolved']['desktop']}")
    ed.check(info["resolved"]["desktop"]["alpha_clip"] is True, "and Alpha Clip")

    made = await ed.call("create_scene", name="Import", type="2D", path="scenes/Import")
    scene = made["scene"]["id"]
    before = await ed.call("get_undo_history", scene=scene, limit=20)
    hero = (await ed.call("create_entity", scene=scene, type="sprite", name="Hero",
                          values={"mesh": {M + "baseColorTexture": "sprites/hero.png"}}))["entity_id"]
    got = await texture_of(ed, scene, hero)
    texture = got["texture"]
    ed.check(isinstance(texture, dict) and texture.get("min_filter") == "nearest" and texture.get("mag_filter") == "nearest",
             f"the reference starts nearest/nearest: {texture}")
    ed.check_eq(got["sprite"].get("pixelsPerUnit"), 32, "the import's Pixels Per Unit (32) in a PPU-16 project")
    after = await ed.call("get_undo_history", scene=scene, limit=20)
    ed.check_eq(len(after["entries"]) - len(before["entries"]), 1, "one undo step")
    await ed.call("undo", scene=scene)
    tree = await ed.call("get_scene_tree", scene=scene, format="json")
    ed.check("Hero" not in str(tree), "one undo removes the sprite")

    soft = (await ed.call("create_entity", scene=scene, type="sprite", name="Soft",
                          values={"mesh": {M + "baseColorTexture": "sprites/soft.png"}}))["entity_id"]
    texture = (await texture_of(ed, scene, soft))["texture"]
    ed.check(texture == "sprites/soft.png" or (isinstance(texture, dict) and texture.get("min_filter") == "linear"),
             f"a bilinear import gives LINEAR: {texture}")
    ed.check_eq((await texture_of(ed, scene, soft))["sprite"].get("pixelsPerUnit", 0), 0, "no PPU of its own: the project's")

    named = (await ed.call("create_entity", scene=scene, type="sprite", name="Named",
                           values={"mesh": {M + "baseColorTexture": {"path": "sprites/hero.png", "min_filter": "linear",
                                                                    "mag_filter": "linear"}}}))["entity_id"]
    texture = (await texture_of(ed, scene, named))["texture"]
    ed.check(isinstance(texture, dict) and texture.get("min_filter") == "linear", f"named filters are kept: {texture}")

    # set_properties assigns the same way
    await ed.call("set_properties", scene=scene, entity=soft, values={"mesh": {M + "baseColorTexture": "sprites/hero.png"}})
    got = await texture_of(ed, scene, soft)
    ed.check(got["texture"].get("min_filter") == "nearest" and got["sprite"].get("pixelsPerUnit") == 32,
             f"set_properties copies them too: {got}")
