"""S94 P2 (docs/specs/S94-rendering-anim-gaps.md F5, F7-F9, F11, F12) through the agent's tools:
set_shader_uniform override: true writes a Mesh's Property Override (one undo step, saved and
reopened, read back by get_entity), the built-in Lit takes baseColorFactor; a Mesh value its
material hides says so in `next` (F8); Blend Mode Custom's factors are material values
(set_properties, get_entity); Transparency Sort Mode takes perspective and orthographic; in Play,
Lua's MaterialPropertyBlock and Mesh:setPropertyBlock read back, Mesh:setShaderTexture writes the
submesh's shader texture and Mesh:setTexture a library shader's main texture, and Texture.ready is
a property.

Runs on an owned empty project.
"""

from __future__ import annotations

from harness import Editor, find_key
from shader_project import TEXTURE, scene as project_scene, texture as make_texture

EDITOR = {"create": True, "name": "Draw Materials"}
TIMEOUT = 300
CI = True

M = "submeshes[0].material."


async def make_box(ed: Editor, name: str):
    made = await ed.call("create_entity", name=name, type="box")
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def mesh_values(ed: Editor, entity, *properties: str) -> dict:
    got = await ed.call("get_entity", entity=entity, components=["mesh"], properties=list(properties), detail="values")
    return got["components"]["mesh"]


async def test_property_overrides(ed: Editor):
    async with project_scene(ed, "test_property_overrides") as path:
        eid = await make_box(ed, "Zone")
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit", M + "shaderParams.baseColor": [1, 1, 1, 1]}})

        set_ = await ed.call("set_shader_uniform", entity=eid, name="baseColor", value=[1, 0, 0, 1], override=True)
        ed.check_eq(set_.get("property"), "propertyOverrides.values", f"{set_}")
        overrides = (await mesh_values(ed, eid, "propertyOverrides")).get("propertyOverrides", {})
        ed.check_eq(overrides.get("values"), {"baseColor": [1, 0, 0, 1]}, f"get_entity lists the override: {overrides}")

        # a submesh's override beside the Mesh's
        await ed.call("set_shader_uniform", entity=eid, submesh=0, name="tiling", value=[2, 2, 0, 0], override=True)
        submesh = (await mesh_values(ed, eid, "submeshes[0].propertyOverrides")).get("submeshes[0].propertyOverrides", {})
        ed.check_eq(submesh.get("values"), {"tiling": [2, 2, 0, 0]}, f"{submesh}")

        # one undo step each
        await ed.call("undo", scope="scene")
        submesh = (await mesh_values(ed, eid, "submeshes[0].propertyOverrides")).get("submeshes[0].propertyOverrides", {})
        ed.check(not submesh.get("values"), f"undone: {submesh}")

        # a library shader refuses a member it does not declare, as a material value does
        await ed.check_error("set_shader_uniform", entity=eid, name="nope", value=1, override=True, contains="has no parameter")

        # the built-in Lit's colour on another box
        lit = await make_box(ed, "LitBox")
        await ed.call("set_shader_uniform", entity=lit, name="baseColorFactor", value=[0, 1, 0, 1], override=True)
        await ed.check_error("set_shader_uniform", entity=lit, name="tint", value=1, override=True, contains="baseColorFactor")

        # saved and opened again
        await ed.call("save_scene", scene=path)
        await ed.call("close_scene", scene=path)
        await ed.call("open_scene", scene=path)
        reopened = (await mesh_values(ed, "Zone", "propertyOverrides")).get("propertyOverrides", {})
        ed.check_eq(reopened.get("values"), {"baseColor": [1, 0, 0, 1]}, f"after a reopen: {reopened}")
        lit_reopened = (await mesh_values(ed, "LitBox", "propertyOverrides")).get("propertyOverrides", {})
        ed.check_eq(lit_reopened.get("values"), {"baseColorFactor": [0, 1, 0, 1]}, f"{lit_reopened}")


async def test_shadowed_mesh_uniform_next(ed: Editor):
    async with project_scene(ed, "test_shadowed_mesh_uniform_next") as path:
        eid = await make_box(ed, "Hidden")
        # a fork on the Mesh (customShader): set_shader_uniform writes shaderUniforms
        await ed.call("set_properties", entity=eid, values={"mesh": {"customShader": "library:unlit", M + "shaderParams.baseColor": [1, 1, 1, 1]}})
        got = await ed.call_raw("set_shader_uniform", {"entity": eid, "name": "baseColor", "value": [0, 0, 1, 1]})
        ed.check(not got.is_error, got.text)
        next_ = (got.structured or {}).get("next", "")
        ed.check("override: true" in next_ and "baseColor" in next_, f"next names the Property Override: {got.structured}")
        await ed.call("save_scene", scene=path)


