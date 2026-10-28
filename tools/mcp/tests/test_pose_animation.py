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
CI = True
TIMEOUT = 600

HERO = "assets/models/hero_tpose.gltf"
ROBOT = "assets/models/robot_apose.gltf"
WALK = "assets/anim/walk.poseanim"


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


# -- create, add, get ----------------------------------------------------------

async def test_create_and_add_poses(ed: Editor):
    r = await ed.call("edit_pose_animation", path=WALK, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "Contact R", "hold": 0.1,
         "angles": {"right_upper_leg.forward": 25, "left_upper_leg.forward": -15, "left_upper_arm.raise": 10,
                    "right_upper_arm.raise": 10, "hips.lean": 0.03}},
        {"op": "add_pose", "name": "Down R", "add_angles": {"right_lower_leg.bend": 20}, "offset": {"forward": 0.02}},
    ])
    ed.check_eq(r["pose_count"], 2, "pose count")
    ed.check_eq([p["index"] for p in r["poses"]], [0, 1], "both poses are touched")
    ed.check_eq(r["clamped"], [], "nothing clamped")
    # as get: `preset` always there (null when the feel matches none), no rig error on a good model
    ed.check("preset" in r, f"preset always present: {sorted(r)}")
    ed.check("rig_error" not in r, f"a usable rig: {r.get('rig_error')}")
    data = file_json(ed, WALK)
    ed.check_eq(data["format"], "jasperengine.poseanim", "format")
    # stored relative to the assets folder, as model files are; tools show project-relative paths
    ed.check_eq(data["preview_model"], "models/hero_tpose.gltf", "preview_model in the file")
    ed.check_eq([p["name"] for p in data["poses"]], ["Contact R", "Down R"], "names")
    ed.check(approx(data["poses"][1]["angles"]["right_upper_leg.forward"], 25), "Down R copies Contact R")
    ed.check(approx(data["poses"][1]["angles"]["right_lower_leg.bend"], 20), "add_angles adds")
    ed.check(approx(data["poses"][1]["offset"]["forward"], 0.02), "offset")

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
    ed.check(all(part in msg for part in ("ops[2].angles", "left_elbow.bend", "left_lower_arm.bend")), msg)
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


# -- rig and metrics -----------------------------------------------------------

async def test_rig_and_metrics_on_the_preview_model(ed: Editor):
    r = await ed.call("get_pose_animation", path=WALK)
    rig = r["rig"]
    ed.check(rig is not None and "error" not in rig, f"rig: {rig}")
    ed.check_eq(rig["model"], HERO, "the preview model's rig")
    ed.check_eq(rig["mapped"], 22, "every hero slot is mapped")
    ed.check_eq(rig["missing"], [], "nothing missing")
    ed.check_eq(rig["bones"]["hips"], "mixamorig:Hips", "auto-detected bones")
    ed.check(0.9 < rig["human_scale"] < 1.1, f"human scale {rig['human_scale']}")
    for pose in r["poses"]:
        m = pose["metrics"]
        ed.check(set(m) >= {"feet", "lowest", "balanced", "balance_margin", "limit_hits"}, f"metrics shape: {m}")
        if pose["grounded"]:
            ed.check(approx(m["lowest"], 0.0, 1e-3), f"{pose['name']}: grounded poses touch the floor: {m}")
            ed.check(m["feet"]["left"]["contact"] or m["feet"]["right"]["contact"], f"{pose['name']}: a foot on the floor: {m}")
    raw = await ed.call_raw("get_pose_animation", {"path": WALK, "detail": "summary"})
    ed.check("bones" not in raw.structured["rig"], "summary rig without the bone list")

    # a neutral standing pose is balanced on both feet; an airborne one has no contact
    path = "assets/anim/metrics.poseanim"
    await ed.call("edit_pose_animation", path=path, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "Stand"},
        {"op": "add_pose", "name": "Air", "grounded": False, "lift": 0.3},
        {"op": "add_pose", "name": "One leg", "from": 0, "angles": {"left_upper_leg.forward": 70, "left_lower_leg.bend": 80}},
    ])
    poses = (await ed.call("get_pose_animation", path=path))["poses"]
    stand, air, one = (p["metrics"] for p in poses)
    ed.check(stand["balanced"] and stand["balance_margin"] > 0, f"stand: {stand}")
    ed.check(stand["feet"]["left"]["contact"] and stand["feet"]["right"]["contact"], f"stand: {stand}")
    ed.check(not air["feet"]["left"]["contact"] and not air["feet"]["right"]["contact"], f"air: {air}")
    ed.check(approx(air["lowest"], 0.3, 2e-3) and not air["balanced"], f"air: {air}")
    ed.check(one["feet"]["right"]["contact"] and not one["feet"]["left"]["contact"], f"one leg: {one}")
    ed.check(one["feet"]["left"]["height"] > 0.1, f"one leg: {one}")


