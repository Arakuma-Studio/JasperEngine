"""Shader library phase 3, the editor side of scene inputs: what agents and the inspector are told
about a draw reading the scene depth or colour and its cost.

- list_shaders: every shader whose scene_inputs is not empty has a cost_note (one line in the
  list, the whole cost with detail), and its features / keyword values say which inputs they turn
  on (scene_inputs), so that with what the shader always requires they make up scene_inputs.
- get_entity: scene_inputs per reading submesh (its shader's and its extra pass's current
  variant), the queue, the opaque-queue error with the fix the inspector's button applies
  (Catalog::getSceneInputQueueFix: blendMode alpha), and scene_inputs_cost; points readers too.
  The fix applied through set_properties clears the error and undo brings it back (the same
  property command as the button).
- get_shader_uniforms: scene_inputs and cost_note of the material's (or component's) variant."""

from __future__ import annotations

from harness import Editor, find_key

TIMEOUT = 600

SCENE = "scenes/SceneInputsEditor.scene"
M = "submeshes[0].material."

VERT = """#version 450
#include "includes/surface_vs.glsl"
"""

# depth always, colour with TINT; transparent by its state
FRAG = """#version 450

// @shader mesh
// @name "Soft editor test"
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
    vec3 color = vec3(1.0, 0.5, 0.2);
    #ifdef TINT
        color *= sceneColor(uv);
    #endif
    g_finalColor = finalizeColor(vec4(color, fade));
}
"""

POINTS_VERT = """#version 450

#include "includes/points_vs.glsl"
"""

POINTS_FRAG = """#version 450

// @shader points
// @name "Soft points editor test"
// @require scenedepth
// @state zwrite=off
// @property softness float 0.6 "Softness"

uniform u_fs_customParams {
    float softness;
} customParams;

out vec4 g_finalColor;

#include "includes/points_fs.glsl"
#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec4 particle = pointVertexColor();
    float soft = clamp((sceneLinearDepth(screenUV()) - fragmentLinearDepth()) / max(customParams.softness, 0.0001), 0.0, 1.0);
    g_finalColor = finalizeStraightColor(vec4(particle.rgb, particle.a * soft));
}
"""


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def write_fork(ed: Editor, base: str, vert: str, frag: str):
    await ed.call("write_file", path=base + ".vert", content=vert, timeout=300)
    await ed.call("write_file", path=base + ".frag", content=frag, timeout=300)


async def entity_inputs(ed: Editor, eid) -> tuple[list, dict, str]:
    """get_entity's scene_inputs, the whole result and its text"""
    r = await ed.call_raw("get_entity", {"entity": eid, "components": ["mesh", "points"], "detail": "summary"})
    ed.check(not r.is_error, f"get_entity: {r.text[:300]}")
    return r.value.get("scene_inputs", []), r.value, r.text


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "write_file", "get_entity", "get_shader_uniforms", "list_shaders", "set_properties", "undo")
    await ed.call("create_scene", name="SceneInputsEditor", path=SCENE)
    await write_fork(ed, "shaders/soft_editor", VERT, FRAG)
    await write_fork(ed, "shaders/soft_points_editor", POINTS_VERT, POINTS_FRAG)


async def test_list_shaders_cost(ed: Editor):
    listing = await ed.call_raw("list_shaders", {})
    shaders = listing.value["shaders"]
    for entry in shaders:
        inputs = entry["scene_inputs"]
        ed.check(bool(inputs) == ("cost_note" in entry), f"cost_note exactly with scene_inputs: {entry!r}")
        if inputs:
            note = entry["cost_note"]
            ed.check(note.startswith("Reads the scene") and "per camera" in note, f"one-line note: {note!r}")
            ed.check("Avoid on low-end mobile" not in note, f"the list keeps it short: {note!r}")
            ed.check(entry["shader"] in listing.text and "per camera]" in listing.text, f"said on its text line: {listing.text[:400]}")

    # with detail: the whole cost, and the features that turn the inputs on
    readers = [s["shader"] for s in shaders if s["scene_inputs"]]
    for reference in readers:
        detail = (await ed.call("list_shaders", name=reference))["shaders"][0]
        note = detail["cost_note"]
        ed.check("Avoid on low-end mobile" in note and "transparent queue" in note, f"{reference} whole cost: {note!r}")
        if "scenecolor" in detail["scene_inputs"]:
            ed.check("scene_color_scale" in note, f"{reference}: names the copy's resolution setting: {note!r}")
        made = set(detail["require"]) & {"scenedepth", "scenecolor"}
        for feature in detail["features"]:
            made |= set(feature.get("scene_inputs", [])) | set(feature.get("scene_inputs_when_off", []))
        for group in detail["keywords"]:
            for names in group.get("scene_inputs", {}).values():
                made |= set(names)
        ed.check_eq(sorted(made), sorted(detail["scene_inputs"]), f"{reference}: require and features make up scene_inputs")
    # a shader reading nothing has no feature marks
    toon = (await ed.call("list_shaders", name="toon"))["shaders"][0]
    ed.check(not toon["scene_inputs"] and "cost_note" not in toon, f"toon: {toon!r}")
    ed.check(all("scene_inputs" not in f for f in toon["features"]), f"toon features unmarked: {toon['features']!r}")
    # one reading through a feature marks only that feature (unlit SOFT, phase 3)
    unlit = (await ed.call("list_shaders", name="unlit"))["shaders"][0]
    ed.check_eq(unlit["scene_inputs"], ["scenedepth"], "unlit may read the scene depth")
    marked = {f["define"]: f["scene_inputs"] for f in unlit["features"] if "scene_inputs" in f}
    ed.check_eq(marked, {"SOFT": ["scenedepth"]}, "only unlit's SOFT is marked")


