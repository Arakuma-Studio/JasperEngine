"""Shader library phase 2 in the editor: a material's stencil, colour write and extra pass, the
stencil and colour write of UI/Points/LineRenderer, through set_properties (every Catalog path)
and the material tools, with the extra pass's values checked against its own shader's manifest;
their round trips (scene, .material, prefab override, model reload); the material presets
(list, apply, undo); list_shaders' stencil and colour write state; and an export of a scene with
stencil masks, colour write and an extra pass."""

from __future__ import annotations

import re
import shutil
from pathlib import Path

import yaml

from harness import Editor, find_key, skip

TIMEOUT = 900

SCENE = "scenes/Phase2.scene"
MATERIAL = "assets/materials/Phase2Pass.material"
TEXTURE = "assets/sprites/rock.png"
M = "submeshes[0].material."
P = M + "extraPass."
REPO = Path(__file__).resolve().parents[3]
MODEL = REPO / "tools/fixtures/shader-library-export/assets/models/box.gltf"


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


async def library_shaders(ed: Editor) -> dict:
    listed = (await ed.call("list_shaders", detail=True))["shaders"]
    return {s["shader"]: s for s in listed}


# the values set on the extra pass of PassBox (library:unlit as the pass shader: every build has it)
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
    ed.check_eq([got[P + k] for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "colorWrite", "order")],
                ["alpha", "front", "on", "less_equal", "rgb", "before"], where)
    ed.check_eq([got[P + k] for k in ("depthOffsetFactor", "depthOffsetUnits")], [1.5, 2], where)
    ed.check_eq([got[P + "stencil." + k] for k in ("compare", "ref", "readMask", "writeMask", "pass")],
                ["not_equal", 1, 1, 0, "keep"], where)


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "set_properties", "list_shaders", "list_material_presets", "apply_material_preset")
    await ed.call("create_scene", name="Phase2", path=SCENE)


# ---------------------------------------------------------------------------- list_shaders

async def test_list_shaders_stencil_state(ed: Editor):
    """state / state_rules carry the stencil keys a @state sets and colorwrite=, as the material
    properties name them; inputs list smoothnormals"""
    shaders = await library_shaders(ed)
    names = {"compare", "ref", "readMask", "writeMask", "pass", "fail", "depthFail"}
    for reference, shader in shaders.items():
        for state in [shader["state"]] + [rule["state"] for rule in shader["state_rules"]]:
            if "stencil" in state:
                ed.check(set(state["stencil"]) <= names, f"{reference} stencil keys: {state['stencil']!r}")
            if "colorWrite" in state:
                ed.check(re.fullmatch(r"none|r?g?b?a?", state["colorWrite"]) is not None, f"{reference}: {state!r}")
    # the phase 2 library shaders, when this build has them
    if "library:stencil_mask" in shaders:
        state = shaders["library:stencil_mask"]["state"]
        ed.check_eq(state.get("colorWrite"), "none")
        ed.check_eq(state.get("stencil"), {"compare": "always", "ref": 16, "writeMask": 240, "pass": "replace"})
    if "library:silhouette" in shaders:
        ed.check_eq(shaders["library:silhouette"]["state"].get("stencil", {}).get("compare"), "not_equal")
    if "library:outline" in shaders:
        ed.check("smoothnormals" in shaders["library:outline"]["require"], f"outline inputs: {shaders['library:outline']!r}")


# ---------------------------------------------------------------------------- material stencil

async def test_material_stencil_and_color_write(ed: Editor):
    eid = await make_entity(ed, "StencilBox")
    await ed.call("set_properties", entity=eid, values={"mesh": {
        M + "stencil.compare": "always",
        M + "stencil.ref": 3,
        M + "stencil.writeMask": 7,
        M + "stencil.pass": "replace",
        M + "stencil.depthFail": "incr",       # the manifest's name of incr_clamp
        M + "colorWrite": "rgb",
    }})
    got = await props(ed, eid, "mesh", M + "stencil", M + "stencil.compare", M + "stencil.ref", M + "colorWrite")
    ed.check_eq(got[M + "stencil"], {"compare": "always", "ref": 3, "readMask": 255, "writeMask": 7, "pass": "replace",
                                     "fail": "keep", "depthFail": "incr_clamp"})
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


# ---------------------------------------------------------------------------- extra pass

