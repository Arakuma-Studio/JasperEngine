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


def same_as_unposed(ed: Editor, text: str, what: str) -> None:
    """The scene file is byte for byte the one saved before any posing (bones, skinning matrices, bounds)."""
    if text == S["unposed_scene"]:
        return
    import difflib
    diff = "".join(list(difflib.unified_diff(S["unposed_scene"].splitlines(True), text.splitlines(True), n=1))[:40])
    ed.check(False, f"{what}: the scene file differs from the unposed save:\n{diff}")


NUMBER = re.compile(r"-?\d+(?:\.\d+)?(?:e[-+]?\d+)?")


def same_line(a: str, b: str, tol: float = 1e-5) -> bool:
    """Two scene file lines are the same text with numbers equal within `tol` (relative)."""
    if a == b:
        return True
    if NUMBER.sub("#", a) != NUMBER.sub("#", b):
        return False
    for x, y in zip(NUMBER.findall(a), NUMBER.findall(b)):
        fx, fy = float(x), float(y)
        if abs(fx - fy) > tol * max(1.0, abs(fx), abs(fy)):
            return False
    return True


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
    # the scene saved without posing (play mode leaves the session idle, bones at rest): a save
    # while posing must write exactly this, byte for byte (bones, skinning matrices, bounds)
    await ed.call("play", action="start")
    await ed.call("play", action="stop")
    ed.check_eq((await state(ed))["mode"], "idle", "idle after play mode")
    await ed.call("wait", seconds=0.3)
    await ed.call("save_scene", scene="Main Scene")
    S["unposed_scene"] = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
    await focus(ed, index=0)
    await ed.call("wait", seconds=0.3)
    ed.check(not close(await bone_rotation(ed), S["rest"]), "posed before the save")
    await ed.call("save_scene", scene="Main Scene")
    text = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
    saved = saved_rotation(text, THIGH)
    ed.check(close(saved, S["rest"]), f"the scene file has the rest rotation: {saved} vs {S['rest']}")
    same_as_unposed(ed, text, "saved while posing")
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


async def test_save_while_previewing_writes_rest(ed: Editor):
    """A save during a playing preview: the file gets the rest bones, the preview goes on."""
    st = (await focus(ed, play=True))["state"]
    ed.check_eq(st["mode"], "previewing", "previewing")
    await ed.call("wait", seconds=0.2)
    await ed.call("save_scene", scene="Main Scene")
    text = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
    saved = saved_rotation(text, THIGH)
    ed.check(close(saved, S["rest"]), f"the scene file has the rest rotation: {saved} vs {S['rest']}")
    same_as_unposed(ed, text, "saved while previewing")
    await ed.call("wait", seconds=0.3)
    st = await state(ed)
    ed.check(st["mode"] == "previewing" and st["playing"] and st["editable"], f"the preview goes on after the save: {st}")
    await focus(ed, play=False)
    ed.check_eq((await state(ed))["mode"], "posing", "stop returns to posing")


async def test_scene_keys_follow_the_character(ed: Editor):
    """The Scene view's keys go to the window while it poses the selected character (also with the
    window behind another tab)."""
    await focus(ed, index=0)
    await ed.call("select_entities", entities=["Ground"])
    ed.check((await state(ed))["scene_keys"] is False, "another GameObject selected: the Scene view keeps its keys")
    # selecting another object leaves posing (phase 5 spec, section 2): pose again
    await focus(ed, index=0)
    await ed.call("select_entities", entities=[THIGH])
    ed.check((await state(ed))["scene_keys"] is True, "a bone of the character: pose keys")
    await ed.call("select_entities", entities=["Hero"])
    ed.check((await state(ed))["scene_keys"] is True, "the character: pose keys")
    # behind another tab of its dock: the viewport handles stay live, and so do the pose keys of
    # the Scene view (final review ruling). Real key events (pose.input, which neither fronts nor
    # focuses the window): Ctrl+D in the focused Scene view duplicates the pose, not the Hero
    heroes = len(hero_rows(await ed.call("get_scene_tree")))
    count = (await state(ed))["pose_count"]
    await ed.call("focus_window", window="console")
    await ed.call("wait", seconds=0.2)
    await focus(ed, input={"focus_scene": True})
    await ed.call("wait", frames=3)
    r = await focus(ed, input={"keys": [{"key": "d", "ctrl": True}]})
    ed.check(r["shown"] is False, f"the window stayed behind the Console tab: {r}")
    ed.check(r["state"]["scene_keys"] is True, f"hidden window, posing Scene view: pose keys: {r['state']}")
    ed.check_eq(r["state"]["pose_count"], count + 1, "Ctrl+D in the Scene view duplicated the pose")
    ed.check_eq(len(hero_rows(await ed.call("get_scene_tree"))), heroes, "and not the Hero")
    await ed.call("undo", scope="project")
    # the Game tab in front again, as before the Scene view was focused (later saves compare files
    # whose light shadow range follows the camera that drew last)
    await ed.call("focus_window", window="game")
    r = await ed.call("focus_window", window="pose_animation")
    ed.check_eq(r["state"]["pose_count"], count, "undone")
    ed.check(r["state"]["scene_keys"] is True, "shown again: pose keys")