async def test_joint_limits_clamp_touched_poses(ed: Editor):
    path = "assets/anim/metrics.poseanim"
    r = await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "update_pose", "index": 0, "angles": {"left_lower_arm.bend": -20, "head.twist": 80}},
    ])
    clamped = {c["field"]: (c["requested"], c["applied"]) for c in r["clamped"]}
    ed.check_eq(clamped.get("poses[0].angles.left_lower_arm.bend"), (-20, 0), f"limits: {r['clamped']}")
    ed.check_eq(clamped.get("poses[0].angles.head.twist"), (80, 55), f"limits: {r['clamped']}")
    ed.check_eq(r["poses"][0]["metrics"]["limit_hits"], [], "clamped poses hit no limit")
    # joint limits off: values are kept and reported as limit hits
    r = await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "set_settings", "humanize": {"joint_limits": False}},
        {"op": "update_pose", "index": 0, "angles": {"left_lower_arm.bend": -20}},
    ])
    ed.check(not any(c["field"].startswith("poses[0].angles") for c in r["clamped"]), f"kept: {r['clamped']}")
    ed.check_eq(r["poses"][0]["metrics"]["limit_hits"], ["left_lower_arm.bend"], "limit hit")
    await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "set_settings", "humanize": {"joint_limits": True}},
        {"op": "update_pose", "index": 0, "angles": {"left_lower_arm.bend": 0, "head.twist": 0}},
    ])


# -- mirror, presets, auto timing ----------------------------------------------

async def test_mirror_twice_is_exact(ed: Editor):
    before = file_json(ed, WALK)["poses"][0]
    r = await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "mirror_pose", "index": 0}])
    mirrored = file_json(ed, WALK)["poses"][0]
    ed.check(approx(mirrored["angles"]["left_upper_leg.forward"], before["angles"]["right_upper_leg.forward"]),
             f"left/right swapped: {mirrored['angles']}")
    ed.check(approx(mirrored["angles"]["right_upper_leg.forward"], before["angles"]["left_upper_leg.forward"]), "swapped back")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "mirror_pose", "index": 0}])
    ed.check_eq(file_json(ed, WALK)["poses"][0], before, "mirroring twice gives the pose back exactly")
    r = await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "mirror_pose", "index": 0, "as_new": True, "name": "Contact L"}])
    data = file_json(ed, WALK)
    ed.check_eq([p["name"] for p in data["poses"]][:2], ["Contact R", "Contact L"], "inserted after")
    ed.check(approx(data["poses"][1]["angles"]["left_upper_leg.forward"], 25), "mirrored copy")
    ed.check_eq([p["index"] for p in r["poses"]], [1], "the copy is touched")
    await undo_project(ed)


async def test_presets(ed: Editor):
    r = await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "apply_preset", "name": "Cartoon"}])
    ed.check_eq(r.get("preset"), "Cartoon", "the result names the matching preset")
    got = await ed.call("get_pose_animation", path=WALK, detail="summary")
    ed.check_eq(got["preset"], "Cartoon", "preset")
    ed.check_eq(got["feel"]["transition"], {"curve": "spring", "duration": 0.1, "overshoot": 0.35, "anticipation": 0.12}, "Cartoon")
    ed.check(approx(got["feel"]["overlap"], 0.1) and approx(got["feel"]["looseness"], 0.65), f"feel {got['feel']}")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "set_settings", "overlap": 0.2}])
    ed.check((await ed.call("get_pose_animation", path=WALK, detail="summary"))["preset"] is None, "no preset matches any more")
    msg = await ed.check_error("edit_pose_animation", path=WALK, ops=[{"op": "apply_preset", "name": "Bouncy"}])
    ed.check("Snappy" in msg and "Stop-motion" in msg, msg)
    await undo_project(ed, 2)
    ed.check_eq((await ed.call("get_pose_animation", path=WALK, detail="summary"))["preset"], "Snappy", "the defaults are Snappy")
    path = "assets/anim/stop.poseanim"
    await ed.call("edit_pose_animation", path=path, create={"preview_model": HERO, "preset": "stop_motion"},
                  ops=[{"op": "add_pose"}])
    got = await ed.call("get_pose_animation", path=path, detail="summary")
    ed.check(got["preset"] == "Stop-motion" and got["feel"]["step_rate"] == 12, f"create.preset: {got['feel']}")