async def test_material_extra_pass(ed: Editor):
    eid = await make_entity(ed, "PassBox")
    await ed.call("set_properties", entity=eid, values={"mesh": PASS_VALUES})
    got = await props(ed, eid, "mesh", P + "shader", P + "shaderParams", P + "shaderTextures", P + "shaderFeatures",
                      *[P + k for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "depthOffsetFactor",
                                        "depthOffsetUnits", "colorWrite", "order")],
                      *[P + "stencil." + k for k in ("compare", "ref", "readMask", "writeMask", "pass")])
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
    shaders = await library_shaders(ed)
    other = next(s for ref, s in sorted(shaders.items())
                 if s["type"] == "mesh" and ref != "library:unlit" and s["properties"] and not s["properties"][0].get("feature"))
    member = other["properties"][0]["member"]
    await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": other["shader"], P + "shaderParams." + member: 0.5}})
    got = await props(ed, eid, "mesh", P + "shaderParams")
    ed.check(member in got[P + "shaderParams"] and "baseColor" in got[P + "shaderParams"], f"leftovers kept: {got!r}")
    # back on unlit: one of its values edits (the other shader's leftover is not checked)
    await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": "library:unlit"}})
    await ed.call("set_properties", entity=eid, values={"mesh": {P + "shaderParams.alphaCutoff": 0.25}})
    await ed.call("set_properties", entity=eid, values={"mesh": {P + "shaderParams." + member: None}})

    # a whole pass merges; null removes it (one undo step brings it back)
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "extraPass": {"depthOffsetUnits": 2, "order": "before"}}})
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "extraPass": None}})
    got = await props(ed, eid, "mesh", M + "extraPass")
    ed.check(got[M + "extraPass"] is None, f"removed: {got!r}")
    await ed.call("undo")
    got = await props(ed, eid, "mesh", P + "shader", P + "shaderParams", P + "shaderTextures", P + "shaderFeatures",
                      *[P + k for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "depthOffsetFactor",
                                        "depthOffsetUnits", "colorWrite", "order")],
                      *[P + "stencil." + k for k in ("compare", "ref", "readMask", "writeMask", "pass")])
    check_pass(ed, got, "after undo")


async def test_unknown_pass_shader_values(ed: Editor):
    """A pass whose shader names a library shader the engine does not have (a hand-written file):
    its values are refused with the library list, no crash; an existing shader takes them again."""
    eid = await make_entity(ed, "UnknownPass")
    await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local o = object('UnknownPass', 'Mesh'); local m = o:getMaterial(0); m.extraPass.shader = 'library:tooon'; "
        "o:setMaterial(0, m); return o:getMaterial(0).extraPass.shader"))
    got = await props(ed, eid, "mesh", P + "shader")
    ed.check_eq(got[P + "shader"], "library:tooon")
    for key, value in ((P + "shaderParams.baseColor", [1, 0, 0, 1]), (P + "shaderParams", {"baseColor": [1, 0, 0, 1]}),
                       (P + "shaderTextures.u_baseTexture", TEXTURE), (P + "shaderFeatures", ["RIM"]),
                       (M + "extraPass", {"shaderParams": {"baseColor": [1, 0, 0, 1]}})):
        await ed.check_error("set_properties", entity=eid, values={"mesh": {key: value}}, contains="library:unlit")
    await ed.check_alive()
    await ed.call("set_properties", entity=eid, values={"mesh": {P + "shader": "library:unlit", P + "shaderParams.alphaCutoff": 0.4}})
    await ed.call("delete_entities", entities=[eid])


# ---------------------------------------------------------------------------- components

async def test_component_stencil_and_color_write(ed: Editor):
    image = await make_entity(ed, "MaskedImage", "image")
    points = await make_entity(ed, "MaskedPoints", "points")
    lines = await make_entity(ed, "MaskedLines", "lines")
    for eid, component in ((image, "ui"), (points, "points"), (lines, "lines")):
        await ed.call("set_properties", entity=eid, values={component: {
            "stencil.compare": "equal", "stencil.ref": 16, "stencil.readMask": 240, "colorWrite": "rgba"}})
        got = await props(ed, eid, component, "stencil", "stencil.ref", "colorWrite")
        ed.check_eq((got["stencil"]["compare"], got["stencil"]["readMask"], got["stencil.ref"], got["colorWrite"]),
                    ("equal", 240, 16, "rgba"), component)
        await ed.check_error("set_properties", entity=eid, values={component: {"stencil.writeMask": -1}}, contains="0..255")
        await ed.check_error("set_properties", entity=eid, values={component: {"colorWrite": "xyz"}}, contains="none")
    # a whole block, as get_entity gives it, sets the same
    await ed.call("set_properties", entity=lines, values={"lines": {"stencil": {"compare": "not_equal", "pass": "zero"}}})
    got = await props(ed, lines, "lines", "stencil")
    ed.check_eq((got["stencil"]["compare"], got["stencil"]["pass"], got["stencil"]["ref"]), ("not_equal", "zero", 16))