async def test_material_variant(ed: Editor):
    eid = await make_entity(ed, "Soft editor box")
    inputs, data, _ = await entity_inputs(ed, eid)
    ed.check(inputs == [] and "scene_inputs_cost" not in data, f"the built-in Lit reads nothing: {data!r}")

    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "shaders/soft_editor"}})
    inputs, data, text = await entity_inputs(ed, eid)
    ed.check_eq(len(inputs), 1, f"one reader: {inputs!r}")
    entry = inputs[0]
    ed.check_eq(entry["submesh"], 0)
    ed.check_eq(entry["reads"], ["scenedepth"], "without TINT only the depth")
    ed.check_eq(entry["queue"], "transparent", "its state blends")
    ed.check("error" not in entry and "fix" not in entry, f"no error: {entry!r}")
    cost = data["scene_inputs_cost"]
    ed.check("Scene depth:" in cost and "Scene colour:" not in cost, f"the depth's cost only: {cost!r}")
    ed.check("Reads the scene depth (a depth pre-pass per camera)" in text, f"said in the text: {text!r}")

    u = await ed.call("get_shader_uniforms", entity=eid, submesh=0)
    ed.check_eq(u["scene_inputs"], ["scenedepth"], f"get_shader_uniforms: {u!r}")
    ed.check("Scene depth:" in u["cost_note"], f"cost_note: {u['cost_note']!r}")

    # TINT is the variant reading the colour too
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shaderFeatures": ["TINT"]}})
    inputs, data, _ = await entity_inputs(ed, eid)
    ed.check_eq(inputs[0]["reads"], ["scenedepth", "scenecolor"], "with TINT both")
    ed.check("Scene colour:" in data["scene_inputs_cost"] and "scene_color_scale" in data["scene_inputs_cost"],
             f"the copy's cost and its setting: {data['scene_inputs_cost']!r}")
    u = await ed.call("get_shader_uniforms", entity=eid, submesh=0)
    ed.check_eq(u["scene_inputs"], ["scenedepth", "scenecolor"], "get_shader_uniforms follows the variant")


async def test_opaque_queue_fix_and_undo(ed: Editor):
    eid = await make_entity(ed, "Soft editor opaque")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "shaders/soft_editor", M + "blendMode": "opaque"}})
    inputs, _, text = await entity_inputs(ed, eid)
    entry = inputs[0]
    ed.check_eq(entry["queue"], "opaque")
    ed.check("opaque queue" in entry.get("error", ""), f"the error: {entry!r}")
    ed.check_eq(entry.get("fix"), {M + "blendMode": "alpha"}, "the inspector button's fix")
    ed.check("ERROR" in text, f"the text says it: {text!r}")

    # the fix, as the button applies it: one property command on blendMode
    await ed.call("set_properties", entity=eid, values={"mesh": entry["fix"]})
    inputs, _, _ = await entity_inputs(ed, eid)
    ed.check_eq(inputs[0]["queue"], "transparent", "fixed")
    ed.check("error" not in inputs[0] and "fix" not in inputs[0], f"no error left: {inputs[0]!r}")
    r = await ed.call("check_shader", timeout=300)
    ed.check(not [f for f in r.get("findings", []) if "Soft editor opaque" in f.get("message", "")], f"checks agree: {r!r}")

    await ed.call("undo")
    inputs, _, _ = await entity_inputs(ed, eid)
    ed.check_eq(inputs[0]["queue"], "opaque", "undo puts it back")
    ed.check("fix" in inputs[0], f"the fix again: {inputs[0]!r}")
    await ed.call("delete_entities", entities=[eid])


async def test_extra_pass_reader(ed: Editor):
    # the pass draws at its opaque submesh's queue position: the submesh's blend is the fix
    eid = await make_entity(ed, "Soft editor pass")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "extraPass": {"shader": "shaders/soft_editor"}}})
    inputs, _, _ = await entity_inputs(ed, eid)
    entry = inputs[0]
    ed.check_eq(entry["reads"], [], "the built-in Lit reads nothing")
    ed.check_eq(entry["extra_pass_reads"], ["scenedepth"], "its pass reads the depth")
    ed.check_eq(entry["queue"], "opaque")
    ed.check_eq(entry.get("fix"), {M + "blendMode": "alpha"}, "fixed through the submesh's blend")
    await ed.call("set_properties", entity=eid, values={"mesh": entry["fix"]})
    inputs, _, _ = await entity_inputs(ed, eid)
    ed.check_eq(inputs[0]["queue"], "transparent")
    await ed.call("delete_entities", entities=[eid])


async def test_points_reader(ed: Editor):
    eid = await make_entity(ed, "Soft editor points", "points")
    inputs, _, _ = await entity_inputs(ed, eid)
    ed.check_eq(inputs, [], "the built-in points shader reads nothing")
    await ed.call("set_properties", entity=eid, values={"points": {"customShader": "shaders/soft_points_editor"}})
    inputs, data, _ = await entity_inputs(ed, eid)
    ed.check_eq(inputs, [{"component": "points", "reads": ["scenedepth"]}], f"points: {inputs!r}")
    ed.check("Scene depth:" in data["scene_inputs_cost"], "its cost")
    u = await ed.call("get_shader_uniforms", entity=eid)
    ed.check_eq(u.get("scene_inputs"), ["scenedepth"], f"get_shader_uniforms of the component: {u!r}")
    await ed.call("delete_entities", entities=[eid])
