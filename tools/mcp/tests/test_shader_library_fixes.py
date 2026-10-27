"""Shader library fixes after the phase 1 review: an unknown library shader does not crash value
edits, values kept from a previous shader do not block edits, the shader a set_properties call
sets is what its other values follow (a mesh's customShader too), a shader change puts an
unsupported lighting mode back to auto, shader textures are images (no camera render texture),
an image move takes .material files along (by GUID), list_shaders refuses unknown types, and Lua colour
parameters, defaults and features follow the shader the submesh draws with."""

from __future__ import annotations

from pathlib import Path

import yaml

from harness import Editor, find_key

TIMEOUT = 600

SCENE = "scenes/ShaderLibraryFixes.scene"
TEXTURE = "assets/sprites/rock.png"
M = "submeshes[0].material."


def close(a, b, eps=1e-3) -> bool:
    if isinstance(a, (int, float)):
        a = [a]
    if isinstance(b, (int, float)):
        b = [b]
    return len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def props(ed: Editor, eid, component: str, *names: str) -> dict:
    got = await ed.call("get_entity", entity=eid, components=[component], properties=list(names), detail="values")
    return got["components"][component]


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "set_properties", "list_shaders")
    await ed.call("create_scene", name="ShaderLibraryFixes", path=SCENE)


# ---------------------------------------------------------------------------- list_shaders

async def test_list_shaders_rejects_unknown_type(ed: Editor):
    msg = await ed.check_error("list_shaders", type="sprite", contains="mesh, ui, points, lines, sky")
    ed.check("sprite" in msg, f"names the bad type: {msg}")
    sky = await ed.call("list_shaders", type="sky")
    ed.check(sky["shaders"] and all(s["type"] == "sky" for s in sky["shaders"]), f"sky filter: {sky!r}")


# ---------------------------------------------------------------------------- unknown library shader

async def test_unknown_library_shader_values(ed: Editor):
    """A material whose shader names a library shader the engine does not have (a scene file
    written by hand, or a renamed shader): its values are refused with the library list, no crash."""
    eid = await make_entity(ed, "UnknownLib")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit"}})
    # the reference itself is refused through MCP: put it there as a scene file would
    await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local o = object('UnknownLib', 'Mesh'); local m = o:getMaterial(0); m.shader = 'library:tooon'; "
        "o:setMaterial(0, m); return o:getMaterial(0).shader"))
    got = await props(ed, eid, "mesh", M + "shader")
    ed.check_eq(got[M + "shader"], "library:tooon")
    for key, value in ((M + "shaderParams.baseColor", [1, 0, 0, 1]), (M + "shaderParams", {"baseColor": [1, 0, 0, 1]}),
                       (M + "shaderTextures.u_baseTexture", TEXTURE), (M + "shaderTextures", {"u_baseTexture": TEXTURE}),
                       (M + "shaderFeatures", ["RIM"]), (M + "shaderFeatures", {"RIM": True})):
        await ed.check_error("set_properties", entity=eid, values={"mesh": {key: value}}, contains="library:unlit")
    await ed.check_error("set_shader_uniform", entity=eid, submesh=0, name="baseColor", value=[1, 0, 0, 1],
                         contains="library:unlit")
    await ed.check_alive()
    # an existing shader takes the values again
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:toon", M + "shaderParams.bands": 4}})
    got = await props(ed, eid, "mesh", M + "shaderParams.bands")
    ed.check(close(got[M + "shaderParams.bands"], [4, 0, 0, 0]), f"bands: {got!r}")
    await ed.call("delete_entities", entities=[eid])


# ---------------------------------------------------------------------------- leftovers after a switch

async def test_leftover_values_after_shader_switch(ed: Editor):
    eid = await make_entity(ed, "Leftovers")
    await ed.call("set_properties", entity=eid, values={"mesh": {
        M + "shader": "library:unlit",
        M + "shaderParams.alphaCutoff": 0.3,
        M + "shaderTextures.u_baseTexture": TEXTURE,
    }})
    # another shader: the unlit values stay, edits of the new shader's own values work
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:toon"}})
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shaderParams.bands": 4}})
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shaderTextures.u_rampTexture": TEXTURE}})
    await ed.call("set_shader_uniform", entity=eid, submesh=0, name="bandSmoothness", value=0.2)
    got = await props(ed, eid, "mesh", M + "shaderParams", M + "shaderTextures")
    params = got[M + "shaderParams"]
    ed.check(close(params["bands"], [4, 0, 0, 0]) and close(params["bandSmoothness"], [0.2, 0, 0, 0]), f"toon values: {params!r}")
    ed.check(close(params["alphaCutoff"], [0.3, 0, 0, 0]), f"the unlit value is kept: {params!r}")
    ed.check_eq(got[M + "shaderTextures"], {"u_baseTexture": TEXTURE, "u_rampTexture": TEXTURE})
    # a member of neither is still refused, and a leftover can be removed
    await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "shaderParams.nope": 1}}, contains="bands")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shaderParams.alphaCutoff": None}})
    got = await props(ed, eid, "mesh", M + "shaderParams")
    ed.check("alphaCutoff" not in got[M + "shaderParams"], f"leftover removed: {got!r}")
    await ed.call("delete_entities", entities=[eid])