# ---------------------------------------------------------------------------- save / reload

async def test_scene_round_trip(ed: Editor):
    ed.require_tools("save_scene", "close_scene", "open_scene")
    await ed.call("save_scene")
    data = yaml.safe_load((ed.project / SCENE).read_text())
    text = (ed.project / SCENE).read_text()
    for key in ("stencil:", "compare: ALWAYS", "colorWrite: rgb", "extraPass:", "order: BEFORE", "compare: NOT_EQUAL",
                "depthFail: INCR_CLAMP", "colorWrite: rgba", "compare: EQUAL"):
        ed.check(key in text, f"the scene file has {key!r}")
    ed.check(data is not None, "the scene parses")

    await ed.call("close_scene", scene=SCENE)
    await ed.call("open_scene", scene=SCENE)
    got = await props(ed, "PassBox", "mesh", P + "shader", P + "shaderParams", P + "shaderTextures", P + "shaderFeatures",
                      *[P + k for k in ("blendMode", "cullMode", "depthWrite", "depthTest", "depthOffsetFactor",
                                        "depthOffsetUnits", "colorWrite", "order")],
                      *[P + "stencil." + k for k in ("compare", "ref", "readMask", "writeMask", "pass")])
    check_pass(ed, got, "after reload")
    got = await props(ed, "StencilBox", "mesh", M + "stencil", M + "colorWrite")
    ed.check_eq(got[M + "stencil"], {"compare": "always", "ref": 3, "readMask": 255, "writeMask": 7, "pass": "replace",
                                     "fail": "keep", "depthFail": "incr_clamp"})
    ed.check_eq(got[M + "colorWrite"], "rgb")
    got = await props(ed, "MaskedImage", "ui", "stencil", "colorWrite")
    ed.check_eq((got["stencil"]["compare"], got["stencil"]["ref"], got["colorWrite"]), ("equal", 16, "rgba"))

    before = (ed.project / SCENE).read_bytes()
    await ed.call("save_scene")
    ed.check_eq((ed.project / SCENE).read_bytes(), before, "a reloaded scene saves byte-identically")


async def test_material_file_round_trip(ed: Editor):
    ed.require_tools("create_material", "update_material", "link_material", "get_asset_info")
    await ed.call("create_material", path="assets/materials/Plain2", values={"roughnessFactor": 0.4})
    plain = (ed.project / "assets/materials/Plain2.material").read_text()
    for key in ("stencil", "colorWrite", "extraPass"):
        ed.check(key not in plain, f"a plain material leaves out {key}: {plain}")

    r = await ed.call("create_material", path=MATERIAL, values={
        "stencil": {"compare": "always", "ref": 2, "pass": "replace"},
        "colorWrite": "rgb",
        "extraPass": {"shader": "library:unlit", "shaderParams": {"baseColor": [0.2, 0.4, 0.6, 1]},
                      "shaderTextures": {"u_baseTexture": TEXTURE}, "shaderFeatures": ["VERTEX_COLOR"],
                      "cullMode": "front", "stencil": {"compare": "not_equal", "ref": 2}, "colorWrite": "none",
                      "order": "before"},
    })
    ed.check_eq(r["material"]["extraPass"]["shader"], "library:unlit")
    data = yaml.safe_load((ed.project / MATERIAL).read_text())
    ed.check_eq(data["stencil"], {"compare": "ALWAYS", "ref": 2, "pass": "REPLACE"})
    ed.check_eq(data["colorWrite"], "rgb")
    extra = data["extraPass"]
    ed.check_eq((extra["shader"], extra["cullMode"], extra["colorWrite"], extra["order"]), ("library:unlit", "FRONT", "none", "BEFORE"))
    ed.check_eq(extra["stencil"], {"compare": "NOT_EQUAL", "ref": 2})
    ed.check_eq(extra["shaderTextures"]["u_baseTexture"]["path"], "sprites/rock.png")
    ed.check_eq(extra["shaderFeatures"], ["VERTEX_COLOR"])

    # a partial update merges into the pass; its values follow the pass shader
    await ed.call("update_material", path=MATERIAL, values={"extraPass": {"shaderParams": {"tiling": [4, 4, 0, 0]}}})
    info = await ed.call("get_asset_info", path=MATERIAL)
    pass_ = info["material"]["extraPass"]
    ed.check(close(pass_["shaderParams"]["baseColor"], [0.2, 0.4, 0.6, 1]) and close(pass_["shaderParams"]["tiling"], [4, 4, 0, 0]),
             f"merged: {pass_!r}")
    ed.check_eq(pass_["cullMode"], "front")
    await ed.check_error("update_material", path=MATERIAL, values={"extraPass": {"shaderParams": {"nope": 1}}}, contains="baseColor")
    await ed.check_error("update_material", path=MATERIAL, values={"extraPass": {"shader": "library:nope"}}, contains="library:unlit")

    # a linked submesh takes the pass, stencil and colour write
    eid = await make_entity(ed, "LinkedPassBox")
    await ed.call("link_material", entity=eid, material=MATERIAL)
    got = await props(ed, eid, "mesh", P + "shader", P + "order", M + "stencil.ref", M + "colorWrite", P + "colorWrite")
    ed.check_eq([got[k] for k in (P + "shader", P + "order", M + "stencil.ref", M + "colorWrite", P + "colorWrite")],
                ["library:unlit", "before", 2, "rgb", "none"])
    await ed.call("update_material", path=MATERIAL, values={"extraPass": {"order": "after"}})
    await ed.call("wait", frames=5)
    got = await props(ed, eid, "mesh", P + "order")
    ed.check_eq(got[P + "order"], "after", "the linked submesh follows the file")

    before = (ed.project / MATERIAL).read_text()
    await ed.call("update_material", path=MATERIAL, values={"extraPass": {"order": "after"}})
    ed.check_eq((ed.project / MATERIAL).read_text(), before, "re-saved material file unchanged")
    await ed.call("save_scene")


