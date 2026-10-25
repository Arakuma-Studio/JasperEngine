"""Stencil/extra-pass public configuration and prefab/model persistence."""

from __future__ import annotations

import shutil
from pathlib import Path

from harness import Editor, find_key
from shader_project import scene as project_scene, TEXTURE

EDITOR = {"create": True}
TIMEOUT = 900
M = "submeshes[0].material."
P = M + "extraPass."

MODEL = Path(__file__).resolve().parents[3] / "tools/fixtures/shader-library-export/assets/models/box.gltf"

PASS_VALUES = {
    P + "shader": "library:unlit",
    P + "shaderParams.baseColor": "#00FF00",
    P + "shaderParams.alphaCutoff": 0.25,
    P + "shaderTextures.u_baseTexture": TEXTURE,
    P + "shaderFeatures": {"ALPHA_CLIP": True},
    P + "blendMode": "alpha",
    P + "cullMode": "front",
    P + "depthWrite": "on",
    P + "depthTest": "less_equal",
    P + "depthOffsetFactor": 1.5,
    P + "depthOffsetUnits": 2,
    P + "stencil.compare": "not_equal",
    P + "stencil.ref": 1,
    P + "stencil.readMask": 1,
    P + "stencil.writeMask": 0,
    P + "colorWrite": "rgb",
    P + "order": "before",
}


def check_pass(ed: Editor, got: dict, where: str):
    ed.check_eq(got[P + "shader"], "library:unlit", where)
    params = got[P + "shaderParams"]
    ed.check(close(params["baseColor"], [0, 1, 0, 1]) and close(params["alphaCutoff"], [0.25, 0, 0, 0]), f"{where}: {params!r}")
    ed.check_eq(got[P + "shaderTextures"], {"u_baseTexture": TEXTURE}, where)
    ed.check_eq(got[P + "shaderFeatures"], ["ALPHA_CLIP"], where)
    ed.check_eq(
        [got[P + k] for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "colorWrite", "order")],
        ["alpha", "front", "on", "less_equal", "rgb", "before"],
        where,
    )
    ed.check_eq([got[P + k] for k in ("depthOffsetFactor", "depthOffsetUnits")], [1.5, 2], where)
    ed.check_eq([got[P + "stencil." + k] for k in ("compare", "ref", "readMask", "writeMask", "pass")], ["not_equal", 1, 1, 0, "keep"], where)


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
    # one component asked: its key is the component's canonical name ("lines" -> linerenderer)
    return next(iter(got["components"].values()))


async def model_mesh_entity(ed: Editor, root) -> object:
    """The GameObject of a model that holds its mesh: the root, else its first descendant with one"""
    got = await ed.call("get_entity", entity=root, detail="values")
    if "mesh" in (got.get("components") or {}):
        return root
    tree = await ed.call("get_scene_tree", root=root)
    for node in find_nodes(tree):
        child = await ed.call("get_entity", entity=node, detail="values")
        if "mesh" in (child.get("components") or {}):
            return node
    ed.check(False, f"no mesh under the model: {tree!r}")


def find_nodes(tree) -> list:
    out = []

    def walk(value):
        if isinstance(value, dict):
            if "id" in value and isinstance(value["id"], int):
                out.append(value["id"])
            for v in value.values():
                walk(v)
        elif isinstance(value, list):
            for v in value:
                walk(v)

    walk(tree)
    return out


