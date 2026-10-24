"""The Pose Animation window (spec: pose animation phase 4, section 4) on the pose fixture project:
focus_window opens it (window "pose_animation", pose {entity, path, index, play, tab}) and reports
its state; it renders (a non-flat region of the editor screenshot); edits made through
edit_pose_animation while it is open show in it at once (it shares the document); undo of a pose
edit is a project undo; posing writes the hero's bone transforms without marking the scene
modified; preview play then entering play mode puts the rest transforms back; saving the scene while
posing writes the rest transforms (and posing resumes after the save).
"""

from __future__ import annotations

import re
import sys

import harness
from harness import Editor

sys.path.insert(0, str(harness.REPO / "tools" / "mcp"))
import pngdiff  # noqa: E402

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

HERO = "assets/models/hero_tpose.gltf"
WALK = "assets/anim/window_walk.poseanim"
SCENE_FILE = "Main Scene.scene"
THIGH = "mixamorig:RightUpLeg"

S: dict = {}


async def focus(ed: Editor, **pose) -> dict:
    args = {"window": "pose_animation"}
    if pose:
        args["pose"] = pose
    return await ed.call("focus_window", **args)


async def state(ed: Editor) -> dict:
    return (await focus(ed))["state"]


async def bone_rotation(ed: Editor, name: str = THIGH):
    """The bone's local rotation quaternion [x, y, z, w] (get_entity's transform rotation)."""
    rotation = harness.find_key(await ed.call("get_entity", entity=name, detail="full"), "rotation")
    if isinstance(rotation, dict):
        rotation = rotation.get("value", rotation)
        rotation = rotation.get("quat", rotation) if isinstance(rotation, dict) else rotation
    return rotation


def close(a, b, tol=1e-4) -> bool:
    """Same rotation (q and -q are equal)."""
    if a is None or b is None or len(a) != len(b):
        return False
    same = all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))
    flipped = all(abs(float(x) + float(y)) <= tol for x, y in zip(a, b))
    return same or flipped


def saved_rotation(text: str, name: str):
    """The transform rotation of the GameObject `name` in a scene file's text."""
    match = re.search(r"name: " + re.escape(name) + r"\n\s+components:\n\s+transform:\n\s+position: \[[^\]]*\]\n\s+rotation: \[([^\]]*)\]",
                      text)
    if not match:
        return None
    w, x, y, z = (float(v) for v in match.group(1).split(","))   # the scene file stores [w, x, y, z]
    return [x, y, z, w]


async def test_window_listed_and_empty_state(ed: Editor):
    ed.require_tools("focus_window", "edit_pose_animation")
    enum = ed.tool("focus_window").input_schema["properties"]["window"]["enum"]
    ed.check_in("pose_animation", enum, "focus_window lists the window")
    S["rest"] = await bone_rotation(ed)
    r = await focus(ed)
    ed.check(r["shown"], f"the window is shown: {r}")
    st = r["state"]
    ed.check_eq(st["empty_state"], "no_character", f"empty state: {st}")
    ed.check(st["character"] is None and st["animation"] is None, f"nothing bound: {st}")
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"bogus": 1}}, contains="valid: entity")
    await ed.check_error("focus_window", {"window": "console", "pose": {}}, contains="pose_animation")
    msg = await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"entity": "Ground"}})
    ed.check("Model" in msg or "model" in msg, msg)


async def test_bind_and_open(ed: Editor):
    await ed.call("edit_pose_animation", path=WALK, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "Contact R", "angles": {"right_upper_leg.forward": 30, "left_upper_leg.forward": -20}},
        {"op": "add_pose", "name": "Passing R", "angles": {"left_upper_leg.forward": 30, "left_lower_leg.bend": 60}},
        {"op": "mirror_pose", "index": 0, "as_new": True, "name": "Contact L"},
        {"op": "add_pose", "name": "Passing L", "angles": {"right_upper_leg.forward": 30, "right_lower_leg.bend": 60}},
    ])
    r = await focus(ed, entity="Hero")
    ed.check_eq(r["state"]["character"], "Hero", f"bound: {r['state']}")
    ed.check_eq(r["state"]["rig"], "Humanoid 22/22", "rig chip")
    ed.check_eq(r["state"]["empty_state"], "no_animation", f"no animation yet: {r['state']}")
    r = await focus(ed, path=WALK)
    st = r["state"]
    ed.check_eq(st["animation"], WALK, "the document")
    ed.check_eq(st["pose_count"], 4, "four poses")
    ed.check_eq(st["mode"], "posing", "opening poses the first pose")
    ed.check_eq(st["primary"], 0, "pose 0")
    ed.check("empty_state" not in st, f"no empty state: {st}")
    # the window renders: its region of the editor screenshot is not flat
    png = await ed.screenshot(view="editor", max_width=0)
    x, y = int(r["pos"][0]), int(r["pos"][1])
    w, h = int(r["size"][0]), int(r["size"][1])
    ed.check(w > 100 and h > 100, f"window size {r['size']}")
    region = pngdiff.crop(pngdiff.decode(png), x, y, w, h)
    ed.check(region[0] > 100 and not pngdiff.flat(region, 8), "the Pose Animation window draws its content")
    # posing writes the bones, the scene is not modified by it
    posed = await bone_rotation(ed)
    ed.check(not close(posed, S["rest"]), f"the right thigh is posed forward: {posed} vs rest {S['rest']}")
    scenes = await ed.call("list_scenes")
    main = next(s for s in harness.find_key(scenes, "scenes") if s["name"] == "Main Scene")
    ed.check(main["modified"] is False, f"posing does not mark the scene modified: {main}")