async def test_auto_timing(ed: Editor):
    path = "assets/anim/timing.poseanim"
    await ed.call("edit_pose_animation", path=path, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "A", "hold": 0.2},
        {"op": "add_pose", "name": "B (small)", "hold": 0.2, "add_angles": {"head.bend": 5}},
        {"op": "add_pose", "name": "C (big)", "hold": 0.2, "angles": {"left_upper_arm.raise": 120, "right_upper_arm.raise": 120,
                                                                     "spine.bend": 20}},
    ])
    before = await ed.call("get_pose_animation", path=path, detail="summary")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "auto_timing"}])
    after = await ed.call("get_pose_animation", path=path, detail="summary")
    ed.check(approx(after["length"], before["length"], 0.02), f"length kept: {before['length']} -> {after['length']}")
    ed.check(all(p["transition"] is not None for p in after["poses"]), "every pose gets its own transition")
    durations = [p["transition"]["duration"] for p in after["poses"]]
    # the big move (B -> C) snaps slower than the small one (A -> B)
    ed.check(durations[1] > durations[0], f"bigger move, longer snap: {durations}")
    ed.check_eq(len(r["poses"]), 3, "every pose touched")
    msg = await ed.check_error("edit_pose_animation", path="assets/anim/one.poseanim", ops=[{"op": "auto_timing"}])
    ed.check("two poses" in msg, msg)


async def test_set_humanoid_checks_bone_names(ed: Editor):
    msg = await ed.check_error("edit_pose_animation", path=WALK, ops=[
        {"op": "set_humanoid", "model": HERO, "bones": {"left_toes": "mixamorig:LeftToeBas"}}])
    ed.check("no bone 'mixamorig:LeftToeBas'" in msg and "mixamorig:LeftToeBase" in msg, msg)
    # unmapping an optional slot through the sidecar shows in the rig
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "set_humanoid", "model": HERO, "bones": {"left_toes": ""}}])
    rig = (await ed.call("get_pose_animation", path=WALK, detail="summary"))["rig"]
    ed.check_eq(rig["mapped"], 21, f"the sidecar unmapped a slot: {rig}")
    await undo_project(ed)
    rig = (await ed.call("get_pose_animation", path=WALK, detail="summary"))["rig"]
    ed.check_eq(rig["mapped"], 22, "undo: the rig follows the sidecar")


# -- bake ----------------------------------------------------------------------

S: dict = {}


async def bake_info(ed: Editor, animation) -> dict:
    return await ed.call("get_animation", entity=animation, max_keys=2)


async def test_bake_creates_an_animation(ed: Editor):
    info = await ed.call("get_pose_animation", path=WALK, detail="summary")
    r = await ed.call("edit_pose_animation", path=WALK, bake={"model": "Hero"})
    bake = r["bake"]
    ed.check(bake["created"] and bake["name"] == "walk", f"bake: {bake}")
    ed.check_eq(bake["tracks"], 23, "22 bone rotations and the hips position")
    ed.check_eq(bake["frames"], info["frame_count"], "one key per frame")
    anim = await bake_info(ed, bake["animation"]["id"])
    kinds = [a["kind"] for a in anim["actions"]]
    ed.check_eq(kinds.count("rotation_track"), 22, f"kinds {kinds}")
    ed.check_eq(kinds.count("position_track"), 1, f"kinds {kinds}")
    ed.check(anim["loop"] is True and approx(anim["duration"], info["length"]), f"loop/duration {anim['loop']} {anim['duration']}")
    hips = next(a for a in anim["actions"] if a["kind"] == "position_track")
    ed.check_eq(hips["target"]["name"], "mixamorig:Hips", "the hips position track")
    ed.check(all(a["target"]["name"].startswith("mixamorig:") for a in anim["actions"]), "tracks target bones")
    # the hero's rest rotations are identity: a posed leg's keys are not
    leg = next(a for a in anim["actions"] if a["target"]["name"] == "mixamorig:RightUpLeg")
    ed.check(abs(leg["keys"][0]["value"]["quat"][3]) < 0.999, f"the right thigh is posed forward: {leg['keys'][0]}")
    ed.check(leg["keys"][0]["time"] == 0 and leg["keys"][1]["time"] > 0, f"key times {leg['keys']}")
    model = await ed.call("get_model_info", entity="Hero", include_bones=False)
    ed.check("walk" in json.dumps(model["animations"]), f"the model lists the clip: {model['animations']}")
    S["bake"] = bake
    S["tracks"] = sorted(a["action"]["id"] for a in anim["actions"])