async def test_material_stencil_and_color_write(ed: Editor):
    async with project_scene(ed, "test_material_stencil_and_color_write", with_texture=True) as SCENE:
        eid = await make_entity(ed, "StencilBox")
        await ed.call(
            "set_properties",
            entity=eid,
            values={
                "mesh": {
                    M + "stencil.compare": "always",
                    M + "stencil.ref": 3,
                    M + "stencil.writeMask": 7,
                    M + "stencil.pass": "replace",
                    M + "stencil.depthFail": "incr",  # the manifest's name of incr_clamp
                    M + "colorWrite": "rgb",
                }
            },
        )
        got = await props(ed, eid, "mesh", M + "stencil", M + "stencil.compare", M + "stencil.ref", M + "colorWrite")
        ed.check_eq(
            got[M + "stencil"], {"compare": "always", "ref": 3, "readMask": 255, "writeMask": 7, "pass": "replace", "fail": "keep", "depthFail": "incr_clamp"}
        )
        ed.check_eq((got[M + "stencil.compare"], got[M + "stencil.ref"], got[M + "colorWrite"]), ("always", 3, "rgb"))

        # the whole block merges the keys given; colour write takes a mask too
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "stencil": {"compare": "lequal"}, M + "colorWrite": 0}})
        got = await props(ed, eid, "mesh", M + "stencil", M + "colorWrite")
        ed.check_eq((got[M + "stencil"]["compare"], got[M + "stencil"]["ref"]), ("less_equal", 3))
        ed.check_eq(got[M + "colorWrite"], "none")

        # out of range or unknown values are refused with the valid ones
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "stencil.ref": 300}}, contains="0..255")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "stencil.compare": "sometimes"}}, contains="not_equal")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "stencil.pass": "explode"}}, contains="incr_wrap")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "colorWrite": "rgbx"}}, contains="rgba")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "stencil": {"bits": 1}}}, contains="writeMask")

        # one undo step per call
        await ed.call("undo")
        got = await props(ed, eid, "mesh", M + "stencil.compare", M + "colorWrite")
        ed.check_eq((got[M + "stencil.compare"], got[M + "colorWrite"]), ("always", "rgb"), "undo restores the previous call")

        # null resets the block (auto)
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "stencil": None}})
        got = await props(ed, eid, "mesh", M + "stencil")
        ed.check_eq(got[M + "stencil"]["compare"], "auto")
        await ed.call("undo")
        await ed.call("save_scene")
        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        got = await props(ed, eid, "mesh", M + "stencil", M + "colorWrite")
        ed.check_eq((got[M + "stencil"]["compare"], got[M + "stencil"]["ref"], got[M + "colorWrite"]), ("always", 3, "rgb"))


