"""Pose animation MCP tools (spec: pose animation phase 2, sections 5, 6 and 8): get_pose_animation
and edit_pose_animation on the pose fixture project (tools/fixtures/pose-animation: a T-pose hero
with Mixamo names, an A-pose robot with Blender names).

create -> add poses -> get (angles, timing) -> metadata updates keep the angles -> move/delete ->
set_settings clamping echoed -> one project undo step per call and rollback on a failing op ->
argument errors naming the op (DOF suggestions) -> set_humanoid writes the sidecar -> the document
follows edits made on disk.
"""

from __future__ import annotations

import json

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 600

HERO = "assets/models/hero_tpose.gltf"
ROBOT = "assets/models/robot_apose.gltf"
WALK = "assets/anim/walk.poseanim"
TOOLS = ("get_pose_animation", "edit_pose_animation")


def file_json(ed: Editor, rel: str) -> dict:
    return json.loads((ed.project / rel).read_text(encoding="utf-8"))


def file_text(ed: Editor, rel: str) -> str:
    return (ed.project / rel).read_text(encoding="utf-8")


def approx(a, b, tol=1e-3) -> bool:
    return abs(float(a) - float(b)) <= tol


async def undo_project(ed: Editor, steps: int = 1) -> dict:
    return await ed.call("undo", scope="project", steps=steps)


async def redo_project(ed: Editor, steps: int = 1) -> dict:
    return await ed.call("redo", scope="project", steps=steps)


# -- registry ------------------------------------------------------------------

async def test_tools_listed(ed: Editor):
    ed.require_tools(*TOOLS)
    get = ed.tool("get_pose_animation")
    edit = ed.tool("edit_pose_animation")
    ed.check(get.annotations.read_only_hint, "get_pose_animation is read-only")
    ed.check(not edit.annotations.read_only_hint, "edit_pose_animation edits")
    ed.check("jasper://docs/pose-animation" in get.description, "get_pose_animation points to the DOF table")
    ops = edit.input_schema["properties"]["ops"]["items"]["properties"]["op"]["enum"]
    for op in ("add_pose", "update_pose", "delete_pose", "move_pose", "mirror_pose", "set_settings", "apply_preset",
               "auto_timing", "set_preview_model", "set_humanoid"):
        ed.check_in(op, ops, "ops enum")


# -- create, add, get ----------------------------------------------------------

async def test_create_and_add_poses(ed: Editor):
    r = await ed.call("edit_pose_animation", path=WALK, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "Contact R", "hold": 0.1,
         "angles": {"right_upper_leg.forward": 25, "left_upper_leg.forward": -15, "left_upper_arm.raise": -80,
                    "right_upper_arm.raise": -80, "hips.lean": 0.03}},
        {"op": "add_pose", "name": "Down R", "add_angles": {"right_lower_leg.bend": 20}, "offset": {"forward": 0.02}},
    ])
    ed.check_eq(r["pose_count"], 2, "pose count")
    ed.check_eq(len(r["changes"]), 3, f"one change line per op plus the creation: {r['changes']}")
    ed.check_eq([p["index"] for p in r["poses"]], [0, 1], "both poses are touched")
    ed.check_eq(r["clamped"], [], "nothing clamped")
    data = file_json(ed, WALK)
    ed.check_eq(data["format"], "jasperengine.poseanim", "format")
    # stored relative to the assets folder, as model files are; tools show project-relative paths
    ed.check_eq(data["preview_model"], "models/hero_tpose.gltf", "preview_model in the file")
    ed.check_eq([p["name"] for p in data["poses"]], ["Contact R", "Down R"], "names")
    ed.check(approx(data["poses"][1]["angles"]["right_upper_leg.forward"], 25), "Down R copies Contact R")
    ed.check(approx(data["poses"][1]["angles"]["right_lower_leg.bend"], 20), "add_angles adds")
    ed.check(approx(data["poses"][1]["offset"]["forward"], 0.02), "offset")
    ed.check(file_text(ed, WALK).endswith("\n"), "the file ends with a newline")

    msg = await ed.check_error("edit_pose_animation", path=WALK, create={})
    ed.check("already exists" in msg, msg)