async def test_rebake_keeps_the_entities(ed: Editor):
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "update_pose", "index": 0, "hold": 0.3}],
                  bake={"model": "Hero"})
    anim = await bake_info(ed, S["bake"]["animation"]["id"])
    ed.check_eq(sorted(a["action"]["id"] for a in anim["actions"]), S["tracks"], "re-bake rewrites the same track entities")
    length = (await ed.call("get_pose_animation", path=WALK, detail="summary"))["length"]
    ed.check(approx(anim["duration"], length), f"new duration {anim['duration']} vs {length}")
    # the re-bake is its own scene undo step; the edit is a project one
    await ed.call("undo", scope="scene")
    anim = await bake_info(ed, S["bake"]["animation"]["id"])
    ed.check_eq(sorted(a["action"]["id"] for a in anim["actions"]), S["tracks"], "undoing the re-bake keeps the first bake")
    await undo_project(ed)


async def test_scene_undo_removes_the_bake(ed: Editor):
    await ed.call("undo", scope="scene")
    r = await ed.call_raw("get_animation", {"entity": S["bake"]["animation"]["id"]})
    ed.check(r.is_error, f"the baked Animation is gone after undo: {r.text[:300]}")
    model = await ed.call("get_model_info", entity="Hero", include_bones=False)
    ed.check("walk" not in json.dumps(model["animations"]), f"the clip list is back: {model['animations']}")


async def test_bake_errors(ed: Editor):
    msg = await ed.check_error("edit_pose_animation", path=WALK, bake={"model": "Ground"})
    ed.check("not a model" in msg, msg)
    msg = await ed.check_error("edit_pose_animation", path=WALK, bake={"model": "Nobody"})
    ed.check("bake.model" in msg, msg)
    msg = await ed.check_error("edit_pose_animation", path="assets/anim/empty.poseanim", create={}, bake={"model": "Hero"})
    ed.check("no poses" in msg, msg)
    ed.check(not (ed.project / "assets/anim/empty.poseanim").exists(), "nothing was written")


# -- the A-pose robot ----------------------------------------------------------

async def test_same_asset_on_the_apose_robot(ed: Editor):
    hero = await ed.call("get_pose_animation", path=WALK)
    robot = await ed.call("get_pose_animation", path=WALK, model="Robot")
    rig = robot["rig"]
    ed.check_eq(rig["model"]["name"], "Robot", "the scene model's rig")
    ed.check_eq(rig["mapped"], 21, f"no upper chest on the robot: {rig}")
    ed.check("upper_chest" not in rig["bones"], "upper chest unmapped")
    ed.check_eq(rig["bones"]["left_upper_arm"], "upper_arm.L", "Blender names")
    for h, r in zip(hero["poses"], robot["poses"]):
        ed.check_eq(h["angles"], r["angles"], "the angles are the asset's, whatever the rig")
        if h["grounded"]:
            ed.check(approx(r["metrics"]["lowest"], 0.0, 1e-3), f"robot pose {r['name']} grounded: {r['metrics']}")
    r = await ed.call("edit_pose_animation", path=WALK, bake={"model": "Robot", "name": "Walk Robot"})
    ed.check_eq(r["bake"]["tracks"], 22, "21 mapped bones and the hips position")
    ed.check_eq(r["bake"]["name"], "Walk Robot", "named bake")
    await ed.call("undo", scope="scene")


