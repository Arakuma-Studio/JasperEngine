"""The shader library in the editor: list_shaders, a material's library shader, its values
(shaderParams, shaderTextures, shaderFeatures) and render state through set_properties and the
material tools, light shading and the scene's light cap, their save/reload round trip (scene and
.material) and an export of a scene drawing with a library shader."""

from __future__ import annotations

import re
from pathlib import Path

import yaml

from harness import Editor, find_key

TIMEOUT = 900

SCENE = "scenes/ShaderLibrary.scene"
MATERIAL = "assets/materials/LibraryUnlit.material"
TEXTURE = "assets/sprites/rock.png"


def close(a, b, eps=1e-3) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


def srgb_to_linear(c: float) -> float:
    return c ** 2.2  # the engine's conversion (Color::sRGBToLinear)


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def mesh_props(ed: Editor, eid, *names: str) -> dict:
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=list(names), detail="values")
    return got["components"]["mesh"]


# ---------------------------------------------------------------------------- list_shaders

async def test_list_shaders(ed: Editor):
    ed.require_tools("list_shaders")
    r = await ed.call("list_shaders")
    shaders = {s["shader"]: s for s in r["shaders"]}
    ed.check("library:unlit" in shaders, f"unlit listed: {list(shaders)}")
    unlit = shaders["library:unlit"]
    ed.check_eq(unlit["type"], "mesh")
    ed.check_eq(unlit["lighting"], ["none"])
    ed.check(unlit["name"] and unlit["category"] and unlit["description"], f"labels: {unlit!r}")
    ed.check("properties" not in unlit, "the summary leaves out the parameters")

    one = await ed.call("list_shaders", name="unlit")
    ed.check_eq(len(one["shaders"]), 1)
    detail = one["shaders"][0]
    props = {p["member"]: p for p in detail["properties"]}
    ed.check_eq(props["baseColor"]["kind"], "color")
    ed.check_eq(props["baseColor"]["default"], [1, 1, 1, 1])
    ed.check_eq(props["alphaCutoff"]["kind"], "range")
    ed.check_eq((props["alphaCutoff"]["min"], props["alphaCutoff"]["max"]), (0, 1))
    ed.check_eq(props["alphaCutoff"]["feature"], "ALPHA_CLIP")
    ed.check_eq([t["uniform"] for t in detail["textures"]], ["u_baseTexture"])
    ed.check_eq(detail["textures"][0]["default"], "white")
    ed.check_eq(sorted(f["define"] for f in detail["features"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
    ed.check_eq(detail["state"].get("blendMode"), "opaque", "@state blend=opaque")
    ed.check(detail["depth_program"], "unlit has @depth")

    meshes = await ed.call("list_shaders", type="mesh")
    ed.check(all(s["type"] == "mesh" for s in meshes["shaders"]), "type filter")
    ui = await ed.call("list_shaders", type="ui")
    ed.check(all(s["type"] == "ui" for s in ui["shaders"]), "ui filter")
    msg = await ed.call("list_shaders", name="no_such_shader", expect_error=True)
    ed.check_in("library:unlit", msg, "lists the library shaders")


# ---------------------------------------------------------------------------- material values

async def test_material_library_shader(ed: Editor):
    ed.require_tools("set_properties", "get_entity", "get_shader_uniforms", "set_shader_uniform")
    await ed.call("create_scene", name="ShaderLibrary", path=SCENE)
    eid = await make_entity(ed, "LibBox")
    m = "submeshes[0].material."

    await ed.call("set_properties", entity=eid, values={"mesh": {
        m + "shader": "library:unlit",
        m + "shaderParams.baseColor": "#FF8000",
        m + "shaderParams.tiling": [2, 3, 0.5, 0],
        m + "shaderTextures.u_baseTexture": TEXTURE,
        m + "shaderFeatures": {"VERTEX_COLOR": True, "ALPHA_CLIP": True},
        m + "blendMode": "alpha",
        m + "cullMode": "off",
        m + "depthWrite": "off",
        m + "depthTest": "less",
        m + "depthOffsetFactor": -1,
        m + "depthOffsetUnits": -2,
        m + "sortPriority": 3,
    }})
    got = await mesh_props(ed, eid, m + "shader", m + "shaderParams", m + "shaderTextures", m + "shaderFeatures",
                           m + "blendMode", m + "cullMode", m + "depthWrite", m + "depthTest",
                           m + "depthOffsetFactor", m + "depthOffsetUnits", m + "sortPriority", m + "lighting")
    ed.check_eq(got[m + "shader"], "library:unlit")
    params = got[m + "shaderParams"]
    ed.check(close(params["baseColor"], [1, srgb_to_linear(128 / 255), 0, 1]), f"hex is sRGB, stored linear: {params!r}")
    ed.check(close(params["tiling"], [2, 3, 0.5, 0]), f"tiling: {params!r}")
    ed.check_eq(got[m + "shaderTextures"], {"u_baseTexture": TEXTURE})
    ed.check_eq(sorted(got[m + "shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
    ed.check_eq([got[m + k] for k in ("blendMode", "cullMode", "depthWrite", "depthTest")], ["alpha", "off", "off", "less"])
    ed.check_eq([got[m + k] for k in ("depthOffsetFactor", "depthOffsetUnits", "sortPriority")], [-1, -2, 3])
    ed.check_eq(got[m + "lighting"], "auto")

    # one entry reads its value, or the manifest default when unset
    one = await mesh_props(ed, eid, m + "shaderParams.alphaCutoff", m + "shaderTextures.u_baseTexture")
    ed.check(close(one[m + "shaderParams.alphaCutoff"], [0.5, 0, 0, 0]), f"manifest default: {one!r}")
    ed.check_eq(one[m + "shaderTextures.u_baseTexture"], TEXTURE)

    # the manifest refuses what the shader does not have
    await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "shaderParams.nope": 1}}, contains="baseColor")
    await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "shaderFeatures": ["NOPE"]}}, contains="VERTEX_COLOR")
    await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "shaderTextures.u_nope": TEXTURE}}, contains="u_baseTexture")
    await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "lighting": "pixel"}}, contains="none")
    await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "shader": "library:no_such_shader"}}, contains="library:unlit")

    # a mesh shader is not a UI shader
    ui = await make_entity(ed, "LibImage", "image")
    await ed.check_error("set_properties", entity=ui, values={"ui": {"customShader": "library:unlit"}}, contains="mesh shader")

    # a removed entry falls back to the default; undo brings it back
    await ed.call("set_properties", entity=eid, values={"mesh": {m + "shaderParams.tiling": None}})
    got = await mesh_props(ed, eid, m + "shaderParams")
    ed.check("tiling" not in got[m + "shaderParams"], f"tiling removed: {got!r}")
    await ed.call("undo")
    got = await mesh_props(ed, eid, m + "shaderParams")
    ed.check("tiling" in got[m + "shaderParams"], f"undo restores tiling: {got!r}")

    # get/set_shader_uniform work on the material's shader (submesh 0 by default here)
    uni = await ed.call("get_shader_uniforms", entity=eid)
    ed.check_eq(uni["submesh"], 0)
    ed.check_eq(uni["effective_shader"], "library:unlit")
    params = {p["member"]: p for p in uni["parameters"]}
    ed.check(params["baseColor"]["has_value"] and not params["alphaCutoff"]["has_value"], f"parameters: {uni['parameters']!r}")
    s = await ed.call("set_shader_uniform", entity=eid, submesh=0, name="alphaCutoff", value=0.3)
    ed.check_eq(s["property"], m + "shaderParams")
    await ed.check_error("set_shader_uniform", entity=eid, name="nope", value=1, contains="alphaCutoff")
    got = await mesh_props(ed, eid, m + "shaderParams.alphaCutoff")
    ed.check(close(got[m + "shaderParams.alphaCutoff"], [0.3, 0, 0, 0]), f"set_shader_uniform: {got!r}")

    # the whole material reads them too
    whole = await mesh_props(ed, eid, "submeshes[0].material")
    material = whole["submeshes[0].material"]
    ed.check_eq(material["shader"], "library:unlit")
    ed.check_eq(material["blendMode"], "alpha")