def hero_rows(tree) -> list:
    """The GameObjects of a get_scene_tree result named Hero (or a copy of it)."""
    found = []

    def walk(value):
        if isinstance(value, dict):
            name = value.get("name")
            if isinstance(name, str) and name.startswith("Hero"):
                found.append(name)
            for v in value.values():
                walk(v)
        elif isinstance(value, list):
            for v in value:
                walk(v)
    walk(tree)
    return found


async def test_card_context_menu_opens_on_a_real_right_click(ed: Editor):
    """A right-click on a strip card opens its context menu (the menu's OpenPopup and BeginPopup
    share one ID scope): real mouse events through ImGui's queue (pose.input)."""
    await focus(ed, index=0, tab="Pose")
    await ed.call("wait", frames=3)
    r = await focus(ed, input={"right_click": {"card": 1}})
    st = r["state"]
    ed.check_eq(st["menu"], "card", f"the card menu is open: {st}")
    ed.check_eq(st["primary"], 1, f"the right-clicked card is selected: {st}")
    # focusing the window closes the menu
    st = await state(ed)
    ed.check(st["menu"] is None, f"closed: {st}")


async def test_mapping_change_rebuilds_the_rig(ed: Editor):
    """Map Bones' undo/redo or MCP set_humanoid: the window's rig follows the sidecar."""
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", "posing before the mapping changes")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "set_humanoid", "model": HERO, "bones": {"left_toes": ""}}])
    await ed.call("wait", seconds=0.6)
    st = await state(ed)
    ed.check_eq(st["rig"], "Humanoid 21/22", f"the rig follows the sidecar: {st}")
    ed.check_eq(st["mode"], "posing", "still posing")
    await ed.call("undo", scope="project")
    await ed.call("wait", seconds=0.6)
    ed.check_eq((await state(ed))["rig"], "Humanoid 22/22", "undo: the mapping is back")


async def test_default_dock_layout(ed: Editor):
    """The bottom dock of a 1600x900 editor: two columns, thumbnails sized to the height."""
    st = await state(ed)
    ed.check_eq(st["layout"], "columns", f"wide and short: strip and timing on the left, tabs on the right: {st}")
    ed.check(64 <= st["thumbnail_height"] <= 116, f"thumbnail height {st['thumbnail_height']}")


async def test_project_window_thumbnail(ed: Editor):
    """A .poseanim gets a Project window thumbnail: its middle pose on the preview model."""
    r = await ed.call_raw("view_asset", {"path": WALK})
    ed.check(not r.is_error and r.images, f"a thumbnail image: {r.text[:300]}")
    image = pngdiff.decode(r.images[0][0])
    ed.check(image[0] >= 64 and not pngdiff.flat(image, 8), "the thumbnail shows the character")
    (ed.scratch.root / "poseanim-thumbnail.png").write_bytes(r.images[0][0])


async def test_save_after_moving_the_posed_character(ed: Editor):
    """The hero moved while posing: a save right after writes the bones' world matrices and the
    skinned mesh bounds where the hero is now, byte for byte the file an unposed save writes."""
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", f"posing: {st}")
    await ed.call("set_transform", entity="Hero", position=[0.45, 0.0, 0.3])
    try:
        await ed.call("wait", seconds=0.3)
        ed.check(not close(await bone_rotation(ed), S["rest"]), "still posed after the move")
        await ed.call("save_scene", scene="Main Scene")
        posed = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
        # the same scene saved unposed: play mode leaves the session idle, the bones at rest
        await ed.call("play", action="start")
        await ed.call("play", action="stop")
        ed.check_eq((await state(ed))["mode"], "idle", "idle after play mode")
        await ed.call("wait", seconds=0.3)
        await ed.call("save_scene", scene="Main Scene")
        idle = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
        # the same text, numbers within float rounding: the render system recomputes the bounds
        # from skinning matrices that cancel the model's move up to the last bits
        a, b = idle.splitlines(), posed.splitlines()
        ed.check_eq(len(a), len(b), "the same lines")
        bad = [(x, y) for x, y in zip(a, b) if not same_line(x, y)]
        ed.check(not bad, f"the save right after the move writes what an unposed save writes: {bad[:6]}")
    finally:
        await ed.call("undo", scene="Main Scene")


