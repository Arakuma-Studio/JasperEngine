"""Shader library phase 3, scene inputs: a fork requiring scenedepth / scenecolor compiles
(includes/scene.glsl), the manifest checks its shader type, reserved textures and queue, a
material putting it in the opaque queue is a check error (check_shader without arguments), and
list_shaders reports scene_inputs."""

from __future__ import annotations

from harness import Editor, find_key

CI = True
EDITOR = {"create": True, "name": "Scene Inputs CI"}

TIMEOUT = 600

SCENE = "scenes/SceneInputs.scene"
M = "submeshes[0].material."

VERT = """#version 450
#include "includes/surface_vs.glsl"
"""

FRAG = """#version 450

// @shader mesh
// @name "Soft test"
// @require scenedepth
// @require scenecolor feature=TINT
// @feature TINT "Tint by the scene"
// @state blend=alpha zwrite=off
// @property softness float 0.5 "Softness"

uniform u_fs_customParams {
    float softness;
} customParams;

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec2 uv = screenUV();
    float fade = clamp((sceneLinearDepth(uv) - fragmentLinearDepth()) / customParams.softness, 0.0, 1.0);
    vec3 color = vec3(1.0, 0.5, 0.2) + sceneWorldPosition(uv) * 0.0;
    #ifdef TINT
        color *= sceneColor(uv);
    #endif
    g_finalColor = finalizeColor(vec4(color, fade));
}
"""


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def write_fork(ed: Editor, base: str, frag: str):
    await ed.call("write_file", path=base + ".vert", content=VERT, timeout=300)
    await ed.call("write_file", path=base + ".frag", content=frag, timeout=300)


def messages(result) -> list[str]:
    return [f.get("message", "") for f in result.get("findings", [])]


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "write_file", "check_shader", "list_shaders", "set_properties")
    await ed.call("create_scene", name="SceneInputs", path=SCENE)


async def test_fork_compiles(ed: Editor):
    await write_fork(ed, "shaders/soft", FRAG)
    for ref in ("shaders/soft", "shaders/soft#TINT"):
        ok = await ed.call("check_shader", shader=ref, shader_type="mesh", timeout=300)
        ed.check_eq(ok["failed"], 0, f"{ref} compiles: {ok!r}")
        ed.check(not [m for m in messages(ok) if "manifest" in m], f"{ref}: no manifest problem: {ok!r}")


async def test_manifest_rules(ed: Editor):
    # a UI shader cannot read the scene
    ui = FRAG.replace("// @shader mesh", "// @shader ui")
    await write_fork(ed, "shaders/soft_ui", ui)
    r = await ed.call("check_shader", shader="shaders/soft_ui", shader_type="ui", timeout=300)
    ed.check(any("needs @shader mesh, points or particles" in m for m in messages(r)), f"ui refused: {r!r}")

    # opaque: drawn before the scene it reads
    opaque = FRAG.replace("// @state blend=alpha zwrite=off", "// @state blend=opaque")
    await write_fork(ed, "shaders/soft_opaque", opaque)
    r = await ed.call("check_shader", shader="shaders/soft_opaque", shader_type="mesh", timeout=300)
    ed.check(any("transparent queue" in m for m in messages(r)), f"opaque state refused: {r!r}")

    # the engine's texture names are reserved
    reserved = FRAG.replace("// @property softness", "// @texture u_sceneColorTexture white\n// @property softness")
    await write_fork(ed, "shaders/soft_reserved", reserved)
    r = await ed.call("check_shader", shader="shaders/soft_reserved", shader_type="mesh", timeout=300)
    ed.check(any("reserved" in m for m in messages(r)), f"reserved texture refused: {r!r}")


async def test_opaque_material_is_a_check_error(ed: Editor):
    eid = await make_entity(ed, "Soft box")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "shaders/soft"}})
    r = await ed.call("check_shader", timeout=300)
    ed.check(not [m for m in messages(r) if "opaque queue" in m], f"blended: no error: {r!r}")

    await ed.call("set_properties", entity=eid, values={"mesh": {M + "blendMode": "opaque"}})
    r = await ed.call("check_shader", timeout=300)
    errors = [f for f in r.get("findings", []) if "opaque queue" in f.get("message", "")]
    ed.check(errors and errors[0].get("severity") == "error", f"opaque material reported: {r!r}")
    ed.check("Soft box" in errors[0]["message"] and "depth and colour" not in errors[0]["message"],
             f"names the GameObject and the depth: {errors[0]!r}")

    # with TINT it reads the colour too
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shaderFeatures": ["TINT"]}})
    r = await ed.call("check_shader", timeout=300)
    errors = [f for f in r.get("findings", []) if "opaque queue" in f.get("message", "")]
    ed.check(errors and "depth and colour" in errors[0]["message"], f"depth and colour: {r!r}")

    await ed.call("set_properties", entity=eid, values={"mesh": {M + "blendMode": "auto"}})
    r = await ed.call("check_shader", timeout=300)
    ed.check(not [m for m in messages(r) if "opaque queue" in m], f"back to its state: no error: {r!r}")


async def test_list_shaders_scene_inputs(ed: Editor):
    listing = await ed.call("list_shaders")
    ed.check(listing["shaders"] and all("scene_inputs" in s for s in listing["shaders"]), f"scene_inputs on every entry: {listing!r}")
    detail = await ed.call("list_shaders", name="toon")
    entry = detail["shaders"][0] if "shaders" in detail else detail
    ed.check_eq(entry.get("scene_inputs"), [], "toon reads nothing of the scene")
    # a feature= rule counts (the picker's cost mark): unlit SOFT, projected_decal always
    for name in ("unlit", "projected_decal"):
        entry = (await ed.call("list_shaders", name=name))["shaders"][0]
        ed.check_eq(entry.get("scene_inputs"), ["scenedepth"], f"{name} may read the scene depth")


async def test_list_shaders_state_priority(ed: Editor):
    # @state priority= (engine review fixes): projected_decal draws before the transparent objects
    # of the same sort priority
    entry = (await ed.call("list_shaders", name="projected_decal"))["shaders"][0]
    ed.check_eq(entry.get("state", {}).get("priority"), -1, f"projected_decal's state priority: {entry.get('state')!r}")
    entry = (await ed.call("list_shaders", name="toon"))["shaders"][0]
    ed.check("priority" not in entry.get("state", {}), f"no priority where the manifest sets none: {entry.get('state')!r}")