async def test_material_extra_pass(ed: Editor):
    async with project_scene(ed, "test_material_extra_pass", with_texture=True) as SCENE:
        eid = await make_entity(ed, "PassBox")
        await ed.call("set_properties", entity=eid, values={"mesh": PASS_VALUES})
        got = await props(
            ed,
            eid,
            "mesh",
            P + "shader",
            P + "shaderParams",
            P + "shaderTextures",
            P + "shaderFeatures",
            *[P + k for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "depthOffsetFactor", "depthOffsetUnits", "colorWrite", "order")],
            *[P + "stencil." + k for k in ("compare", "ref", "readMask", "writeMask", "pass")],
        )
        check_pass(ed, got, "set through set_properties")

        # one entry reads its value, else the pass shader's manifest default
        one = await props(ed, eid, "mesh", P + "shaderParams.baseColor", P + "shaderParams.tiling", P + "shaderTextures.u_baseTexture")
        ed.check(close(one[P + "shaderParams.tiling"], [1, 1, 0, 0]), f"the pass shader's default: {one!r}")
        ed.check_eq(one[P + "shaderTextures.u_baseTexture"], TEXTURE)

        # the whole pass reads as an object, and the material carries it
        whole = await props(ed, eid, "mesh", M + "extraPass", "submeshes[0].material")
        ed.check_eq(whole[M + "extraPass"]["shader"], "library:unlit")
        ed.check_eq(whole[M + "extraPass"]["stencil"]["compare"], "not_equal")
        ed.check_eq(whole["submeshes[0].material"]["extraPass"]["order"], "before")
        ed.check_eq(whole["submeshes[0].material"]["shader"], "", "the material keeps the built-in Lit")

        # the pass's values follow its own shader's manifest, not the material's (built-in Lit here)
        await ed.check_error("set_properties", entity=eid, values={"mesh": {P + "shaderParams.nope": 1}}, contains="baseColor")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {P + "shaderFeatures": ["NOPE"]}}, contains="ALPHA_CLIP")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {P + "shaderTextures.u_nope": TEXTURE}}, contains="u_baseTexture")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {P + "shader": "library:no_such_shader"}}, contains="library:unlit")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {P + "shader": "library:ui_fx"}}, contains="mesh shader")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {M + "extraPass": {"lighting": "pixel"}}}, contains="order")
        await ed.check_error("set_properties", entity=eid, values={"mesh": {P + "order": "sideways"}}, contains="before")

        # a shader set in the same call is what the pass values follow; another shader's values stay
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": "library:toon", P + "shaderParams.bands": 4}})
        got = await props(ed, eid, "mesh", P + "shaderParams")
        ed.check(close(got[P + "shaderParams"]["bands"], [4, 0, 0, 0]), "the pass takes the newly selected shader's value")
        ed.check(close(got[P + "shaderParams"]["baseColor"], [0, 1, 0, 1]), "the old shader's value remains stored")
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": "library:unlit"}})
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shaderParams.alphaCutoff": 0.25}})
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shaderParams.bands": None}})

        # a whole pass merges; null removes it (one undo step brings it back)
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "extraPass": {"depthOffsetUnits": 2, "order": "before"}}})
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "extraPass": None}})
        got = await props(ed, eid, "mesh", M + "extraPass")
        ed.check(got[M + "extraPass"] is None, f"removed: {got!r}")
        await ed.call("undo")
        got = await props(
            ed,
            eid,
            "mesh",
            P + "shader",
            P + "shaderParams",
            P + "shaderTextures",
            P + "shaderFeatures",
            *[P + k for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "depthOffsetFactor", "depthOffsetUnits", "colorWrite", "order")],
            *[P + "stencil." + k for k in ("compare", "ref", "readMask", "writeMask", "pass")],
        )
        check_pass(ed, got, "after undo")
        eid = await make_entity(ed, "UnknownPass")
        await ed.call(
            "eval_lua",
            allow_edit_mode=True,
            code=(
                "local o = object('UnknownPass', 'Mesh'); local m = o:getMaterial(0); m.extraPass.shader = 'library:tooon'; "
                "o:setMaterial(0, m); return o:getMaterial(0).extraPass.shader"
            ),
        )
        got = await props(ed, eid, "mesh", P + "shader")
        ed.check_eq(got[P + "shader"], "library:tooon")
        for key, value in (
            (P + "shaderParams.baseColor", [1, 0, 0, 1]),
            (P + "shaderParams", {"baseColor": [1, 0, 0, 1]}),
            (P + "shaderTextures.u_baseTexture", TEXTURE),
            (P + "shaderFeatures", ["RIM"]),
            (M + "extraPass", {"shaderParams": {"baseColor": [1, 0, 0, 1]}}),
        ):
            await ed.check_error("set_properties", entity=eid, values={"mesh": {key: value}}, contains="library:unlit")
        await ed.check_alive()
        await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": "library:unlit", P + "shaderParams.alphaCutoff": 0.4}})
        await ed.call("delete_entities", entities=[eid])
        await ed.call("save_scene")
        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        got = await props(
            ed,
            "PassBox",
            "mesh",
            P + "shader",
            P + "shaderParams",
            P + "shaderTextures",
            P + "shaderFeatures",
            *[P + k for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "depthOffsetFactor", "depthOffsetUnits", "colorWrite", "order")],
            *[P + "stencil." + k for k in ("compare", "ref", "readMask", "writeMask", "pass")],
        )
        check_pass(ed, got, "after reload")