async def test_lighting_modes_follow_manifest(ed: Editor):
    """A material's lighting takes exactly the modes its shader lists (the runtime's rules):
    every mesh library shader, and the built-in Lit (pixel, vertex, mixed)."""
    ed.require_tools("list_shaders", "set_properties")
    m = "submeshes[0].material."
    modes = ["pixel", "vertex", "mixed", "none"]
    listed = (await ed.call("list_shaders"))["shaders"]
    cases = [("", ["pixel", "vertex", "mixed"])]
    cases += [(s["shader"], s["lighting"]) for s in listed if s["type"] == "mesh" and s["shader"].startswith("library:")]
    eid = await make_entity(ed, "LibLightingModes")
    for shader, allowed in cases:
        await ed.call("set_properties", entity=eid, values={"mesh": {m + "shader": shader, m + "lighting": "auto"}})
        for mode in modes:
            if mode in allowed:
                await ed.call("set_properties", entity=eid, values={"mesh": {m + "lighting": mode}})
                continue
            msg = await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "lighting": mode}})
            takes = re.search(r"takes ([a-z, ]+)\.", msg)
            ed.check(takes is not None, f"{shader or 'Lit'} {mode}: {msg}")
            # the default (what auto draws) first, then the others
            got = takes.group(1).split(", ")
            ed.check_eq(got[0], "auto")
            ed.check_eq(got[1], allowed[0], f"{shader or 'Lit'}: the default first")
            ed.check_eq(sorted(got[1:]), sorted(allowed), f"{shader or 'Lit'}: {msg}")
    # the scene round trip below expects no lighting set
    await ed.call("delete_entities", entities=[eid])


