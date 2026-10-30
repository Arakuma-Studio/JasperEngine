"""Scene-reading shader manifests and queue validation on an owned blank project."""

from __future__ import annotations


from harness import Editor, find_key
from shader_project import scene as project_scene

CI = True

EDITOR = {"create": True}
TIMEOUT = 900
M = "submeshes[0].material."
P = M + "extraPass."

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


async def test_fork_compiles(ed: Editor):
    async with project_scene(ed, "test_fork_compiles", with_texture=False) as SCENE:
        await write_fork(ed, "shaders/soft", FRAG)
        for ref in ("shaders/soft", "shaders/soft#TINT"):
            ok = await ed.call("check_shader", shader=ref, shader_type="mesh", timeout=300)
            ed.check_eq(ok["failed"], 0, f"{ref} compiles: {ok!r}")
            ed.check(not [m for m in messages(ok) if "manifest" in m], f"{ref}: no manifest problem: {ok!r}")
        broken = FRAG.replace("void main()", "void main()\n{ unknown_shader_symbol = 1.0; }\nvoid unused()", 1)
        result = await ed.call("write_file", path="shaders/soft.frag", content=broken)
        checks = result.get("shader_check", [])
        ed.check(checks and any(not check["ok"] and "unknown_shader_symbol" in check.get("log", "") for check in checks),
                 "editing a GLSL error reports its actual compiler diagnostic")
        await ed.call("undo", scope="project")
        restored = await ed.call("check_shader", shader="shaders/soft", shader_type="mesh")
        ed.check_eq(restored["failed"], 0, "undo restores a compiling shader")


async def test_manifest_rules(ed: Editor):
    async with project_scene(ed, "test_manifest_rules", with_texture=False) as SCENE:
        # a UI shader cannot read the scene
        ui = FRAG.replace("// @shader mesh", "// @shader ui")
        await write_fork(ed, "shaders/soft_ui", ui)
        r = await ed.call("check_shader", shader="shaders/soft_ui", shader_type="ui", timeout=300)
        ed.check(any("needs @shader mesh or points" in m for m in messages(r)), f"ui refused: {r!r}")

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
    async with project_scene(ed, "test_opaque_material_is_a_check_error", with_texture=False) as SCENE:
        await write_fork(ed, "shaders/soft", FRAG)
        eid = await make_entity(ed, "Soft box")
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "shader": "shaders/soft"}})
        r = await ed.call("check_shader", timeout=300)
        ed.check(not [m for m in messages(r) if "opaque queue" in m], f"blended: no error: {r!r}")

        await ed.call("set_properties", entity=eid, values={"mesh": {M + "blendMode": "opaque"}})
        r = await ed.call("check_shader", timeout=300)
        errors = [f for f in r.get("findings", []) if "opaque queue" in f.get("message", "")]
        ed.check(errors and errors[0].get("severity") == "error", f"opaque material reported: {r!r}")
        # with TINT it reads the colour too
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "shaderFeatures": ["TINT"]}})
        r = await ed.call("check_shader", timeout=300)
        errors = [f for f in r.get("findings", []) if "opaque queue" in f.get("message", "")]
        ed.check(errors and errors[0]["severity"] == "error", f"colour reader remains invalid in the opaque queue: {r!r}")

        await ed.call("set_properties", entity=eid, values={"mesh": {M + "blendMode": "auto"}})
        r = await ed.call("check_shader", timeout=300)
        ed.check(not [m for m in messages(r) if "opaque queue" in m], f"back to its state: no error: {r!r}")