async def test_get_full_and_summary(ed: Editor):
    r = await ed.call("get_pose_animation", path=WALK)
    ed.check_eq(r["path"], WALK, "path")
    ed.check_eq(r["preview_model"], HERO, "preview_model project-relative")
    ed.check_eq(r["pose_count"], 2, "pose count")
    ed.check(r["loop"] is True and r["frame_rate"] == 30, f"defaults: {r}")
    # two holds of 0.1 and two 0.12 s transitions (looping: back to pose 0)
    ed.check(approx(r["length"], 0.44), f"length {r['length']}")
    ed.check_eq(r["frame_count"], 14, "frames f = 0..round(0.44 * 30)")
    p0, p1 = r["poses"]
    ed.check(approx(p0["start_time"], 0.0) and approx(p1["start_time"], 0.22), f"start times {p0['start_time']} {p1['start_time']}")
    ed.check_eq(p0["angles"]["right_upper_leg.forward"], 25.0, "angle")
    ed.check("hips.lean" not in p0["angles"], "angles below 0.05 degrees are left out")
    ed.check(p0["transition"] is None, "uses the animation's transition")
    ed.check_eq(r["feel"]["transition"], {"curve": "spring", "duration": 0.12, "overshoot": 0.25, "anticipation": 0.06},
                "feel transition")
    ed.check_eq(r["humanize"]["joint_limits"], True, "humanize")
    summary = await ed.call("get_pose_animation", path=WALK, detail="summary", poses=[1])
    ed.check_eq([p["index"] for p in summary["poses"]], [1], "poses filter")
    ed.check("angles" not in summary["poses"][0], "summary omits angles")
    raw = await ed.call_raw("get_pose_animation", {"path": WALK})
    lines = raw.text.splitlines()
    ed.check_eq(len(lines), 3, f"a header and one line per pose: {raw.text}")
    ed.check("Contact R" in lines[1] and "Down R" in lines[2], raw.text)

    await ed.check_error("get_pose_animation", path="assets/anim/missing.poseanim", contains="does not exist")
    await ed.check_error("get_pose_animation", path=WALK, poses=[5], contains="no pose 5")
    await ed.check_error("get_pose_animation", path="assets/models/hero_tpose.gltf", contains=".poseanim")


# -- metadata, order -----------------------------------------------------------

async def test_update_metadata_keeps_angles(ed: Editor):
    before = file_json(ed, WALK)["poses"][1]
    r = await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "update_pose", "index": 1, "name": "Down", "hold": 0.2, "transition": {"duration": 0.08}},
        {"op": "update_pose", "index": 1, "grounded": False, "lift": 0.1},
    ])
    after = file_json(ed, WALK)["poses"][1]
    ed.check_eq(after["angles"], before["angles"], "metadata edits never change angles")
    ed.check_eq(after["offset"], before["offset"], "metadata edits never change the offset")
    ed.check_eq(after["name"], "Down", "name")
    # a partial transition starts from the one the pose used (the animation's)
    ed.check_eq(after["transition"], {"curve": "spring", "duration": 0.08, "overshoot": 0.25, "anticipation": 0.06},
                "custom transition")
    ed.check(after["grounded"] is False and approx(after["lift"], 0.1), "lift")
    ed.check_eq([p["index"] for p in r["poses"]], [1], "touched pose")
    ed.check_eq(r["poses"][0]["transition"]["duration"], 0.08, "result shape: full pose")
    r = await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "update_pose", "index": 1, "transition": None}])
    ed.check(file_json(ed, WALK)["poses"][1]["transition"] is None, "null clears the custom transition")
    ed.check(r["poses"][0]["transition"] is None, "result")


async def test_move_and_delete(ed: Editor):
    await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "add_pose", "name": "Passing R", "after": -1, "from": 1, "blend_to": 0, "blend": 0.5},
    ])
    data = file_json(ed, WALK)
    ed.check_eq([p["name"] for p in data["poses"]], ["Passing R", "Contact R", "Down"], "inserted first")
    blended = data["poses"][0]["angles"]["right_lower_leg.bend"]
    ed.check(approx(blended, 10), f"blend lerps the angles halfway: {blended}")
    r = await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "move_pose", "index": 0, "to": 2}])
    ed.check_eq([p["name"] for p in file_json(ed, WALK)["poses"]], ["Contact R", "Down", "Passing R"], "moved")
    ed.check_eq([p["index"] for p in r["poses"]], [2], "the moved pose is touched")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "delete_pose", "index": 2}])
    ed.check_eq([p["name"] for p in file_json(ed, WALK)["poses"]], ["Contact R", "Down"], "deleted")


async def test_delete_last_pose_refused(ed: Editor):
    path = "assets/anim/one.poseanim"
    await ed.call("edit_pose_animation", path=path, create={}, ops=[{"op": "add_pose", "name": "Only"}])
    msg = await ed.check_error("edit_pose_animation", path=path, ops=[{"op": "delete_pose", "index": 0}])
    ed.check("ops[0]" in msg and "last pose" in msg, msg)
    r = await ed.call("get_pose_animation", path=path)
    ed.check_eq(r["pose_count"], 1, "still one pose")
    # one pose: static, its hold is the length
    ed.check(approx(r["length"], 0.1), f"length {r['length']}")