async def test_baked_animation_plays(ed: Editor):
    """Model:playAnimation(name) plays the baked clip in play mode."""
    r = await ed.call("edit_pose_animation", path=WALK, bake={"model": "Hero"})
    ed.check(r["bake"]["created"], f"bake: {r['bake']}")
    rest = harness.find_key(await ed.call("get_entity", entity="mixamorig:RightUpLeg", detail="full"), "rotation")
    await ed.call("play", action="start")
    try:
        await ed.call("eval_lua", code='object("Hero", "Model"):playAnimation("walk"); return true')
        await ed.call("wait", game_seconds=0.15)
        got = await ed.call("get_runtime_entity", entity="mixamorig:RightUpLeg")
        playing = harness.find_key(got, "rotation")
        ed.check(rest is not None and playing is not None and playing != rest, f"the thigh moves: rest {rest}, playing {playing}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("undo", scope="scene")


# -- fix round 1 ---------------------------------------------------------------

async def anim_ids(ed: Editor, animation) -> list:
    return sorted(a["action"]["id"] for a in (await bake_info(ed, animation))["actions"])


async def test_default_named_bakes_on_two_models(ed: Editor):
    """Animation names are per model: the default "walk" on the Robot after the Hero is "walk" too."""
    hero = (await ed.call("edit_pose_animation", path=WALK, bake={"model": "Hero"}))["bake"]
    robot = (await ed.call("edit_pose_animation", path=WALK, bake={"model": "Robot"}))["bake"]
    ed.check(hero["name"] == "walk" and robot["name"] == "walk", f"names: {hero['name']} {robot['name']}")
    ed.check(hero["animation"]["id"] != robot["animation"]["id"], "one Animation per model")
    ids = await anim_ids(ed, robot["animation"]["id"])
    again = (await ed.call("edit_pose_animation", path=WALK, bake={"model": "Robot"}))["bake"]
    ed.check(not again["created"] and again["name"] == "walk", f"re-bake: {again}")
    ed.check_eq(again["animation"]["id"], robot["animation"]["id"], "the same Animation")
    ed.check_eq(await anim_ids(ed, robot["animation"]["id"]), ids, "the same track entities")
    rest = harness.find_key(await ed.call("get_entity", entity="thigh.R", detail="full"), "rotation")
    await ed.call("play", action="start")
    try:
        await ed.call("eval_lua", code='object("Robot", "Model"):playAnimation("' + again["name"] + '"); return true')
        await ed.call("wait", game_seconds=0.15)
        playing = harness.find_key(await ed.call("get_runtime_entity", entity="thigh.R"), "rotation")
        ed.check(rest is not None and playing is not None and playing != rest, f"the robot's thigh moves: {rest} -> {playing}")
        console = await ed.console_since("last")
        ed.check(not any("non-existent animation" in str(line) for line in console), f"playAnimation found it: {console}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("undo", scope="scene", steps=3)
    for bake in (hero, robot):
        ed.check((await ed.call_raw("get_animation", {"entity": bake["animation"]["id"]})).is_error, "undone")


async def test_bake_the_pose_animator_asset(ed: Editor):
    """bake without path does what the Inspector's Bake button does: the Pose Animator's animation
    (given over MCP as the pose tools name it, assets/anim/walk.poseanim; stored relative to the
    assets folder), echoed project-relative."""
    await ed.call("add_component", entity="Hero", component="poseanimator", values={"poseanimator": {"animation": WALK}})
    r = await ed.call_raw("edit_pose_animation", {"bake": {"model": "Hero"}})
    ed.check(not r.is_error, r.text)
    bake = r.structured["bake"]
    ed.check(bake["created"] and bake["name"] == "walk" and bake["source"] == WALK, f"bake: {bake}")
    await ed.check_error("edit_pose_animation", {"bake": {"model": "Robot"}}, contains="no Pose Animator")
    await ed.check_error("edit_pose_animation", {"bake": {"model": "Hero", "name": "x"}}, contains="pass path")
    await ed.call("undo", scope="scene", steps=2)


async def test_own_transitions_are_not_hidden(ed: Editor):
    path = "assets/anim/timing.poseanim"
    ed.check(all(p["transition"] is not None for p in (await ed.call("get_pose_animation", path=path, detail="summary"))["poses"]),
             "auto_timing gave every pose its own transition")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "apply_preset", "name": "Heavy"}])
    ed.check(any("own transition" in w for w in r["warnings"]), f"warned: {r['warnings']}")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "set_settings", "transition": {"duration": 0.3}}])
    ed.check(any("have their own transition" in w for w in r["warnings"]), f"warned: {r['warnings']}")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "apply_preset", "name": "Snappy", "clear_custom": True}])
    ed.check(not any("own transition" in w for w in r["warnings"]), f"no warning once cleared: {r['warnings']}")
    got = await ed.call("get_pose_animation", path=path, detail="summary")
    ed.check(all(p["transition"] is None for p in got["poses"]), "clear_custom: every pose follows the animation again")
    ed.check_eq(got["preset"], "Snappy", "preset")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "update_pose", "index": 0, "transition": {"duration": 0.2}},
                                                             {"op": "set_settings", "overlap": 0.07, "clear_custom": True}])
    ed.check(all(p["transition"] is None for p in (await ed.call("get_pose_animation", path=path, detail="summary"))["poses"]),
             "set_settings clear_custom")


