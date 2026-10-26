"""Pose animation templates (spec: pose animation phase 6, sections 2, 3, 5 and 7) on the pose
fixture project (tools/fixtures/pose-animation: a T-pose hero with Mixamo names, an A-pose robot
with Blender names).

create: {template} chooses Walk on the hero and Jump on the robot; the C++ template suite owns
the complete library's content and rig compatibility. save_template writes a project template
(one undo step) that the list shows
as user and that shadows a built-in of the same name -> apply_template replace / append and their
project undo -> argument errors.
"""

from __future__ import annotations

import json

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

HERO = "assets/models/hero_tpose.gltf"
ROBOT = "assets/models/robot_apose.gltf"


def anim_path(name: str, rig: str) -> str:
    return f"assets/anim/{name.lower().replace(' ', '_')}_{rig}.poseanim"


def file_json(ed: Editor, rel: str) -> dict:
    return json.loads((ed.project / rel).read_text(encoding="utf-8"))


async def templates(ed: Editor) -> list:
    return (await ed.call("get_pose_animation", templates=True))["templates"]


async def undo_project(ed: Editor, steps: int = 1) -> dict:
    return await ed.call("undo", scope="project", steps=steps)


async def test_create_templates_on_both_rigs(ed: Editor):
    # Template contents and rig-wide limit/contact guarantees belong to PoseTemplatesTest.cpp.
    # Here two distinct templates check the MCP creation path and model selection.
    for name, rig, model, count, loop in (("Walk", "hero", HERO, 4, True),
                                       ("Jump", "robot", ROBOT, 6, False)):
        path = anim_path(name, rig)
        await ed.call("edit_pose_animation", path=path, create={"template": name, "preview_model": model})
        data = file_json(ed, path)
        ed.check_eq(len(data["poses"]), count, f"{name} creates the template's poses")
        ed.check_eq(data["loop"], loop, f"{name} loop")
        ed.check_eq(data["preview_model"], model.removeprefix("assets/"), "the selected model is persisted")
        ed.check("template" not in data, "the created asset is an animation")
        info = await ed.call("get_pose_animation", path=path)
        ed.check_eq(info["rig"]["missing"], [], f"{name} on {rig}: a usable rig")


async def test_save_template_lists_as_user_and_undoes(ed: Editor):
    src = anim_path("Wave", "hero")
    await ed.call("edit_pose_animation", path=src, create={"template": "Wave", "preview_model": HERO})
    builtin_names = {t["name"] for t in await templates(ed)}
    before = file_json(ed, src)
    r = await ed.call("edit_pose_animation", path=src, ops=[
        {"op": "update_pose", "index": 0, "name": "Hello"},
        {"op": "save_template", "name": "Greeting", "description": "Hello there", "category": "Mine"},
    ])
    saved = ed.project / "pose-templates" / "Greeting.poseanim"
    ed.check(saved.is_file(), "the project template file")
    data = json.loads(saved.read_text(encoding="utf-8"))
    ed.check_eq(data["template"], {"name": "Greeting", "description": "Hello there", "category": "Mine"}, "template object")
    ed.check_eq(data["preview_model"], "", "no preview model in a template")
    ed.check_eq(data["poses"][0]["name"], "Hello", "saved after the earlier ops of the call")
    listed = await templates(ed)
    ed.check_eq({t["name"] for t in listed}, builtin_names | {"Greeting"}, "the user template joins the built-ins")
    ed.check_eq((listed[-1]["name"], listed[-1]["user"], listed[-1]["category"]), ("Greeting", True, "Mine"), "user last")
    # the edit and the template are one undo step
    await undo_project(ed)
    ed.check(not saved.exists(), "undo removes the template")
    ed.check(not (ed.project / "pose-templates").exists(), "and the folder it created")
    ed.check_eq(file_json(ed, src), before, "and the edit")
    ed.check_eq({t["name"] for t in await templates(ed)}, builtin_names, "undo restores the built-ins")
    await ed.call("redo", scope="project")
    ed.check(saved.is_file(), "redo")

    # a project template shadows the built-in of the same name; create uses it
    await ed.call("edit_pose_animation", path=anim_path("Bow", "hero"), create={"template": "Bow", "preview_model": HERO})
    await ed.call("edit_pose_animation", path=anim_path("Bow", "hero"), ops=[
        {"op": "save_template", "name": "walk", "description": "my walk"}])
    listed = await templates(ed)
    walks = [t for t in listed if t["name"].lower() == "walk"]
    ed.check_eq(len(walks), 1, f"the built-in Walk is shadowed: {walks}")
    ed.check(walks[0]["user"] and walks[0]["poses"] == 3 and walks[0]["category"] == "User", walks)
    r = await ed.call("edit_pose_animation", path="assets/anim/my_walk.poseanim", create={"template": "Walk", "preview_model": HERO})
    ed.check_eq(r["pose_count"], 3, "create takes the project template")
    await undo_project(ed, 2)
    listed = await templates(ed)
    ed.check(any(t["name"] == "Walk" and not t["user"] for t in listed), "the built-in Walk is back")