# -- settings and clamping -----------------------------------------------------

async def test_set_settings_clamping_echoed(ed: Editor):
    r = await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "set_settings", "loop": False, "frame_rate": 500, "transition": {"overshoot": 2},
         "step_rate": 12, "weights": {"legs": 3}, "humanize": {"enabled": True, "moving_holds": 0.9}},
        {"op": "update_pose", "index": 0, "hold": 9},
    ])
    clamped = {c["field"]: (c["requested"], c["applied"]) for c in r["clamped"]}
    ed.check_eq(clamped.get("frame_rate"), (500, 120), f"frame_rate clamp: {r['clamped']}")
    ed.check_eq(clamped.get("feel.transition.overshoot"), (2, 0.9), f"overshoot: {r['clamped']}")
    ed.check_eq(clamped.get("feel.weights.legs"), (3, 2), f"weights: {r['clamped']}")
    ed.check_eq(clamped.get("humanize.moving_holds"), (0.9, 0.5), f"moving holds: {r['clamped']}")
    ed.check_eq(clamped.get("poses[0].hold"), (9, 5), f"hold: {r['clamped']}")
    raw = await ed.call_raw("get_pose_animation", {"path": WALK, "detail": "summary"})
    got = raw.structured
    ed.check(got["loop"] is False and got["frame_rate"] == 120, f"applied: {got['loop']} {got['frame_rate']}")
    ed.check_eq(got["feel"]["transition"]["overshoot"], 0.9, "overshoot applied")
    ed.check_eq(got["feel"]["step_rate"], 12, "step rate")
    ed.check(got["humanize"]["enabled"] is True, "humanize on")
    # put the timing back for the later tests
    await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "set_settings", "loop": True, "frame_rate": 30, "transition": {"overshoot": 0.25}, "step_rate": 0,
         "weights": {"legs": 0}, "humanize": {"enabled": False, "moving_holds": 0.08}},
        {"op": "update_pose", "index": 0, "hold": 0.1},
    ])


# -- errors --------------------------------------------------------------------

async def test_errors_name_the_op(ed: Editor):
    msg = await ed.check_error("edit_pose_animation", path=WALK, ops=[
        {"op": "update_pose", "index": 0, "name": "x"},
        {"op": "update_pose", "index": 1, "name": "y"},
        {"op": "update_pose", "index": 0, "angles": {"left_elbow.bend": 30}},
    ])
    ed.check("ops[2].angles: unknown DOF 'left_elbow.bend' (did you mean 'left_lower_arm.bend'?)" in msg, msg)
    ed.check_eq([p["name"] for p in file_json(ed, WALK)["poses"]], ["Contact R", "Down"], "all or nothing")
    cases = [
        ({"op": "update_pose", "index": 0, "angles": {"right_knee.bend": 10}}, "right_lower_leg.bend"),
        ({"op": "update_pose", "index": 0, "angles": {"left_upper_arm.rais": 10}}, "left_upper_arm.raise"),
        ({"op": "update_pose", "index": 0, "angles": {"left_upper_arm.raise": "up"}}, "must be a number"),
        ({"op": "update_pose", "index": 7}, "no pose 7"),
        ({"op": "update_pose", "index": 0, "colour": 1}, "unknown key 'colour'"),
        ({"op": "add_pos"}, "did you mean 'add_pose'"),
        ({"op": "add_pose", "blend": 0.5}, "needs blend_to"),
        ({"op": "set_settings", "transition": {"curve": "bouncy"}}, "spring, linear or smooth"),
        ({"op": "set_settings", "humanize": {"seed": -1}}, "seed"),
        ({"op": "set_preview_model", "model": "assets/models/nope.gltf"}, "does not exist"),
        ({"op": "set_humanoid", "model": HERO, "bones": {"left_elbow": "x"}}, "did you mean 'left_lower_arm'"),
    ]
    for op, needle in cases:
        msg = await ed.check_error("edit_pose_animation", path=WALK, ops=[op])
        ed.check(needle in msg and "ops[0]" in msg, f"{op}: {msg}")
    await ed.check_error("edit_pose_animation", path=WALK, contains="Nothing to do")


# -- undo ----------------------------------------------------------------------

