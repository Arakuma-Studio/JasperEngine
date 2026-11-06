"""The scene's default shader property values (docs/superpowers/specs/2026-11-04-code-editor-git-design.md
6.8, AC11, AC12, AC14): set_shader_uniform with shader_type sets a value of the scene's default shader;
get_shader_uniforms reports it on every object drawing with that default (source scene_default) and not on
one with a shader of its own; an object's own value overrides it; set_scene_settings edits the whole map
(null removes); each edit is one undo step; the values survive a save and reopen.
"""

from __future__ import annotations

from harness import Editor, find_key
from shader_project import scene as project_scene

EDITOR = {"create": True}
TIMEOUT = 300
CI = True

UNLIT = "library:unlit"
RED = [1, 0, 0, 1]


async def make_box(ed: Editor, name: str):
    made = await ed.call("create_entity", name=name, type="box")
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


def parameter(got: dict, member: str) -> dict:
    for entry in got.get("parameters", []):
        if entry.get("member") == member or entry.get("name") == member:
            return entry
    return {}


async def test_scene_default_values(ed: Editor):
    async with project_scene(ed, "test_scene_default_values", with_texture=False) as path:
        inheriting = await make_box(ed, "Inheriting")
        own = await make_box(ed, "OwnShader")

        # before a default shader: kept, with a note
        early = await ed.call_raw("set_shader_uniform", {"shader_type": "mesh", "name": "baseColor", "value": RED})
        ed.check(not early.is_error and "no default mesh shader" in early.text, f"{early.text}")
        await ed.call("set_shader_uniform", shader_type="mesh", name="baseColor", remove=True)

        await ed.call("set_scene_settings", settings={"default_mesh_shader": UNLIT})
        set_ = await ed.call("set_shader_uniform", shader_type="mesh", name="baseColor", value="#FF0000")
        ed.check_eq(set_.get("property"), "default_mesh_shader_uniforms", f"{set_}")
        users = set_.get("used_by")
        ed.check(isinstance(users, int) and users >= 2, f"both boxes draw with the default: {set_}")
        # a shader of its own: that box no longer draws with the default
        await ed.call("set_properties", entity=own, values={"mesh": {"customShader": UNLIT}})
        tree = await ed.call("get_scene_tree")
        ed.check_eq((await ed.call("get_shader_uniforms", shader_type="mesh")).get("used_by"), users - 1, f"{tree}")

        got = await ed.call("get_shader_uniforms", entity=inheriting)
        entry = parameter(got, "baseColor")
        ed.check_eq(entry.get("source"), "scene_default", f"inherited: {entry}")
        ed.check_eq(entry.get("effective"), RED, f"{entry}")
        other = parameter(await ed.call("get_shader_uniforms", entity=own), "baseColor")
        ed.check_eq(other.get("source"), "shader_default", f"its own shader inherits nothing: {other}")

        scene_default = await ed.call("get_shader_uniforms", shader_type="mesh")
        ed.check_eq(scene_default.get("default_shader"), UNLIT, f"{scene_default}")
        ed.check_eq(scene_default.get("values"), {"baseColor": RED}, f"{scene_default}")
        ed.check_eq(scene_default.get("used_by"), users - 1, f"{scene_default}")
        settings = (await ed.call("get_scene_settings", names=["default_mesh_shader_uniforms"]))["settings"]
        ed.check_eq(settings.get("default_mesh_shader_uniforms"), {"baseColor": RED}, f"{settings}")

        # a library shader refuses a member it does not declare
        await ed.check_error("set_shader_uniform", shader_type="mesh", name="nope", value=1, contains="has no parameter")
        await ed.check_error("set_shader_uniform", shader_type="depth", name="baseColor", value=1)

        # the object's own value wins
        await ed.call("set_shader_uniform", entity=inheriting, name="baseColor", value=[0, 1, 0, 1])
        entry = parameter(await ed.call("get_shader_uniforms", entity=inheriting), "baseColor")
        ed.check_eq((entry.get("source"), entry.get("effective")), ("own", [0, 1, 0, 1]), f"{entry}")
        await ed.call("set_shader_uniform", entity=inheriting, name="baseColor", remove=True)

        # the whole map through the scene settings; undo brings the value back
        await ed.call("set_scene_settings", settings={"default_mesh_shader_uniforms": {"baseColor": None}})
        cleared = await ed.call("get_shader_uniforms", shader_type="mesh")
        ed.check_eq(cleared.get("values"), {}, f"{cleared}")
        await ed.call("undo", scope="scene")
        restored = await ed.call("get_shader_uniforms", shader_type="mesh")
        ed.check_eq(restored.get("values"), {"baseColor": RED}, f"undo: {restored}")

        # saved and opened again
        await ed.call("save_scene", scene=path)
        await ed.call("close_scene", scene=path)
        await ed.call("open_scene", scene=path)
        reopened = await ed.call("get_shader_uniforms", shader_type="mesh", scene=path)
        ed.check_eq(reopened.get("values"), {"baseColor": RED}, f"after a reopen: {reopened}")