async def test_scene_undo_while_posed_is_not_reverted_by_the_restore(ed: Editor):
    """A scene undo of a bone edit made before posing, done while posed: leaving posing restores
    what the undo left (the rest), not the transform saved when posing began."""
    await ed.call("select_entities", entities=["Ground"])
    ed.check_eq((await state(ed))["mode"], "idle", "not posing")
    ed.check(close(await bone_rotation(ed), S["rest"]), "rest")
    await ed.call("set_transform", entity=THIGH, rotation={"euler": [25, 0, 0]})
    edited = await bone_rotation(ed)
    ed.check(not close(edited, S["rest"]), f"the thigh turned: {edited}")
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", f"posing: {st}")
    await ed.call("wait", seconds=0.2)
    await ed.call("undo", scene="Main Scene")
    await ed.call("wait", seconds=0.2)
    # a selection change off the character leaves posing
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("select_entities", entities=["Ground"])
    ed.check_eq((await state(ed))["mode"], "idle", "posing left")
    back = await bone_rotation(ed)
    ed.check(close(back, S["rest"]), f"the undo stays: the rest, not the edit saved by posing: {back} vs rest {S['rest']}")


async def leave_posing(ed: Editor):
    # a selection change off the character leaves posing
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("select_entities", entities=["Ground"])
    ed.check_eq((await state(ed))["mode"], "idle", "posing left")


async def pose_again(ed: Editor):
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", f"posing: {st}")
    await ed.call("wait", seconds=0.2)


async def test_bone_edit_while_posed_is_recorded_on_the_rest(ed: Editor):
    """MCP set_transform on a posed bone records and edits the rest pose (never the posed values):
    its undo and redo while posed leave the rest and the edit, not the pose (re-review probe
    /tmp/f1rr-probe/test_probe_undo_leak.py)."""
    await leave_posing(ed)
    ed.check(close(await bone_rotation(ed), S["rest"]), "rest")
    await pose_again(ed)
    posed = await bone_rotation(ed)
    ed.check(not close(posed, S["rest"]), f"posed: {posed}")
    await ed.call("set_transform", entity=THIGH, rotation={"euler": [10, 0, 0]})
    await ed.call("wait", seconds=0.2)
    ed.check(not close(await bone_rotation(ed), S["rest"]), "still posed after the edit")
    await leave_posing(ed)
    edited = await bone_rotation(ed)
    ed.check(not close(edited, S["rest"]) and not close(edited, posed), f"the edit applied to the rest: {edited}")
    # undo while posed: the rest comes back, not the pose
    await pose_again(ed)
    await ed.call("undo", scene="Main Scene")
    await ed.call("wait", seconds=0.2)
    ed.check(not close(await bone_rotation(ed), S["rest"]), "posed again after the undo")
    await leave_posing(ed)
    back = await bone_rotation(ed)
    ed.check(close(back, S["rest"]), f"undo while posed: the rest: {back} vs rest {S['rest']} (posed {posed})")
    # redo while posed: the edit again, not the pose
    await pose_again(ed)
    await ed.call("redo", scene="Main Scene")
    await ed.call("wait", seconds=0.2)
    await leave_posing(ed)
    again = await bone_rotation(ed)
    ed.check(close(again, edited), f"redo while posed: the edit: {again} vs {edited} (posed {posed})")
    await ed.call("undo", scene="Main Scene")
    ed.check(close(await bone_rotation(ed), S["rest"]), "undone")


def entity_ids(result) -> list:
    """The ids of the GameObjects a find_entities result lists."""
    ids = []

    def walk(value):
        if isinstance(value, dict):
            if isinstance(value.get("id"), int) and "name" in value:
                ids.append(value["id"])
            for v in value.values():
                walk(v)
        elif isinstance(value, list):
            for v in value:
                walk(v)
    walk(result)
    return ids


async def test_copies_of_a_posed_character_get_its_rest_bones(ed: Editor):
    """MCP duplicate (the DuplicateEntityCmd every path shares) while the Hero is posed: the copy has
    the rest bones (final review, /tmp/fw-review/probe2.py)."""
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", f"posing: {st}")
    await ed.call("wait", seconds=0.2)
    ed.check(not close(await bone_rotation(ed), S["rest"]), "the Hero is posed")
    original = entity_ids(await ed.call("find_entities", name=THIGH))
    ed.check_eq(len(original), 1, f"one thigh: {original}")
    await ed.call("duplicate_entities", entity="Hero")
    try:
        thighs = entity_ids(await ed.call("find_entities", name=THIGH))
        copies = [i for i in thighs if i not in original]
        ed.check_eq(len(copies), 1, f"the copy's thigh: {thighs}")
        copied = harness.find_key(await ed.call("get_entity", entity=copies[0], detail="full"), "rotation")
        if isinstance(copied, dict):
            copied = copied.get("value", copied)
            copied = copied.get("quat", copied) if isinstance(copied, dict) else copied
        ed.check(close(copied, S["rest"]), f"the copy has the rest bones: {copied} vs {S['rest']}")
    finally:
        await ed.call("undo", scene="Main Scene")