async def test_one_undo_step_per_call(ed: Editor):
    before = file_text(ed, WALK)
    await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "add_pose", "name": "Up R", "add_angles": {"right_upper_leg.forward": -10}},
        {"op": "update_pose", "index": 0, "hold": 0.15},
        {"op": "set_settings", "overlap": 0.1},
    ])
    after = file_text(ed, WALK)
    ed.check(after != before, "edited")
    await undo_project(ed)
    ed.check_eq(file_text(ed, WALK), before, "one undo restores the file before the call")
    r = await ed.call("get_pose_animation", path=WALK, detail="summary")
    ed.check_eq(r["pose_count"], 2, "the document follows the undo")
    await redo_project(ed)
    ed.check_eq(file_text(ed, WALK), after, "redo")
    r = await ed.call("get_pose_animation", path=WALK, detail="summary")
    ed.check_eq(r["pose_count"], 3, "the document follows the redo")
    await undo_project(ed)
    ed.check_eq(file_text(ed, WALK), before, "back")


async def test_failed_call_writes_nothing(ed: Editor):
    before = file_text(ed, WALK)
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "update_pose", "index": 0, "hold": 0.12}])
    edited = file_text(ed, WALK)
    await ed.check_error("edit_pose_animation", path=WALK, ops=[
        {"op": "update_pose", "index": 0, "hold": 0.3},
        {"op": "delete_pose", "index": 9},
    ], contains="no pose 9")
    ed.check_eq(file_text(ed, WALK), edited, "a failing op writes nothing")
    # no undo step either: one undo goes back to before the successful call
    await undo_project(ed)
    ed.check_eq(file_text(ed, WALK), before, "the failed call added no undo step")


async def test_no_change_writes_nothing(ed: Editor):
    before = file_text(ed, WALK)
    r = await ed.call_raw("edit_pose_animation", {"path": WALK, "ops": [{"op": "update_pose", "index": 0, "name": "Contact R"}]})
    ed.check(not r.is_error and "no change" in r.text, r.text)
    ed.check_eq(file_text(ed, WALK), before, "same text")


async def test_create_is_one_undo_step(ed: Editor):
    path = "assets/anim/jump.poseanim"
    await ed.call("edit_pose_animation", path=path, create={"preview_model": ROBOT},
                  ops=[{"op": "add_pose", "name": "Crouch", "angles": {"left_lower_leg.bend": 60, "right_lower_leg.bend": 60}}])
    ed.check((ed.project / path).exists(), "created")
    await undo_project(ed)
    ed.check(not (ed.project / path).exists(), "undo removes the new file")
    await ed.check_error("get_pose_animation", path=path, contains="does not exist")
    await redo_project(ed)
    r = await ed.call("get_pose_animation", path=path)
    ed.check_eq(r["poses"][0]["name"], "Crouch", "redo brings it back")
    ed.check_eq(r["preview_model"], ROBOT, "preview model")


# -- humanoid sidecar ----------------------------------------------------------

async def test_set_humanoid_writes_the_sidecar(ed: Editor):
    sidecar = HERO + ".humanoid"
    ed.check(not (ed.project / sidecar).exists(), "no sidecar yet")
    walk_before = file_text(ed, WALK)
    r = await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "set_humanoid", "model": HERO, "bones": {"left_toes": "", "upper_chest": "mixamorig:Spine2"}},
        {"op": "set_preview_model", "model": HERO},
    ])
    ed.check(any("set_humanoid" in line for line in r["changes"]), r["changes"])
    data = file_json(ed, sidecar)
    ed.check_eq(data["format"], "jasperengine.humanoid", "format")
    ed.check_eq(data["bones"], {"upper_chest": "mixamorig:Spine2", "left_toes": ""}, "overrides only, in slot order")
    ed.check_eq(file_text(ed, WALK), walk_before, "the animation itself is unchanged")
    await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "set_humanoid", "model": HERO, "bones": {"upper_chest": None}},
    ])
    ed.check_eq(file_json(ed, sidecar)["bones"], {"left_toes": ""}, "null goes back to auto-detect")
    await undo_project(ed, 2)
    ed.check(not (ed.project / sidecar).exists(), "undo removes the sidecar it created")


# -- disk edits ----------------------------------------------------------------

async def test_document_follows_disk_edits(ed: Editor):
    await ed.call("get_pose_animation", path=WALK)
    data = file_json(ed, WALK)
    data["poses"][0]["name"] = "Edited On Disk"
    (ed.project / WALK).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    r = await ed.call("get_pose_animation", path=WALK, detail="summary")
    ed.check_eq(r["poses"][0]["name"], "Edited On Disk", "picked up the file edited outside the editor")
    # the next edit starts from the disk version
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "update_pose", "index": 0, "name": "Contact R"}])
    ed.check_eq(file_json(ed, WALK)["poses"][0]["name"], "Contact R", "edited")