async def test_metadata_updates_are_not_limit_clamped(ed: Editor):
    path = "assets/anim/metrics.poseanim"
    await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "set_settings", "humanize": {"joint_limits": False}},
        {"op": "update_pose", "index": 0, "angles": {"left_lower_arm.bend": -20}},
    ])
    r = await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "set_settings", "humanize": {"joint_limits": True}},
        {"op": "update_pose", "index": 0, "hold": 0.25, "name": "Stand still"},
        {"op": "move_pose", "index": 1, "to": 2},
    ])
    ed.check_eq(r["clamped"], [], "metadata edits and moves clamp nothing")
    ed.check(approx(file_json(ed, path)["poses"][0]["angles"]["left_lower_arm.bend"], -20), "the angle is kept")
    ed.check_eq(r["poses"][0]["metrics"]["limit_hits"], ["left_lower_arm.bend"], "and reported as a limit hit")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "update_pose", "index": 0, "add_angles": {"head.bend": 1}}])
    ed.check(any(c["field"] == "poses[0].angles.left_lower_arm.bend" for c in r["clamped"]), f"an angle edit clamps: {r['clamped']}")


async def test_edit_reports_the_rig_error_like_get(ed: Editor):
    # a required slot unmapped: no metrics, and the edit result says why (as get does)
    path = "assets/anim/broken_rig.poseanim"
    r = await ed.call("edit_pose_animation", path=path, create={"preview_model": ROBOT}, ops=[
        {"op": "set_humanoid", "model": ROBOT, "bones": {"left_upper_leg": ""}},
        {"op": "add_pose", "name": "a"},
    ])
    try:
        ed.check(r.get("rig_error"), f"rig_error in the edit result: {sorted(r)}")
        ed.check(all("metrics" not in p for p in r["poses"]), "no metrics")
        raw = await ed.call_raw("edit_pose_animation", {"path": path, "ops": [{"op": "update_pose", "index": 0, "hold": 0.3}]})
        ed.check("no metrics: " in raw.text, raw.text[:500])
        got = await ed.call("get_pose_animation", path=path, detail="summary")
        ed.check_eq(got["rig"]["error"], r["rig_error"], "the same error as get")
    finally:
        await ed.call("edit_pose_animation", path=path, ops=[{"op": "set_humanoid", "model": ROBOT, "bones": {"left_upper_leg": None}}])


async def test_blend_to_is_a_plain_lerp_across_the_seam(ed: Editor):
    # DOF values are joint angles, not headings: 170 -> -60 at 0.5 is 55 (the wrapped lerp gave 235)
    path = "assets/anim/seam.poseanim"
    r = await ed.call("edit_pose_animation", path=path, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "up", "angles": {"left_upper_arm.forward": 170}},
        {"op": "add_pose", "name": "back", "angles": {"left_upper_arm.forward": -60}},
        {"op": "add_pose", "after": 0, "blend_to": 1, "blend": 0.5, "name": "mid"},
    ])
    ed.check_eq(r["clamped"], [], "nothing clamped")
    poses = file_json(ed, path)["poses"]
    ed.check_eq([p["name"] for p in poses], ["up", "mid", "back"], "order")
    ed.check(approx(poses[1]["angles"]["left_upper_arm.forward"], 55), f"plain lerp: {poses[1]['angles']}")