async def test_component_stencil_and_color_write(ed: Editor):
    async with project_scene(ed, "test_component_stencil_and_color_write", with_texture=True) as SCENE:
        image = await make_entity(ed, "MaskedImage", "image")
        points = await make_entity(ed, "MaskedPoints", "points")
        lines = await make_entity(ed, "MaskedLines", "lines")
        for eid, component in ((image, "ui"), (points, "points"), (lines, "lines")):
            await ed.call(
                "set_properties", entity=eid, values={component: {"stencil.compare": "equal", "stencil.ref": 16, "stencil.readMask": 240, "colorWrite": "rgba"}}
            )
            got = await props(ed, eid, component, "stencil", "stencil.ref", "colorWrite")
            ed.check_eq((got["stencil"]["compare"], got["stencil"]["readMask"], got["stencil.ref"], got["colorWrite"]), ("equal", 240, 16, "rgba"), component)
            await ed.check_error("set_properties", entity=eid, values={component: {"stencil.writeMask": -1}}, contains="0..255")
            await ed.check_error("set_properties", entity=eid, values={component: {"colorWrite": "xyz"}}, contains="none")
        # a whole block, as get_entity gives it, sets the same
        await ed.call("set_properties", entity=lines, values={"lines": {"stencil": {"compare": "not_equal", "pass": "zero"}}})
        got = await props(ed, lines, "lines", "stencil")
        ed.check_eq((got["stencil"]["compare"], got["stencil"]["pass"], got["stencil"]["ref"]), ("not_equal", "zero", 16))

        await ed.call("save_scene")
        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        for eid, component, compare in ((image, "ui", "equal"), (points, "points", "equal"), (lines, "lines", "not_equal")):
            got = await props(ed, eid, component, "stencil", "colorWrite")
            ed.check_eq(
                (got["stencil"]["ref"], got["stencil"]["compare"], got["colorWrite"]),
                (16, compare, "rgba"),
                "component stencil and color masks survive scene reload",
            )


async def test_prefab_override(ed: Editor):
    async with project_scene(ed, "test_prefab_override", with_texture=True) as SCENE:
        eid = await make_entity(ed, "PassCrate")
        await ed.call(
            "set_properties",
            entity=eid,
            values={"mesh": {P + "shader": "library:unlit", P + "shaderParams.baseColor": [1, 0, 0, 1], M + "stencil.compare": "always"}},
        )
        await ed.call("create_prefab", entity=eid, path="prefabs/PassCrate.prefab")
        made = await ed.call("instantiate_prefab", prefab="prefabs/PassCrate.prefab", position=[3, 0, 0], name="PassCrate B")
        b = made["instance"]["id"]
        got = await props(ed, b, "mesh", P + "shader", M + "stencil.compare")
        ed.check_eq((got[P + "shader"], got[M + "stencil.compare"]), ("library:unlit", "always"), "the instance has the prefab's")

        await ed.call(
            "set_properties",
            entity=b,
            values={"mesh": {P + "shaderParams.baseColor": [0, 0, 1, 1], M + "stencil.ref": 5, M + "colorWrite": "a", P + "order": "before"}},
        )
        # the source instance is untouched
        got = await props(ed, eid, "mesh", P + "shaderParams", M + "stencil.ref", P + "order")
        ed.check(close(got[P + "shaderParams"]["baseColor"], [1, 0, 0, 1]) and got[M + "stencil.ref"] == 0, f"source: {got!r}")

        await ed.call("save_scene")
        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        got = await props(ed, "PassCrate B", "mesh", P + "shaderParams", M + "stencil.ref", M + "stencil.compare", M + "colorWrite", P + "order", P + "shader")
        ed.check(close(got[P + "shaderParams"]["baseColor"], [0, 0, 1, 1]), f"override kept: {got!r}")
        ed.check_eq(
            [got[k] for k in (M + "stencil.ref", M + "stencil.compare", M + "colorWrite", P + "order", P + "shader")],
            [5, "always", "a", "before", "library:unlit"],
        )
        got = await props(ed, "PassCrate", "mesh", P + "order", M + "colorWrite")
        ed.check_eq((got[P + "order"], got[M + "colorWrite"]), ("after", "auto"), "the source keeps the prefab's values")