async def test_prefab_override(ed: Editor):
    ed.require_tools("create_prefab", "instantiate_prefab", "get_prefab_info")
    eid = await make_entity(ed, "PassCrate")
    await ed.call("set_properties", entity=eid, values={"mesh": {
        P + "shader": "library:unlit", P + "shaderParams.baseColor": [1, 0, 0, 1], M + "stencil.compare": "always"}})
    await ed.call("create_prefab", entity=eid, path="prefabs/PassCrate.prefab")
    made = await ed.call("instantiate_prefab", prefab="prefabs/PassCrate.prefab", position=[3, 0, 0], name="PassCrate B")
    b = made["instance"]["id"]
    got = await props(ed, b, "mesh", P + "shader", M + "stencil.compare")
    ed.check_eq((got[P + "shader"], got[M + "stencil.compare"]), ("library:unlit", "always"), "the instance has the prefab's")

    await ed.call("set_properties", entity=b, values={"mesh": {
        P + "shaderParams.baseColor": [0, 0, 1, 1], M + "stencil.ref": 5, M + "colorWrite": "a", P + "order": "before"}})
    info = await ed.call("get_prefab_info", entity=b, usage=False)
    names = set()
    for entry in info["overrides"]:
        names.update(entry.get("properties") or [])
    for prop in (P + "shaderParams", M + "stencil.ref", M + "colorWrite", P + "order"):
        ed.check(prop in names, f"{prop} is an override: {sorted(names)}")
    # the source instance is untouched
    got = await props(ed, eid, "mesh", P + "shaderParams", M + "stencil.ref", P + "order")
    ed.check(close(got[P + "shaderParams"]["baseColor"], [1, 0, 0, 1]) and got[M + "stencil.ref"] == 0, f"source: {got!r}")

    await ed.call("save_scene")
    await ed.call("close_scene", scene=SCENE)
    await ed.call("open_scene", scene=SCENE)
    got = await props(ed, "PassCrate B", "mesh", P + "shaderParams", M + "stencil.ref", M + "stencil.compare", M + "colorWrite",
                      P + "order", P + "shader")
    ed.check(close(got[P + "shaderParams"]["baseColor"], [0, 0, 1, 1]), f"override kept: {got!r}")
    ed.check_eq([got[k] for k in (M + "stencil.ref", M + "stencil.compare", M + "colorWrite", P + "order", P + "shader")],
                [5, "always", "a", "before", "library:unlit"])
    got = await props(ed, "PassCrate", "mesh", P + "order", M + "colorWrite")
    ed.check_eq((got[P + "order"], got[M + "colorWrite"]), ("after", "auto"), "the source keeps the prefab's values")


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