async def test_apply_template_replace_append_and_undo(ed: Editor):
    path = anim_path("Punch", "hero")
    await ed.call("edit_pose_animation", path=path, create={"template": "Punch", "preview_model": HERO})
    before = file_json(ed, path)
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "apply_template", "name": "Wave", "mode": "append"}])
    ed.check_eq(r["pose_count"], 6, "4 + 2 poses")
    data = file_json(ed, path)
    ed.check_eq([p["name"] for p in data["poses"]][4:], ["Hand Left", "Hand Right"], "appended at the end")
    ed.check_eq(data["feel"], before["feel"], "append keeps the settings")
    ed.check_eq(data["loop"], False, "append keeps loop")
    ed.check_eq([p["index"] for p in r["poses"]], [4, 5], "the appended poses are touched")

    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "apply_template", "name": "sit down"}])
    ed.check_eq(r["pose_count"], 4, "replace (the default mode)")
    data = file_json(ed, path)
    ed.check_eq([p["name"] for p in data["poses"]], ["Stand", "Lower", "Sit", "Settle"], "Sit Down's poses")
    ed.check_eq(data["preview_model"], before["preview_model"], "replace keeps the preview model")
    ed.check_eq(r.get("preset"), "Heavy", "and takes the template's feel")

    await undo_project(ed)
    ed.check_eq(len(file_json(ed, path)["poses"]), 6, "undo the replace")
    await undo_project(ed)
    ed.check_eq(file_json(ed, path), before, "undo the append")

    # a template's own transitions (Jump's moves into floor contacts) are not reported as stale
    # leftovers when the template changes the animation's feel
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "apply_template", "name": "Jump"}])
    ed.check(not any("own transition" in w for w in r["warnings"]), f"no own-transition warning: {r['warnings']}")
    ed.check_eq(r.get("preset"), "Rubbery", "Jump's feel")
    r = await ed.call("edit_pose_animation", path="assets/anim/jump_snappy.poseanim",
                      create={"template": "Jump", "preview_model": HERO}, ops=[{"op": "apply_preset", "name": "Snappy"}])
    ed.check(not any("own transition" in w for w in r["warnings"]), f"created from the template: {r['warnings']}")
    r = await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "update_pose", "index": 2, "transition": {"duration": 0.3}},
        {"op": "apply_preset", "name": "Heavy"}])
    ed.check(any("own transition" in w for w in r["warnings"]), f"transitions of an animation edited later are: {r['warnings']}")
    await undo_project(ed, 3)   # the preset edit, the jump_snappy creation, the Jump replace
    ed.check_eq(file_json(ed, path), before, "undone")


async def test_template_errors(ed: Editor):
    path = anim_path("Bow", "hero")
    msg = await ed.check_error("edit_pose_animation", path=path, ops=[{"op": "apply_template", "name": "Moonwalk"}])
    ed.check("no pose template named 'Moonwalk'" in msg and "Cartoon Walk" in msg, msg)
    msg = await ed.check_error("edit_pose_animation", path=path, ops=[{"op": "apply_template", "name": "Walk", "mode": "mix"}])
    ed.check("mode" in msg, msg)
    msg = await ed.check_error("edit_pose_animation", path=path, ops=[{"op": "apply_template", "name": "Walk", "mode": 1}])
    ed.check("mode" in msg, msg)
    msg = await ed.check_error("edit_pose_animation", path=path, ops=[{"op": "save_template", "name": "  "}])
    ed.check("empty" in msg, msg)
    msg = await ed.check_error("edit_pose_animation", path=path, ops=[
        {"op": "save_template", "name": "Twice"}, {"op": "update_pose", "index": 0, "hold": 0.3},
        {"op": "save_template", "name": "twice"}])
    ed.check("ops[2].name" in msg and "already saves" in msg, msg)
    ed.check(not (ed.project / "pose-templates" / "Twice.poseanim").exists(), "nothing written")
    msg = await ed.check_error("edit_pose_animation", path="assets/anim/x.poseanim", create={"template": "Nope"})
    ed.check("create.template" in msg, msg)
    ed.check(not (ed.project / "assets/anim/x.poseanim").exists(), "nothing created")
    msg = await ed.check_error("get_pose_animation", path=path, templates=True)
    ed.check("leave out path" in msg, msg)
    msg = await ed.check_error("get_pose_animation")
    ed.check("path is required" in msg, msg)
