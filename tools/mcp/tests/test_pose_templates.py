"""Pose animation templates (spec: pose animation phase 6, sections 2, 3, 5 and 7) on the pose
fixture project (tools/fixtures/pose-animation: a T-pose hero with Mixamo names, an A-pose robot
with Blender names).

get_pose_animation {templates: true} lists the 10 built-ins -> create: {template} from each
built-in on the hero and on the robot: every pose of Walk has a foot in contact, Idle is balanced,
Jump's apex has no contact, no template hits a joint limit on either rig -> a sheet per template
renders (not flat) -> save_template writes a project template (one undo step) that the list shows
as user and that shadows a built-in of the same name -> apply_template replace / append and their
project undo -> argument errors.
"""

from __future__ import annotations

import json

import harness
from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

HERO = "assets/models/hero_tpose.gltf"
ROBOT = "assets/models/robot_apose.gltf"
BUILTINS = {
    "Idle": ("Basics", 2, True),
    "Walk": ("Locomotion", 4, True),
    "Run": ("Locomotion", 4, True),
    "Sneak": ("Locomotion", 4, True),
    "Cartoon Walk": ("Locomotion", 8, True),
    "Jump": ("Actions", 6, False),
    "Wave": ("Gestures", 2, True),
    "Sit Down": ("Actions", 4, False),
    "Punch": ("Actions", 4, False),
    "Bow": ("Gestures", 3, False),
}


def anim_path(name: str, rig: str) -> str:
    return f"assets/anim/{name.lower().replace(' ', '_')}_{rig}.poseanim"


def file_json(ed: Editor, rel: str) -> dict:
    return json.loads((ed.project / rel).read_text(encoding="utf-8"))


async def templates(ed: Editor) -> list:
    return (await ed.call("get_pose_animation", templates=True))["templates"]


async def undo_project(ed: Editor, steps: int = 1) -> dict:
    return await ed.call("undo", scope="project", steps=steps)


async def test_tools_describe_templates(ed: Editor):
    get = ed.tool("get_pose_animation")
    edit = ed.tool("edit_pose_animation")
    ed.check("templates" in get.input_schema["properties"], "get_pose_animation has templates")
    ed.check("path" not in get.input_schema.get("required", []), "path is optional (templates: true has none)")
    ops = edit.input_schema["properties"]["ops"]["items"]["properties"]
    ed.check_in("apply_template", ops["op"]["enum"], "ops enum")
    ed.check_in("save_template", ops["op"]["enum"], "ops enum")
    ed.check_eq(ops["mode"]["enum"], ["replace", "append"], "apply_template modes")
    ed.check("template" in edit.input_schema["properties"]["create"]["properties"], "create.template")


async def test_list_builtins(ed: Editor):
    raw = await ed.call_raw("get_pose_animation", {"templates": True})
    ed.check(not raw.is_error, raw.text)
    listed = raw.structured["templates"]
    ed.check_eq(len(listed), len(BUILTINS), f"10 built-ins: {[t['name'] for t in listed]}")
    for t in listed:
        ed.check_in(t["name"], BUILTINS, "a known template")
        category, poses, loop = BUILTINS[t["name"]]
        ed.check_eq((t["category"], t["poses"], t["loop"], t["user"]), (category, poses, loop, False), t["name"])
        ed.check(t["description"], f"{t['name']} has a description")
    # sorted by category, then name
    keys = [(t["category"].lower(), t["name"].lower()) for t in listed]
    ed.check_eq(keys, sorted(keys), "category, then name")
    ed.check("Cartoon Walk (Locomotion, 8 poses, loop)" in raw.text, raw.text)
    ed.check(not (ed.project / "pose-templates").exists(), "listing creates no folder")