async def test_custom_blend_factors(ed: Editor):
    async with project_scene(ed, "test_custom_blend_factors"):
        eid = await make_box(ed, "Smoke")
        await ed.call("set_properties", entity=eid, values={"mesh": {
            M + "blendMode": "custom", M + "blendSrc": "dst_color", M + "blendDst": "src_color", M + "blendOp": "add"}})
        got = await mesh_values(ed, eid, M + "blendMode", M + "blendSrc", M + "blendDst")
        ed.check_eq((got.get(M + "blendMode"), got.get(M + "blendSrc"), got.get(M + "blendDst")), ("custom", "dst_color", "src_color"), f"{got}")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "blendSrc": "dst_colour"}})


async def test_transparency_sort_modes(ed: Editor):
    for mode in ("perspective", "orthographic", "default"):
        await ed.call("set_project_settings", values={"graphics.transparency_sort_mode": mode})
        got = (await ed.call("get_project_settings", keys=["graphics.transparency_sort_mode"]))["settings"]
        ed.check_eq(got["graphics.transparency_sort_mode"], mode, f"{mode} set")
    bad = await ed.call_raw("set_project_settings", {"values": {"graphics.transparency_sort_mode": "isometric"}})
    ed.check(bad.is_error and "orthographic" in bad.text, f"the error names the modes: {bad.text}")


PLAY_LUA = """
local mesh = Mesh(scene, scene:findEntity("Board"))
local block = MaterialPropertyBlock()
local empty = block:isEmpty()
block:setColor("baseColor", Vector4(1, 0, 0, 1))
block:setFloat("speed", 2)
block:setTexture("u_baseTexture", "TEXTURE")
mesh:setPropertyBlock(block)
local back = mesh:getPropertyBlock()
local c = back:getColor("baseColor")
mesh:setShaderTexture("u_baseTexture", "TEXTURE")
local shaderTexture = mesh:getShaderTexture("u_baseTexture")
local plain = Mesh(scene, scene:findEntity("Plain"))
plain:setTexture("TEXTURE")
local main = plain:getShaderTexture("u_baseTexture")
local t = Texture("TEXTURE")
local readyType = type(t.ready)
return {empty, back:isEmpty(), c.x, back:getFloat("speed"), back:getTexture("u_baseTexture"), shaderTexture, main, readyType}
"""


async def test_lua_property_block_and_textures(ed: Editor):
    async with project_scene(ed, "test_lua_property_block", with_texture=True) as path:
        for name in ("Board", "Plain"):
            eid = await make_box(ed, name)
            await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit"}})
        await ed.call("save_scene", scene=path)
        started = await ed.call("play", action="start", scene=path)
        ed.check_eq(started["state"], "playing", "the scene plays")
        try:
            values = (await ed.call("eval_lua", code=PLAY_LUA.replace("TEXTURE", TEXTURE)))["values"][0]
            ed.check_eq(values[0], True, f"a new block is empty: {values}")
            ed.check_eq(values[1], False, f"the read-back block is not: {values}")
            ed.check(abs(values[2] - 1.0) < 1e-4, f"getColor gives the sRGB colour set: {values}")
            ed.check_eq(values[3], 2, f"getFloat: {values}")
            ed.check_eq(values[4], TEXTURE, f"getTexture: {values}")
            ed.check_eq(values[5], TEXTURE, f"Mesh:setShaderTexture writes the submesh's shaderTextures: {values}")
            ed.check_eq(values[6], TEXTURE, f"Mesh:setTexture writes the library shader's main texture: {values}")
            ed.check_eq(values[7], "boolean", f"Texture.ready is a property: {values}")
            await ed.call("wait", frames=3)
            textures = (await mesh_values(ed, "Board", M + "shaderTextures")).get(M + "shaderTextures", {})
            ed.check(TEXTURE in str(textures), f"get_entity shows the shader texture: {textures}")
            console = await ed.call("read_console", levels=["error"])
            ed.check(not console.get("entries"), f"no errors: {console}")
        finally:
            await ed.call("play", action="stop")