async def test_shader_change_resets_unsupported_lighting(ed: Editor):
    eid = await make_entity(ed, "LightingReset")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:toon", M + "lighting": "vertex"}})
    # unlit lists none only: vertex would draw its default anyway
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit"}})
    got = await props(ed, eid, "mesh", M + "lighting")
    ed.check_eq(got[M + "lighting"], "auto", "an unsupported lighting mode goes back to auto")
    await ed.call("undo")
    got = await props(ed, eid, "mesh", M + "shader", M + "lighting")
    ed.check_eq((got[M + "shader"], got[M + "lighting"]), ("library:toon", "vertex"), "one undo step")
    # a supported mode stays
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": ""}})
    got = await props(ed, eid, "mesh", M + "lighting")
    ed.check_eq(got[M + "lighting"], "vertex", "the built-in Lit takes vertex")
    await ed.call("delete_entities", entities=[eid])


# ---------------------------------------------------------------------------- same-call shader

async def test_same_call_custom_shader_and_values(ed: Editor):
    """Values set in the same call as a shader follow that shader: a component's customShader,
    a mesh's customShader (its shaderUniforms and the materials without their own shader)."""
    eid = await make_entity(ed, "SameCallMesh")
    # the shader before the call is another library shader: its manifest must not be the one checked
    await ed.call("set_properties", entity=eid, values={"mesh": {"customShader": "library:unlit"}})
    await ed.call("set_properties", entity=eid, values={"mesh": {
        "customShader": "library:toon",
        "shaderUniforms.bands": 4,
        M + "shaderParams.bandSmoothness": 0.25,
        M + "shaderFeatures": {"RIM": True},
    }})
    got = await props(ed, eid, "mesh", "customShader", "shaderUniforms", M + "shaderParams", M + "shaderFeatures")
    ed.check_eq(got["customShader"], "library:toon")
    ed.check(close(got["shaderUniforms"]["bands"], [4, 0, 0, 0]), f"mesh uniforms: {got!r}")
    ed.check(close(got[M + "shaderParams"]["bandSmoothness"], [0.25, 0, 0, 0]), f"material params: {got!r}")
    ed.check_eq(got[M + "shaderFeatures"], ["RIM"])
    # the material's own shader in the same call wins over the mesh's
    await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit",
                                                                        M + "shaderParams.bands": 2}}, contains="alphaCutoff")
    await ed.call("delete_entities", entities=[eid])

    # a component's customShader set in the same call: its values follow it
    points = await make_entity(ed, "SameCallParticles", "points")
    await ed.call("set_properties", entity=points, values={"points": {
        "customShader": "library:particle",
        "shaderFeatures": {"GLOW": True},
        "shaderUniforms.glowScale": 2,
    }})
    got = await props(ed, points, "points", "shaderUniforms", "shaderFeatures")
    ed.check(close(got["shaderUniforms"]["glowScale"], [2, 0, 0, 0]), f"points uniforms: {got!r}")
    await ed.call("delete_entities", entities=[points])


# ---------------------------------------------------------------------------- camera textures

async def test_camera_texture_refused_in_shader_slot(ed: Editor):
    eid = await make_entity(ed, "CamSlot")
    await make_entity(ed, "SlotCam", "camera")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit"}})
    for key, value in ((M + "shaderTextures.u_baseTexture", {"camera": "SlotCam"}),
                       (M + "shaderTextures", {"u_baseTexture": {"camera": "SlotCam"}}),
                       (M + "shaderTextures.u_baseTexture", {"id": "camera|1"})):
        await ed.check_error("set_properties", entity=eid, values={"mesh": {key: value}}, contains="image")
    got = await props(ed, eid, "mesh", M + "shaderTextures")
    ed.check(not got[M + "shaderTextures"], f"nothing stored: {got!r}")
    # the built-in material texture still takes it
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "baseColorTexture": {"camera": "SlotCam"}}})
    await ed.call("delete_entities", entities=[eid])


