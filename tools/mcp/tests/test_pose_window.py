"""The Pose Animation window (spec: pose animation phase 4, section 4) on the pose fixture project:
focus_window opens it (window "pose_animation", pose {entity, path, index, play, tab}) and reports
its state; edits made through
edit_pose_animation while it is open show in it at once (it shares the document); undo of a pose
edit is a project undo; posing writes the hero's bone transforms without marking the scene
modified; preview play then entering play mode puts the rest transforms back; saving the scene while
posing writes the rest transforms (and posing resumes after the save).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess

import harness
from harness import Editor

# the harness owns the Xvfb so xdotool can press real keys in the editor window (keyboard undo).
# Selection changes are not undo steps here (Editor Settings > Undo, S24): leaving and re-entering
# posing selects, and these tests undo the edits made around it (test_undo covers selection steps).
EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness",
          "settings": {"undo": {"record_selection": False}}}
CI = True
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


async def test_bind_and_open(ed: Editor):
    S["rest"] = await bone_rotation(ed)
    await ed.call("edit_pose_animation", path=WALK, create={"preview_model": HERO}, ops=[
        {"op": "add_pose", "name": "Contact R", "angles": {"right_upper_leg.forward": 30, "left_upper_leg.forward": -20}},
        {"op": "add_pose", "name": "Passing R", "angles": {"left_upper_leg.forward": 30, "left_lower_leg.bend": 60}},
        {"op": "mirror_pose", "index": 0, "as_new": True, "name": "Contact L"},
        {"op": "add_pose", "name": "Passing L", "angles": {"right_upper_leg.forward": 30, "right_lower_leg.bend": 60}},
    ])
    r = await focus(ed, entity="Hero")
    ed.check_eq(r["state"]["character"], "Hero", f"bound: {r['state']}")
    ed.check("22/22" in r["state"]["rig"], "the rig chip reports every mapped slot")
    ed.check_eq(r["state"]["empty_state"], "no_animation", f"no animation yet: {r['state']}")
    r = await focus(ed, path=WALK)
    st = r["state"]
    ed.check_eq(st["animation"], WALK, "the document")
    ed.check_eq(st["pose_count"], 4, "four poses")
    ed.check_eq(st["mode"], "posing", "opening poses the first pose")
    ed.check_eq(st["primary"], 0, "pose 0")
    ed.check("empty_state" not in st, f"no empty state: {st}")
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


async def test_mapping_change_rebuilds_the_rig(ed: Editor):
    """Map Bones' undo/redo or MCP set_humanoid: the window's rig follows the sidecar."""
    st = (await focus(ed, index=0))["state"]
    ed.check_eq(st["mode"], "posing", "posing before the mapping changes")
    await ed.call("edit_pose_animation", path=WALK, ops=[{"op": "set_humanoid", "model": HERO, "bones": {"left_toes": ""}}])
    await ed.call("wait", seconds=0.6)
    st = await state(ed)
    ed.check("21/22" in st["rig"], f"the rig follows the sidecar: {st}")
    ed.check_eq(st["mode"], "posing", "still posing")
    await ed.call("undo", scope="project")
    await ed.call("wait", seconds=0.6)
    ed.check("22/22" in (await state(ed))["rig"], "undo: the mapping is back")


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


# bones the pose leaves at their rest value (a slot the pose does not turn, an end bone): an undo
# writing their old rest must not look like "no change" (second re-review probes,
# /tmp/f1rr3/probes/test_probe_equal.py and test_probe_equal2.py)
UNTOUCHED = ["mixamorig:Spine", "mixamorig:HeadTop_End", "mixamorig:LeftForeArm"]


async def test_undo_while_posed_of_an_edit_made_before_posing(ed: Editor):
    for bone in UNTOUCHED:
        await leave_posing(ed)
        rest = await bone_rotation(ed, bone)
        await ed.call("set_transform", entity=bone, rotation={"euler": [30, 0, 0]})
        edited = await bone_rotation(ed, bone)
        ed.check(not close(edited, rest), f"{bone}: edited {edited}")
        await pose_again(ed)
        await ed.call("undo", scene="Main Scene")
        await ed.call("wait", seconds=0.2)
        await leave_posing(ed)
        back = await bone_rotation(ed, bone)
        ed.check(close(back, rest), f"{bone}: the undo stays: {back} vs rest {rest} (edited {edited})")


async def test_undo_while_posed_of_an_edit_made_while_posed(ed: Editor):
    for bone in UNTOUCHED + [THIGH]:
        await leave_posing(ed)
        rest = await bone_rotation(ed, bone)
        await pose_again(ed)
        await ed.call("set_transform", entity=bone, rotation={"euler": [30, 0, 0]})
        await ed.call("wait", seconds=0.2)
        await leave_posing(ed)
        edited = await bone_rotation(ed, bone)
        ed.check(not close(edited, rest), f"{bone}: the edit applied to the rest: {edited}")
        await pose_again(ed)
        await ed.call("undo", scene="Main Scene")
        await ed.call("wait", seconds=0.2)
        await leave_posing(ed)
        back = await bone_rotation(ed, bone)
        ed.check(close(back, rest), f"{bone}: undo while posed gives the rest: {back} vs {rest} (edited {edited})")
        # and redo while posed: the edit again
        await pose_again(ed)
        await ed.call("redo", scene="Main Scene")
        await ed.call("wait", seconds=0.2)
        await leave_posing(ed)
        ed.check(close(await bone_rotation(ed, bone), edited), f"{bone}: redo while posed gives the edit")
        await ed.call("undo", scene="Main Scene")
        ed.check(close(await bone_rotation(ed, bone), rest), f"{bone}: undone")


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


def same_file(ed: Editor, a: str, b: str, what: str) -> None:
    """Two scene file texts are byte for byte the same (the diff's first lines otherwise)."""
    if a == b:
        return
    import difflib
    diff = list(difflib.unified_diff(a.splitlines(), b.splitlines(), lineterm="", n=0))
    ed.check(False, f"{what}: the files differ ({len(diff)} diff lines):\n" + "\n".join(diff[:24]))


async def save_text(ed: Editor) -> str:
    await ed.call("wait", seconds=0.3)
    await ed.call("save_scene", scene="Main Scene")
    return (ed.project / SCENE_FILE).read_text(encoding="utf-8")


async def bone_local(ed: Editor, name: str):
    full = await ed.call("get_entity", entity=name, detail="full")

    def value(key):
        v = harness.find_key(full, key)
        if isinstance(v, dict):
            v = v.get("value", v)
            v = v.get("quat", v) if isinstance(v, dict) else v
        return v
    return value("rotation"), value("position")


def run_x(ed: Editor, *args: str, capture: bool = False) -> bytes:
    if not shutil.which("xdotool"):
        harness.skip("xdotool is not installed")
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    done = subprocess.run(list(args), env=env, check=True, capture_output=True, timeout=20)
    return done.stdout if capture else b""


async def press(ed: Editor, combo: str):
    """A real key chord to the editor's X window: modifiers down, key down/up, modifiers up (one
    xdotool `key ctrl+z` can land in one frame with the release applied first)."""
    ids = run_x(ed, "xdotool", "search", "--onlyvisible", "--name", "JasperEngine", capture=True).split()
    if not ids:
        ed.fail("the editor's X window was not found")
    run_x(ed, "xdotool", "windowfocus", "--sync", ids[-1].decode())
    *modifiers, last = combo.split("+")
    for modifier in modifiers:
        run_x(ed, "xdotool", "keydown", "--clearmodifiers", modifier)
    run_x(ed, "xdotool", "keydown", last)
    run_x(ed, "xdotool", "keyup", last)
    for modifier in reversed(modifiers):
        run_x(ed, "xdotool", "keyup", modifier)
    await ed.call("wait", frames=6)


async def test_keyboard_undo_while_posed_saves_consistent_bones(ed: Editor):
    """Ctrl+Z with the Hierarchy focused (App::show's undo, in the frame the session then saves the
    bones again, before the transform system ran): a save while posed and one after leaving posing
    are what the scene saved before the edit (the bones' world matrices recomputed when saved).
    Real key events through xdotool on the harness' Xvfb (re-review probe test_probe_kbd_undo.py)."""
    bone = "mixamorig:LeftForeArm"
    await leave_posing(ed)
    before = await save_text(ed)
    await pose_again(ed)
    await ed.call("set_transform", entity=bone, rotation={"euler": [30, 0, 0]})
    await ed.call("wait", seconds=0.3)
    await ed.call("focus_window", window="hierarchy")
    await ed.call("wait", seconds=0.3)
    await press(ed, "ctrl+z")
    await ed.call("wait", seconds=0.5)
    ed.check_eq((await state(ed))["mode"], "posing", "still posing after the undo")
    posed = await save_text(ed)
    await leave_posing(ed)
    after = await save_text(ed)
    same_file(ed, before, after, "the keyboard undo took the edit back")
    same_file(ed, after, posed, "the save while posed after the keyboard undo")


async def test_prefab_revert_of_a_posed_bone(ed: Editor):
    """A prefab override revert on a posed bone writes its rest (the pose's restore does not put the
    edit back), and the scene saves as the unedited instance."""
    bone = "mixamorig:LeftForeArm"
    await leave_posing(ed)
    await ed.call("create_prefab", entity="Hero", path="assets/prefabs/pose_hero.prefab")
    try:
        rest = await bone_local(ed, bone)
        base = await save_text(ed)
        await pose_again(ed)
        await ed.call("set_transform", entity=bone, rotation={"euler": [30, 0, 0]})
        await ed.call("wait", seconds=0.3)
        await ed.call("prefab_overrides", action="revert", entity=bone, component="transform")
        await ed.call("wait", seconds=0.3)
        ed.check_eq((await state(ed))["mode"], "posing", "still posing after the revert")
        await leave_posing(ed)
        back = await bone_local(ed, bone)
        ed.check(close(back[0], rest[0]), f"reverted to the prefab's rest: {back} vs {rest}")
        same_file(ed, base, await save_text(ed), "the reverted instance")
    finally:
        await ed.call("select_entities", entities=["Ground"])
        await ed.call("undo", scene="Main Scene", steps=3)


async def test_reparenting_a_posed_bone_keeps_its_rest_world(ed: Editor):
    """Moving a posed bone in the hierarchy (MoveEntityOrderCmd, keep world: joints can only be
    reordered among their siblings) recomputes its local transform from the rest world, as the same
    move made without posing does: the same transform and the same saved file."""
    bone = THIGH
    await leave_posing(ed)
    await ed.call("move_entity", entity=bone, before="mixamorig:LeftUpLeg")
    idle = await bone_local(ed, bone)
    idle_text = await save_text(ed)
    await ed.call("undo", scene="Main Scene")
    await pose_again(ed)
    ed.check(not close((await bone_local(ed, bone))[0], idle[0]), "the thigh is posed")
    await ed.call("move_entity", entity=bone, before="mixamorig:LeftUpLeg")
    await ed.call("wait", seconds=0.3)
    try:
        await leave_posing(ed)
        moved = await bone_local(ed, bone)
        ed.check(close(moved[0], idle[0], 1e-4) and all(abs(a - b) <= 1e-4 for a, b in zip(moved[1], idle[1])),
                 f"the local transform from the rest world: {moved} vs {idle}")
        same_file(ed, idle_text, await save_text(ed), "the reparented bone")
    finally:
        await ed.call("select_entities", entities=["Ground"])
        await ed.call("undo", scene="Main Scene")


async def test_moving_into_a_prop_held_by_a_posed_bone(ed: Editor):
    """A GameObject moved INTO a prop hanging from a posed bone (keep world): its new local is taken
    against the prop's rest world, as the same move made without posing."""
    await leave_posing(ed)
    await ed.call("create_entity", name="Prop", parent="mixamorig:RightLeg", position=[0.1, 0.0, 0.0])
    await ed.call("create_entity", name="Item", position=[0.3, 1.0, 0.2])
    try:
        await ed.call("move_entity", entity="Item", parent="Prop")
        idle = await bone_local(ed, "Item")
        idle_text = await save_text(ed)
        await ed.call("undo", scene="Main Scene")
        await pose_again(ed)
        await ed.call("move_entity", entity="Item", parent="Prop")
        await ed.call("wait", seconds=0.3)
        await leave_posing(ed)
        moved = await bone_local(ed, "Item")
        ed.check(close(moved[0], idle[0], 1e-4) and all(abs(a - b) <= 1e-4 for a, b in zip(moved[1], idle[1])),
                 f"the local against the prop's rest world: {moved} vs {idle}")
        same_file(ed, idle_text, await save_text(ed), "the item moved into the prop")
        await ed.call("undo", scene="Main Scene")
    finally:
        await ed.call("select_entities", entities=["Ground"])
        await ed.call("undo", scene="Main Scene", steps=2)


async def test_undoing_a_move_into_a_prop_held_by_a_posed_bone(ed: Editor):
    """The undo of that move while posed (Item back to the scene root, keeping its world): the world under
    the prop's rest, so Item is back where it was. The undo suspends the pose before the step, which puts
    the bones back but not what hangs below them: MoveEntityOrderCmd's undo recomputes Item's chain from
    the rest before the move reads it (merge of animation-clips: prepareReparent for execute and undo)."""
    await leave_posing(ed)
    await ed.call("create_entity", name="Prop", parent="mixamorig:RightLeg", position=[0.1, 0.0, 0.0])
    await ed.call("create_entity", name="Item", position=[0.3, 1.0, 0.2])
    try:
        before = await bone_local(ed, "Item")
        await pose_again(ed)
        await ed.call("move_entity", entity="Item", parent="Prop")
        await ed.call("wait", seconds=0.3)
        await ed.call("undo", scene="Main Scene")
        await ed.call("wait", seconds=0.3)
        ed.check_eq((await state(ed))["mode"], "posing", "still posing after the undo")
        await leave_posing(ed)
        after = await bone_local(ed, "Item")
        ed.check(close(after[0], before[0], 1e-4) and all(abs(a - b) <= 1e-4 for a, b in zip(after[1], before[1])),
                 f"Item back where it was: {after} vs {before}")
    finally:
        await ed.call("select_entities", entities=["Ground"])
        await ed.call("undo", scene="Main Scene", steps=2)


async def test_moving_a_prop_off_a_posed_bone_keeps_its_rest_world(ed: Editor):
    """A GameObject held by a posed bone (a prop under the right leg, whose world the posed thigh
    moves) reparented to the scene root keeping its world: the rest world, as without posing."""
    await leave_posing(ed)
    await ed.call("create_entity", name="Prop", parent="mixamorig:RightLeg", position=[0.1, 0.0, 0.0])
    try:
        await ed.call("move_entity", entity="Prop", parent=None)
        idle = await bone_local(ed, "Prop")
        idle_text = await save_text(ed)
        await ed.call("undo", scene="Main Scene")
        await pose_again(ed)
        await ed.call("move_entity", entity="Prop", parent=None)
        await ed.call("wait", seconds=0.3)
        await leave_posing(ed)
        moved = await bone_local(ed, "Prop")
        ed.check(close(moved[0], idle[0], 1e-4) and all(abs(a - b) <= 1e-4 for a, b in zip(moved[1], idle[1])),
                 f"the prop's world is the rest's: {moved} vs {idle}")
        same_file(ed, idle_text, await save_text(ed), "the moved prop")
        await ed.call("undo", scene="Main Scene")
    finally:
        await ed.call("select_entities", entities=["Ground"])
        await ed.call("undo", scene="Main Scene")


async def test_delete_goes_to_the_window_not_the_scene(ed: Editor):
    """S61 AC2: Delete in the focused window removes its pose, never the selected Hero."""
    await ed.call("select_entities", entities=["Hero"])
    await focus(ed, entity="Hero")
    st = (await focus(ed, path=WALK))["state"]
    poses = st["pose_count"]
    ed.check(poses > 1, f"poses to delete: {st}")
    heroes = len(harness.find_key(await ed.call("find_entities", name="Hero*"), "entities") or [])
    await press(ed, "Delete")
    ed.check_eq((await state(ed))["pose_count"], poses - 1, "Delete removed the pose")
    ed.check_eq(len(harness.find_key(await ed.call("find_entities", name="Hero*"), "entities") or []), heroes, "the GameObject stays")
    await ed.call("undo", scope="project")
    ed.check_eq((await state(ed))["pose_count"], poses, "one undo step")