async def test_derived_poses_read_the_clamped_values(ed: Editor):
    # joint limits on: a pose clamped by one op is what later ops of the same call copy, mirror
    # and blend (not the unclamped value it was asked for)
    path = "assets/anim/derived.poseanim"
    r = await ed.call("edit_pose_animation", path=path, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "a"},
        {"op": "update_pose", "index": 0, "angles": {"left_upper_arm.raise": 200}},
        {"op": "mirror_pose", "index": 0, "as_new": True, "name": "mirrored"},
        {"op": "add_pose", "name": "rest", "angles": {"left_upper_arm.raise": 0}},
        {"op": "add_pose", "from": 0, "blend_to": 2, "blend": 0.5, "name": "half"},
        {"op": "add_pose", "from": 0, "name": "copy"},
    ])
    poses = file_json(ed, path)["poses"]
    ed.check_eq([p["name"] for p in poses], ["a", "mirrored", "rest", "half", "copy"], "order")
    clamped = {c["field"]: (c["requested"], c["applied"]) for c in r["clamped"]}
    ed.check_eq(clamped.get("poses[0].angles.left_upper_arm.raise"), (200, 175), f"clamped once: {r['clamped']}")
    ed.check_eq(len(r["clamped"]), 1, f"derived poses report no clamps of their own: {r['clamped']}")
    ed.check(approx(poses[0]["angles"]["left_upper_arm.raise"], 175), f"stored clamped: {poses[0]['angles']}")
    ed.check(approx(poses[1]["angles"]["right_upper_arm.raise"], 175), f"mirror of the stored pose: {poses[1]['angles']}")
    ed.check(approx(poses[3]["angles"]["left_upper_arm.raise"], 87.5), f"blend from the stored pose: {poses[3]['angles']}")
    ed.check(approx(poses[4]["angles"]["left_upper_arm.raise"], 175), f"copy of the stored pose: {poses[4]['angles']}")
    # a later op setting the clamped DOF again within the limits: no stale clamp report
    r = await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "update_pose", "index": 0, "angles": {"left_upper_arm.raise": 190}},
        {"op": "update_pose", "index": 0, "angles": {"left_upper_arm.raise": 30}},
        {"op": "move_pose", "index": 0, "to": 4},
    ])
    ed.check_eq(r["clamped"], [], f"the last request was within the limits: {r['clamped']}")
    r = await ed.call("edit_pose_animation", path=path, ops=[
        {"op": "update_pose", "index": 4, "angles": {"left_upper_arm.raise": 190}},
        {"op": "move_pose", "index": 4, "to": 0},
    ])
    clamped = {c["field"]: (c["requested"], c["applied"]) for c in r["clamped"]}
    ed.check_eq(clamped, {"poses[0].angles.left_upper_arm.raise": (190, 175)}, f"the clamp moves with its pose: {r['clamped']}")


async def test_humanoid_and_ops_are_one_undo_step(ed: Editor):
    before = file_text(ed, WALK)
    sidecar = ed.project / (HERO + ".humanoid")
    ed.check(not sidecar.exists(), "no sidecar")
    await ed.call("edit_pose_animation", path=WALK, ops=[
        {"op": "set_humanoid", "model": HERO, "bones": {"right_toes": ""}},
        {"op": "update_pose", "index": 0, "hold": 0.33},
    ])
    ed.check(sidecar.exists() and file_text(ed, WALK) != before, "both written")
    await undo_project(ed)
    ed.check(not sidecar.exists() and file_text(ed, WALK) == before, "one undo takes both back")


async def test_warnings_outside_assets_and_from_the_file(ed: Editor):
    path = "loose/outside.poseanim"
    r = await ed.call("edit_pose_animation", path=path, create={}, ops=[{"op": "add_pose"}])
    ed.check(any("outside the assets folder" in w for w in r["warnings"]), f"warned: {r['warnings']}")
    got = await ed.call("get_pose_animation", path=path, detail="summary")
    ed.check(any("outside the assets folder" in w for w in got.get("warnings", [])), f"get warns: {got.get('warnings')}")
    data = file_json(ed, path)
    data["mystery"] = 1
    (ed.project / path).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    r = await ed.call("edit_pose_animation", path=path, ops=[{"op": "update_pose", "index": 0, "hold": 0.2}])
    ed.check(any("mystery" in w for w in r["warnings"]), f"the file's load warnings: {r['warnings']}")