# ---------------------------------------------------------------------------- asset moves

async def test_move_image_updates_material_files(ed: Editor):
    ed.require_tools("move_asset", "create_material")
    await ed.call("move_asset", path=TEXTURE, to="assets/sprites/rock_shader.png", copy=True)
    material = "assets/materials/MovedTexture"
    await ed.call("create_material", path=material, values={
        "shader": "library:unlit", "shaderTextures": {"u_baseTexture": "assets/sprites/rock_shader.png"}})
    path = ed.project / (material + ".material")
    text = path.read_text()
    data = yaml.safe_load(text)
    ed.check_eq(data["shaderTextures"]["u_baseTexture"]["path"], "sprites/rock_shader.png")
    ed.check(data["shaderTextures"]["u_baseTexture"].get("guid"), f"the texture is written with its GUID: {data!r}")

    # S04 R13: the move rewrites no referrer; the .material file's reference follows by GUID
    await ed.call("move_asset", path="assets/sprites/rock_shader.png", to="assets/sprites/rock_moved.png")
    ed.check_eq(path.read_text(), text, "the move rewrites no referrer file")
    deps = await ed.call("get_asset_dependencies", asset=material + ".material")
    targets = [d["target"]["path"] for d in deps["dependencies"]]
    ed.check(any(t.endswith("sprites/rock_moved.png") for t in targets), f"the .material follows the move: {targets}")


# ---------------------------------------------------------------------------- Lua

async def test_lua_colour_alpha_and_drawn_shader(ed: Editor):
    ed.require_tools("eval_lua")
    eid = await make_entity(ed, "LuaLib")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "library:unlit"}})
    got = await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local o = object('LuaLib', 'Mesh'); local m = o:getMaterial(0); "
        "m:setShaderParam('baseColor', Vector3(1, 0.5, 0)); m:setShaderParam('tiling', Vector3(2, 3, 4)); "
        "o:setMaterial(0, m); local c = o:getMaterial(0):getShaderParam('baseColor'); "
        "local t = o.material:getShaderParam('tiling'); return c.w, t.w"))
    ed.check_eq(got["values"], [1, 0], "a Vector3 colour is opaque, a vector keeps w = 0")

    # a component's colour parameter too, with removeShaderParam
    await make_entity(ed, "LuaParticles", "points")
    await ed.call("set_properties", entity="LuaParticles", values={"points": {"customShader": "library:particle"}})
    got = await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local p = object('LuaParticles', 'Points'); p:setShaderParam('tint', Vector3(1, 0, 0)); "
        "local a = p:getShaderParam('tint').w; p:setShaderParam('glowScale', 2); "
        "local removed = p:removeShaderParam('glowScale'); return a, removed, p:getShaderParam('glowScale').x"))
    ed.check_eq(got["values"], [1, True, 1], "points colour alpha, removeShaderParam back to the default")

    # a material without its own shader follows the mesh's: defaults, default features, keywords
    drawn = await make_entity(ed, "LuaDrawn")
    await ed.call("set_properties", entity=drawn, values={"mesh": {"customShader": "library:toon"}})
    got = await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local o = object('LuaDrawn', 'Mesh'); local m = o:getMaterial(0); "
        "local bands = m:getShaderParam('bands').x; local viaProperty = o.material:getShaderParam('bands').x; "
        "m:setShaderParam('shadowColor', Vector3(0, 0, 1)); local alpha = m:getShaderParam('shadowColor').w; "
        "m:setShaderFeature('RIM', true); local rim = m:isShaderFeatureEnabled('RIM'); o:setMaterial(0, m); "
        "return bands, viaProperty, alpha, rim, m.shader"))
    ed.check_eq(got["values"], [2, 2, 1, True, ""], "the toon defaults through the mesh's customShader")
    got = await props(ed, drawn, "mesh", M + "shaderFeatures", M + "shaderParams")
    ed.check_eq(got[M + "shaderFeatures"], ["RIM"])
    ed.check(close(got[M + "shaderParams"]["shadowColor"], [0, 0, 1, 1]), f"stored: {got!r}")
    for name in ("LuaLib", "LuaParticles", "LuaDrawn"):
        await ed.call("delete_entities", entities=[name])


async def test_save_scene(ed: Editor):
    await ed.call("save_scene")
    ed.check((ed.project / SCENE).exists(), "scene saved")