async def test_edits_show_in_the_window(ed: Editor):
    """Shortcut-equivalent edits through the MCP path: the open window follows them."""
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "add_pose", "after": 0, "name": "Down R"}])
    st = await state(ed)
    ed.check_eq(st["pose_count"], 5, f"the window shows the added pose: {st}")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "mirror_pose", "index": 1, "as_new": True}])
    ed.check_eq((await state(ed))["pose_count"], 6, "mirrored copy")
    await ed.call("undo", scope="project")
    ed.check_eq((await state(ed))["pose_count"], 5, "a pose edit is a project undo step")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "delete_pose", "index": 1}])
    st = await state(ed)
    ed.check_eq(st["pose_count"], 4, "deleted")
    ed.check(0 <= st["primary"] < 4, f"the selection stays valid: {st}")
    # selecting a pose poses it
    st = (await focus(ed, index=1))["state"]
    ed.check_eq(st["primary"], 1, "pose 1 selected")
    ed.check_eq(st["mode"], "posing", "posing")
    # an edit of the primary pose re-applies it to the bones (undo too)
    before = await bone_rotation(ed, "mixamorig:LeftUpLeg")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "update_pose", "index": 1, "angles": {"left_upper_leg.forward": 70}}])
    after = await bone_rotation(ed, "mixamorig:LeftUpLeg")
    ed.check(not close(before, after), f"the bones follow the edit: {before} -> {after}")
    await ed.call("undo", scope="project")
    ed.check(close(await bone_rotation(ed, "mixamorig:LeftUpLeg"), before), "undo re-applies the pose")
    # tabs
    ed.check_eq((await focus(ed, tab="feel"))["state"]["tab"], "Feel", "Feel tab")
    ed.check_eq((await focus(ed, tab="Pose"))["state"]["tab"], "Pose", "Pose tab")


async def test_save_while_posing_writes_rest(ed: Editor):
    await focus(ed, index=0)
    ed.check(not close(await bone_rotation(ed), S["rest"]), "posed before the save")
    await ed.call("save_scene", scene="Main Scene")
    text = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
    saved = saved_rotation(text, THIGH)
    ed.check(close(saved, S["rest"]), f"the scene file has the rest rotation: {saved} vs {S['rest']}")
    await ed.call("wait", seconds=0.3)
    ed.check(not close(await bone_rotation(ed), S["rest"]), "posing resumes after the save")
    ed.check_eq((await state(ed))["mode"], "posing", "still posing")


async def test_preview_then_play_mode_restores(ed: Editor):
    st = (await focus(ed, play=True))["state"]
    ed.check_eq(st["mode"], "previewing", f"previewing: {st}")
    ed.check(st["playing"], "playing")
    ed.check(st["length"] > 0, "the preview has frames")
    await ed.call("wait", seconds=0.4)
    st = await state(ed)
    ed.check(st["time"] > 0, f"the playhead moves: {st}")
    ed.check(not close(await bone_rotation(ed), S["rest"]), "the preview shows the animation")
    await ed.call("play", action="start")
    try:
        st = await state(ed)
        ed.check_eq(st["mode"], "idle", f"play mode stops the preview: {st}")
    finally:
        await ed.call("play", action="stop")
    ed.check(close(await bone_rotation(ed), S["rest"]), "the rest transforms are back after play mode")
    st = await state(ed)
    ed.check_eq(st["mode"], "idle", "idle after play mode")
    # posing again
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", "a pose click poses again")
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"index": 99}}, contains="pose.index")


async def test_project_window_thumbnail(ed: Editor):
    """A .poseanim gets a Project window thumbnail: its middle pose on the preview model."""
    r = await ed.call_raw("view_asset", {"path": WALK})
    ed.check(not r.is_error and r.images, f"a thumbnail image: {r.text[:300]}")
    image = pngdiff.decode(r.images[0][0])
    ed.check(image[0] >= 64 and not pngdiff.flat(image, 8), "the thumbnail shows the character")
    (ed.scratch.root / "poseanim-thumbnail.png").write_bytes(r.images[0][0])