async def test_model_reload_keeps_phase2_fields(ed: Editor):
    async with project_scene(ed, "test_model_reload_keeps_phase2_fields", with_texture=True) as SCENE:
        models = ed.project / "assets" / "models"
        models.mkdir(parents=True, exist_ok=True)
        shutil.copy(MODEL, models / "box.gltf")
        await ed.call("sync_files", paths=["assets/models/box.gltf"])
        made = await ed.call("set_model", create={"name": "PassModel"}, path="assets/models/box.gltf")
        root = find_key(made, "entity_id", "id")
        mesh = await model_mesh_entity(ed, root if root is not None else "PassModel")
        await ed.call(
            "set_properties",
            entity=mesh,
            values={
                "mesh": {
                    P + "shader": "library:unlit",
                    P + "shaderParams.baseColor": [1, 1, 0, 1],
                    P + "cullMode": "front",
                    M + "stencil.compare": "always",
                    M + "stencil.pass": "replace",
                    M + "colorWrite": "rgb",
                }
            },
        )
        await ed.call("save_scene")

        await ed.call("close_scene", scene=SCENE)
        await ed.call("open_scene", scene=SCENE)
        await ed.call("wait", frames=10)
        got = await props(ed, mesh, "mesh", P + "shader", P + "shaderParams", P + "cullMode", M + "stencil.compare", M + "stencil.pass", M + "colorWrite")
        ed.check_eq(
            [got[k] for k in (P + "shader", P + "cullMode", M + "stencil.compare", M + "stencil.pass", M + "colorWrite")],
            ["library:unlit", "front", "always", "replace", "rgb"],
            "kept across the model load",
        )
        ed.check(close(got[P + "shaderParams"]["baseColor"], [1, 1, 0, 1]), f"pass params kept: {got!r}")


async def test_material_presets(ed: Editor):
    async with project_scene(ed, "test_material_presets", with_texture=True) as SCENE:
        eid = await make_entity(ed, "PresetBox")
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "roughnessFactor": 0.3}})
        before = (await props(ed, eid, "mesh", "submeshes[0].material"))["submeshes[0].material"]
        await ed.call("apply_material_preset", preset="stencil_mask", entity=eid)
        got = await props(ed, eid, "mesh", M + "shader", M + "sortPriority", M + "roughnessFactor")
        ed.check_eq(got[M + "shader"], "library:stencil_mask")
        ed.check_eq(got[M + "sortPriority"], -1)
        ed.check(close(got[M + "roughnessFactor"], 0.3), "the preset preserves unrelated roughness")
        await ed.call("undo")
        after = (await props(ed, eid, "mesh", "submeshes[0].material"))["submeshes[0].material"]
        ed.check_eq(after, before, "one undo restores the entire previous material")
        await ed.call("redo")
        got = await props(ed, eid, "mesh", M + "shader")
        ed.check_eq(got[M + "shader"], "library:stencil_mask", "redo restores the preset")

        await ed.call("create_material", path="assets/materials/PresetFile", values={"roughnessFactor": 0.3})
        linked = await make_entity(ed, "PresetLinked")
        await ed.call("link_material", entity=linked, material="assets/materials/PresetFile.material")
        await ed.call("apply_material_preset", preset="stencil_mask", path="assets/materials/PresetFile.material")
        await ed.call("wait", frames=1)
        got = await props(ed, linked, "mesh", M + "shader", M + "roughnessFactor")
        ed.check_eq(got[M + "shader"], "library:stencil_mask", "the linked submesh follows the preset applied to its file")
        ed.check(close(got[M + "roughnessFactor"], 0.3), "the file keeps unrelated roughness")
        r = await ed.call("apply_material_preset", preset="outline_hull", entity=linked)
        ed.check(r["applied"][0]["unlinked"], "a preset on an instance makes its material independent")
        await ed.call("undo")
        got = (await props(ed, linked, "mesh", "submeshes[0].material"))["submeshes[0].material"]
        ed.check_eq(got.get("file"), "assets/materials/PresetFile.material", "undo restores the link")
        await ed.check_error("apply_material_preset", preset="no_such_preset", entity=eid, contains="list_material_presets")
        await ed.check_error("apply_material_preset", preset="stencil_mask", entity=eid, submesh=3, contains="submesh")