async def test_model_reload_keeps_phase2_fields(ed: Editor):
    """A model's submesh keeps its stencil, colour write and extra pass across a load of the file
    (Submesh::overrideFields), the whole pass under one bit"""
    ed.require_tools("set_model")
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    shutil.copy(MODEL, models / "box.gltf")
    await ed.call("sync_files", paths=["assets/models/box.gltf"])
    made = await ed.call("set_model", create={"name": "PassModel"}, path="assets/models/box.gltf")
    root = find_key(made, "entity_id", "id")
    mesh = await model_mesh_entity(ed, root if root is not None else "PassModel")
    await ed.call("set_properties", entity=mesh, values={"mesh": {
        P + "shader": "library:unlit", P + "shaderParams.baseColor": [1, 1, 0, 1], P + "cullMode": "front",
        M + "stencil.compare": "always", M + "stencil.pass": "replace", M + "colorWrite": "rgb"}})
    await ed.call("save_scene")
    text = (ed.project / SCENE).read_text()
    flags = [int(v) for v in re.findall(r"overrideFields: (\d+)", text)]
    bits = (1 << 27) | (1 << 28) | (1 << 29)
    ed.check(any((f & bits) == bits for f in flags), f"stencil, colour write and extra pass bits saved: {flags}")

    await ed.call("close_scene", scene=SCENE)
    await ed.call("open_scene", scene=SCENE)
    await ed.call("wait", frames=10)
    got = await props(ed, mesh, "mesh", P + "shader", P + "shaderParams", P + "cullMode", M + "stencil.compare",
                      M + "stencil.pass", M + "colorWrite")
    ed.check_eq([got[k] for k in (P + "shader", P + "cullMode", M + "stencil.compare", M + "stencil.pass", M + "colorWrite")],
                ["library:unlit", "front", "always", "replace", "rgb"], "kept across the model load")
    ed.check(close(got[P + "shaderParams"]["baseColor"], [1, 1, 0, 1]), f"pass params kept: {got!r}")


# ---------------------------------------------------------------------------- presets

async def test_material_presets(ed: Editor):
    listed = await ed.call("list_material_presets")
    presets = listed["presets"]
    for preset in presets:
        ed.check(re.fullmatch(r"[a-z0-9_]+", preset["id"]) and preset["name"] and preset["description"], f"{preset!r}")
        ed.check(isinstance(preset["sets"], dict) and preset["sets"], f"{preset['id']} sets something: {preset!r}")
    await ed.check_error("apply_material_preset", preset="no_such_preset", entity="StencilBox", contains="list_material_presets")
    if not presets:
        skip("this build has no material presets")

    eid = await make_entity(ed, "PresetBox")
    before = (await props(ed, eid, "mesh", "submeshes[0].material"))["submeshes[0].material"]
    for preset in presets:
        r = await ed.call("apply_material_preset", preset=preset["id"], entity=eid)
        ed.check_eq([a["submesh"] for a in r["applied"]], [0], preset["id"])
        material = (await props(ed, eid, "mesh", "submeshes[0].material"))["submeshes[0].material"]
        for key, value in preset["sets"].items():
            if key in ("extraPass", "stencil") and isinstance(value, dict):
                # a submesh's read leaves out the pass fields at their defaults
                for k, v in value.items():
                    if k in material[key] or k in ("shader", "compare"):
                        ed.check_eq(material[key][k], v, f"{preset['id']} sets {key}.{k}")
            elif key != "name":
                ed.check_eq(material[key], value, f"{preset['id']} sets {key}")
        # one undo step takes the material back
        await ed.call("undo")
        after = (await props(ed, eid, "mesh", "submeshes[0].material"))["submeshes[0].material"]
        ed.check_eq(after, before, f"undo of {preset['id']}")
    await ed.call("redo")
    redone = (await props(ed, eid, "mesh", "submeshes[0].material"))["submeshes[0].material"]
    ed.check(redone != before, "redo applies the last preset again")
    await ed.call("undo")

    # a .material file: linked submeshes follow it
    first = presets[0]
    await ed.call("create_material", path="assets/materials/PresetFile", values={"roughnessFactor": 0.3})
    linked = await make_entity(ed, "PresetLinked")
    await ed.call("link_material", entity=linked, material="assets/materials/PresetFile.material")
    r = await ed.call("apply_material_preset", preset=first["id"], path="assets/materials/PresetFile.material")
    ed.check_eq(len(r["linked_by"]), 1)
    ed.check(close(r["material"]["roughnessFactor"], 0.3), "a preset keeps what it does not set")
    await ed.call("wait", frames=5)
    got = (await props(ed, linked, "mesh", "submeshes[0].material"))["submeshes[0].material"]
    for key, value in first["sets"].items():
        if not isinstance(value, dict) and key != "name":
            ed.check_eq(got[key], value, f"the linked submesh follows: {key}")

    # applied to a linked submesh, it becomes the submesh's own material (one undo step)
    second = presets[-1]
    r = await ed.call("apply_material_preset", preset=second["id"], entity=linked)
    ed.check(r["applied"][0]["unlinked"], f"unlinked: {r!r}")
    await ed.call("undo")
    got = (await props(ed, linked, "mesh", "submeshes[0].material"))["submeshes[0].material"]
    ed.check_eq(got.get("file"), "assets/materials/PresetFile.material", "undo links it again")

    # entities and a single submesh
    await ed.check_error("apply_material_preset", preset=first["id"], entity=eid, submesh=3, contains="submesh")
    await ed.check_error("apply_material_preset", preset=first["id"], entity="MaskedImage", contains="Mesh")


