"""Library material values, independent scene/file persistence, and lighting configuration."""

from __future__ import annotations

import yaml

from harness import Editor, find_key
from shader_project import scene as project_scene, TEXTURE

EDITOR = {"create": True}
TIMEOUT = 900
M = "submeshes[0].material."
P = M + "extraPass."

MATERIAL = "assets/materials/LibraryUnlit.material"


def close(a, b, eps=1e-3) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def mesh_props(ed: Editor, eid, *names: str) -> dict:
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=list(names), detail="values")
    return got["components"]["mesh"]


async def test_material_library_shader(ed: Editor):
    async with project_scene(ed, "test_material_library_shader", with_texture=True) as SCENE:
        eid = await make_entity(ed, "LibBox")
        m = "submeshes[0].material."

        await ed.call(
            "set_properties",
            entity=eid,
            values={
                "mesh": {
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
                }
            },
        )
        got = await mesh_props(
            ed,
            eid,
            m + "shader",
            m + "shaderParams",
            m + "shaderTextures",
            m + "shaderFeatures",
            m + "blendMode",
            m + "cullMode",
            m + "depthWrite",
            m + "depthTest",
            m + "depthOffsetFactor",
            m + "depthOffsetUnits",
            m + "sortPriority",
            m + "lighting",
        )
        ed.check_eq(got[m + "shader"], "library:unlit")
        params = got[m + "shaderParams"]
        ed.check(close(params["baseColor"], [1, 0.22, 0, 1], eps=0.01), f"hex is sRGB, stored linear: {params!r}")
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
        await ed.call("save_scene")
        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        m = "submeshes[0].material."
        got = await mesh_props(
            ed,
            "LibBox",
            m + "shader",
            m + "shaderParams",
            m + "shaderTextures",
            m + "shaderFeatures",
            m + "blendMode",
            m + "cullMode",
            m + "depthWrite",
            m + "depthTest",
            m + "depthOffsetFactor",
            m + "depthOffsetUnits",
            m + "sortPriority",
        )
        ed.check_eq(got[m + "shader"], "library:unlit")
        ed.check(close(got[m + "shaderParams"]["baseColor"], [1, 0.22, 0, 1], eps=0.01), f"reloaded params: {got!r}")
        ed.check(close(got[m + "shaderParams"]["alphaCutoff"], [0.3, 0, 0, 0]), f"reloaded params: {got!r}")
        ed.check_eq(got[m + "shaderTextures"], {"u_baseTexture": TEXTURE})
        ed.check_eq(sorted(got[m + "shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
        ed.check_eq([got[m + k] for k in ("blendMode", "cullMode", "depthWrite", "depthTest")], ["alpha", "off", "off", "less"])
        ed.check_eq([got[m + k] for k in ("depthOffsetFactor", "depthOffsetUnits", "sortPriority")], [-1, -2, 3])


async def test_lighting_modes_follow_manifest(ed: Editor):
    async with project_scene(ed, "test_lighting_modes_follow_manifest", with_texture=False) as SCENE:
        m = "submeshes[0].material."
        eid = await make_entity(ed, "LibLightingModes")
        for shader, allowed, refused in (
            ("", ["pixel", "vertex", "mixed"], "none"),
            ("library:unlit", ["none"], "pixel"),
            ("library:toon", ["pixel", "vertex", "mixed"], "none"),
        ):
            await ed.call("set_properties", entity=eid, values={"mesh": {m + "shader": shader, m + "lighting": "auto"}})
            for mode in allowed:
                await ed.call("set_properties", entity=eid, values={"mesh": {m + "lighting": mode}})
                got = await mesh_props(ed, eid, m + "lighting")
                ed.check_eq(got[m + "lighting"], mode)
            await ed.check_error("set_properties", entity=eid, values={"mesh": {m + "lighting": refused}}, contains="lighting")
        await ed.call("delete_entities", entities=[eid])


async def test_light_shading_and_cap(ed: Editor):
    async with project_scene(ed, "test_light_shading_and_cap", with_texture=False) as SCENE:
        light = await make_entity(ed, "LibLight", "point_light")
        await ed.call("set_properties", entity=light, values={"light": {"shading": "vertex"}})
        got = await ed.call("get_entity", entity=light, components=["light"], properties=["shading"], detail="values")
        ed.check_eq(got["components"]["light"]["shading"], "vertex")
        await ed.check_error("set_properties", entity=light, values={"light": {"shading": "sometimes"}}, contains="both")

        await ed.call("set_scene_settings", settings={"max_lights_per_object": 4})
        settings = await ed.call("get_scene_settings")
        ed.check_eq(find_key(settings, "max_lights_per_object"), 4)
        await ed.check_error("set_scene_settings", settings={"max_lights_per_object": 17}, contains="1 to 16")
        await ed.call("save_scene")
        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        reloaded = await ed.call("get_entity", entity=light, components=["light"], properties=["shading"], detail="values")
        ed.check_eq(reloaded["components"]["light"]["shading"], "vertex")
        ed.check_eq(find_key(await ed.call("get_scene_settings"), "max_lights_per_object"), 4)


async def test_material_file_round_trip(ed: Editor):
    async with project_scene(ed, "test_material_file_round_trip", with_texture=True) as SCENE:
        r = await ed.call(
            "create_material",
            path=MATERIAL,
            values={
                "shader": "library:unlit",
                "shaderParams": {"baseColor": [0.2, 0.4, 0.6, 1]},
                "shaderTextures": {"u_baseTexture": TEXTURE},
                "shaderFeatures": ["VERTEX_COLOR"],
                "stencil": {"compare": "always", "ref": 2, "pass": "replace"},
                "colorWrite": "rgb",
                "extraPass": {
                    "shader": "library:unlit",
                    "shaderParams": {"baseColor": [0.2, 0.4, 0.6, 1]},
                    "shaderTextures": {"u_baseTexture": TEXTURE},
                    "shaderFeatures": ["VERTEX_COLOR"],
                    "stencil": {"compare": "not_equal", "ref": 2},
                    "colorWrite": "none",
                    "order": "before",
                },
                "blendMode": "additive",
                "cullMode": "front",
                "depthWrite": "on",
                "depthTest": "always",
                "depthOffsetFactor": 1.5,
                "depthOffsetUnits": 2,
                "sortPriority": -4,
            },
        )
        ed.check_eq(r["material"]["shader"], "library:unlit")
        data = yaml.safe_load((ed.project / MATERIAL).read_text())
        ed.check_eq(data["shader"], "library:unlit")
        ed.check_eq([float(v) for v in data["shaderParams"]["baseColor"]], [0.2, 0.4, 0.6, 1.0])
        ed.check_eq(data["shaderTextures"]["u_baseTexture"]["path"], TEXTURE)
        ed.check_eq(data["shaderFeatures"], ["VERTEX_COLOR"])
        ed.check_eq((data["blendMode"], data["cullMode"], data["depthWrite"], data["depthTest"]), ("ADDITIVE", "FRONT", "ENABLED", "ALWAYS"))
        ed.check_eq((float(data["depthOffsetFactor"]), float(data["depthOffsetUnits"]), int(data["sortPriority"])), (1.5, 2.0, -4))

        ed.check_eq(data["stencil"], {"compare": "ALWAYS", "ref": 2, "pass": "REPLACE"})
        ed.check_eq(data["colorWrite"], "rgb")
        ed.check_eq(data["extraPass"]["shader"], "library:unlit")
        ed.check_eq(data["extraPass"]["stencil"], {"compare": "NOT_EQUAL", "ref": 2})
        ed.check_eq(data["extraPass"]["colorWrite"], "none")
        ed.check_eq(data["extraPass"]["order"], "BEFORE")
        ed.check_eq(data["extraPass"]["shaderParams"]["baseColor"], [0.2, 0.4, 0.6, 1])
        ed.check_eq(data["extraPass"]["shaderTextures"]["u_baseTexture"]["path"], TEXTURE)
        ed.check_eq(data["extraPass"]["shaderFeatures"], ["VERTEX_COLOR"])

        # a partial update keeps the rest; parameters merge
        await ed.call("update_material", path=MATERIAL, values={"shaderParams": {"tiling": [4, 4, 0, 0]}, "shaderFeatures": {"ALPHA_CLIP": True}})
        info = await ed.call("get_asset_info", path=MATERIAL)
        mat = info["material"]
        ed.check(close(mat["shaderParams"]["baseColor"], [0.2, 0.4, 0.6, 1]) and close(mat["shaderParams"]["tiling"], [4, 4, 0, 0]), f"merged params: {mat!r}")
        ed.check_eq(sorted(mat["shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
        ed.check_eq(mat["blendMode"], "additive")
        await ed.check_error("update_material", path=MATERIAL, values={"shaderParams": {"nope": 1}}, contains="baseColor")
        await ed.call("update_material", path=MATERIAL, values={"extraPass": {"shaderParams": {"tiling": [4, 4, 0, 0]}, "order": "after"}})
        extra = (await ed.call("get_asset_info", path=MATERIAL))["material"]["extraPass"]
        ed.check(
            close(extra["shaderParams"]["baseColor"], [0.2, 0.4, 0.6, 1]) and close(extra["shaderParams"]["tiling"], [4, 4, 0, 0]),
            "a partial pass update preserves existing parameters",
        )
        ed.check_eq(extra["shaderTextures"], {"u_baseTexture": TEXTURE})
        ed.check_eq(extra["shaderFeatures"], ["VERTEX_COLOR"])

        # a linked submesh takes every field of the file
        eid = await make_entity(ed, "LinkedBox")
        await ed.call("link_material", entity=eid, material=MATERIAL)
        m = "submeshes[0].material."
        got = await mesh_props(ed, eid, m + "shader", m + "shaderFeatures", m + "blendMode", m + "sortPriority", P + "shader", P + "order")
        ed.check_eq(got[m + "shader"], "library:unlit")
        ed.check_eq(sorted(got[m + "shaderFeatures"]), ["ALPHA_CLIP", "VERTEX_COLOR"])
        ed.check_eq(got[m + "blendMode"], "additive")
        ed.check_eq(got[m + "sortPriority"], -4)
        ed.check_eq((got[P + "shader"], got[P + "order"]), ("library:unlit", "after"))
        # and follows an edit of it
        await ed.call("update_material", path=MATERIAL, values={"sortPriority": 7})
        await ed.call("wait", frames=1)
        got = await mesh_props(ed, eid, m + "sortPriority")
        ed.check_eq(got[m + "sortPriority"], 7, "the linked submesh follows the file")

        # Moving an image remaps the actual persisted shader texture reference too.
        await ed.call("move_asset", path=TEXTURE, to="assets/textures/moved.png")
        updated = yaml.safe_load((ed.project / MATERIAL).read_text())
        ed.check_eq(updated["shaderTextures"]["u_baseTexture"]["path"], "assets/textures/moved.png")
        ed.check_eq(updated["extraPass"]["shaderTextures"]["u_baseTexture"]["path"], "assets/textures/moved.png")

        await ed.call("save_scene")