async def test_create_from_every_builtin_on_both_rigs(ed: Editor):
    for name, (_, poses, loop) in BUILTINS.items():
        for rig, model in (("hero", HERO), ("robot", ROBOT)):
            path = anim_path(name, rig)
            r = await ed.call("edit_pose_animation", path=path, create={"template": name, "preview_model": model})
            ed.check_eq(r["pose_count"], poses, f"{name} on {rig}")
            # the result shows the template's poses with their metrics (the call made them all)
            ed.check_eq([p["index"] for p in r["poses"]], list(range(poses)), f"{name}: every pose in the result")
            ed.check(all("metrics" in p for p in r["poses"]), f"{name}: with metrics")
            ed.check("preset" in r, f"{name}: preset always present: {sorted(r)}")
            ed.check(any(f'from the template "{name}"' in c for c in r["changes"]), r["changes"])
            ed.check_eq(r["clamped"], [], f"{name}: nothing clamped")
            data = file_json(ed, path)
            ed.check("template" not in data, "an animation made from a template is not a template")
            ed.check_eq(data["loop"], loop, f"{name} loop")
            info = await ed.call("get_pose_animation", path=path)
            ed.check_eq(info["rig"]["missing"], [], f"{name} on {rig}: a full rig")
            ed.check("warnings" not in info, f"{name}: loads without warnings")
            for pose in info["poses"]:
                m = pose["metrics"]
                label = f"{name} on {rig}, pose {pose['index']} {pose['name']}"
                ed.check_eq(m["limit_hits"], [], label)
                # standing poses are balanced on their feet; the others are intentionally lifted
                # (airborne, or sitting on a seat)
                if pose["grounded"]:
                    ed.check(m["feet"]["left"]["contact"] or m["feet"]["right"]["contact"], f"{label}: a foot on the floor")
                    ed.check(m["balanced"], f"{label}: balanced ({m['balance_margin']})")
            if name == "Walk":
                for pose in info["poses"]:
                    ed.check(pose["metrics"]["feet"]["left"]["contact"] or pose["metrics"]["feet"]["right"]["contact"],
                             f"Walk on {rig}: every pose has a contact")
            if name == "Idle":
                ed.check(all(p["metrics"]["balanced"] for p in info["poses"]), f"Idle on {rig} is balanced")
                ed.check(info["humanize"]["enabled"], "Idle humanizes")
            if name == "Jump":
                apex = next(p for p in info["poses"] if p["name"] == "Apex")
                feet = apex["metrics"]["feet"]
                ed.check(not feet["left"]["contact"] and not feet["right"]["contact"], f"Jump apex on {rig} is airborne")
                ed.check(not apex["grounded"] and apex["lift"] > 0.2, "the apex is lifted")
            if name == "Run":
                flights = [p for p in info["poses"] if p["name"].startswith("Flight")]
                ed.check(flights and all(not p["grounded"] for p in flights), "Run's flight poses are lifted")


async def test_a_sheet_per_template(ed: Editor):
    for name in BUILTINS:
        r = await ed.call_raw("render_pose_animation", {"path": anim_path(name, "hero"), "cell": [96, 128]})
        ed.check(not r.is_error, f"{name}: {r.text}")
        width, height, ch, rows = decode_png(r.images[0][0])
        colours = {bytes(rows[y][x * ch:x * ch + 3]) for y in range(0, height, 3) for x in range(0, width, 3)}
        ed.check(len(colours) > 40, f"{name}: the sheet is not flat ({len(colours)} colours)")
        ed.check_eq(len(r.structured["columns"]), BUILTINS[name][1], f"{name}: one column per pose")
    r = await ed.call_raw("render_pose_animation", {"path": anim_path("Walk", "robot"), "mode": "motion", "frames": 6})
    ed.check(not r.is_error, r.text)


async def test_save_template_lists_as_user_and_undoes(ed: Editor):
    src = anim_path("Wave", "hero")
    before = file_json(ed, src)
    r = await ed.call("edit_pose_animation", path=src, ops=[
        {"op": "update_pose", "index": 0, "name": "Hello"},
        {"op": "save_template", "name": "Greeting", "description": "Hello there", "category": "Mine"},
    ])
    ed.check(any("save_template \"Greeting\"" in c for c in r["changes"]), r["changes"])
    saved = ed.project / "pose-templates" / "Greeting.poseanim"
    ed.check(saved.is_file(), "the project template file")
    data = json.loads(saved.read_text(encoding="utf-8"))
    ed.check_eq(data["template"], {"name": "Greeting", "description": "Hello there", "category": "Mine"}, "template object")
    ed.check_eq(data["preview_model"], "", "no preview model in a template")
    ed.check_eq(data["poses"][0]["name"], "Hello", "saved after the earlier ops of the call")
    listed = await templates(ed)
    ed.check_eq(len(listed), len(BUILTINS) + 1, "one more")
    ed.check_eq((listed[-1]["name"], listed[-1]["user"], listed[-1]["category"]), ("Greeting", True, "Mine"), "user last")
    # the edit and the template are one undo step
    await undo_project(ed)
    ed.check(not saved.exists(), "undo removes the template")
    ed.check(not (ed.project / "pose-templates").exists(), "and the folder it created")
    ed.check_eq(file_json(ed, src), before, "and the edit")
    ed.check_eq(len(await templates(ed)), len(BUILTINS), "back to the built-ins")
    await ed.call("redo", scope="project")
    ed.check(saved.is_file(), "redo")

    # a project template shadows the built-in of the same name; create uses it
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