# ---------------------------------------------------------------------------- export

async def test_export_phase2_scene(ed: Editor):
    ed.require_tools("build_project")
    # a mask, a reader inside it with its own colour write, and an extra pass with a texture
    mask = await make_entity(ed, "ExportMask", "plane")
    await ed.call("set_properties", entity=mask, values={"mesh": {
        M + "stencil": {"compare": "always", "ref": 16, "writeMask": 240, "pass": "replace"},
        M + "colorWrite": "none", M + "sortPriority": -1}})
    reader = await make_entity(ed, "ExportReader", "sphere")
    await ed.call("set_properties", entity=reader, values={"mesh": {
        M + "stencil": {"compare": "equal", "ref": 16, "readMask": 240}, M + "colorWrite": "rgb",
        P + "shader": "library:unlit", P + "shaderParams.baseColor": [1, 0.5, 0, 1],
        P + "shaderTextures.u_baseTexture": TEXTURE, P + "shaderFeatures": ["ALPHA_CLIP"], P + "cullMode": "front",
        P + "order": "before"}})
    image = await ed.call("get_entity", entity="MaskedImage", components=["ui"], detail="values")
    ed.check(image is not None, "the masked image is there")
    await ed.call("save_scene")

    out = ed.scratch.root / "export-phase2"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux",
                      backends=["opengl"], timeout_seconds=900, timeout=1000)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    generated = "\n".join(p.read_text(errors="replace") for p in out.rglob("*.cpp")
                          if "extraPass" in p.read_text(errors="replace") or "stencil" in p.read_text(errors="replace"))
    for line in ('.stencil.compare = MaterialStencilCompare::ALWAYS;', '.stencil.ref = 16;', '.stencil.writeMask = 240;',
                 '.stencil.pass = MaterialStencilOp::REPLACE;', '.colorWrite = 0;', '.colorWrite = 7;',
                 '.stencil.compare = MaterialStencilCompare::EQUAL;', '.stencil.readMask = 240;',
                 '.extraPass.shader = "library:unlit";', '.extraPass.cullMode = MaterialCullMode::FRONT;',
                 '.extraPass.order = MaterialPassOrder::BEFORE;', '.extraPass.shaderTextures.push_back({"u_baseTexture", texture});',
                 '.extraPass.shaderFeatures = {"ALPHA_CLIP"};', 'ui.stencil.compare = MaterialStencilCompare::EQUAL;',
                 'ui.colorWrite = 15;', '.extraPass.stencil.compare = MaterialStencilCompare::NOT_EQUAL;'):
        ed.check(line in generated, f"generated code has {line!r}")
    ed.check(re.search(r'\.extraPass\.shaderParams = \{\{"baseColor"', generated), "generated code sets the pass parameters")
    ed.check((out / "project" / "assets" / "sprites" / "rock.png").exists(), "the pass texture ships")
    # the pass variants are precompiled: unlit with its feature and the blend define of its state
    header = (out / "shaders" / "glsl410.h").read_text(errors="replace")
    ed.check(re.search(r"mesh_library_unlit_ALPHA_CLIP_BLEND_\w+_glsl410", header), "the extra pass's variant is precompiled")