async def test_light_shading_and_cap(ed: Editor):
    ed.require_tools("set_properties", "set_scene_settings", "get_scene_settings")
    light = await make_entity(ed, "LibLight", "point_light")
    await ed.call("set_properties", entity=light, values={"light": {"shading": "vertex"}})
    got = await ed.call("get_entity", entity=light, components=["light"], properties=["shading"], detail="values")
    ed.check_eq(got["components"]["light"]["shading"], "vertex")
    await ed.check_error("set_properties", entity=light, values={"light": {"shading": "sometimes"}}, contains="both")

    await ed.call("set_scene_settings", settings={"max_lights_per_object": 4})
    settings = await ed.call("get_scene_settings")
    ed.check_eq(find_key(settings, "max_lights_per_object"), 4)
    await ed.check_error("set_scene_settings", settings={"max_lights_per_object": 17}, contains="1 to 16")


# ---------------------------------------------------------------------------- save / reload

async def test_scene_round_trip(ed: Editor):
    ed.require_tools("save_scene", "close_scene", "open_scene")
    await ed.call("save_scene")
    text = (ed.project / SCENE).read_text()
    for key in ("shader: library:unlit", "shaderParams:", "shaderTextures:", "u_baseTexture:", "shaderFeatures:",
                "blendMode: ALPHA", "cullMode: OFF", "depthWrite: DISABLED", "depthTest: LESS", "depthOffsetFactor:",
                "depthOffsetUnits:", "sortPriority: 3", "shading: VERTEX", "maxLightsPerObject: 4"):
        ed.check(key in text, f"the scene file has {key!r}")
    ed.check("lighting:" not in text, "lighting at its default is left out")

    await ed.call("close_scene", scene=SCENE)
    await ed.call("open_scene", scene=SCENE)
    m = "submeshes[0].material."
    got = await mesh_props(ed, "LibBox", m + "shader", m + "shaderParams", m + "shaderTextures", m + "shaderFeatures",
                           m + "blendMode", m + "cullMode", m + "depthWrite", m + "depthTest",
                           m + "depthOffsetFactor", m + "depthOffsetUnits", m + "sortPriority")
    ed.check_eq(got[m + "shader"], "library:unlit")
    ed.check(close(got[m + "shaderParams"]["baseColor"], [1, srgb_to_linear(128 / 255), 0, 1]), f"reloaded params: {got!r}")
    ed.check(close(got[m + "shaderParams"]["alphaCutoff"], [0.3, 0, 0, 0]), f"reloaded params: {got!r}")
    ed.check_eq(got[m + "shaderTextures"], {"u_baseTexture": TEXTURE})
    ed.check_eq(sorted(got[m + "shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
    ed.check_eq([got[m + k] for k in ("blendMode", "cullMode", "depthWrite", "depthTest")], ["alpha", "off", "off", "less"])
    ed.check_eq([got[m + k] for k in ("depthOffsetFactor", "depthOffsetUnits", "sortPriority")], [-1, -2, 3])
    light = await ed.call("get_entity", entity="LibLight", components=["light"], properties=["shading"], detail="values")
    ed.check_eq(light["components"]["light"]["shading"], "vertex")
    settings = await ed.call("get_scene_settings")
    ed.check_eq(find_key(settings, "max_lights_per_object"), 4)

    # saving again changes nothing
    before = (ed.project / SCENE).read_bytes()
    await ed.call("save_scene")
    ed.check_eq((ed.project / SCENE).read_bytes(), before, "a reloaded scene saves byte-identically")


async def test_material_file_round_trip(ed: Editor):
    ed.require_tools("create_material", "update_material", "link_material", "get_asset_info")
    # a material without library values stores none of their keys
    await ed.call("create_material", path="assets/materials/Plain", values={"roughnessFactor": 0.4})
    plain = (ed.project / "assets/materials/Plain.material").read_text()
    for key in ("shader", "blendMode", "cullMode", "depthWrite", "depthTest", "depthOffset", "sortPriority", "lighting"):
        ed.check(key not in plain, f"a plain material leaves out {key}: {plain}")

    r = await ed.call("create_material", path=MATERIAL, values={
        "shader": "library:unlit",
        "shaderParams": {"baseColor": [0.2, 0.4, 0.6, 1]},
        "shaderTextures": {"u_baseTexture": TEXTURE},
        "shaderFeatures": ["VERTEX_COLOR"],
        "blendMode": "additive", "cullMode": "front", "depthWrite": "on", "depthTest": "always",
        "depthOffsetFactor": 1.5, "depthOffsetUnits": 2, "sortPriority": -4,
    })
    ed.check_eq(r["material"]["shader"], "library:unlit")
    data = yaml.safe_load((ed.project / MATERIAL).read_text())
    ed.check_eq(data["shader"], "library:unlit")
    ed.check_eq([float(v) for v in data["shaderParams"]["baseColor"]], [0.2, 0.4, 0.6, 1.0])
    ed.check_eq(data["shaderTextures"]["u_baseTexture"]["path"], "sprites/rock.png")
    ed.check_eq(data["shaderFeatures"], ["VERTEX_COLOR"])
    ed.check_eq((data["blendMode"], data["cullMode"], data["depthWrite"], data["depthTest"]),
                ("ADDITIVE", "FRONT", "ENABLED", "ALWAYS"))
    ed.check_eq((float(data["depthOffsetFactor"]), float(data["depthOffsetUnits"]), int(data["sortPriority"])), (1.5, 2.0, -4))

    # a partial update keeps the rest; parameters merge
    await ed.call("update_material", path=MATERIAL, values={"shaderParams": {"tiling": [4, 4, 0, 0]},
                                                           "shaderFeatures": {"ALPHA_CLIP": True}})
    info = await ed.call("get_asset_info", path=MATERIAL)
    mat = info["material"]
    ed.check(close(mat["shaderParams"]["baseColor"], [0.2, 0.4, 0.6, 1]) and close(mat["shaderParams"]["tiling"], [4, 4, 0, 0]),
             f"merged params: {mat!r}")
    ed.check_eq(sorted(mat["shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
    ed.check_eq(mat["blendMode"], "additive")
    await ed.check_error("update_material", path=MATERIAL, values={"shaderParams": {"nope": 1}}, contains="baseColor")

    # a linked submesh takes every field of the file
    eid = await make_entity(ed, "LinkedBox")
    await ed.call("link_material", entity=eid, material=MATERIAL)
    m = "submeshes[0].material."
    got = await mesh_props(ed, eid, m + "shader", m + "shaderFeatures", m + "blendMode", m + "sortPriority")
    ed.check_eq(got[m + "shader"], "library:unlit")
    ed.check_eq(sorted(got[m + "shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
    ed.check_eq(got[m + "blendMode"], "additive")
    ed.check_eq(got[m + "sortPriority"], -4)
    # and follows an edit of it
    await ed.call("update_material", path=MATERIAL, values={"sortPriority": 7})
    await ed.call("wait", frames=5)
    got = await mesh_props(ed, eid, m + "sortPriority")
    ed.check_eq(got[m + "sortPriority"], 7, "the linked submesh follows the file")

    # a material file saves byte-identically after a load
    before = (ed.project / MATERIAL).read_text()
    await ed.call("update_material", path=MATERIAL, values={"sortPriority": 7})
    ed.check_eq((ed.project / MATERIAL).read_text(), before, "re-saved material file unchanged")
    await ed.call("save_scene")


# ---------------------------------------------------------------------------- export

async def test_export_library_scene(ed: Editor):
    ed.require_tools("build_project")
    out = ed.scratch.root / "export-library"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux",
                      backends=["opengl"], timeout_seconds=900, timeout=1000)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    generated = "\n".join(p.read_text(errors="replace") for p in out.rglob("*.cpp") if "ShaderLibrary" in p.read_text(errors="replace")
                          or "library:unlit" in p.read_text(errors="replace"))
    ed.check('material.shader = "library:unlit";' in generated, "the generated scene sets the material shader")
    for line in ('.blendMode = MaterialBlendMode::ALPHA;', '.cullMode = MaterialCullMode::OFF;',
                 '.depthWrite = MaterialDepthWrite::DISABLED;', '.depthTest = MaterialDepthTest::LESS;',
                 '.sortPriority = 3;', '.shaderTextures.push_back({"u_baseTexture", texture});', 'light.shading = LightShading::VERTEX;',
                 'scene->setMaxLightsPerObject(4);'):
        ed.check(line in generated, f"generated code has {line!r}")
    ed.check(re.search(r'\.shaderFeatures = \{"(ALPHA_CLIP|VERTEX_COLOR)", "(ALPHA_CLIP|VERTEX_COLOR)"\};', generated),
             "generated code sets the features")
    ed.check(re.search(r'\.shaderParams = \{\{"baseColor"', generated), "generated code sets the parameters")
    ed.check((out / "project" / "assets" / "sprites" / "rock.png").exists(), "the shader texture ships")
    # the library variant with its features, and its depth program, are precompiled
    header = (out / "shaders" / "glsl410.h").read_text(errors="replace")
    ed.check(re.search(r"mesh_library_unlit_ALPHA_CLIP_VERTEX_COLOR_\w+_glsl410", header), "the mesh variant with its features")
    ed.check(re.search(r"depth_library_unlit_ALPHA_CLIP_VERTEX_COLOR_\w+_glsl410", header), "its @depth program")
