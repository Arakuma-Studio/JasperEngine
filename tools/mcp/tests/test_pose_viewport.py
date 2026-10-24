"""Pose animation phase 5 (spec: docs/superpowers/specs/2026-10-23-pose-animation-phase5-viewport-design.md,
section 7): posing in the Scene view on the pose fixture hero.

Bind + posing shows the handles (the screenshot differs from Idle in the handle colours), the floor
with contact rings and the onion ghost, and never marks the scene modified or adds GameObjects. A
hand drag (IK) commits one project undo step; a ring drag stops at the joint limit and Ctrl snaps
it to 5 degrees; the hips drag keeps the feet planted; Global Edit shows the orange baseline ghost
and spreads the change to the other poses in one undo step; previewing hides the handles; play
mode, saving the scene and selecting another object each restore the rest bones and drop the
ghosts.

The Scene view is driven through focus_window pose_animation's `pose.viewport` test hook, which
calls the same SceneRender mouse functions as SceneWindow (send_input only reaches a running game).
Screenshots go to $POSE_SHOTS (default /tmp/pose-research/reports/D3-shots) when that folder exists.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import harness
from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

HERO = "assets/models/hero_tpose.gltf"
WALK = "assets/anim/walk.poseanim"
SCENE = "Main Scene"
SHOTS = Path(os.environ.get("POSE_SHOTS", "/tmp/pose-research/reports/D3-shots/fix1"))

HIPS_YELLOW = (0xFF, 0xD5, 0x4F)
ORANGE_GHOST = (0xFF, 0x9F, 0x43)
CAMERA = {"position": [0.55, 1.35, 2.35], "target": [-0.75, 0.8, 0.0]}   # the hero from its front left

END_HANDLES = ("left_hand", "right_hand", "left_foot", "right_foot", "hips")
JOINT_DOTS = ("spine", "chest", "upper_chest", "neck", "head", "left_shoulder", "right_shoulder", "left_upper_arm",
              "right_upper_arm", "left_lower_arm", "right_lower_arm", "left_upper_leg", "right_upper_leg",
              "left_lower_leg", "right_lower_leg", "left_toes", "right_toes")


# -- helpers -------------------------------------------------------------------

def shot_path(name: str) -> Path | None:
    return SHOTS / f"{name}.png" if SHOTS.is_dir() else None


async def scene_shot(ed: Editor, name: str) -> bytes:
    return await ed.screenshot("scene", shot_path(name) or Path(ed.scratch.root) / f"{name}.png")


def pixel(png: bytes, view: list, point: list) -> tuple:
    """The screenshot pixel at a point in Scene view pixels (the image may be scaled)."""
    w, h, ch, rows = decode_png(png)
    x = min(w - 1, max(0, int(point[0] * w / view[0])))
    y = min(h - 1, max(0, int(point[1] * h / view[1])))
    return tuple(rows[y][x * ch:x * ch + 3])


def near(a, b, tol) -> bool:
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def changed_fraction(a: bytes, b: bytes, tol: int = 24) -> float:
    """The share of pixels that differ by more than tol in a channel"""
    wa, ha, ca, ra = decode_png(a)
    wb, hb, cb, rb = decode_png(b)
    assert (wa, ha) == (wb, hb), "same size"
    changed = sum(1 for y in range(ha) for x in range(wa)
                  if any(abs(ra[y][x * ca + k] - rb[y][x * cb + k]) > tol for k in range(3)))
    return changed / float(wa * ha)


def max_warmth(png: bytes, view: list, point: list, radius: int = 7) -> int:
    """The largest red-minus-blue of the pixels around a point (orange shows, grey does not)"""
    w, h, ch, rows = decode_png(png)
    cx, cy = int(point[0] * w / view[0]), int(point[1] * h / view[1])
    best = -255
    for y in range(max(0, cy - radius), min(h, cy + radius + 1)):
        for x in range(max(0, cx - radius), min(w, cx + radius + 1)):
            best = max(best, rows[y][x * ch] - rows[y][x * ch + 2])
    return best


def count_near(png: bytes, color, tol: int, step: int = 2) -> int:
    w, h, ch, rows = decode_png(png)
    return sum(1 for y in range(0, h, step) for x in range(0, w, step) if near(rows[y][x * ch:x * ch + 3], color, tol))


async def window_state(ed: Editor, pose: dict | None = None) -> dict:
    args = {"window": "pose_animation"}
    if pose is not None:
        args["pose"] = pose
    return await ed.call("focus_window", args)


async def viewport_or_none(ed: Editor):
    return (await window_state(ed))["state"].get("viewport")


async def viewport(ed: Editor) -> dict:
    """What the Scene view shows (as of its last draw: one is forced first)"""
    await ed.call("screenshot", view="scene", max_width=64)
    return (await window_state(ed))["state"].get("viewport") or {}


async def hook(ed: Editor, **viewport_input) -> dict:
    return await window_state(ed, {"viewport": viewport_input})


async def angles(ed: Editor, index: int) -> dict:
    data = await ed.call("get_pose_animation", path=WALK)
    return data["poses"][index].get("angles", {})


async def pose_data(ed: Editor, index: int) -> dict:
    data = await ed.call("get_pose_animation", path=WALK)
    return data["poses"][index]


# The session puts the bones' derived matrices back with them; the skinned mesh bounds it does not
# yet (the window task's fix): only those are left out of the comparison
DERIVED = re.compile(r"^\s*(min|max):")


def authored(scene_text: str) -> list:
    """The scene file without the mesh bounds (min/max), which a save right after the bones are
    put back may write before they are recomputed"""
    return [line for line in scene_text.splitlines() if not DERIVED.match(line)]


def scene_modified(scenes: dict) -> bool:
    for s in scenes["scenes"]:
        if s["name"] == SCENE:
            return bool(s.get("modified"))
    raise AssertionError(f"no scene {SCENE}: {scenes}")


async def entity_count(ed: Editor) -> int:
    for s in (await ed.call("list_scenes"))["scenes"]:
        if s["name"] == SCENE:
            return int(s["entity_count"])
    raise AssertionError(f"no scene {SCENE}")


async def bone_transform(ed: Editor, name: str):
    """The bone's rotation as get_entity reports it."""
    return harness.find_key(await ed.call("get_entity", entity=name, detail="full"), "rotation")


def drag_path(start: list, dx: float, dy: float, steps: int = 6) -> list:
    return [[start[0] + dx * i / steps, start[1] + dy * i / steps] for i in range(1, steps + 1)]


async def undo_project(ed: Editor) -> None:
    await ed.call("undo", scope="project", steps=1)


async def redo_project(ed: Editor) -> None:
    await ed.call("redo", scope="project", steps=1)


async def setup_posing(ed: Editor, index: int = 1) -> dict:
    """A three pose walk on the hero, posing `index`, the Scene view framing the hero."""
    exists = (ed.project / WALK).exists()
    if not exists:
        await ed.call("edit_pose_animation", path=WALK, create={"preview_model": HERO}, ops=[
            {"op": "add_pose", "name": "Contact R", "angles": {
                "right_upper_leg.forward": 25, "left_upper_leg.forward": -15, "left_upper_arm.raise": 8,
                "right_upper_arm.raise": 8, "left_upper_arm.forward": -20, "right_upper_arm.forward": 20}},
            {"op": "add_pose", "name": "Down R", "add_angles": {"right_lower_leg.bend": 25, "left_upper_arm.forward": 45}},
            {"op": "add_pose", "name": "Pass R", "angles": {"left_upper_arm.raise": 6, "right_upper_arm.raise": 6,
                                                            "left_upper_leg.forward": 10, "right_lower_leg.bend": 10}},
        ])
    await ed.call("select_entities", scene=SCENE, entities=[])
    await ed.call("set_scene_camera", scene=SCENE, **CAMERA)
    await window_state(ed, {"viewport": {"global_edit": "cancel", "onion": {"previous": True, "next": False},
                                         "ghost_during_preview": False}} if (await viewport_or_none(ed)) else None)
    r = await window_state(ed, {"entity": "Hero", "path": WALK, "index": index})
    ed.check_eq(r["state"]["mode"], "posing", "selecting a pose poses it")
    # the Scene view draws (and the ghost material compiles)
    await ed.screenshot("scene", Path(ed.scratch.root) / "warmup.png")
    return r


# -- tests ---------------------------------------------------------------------

async def test_posing_shows_handles_floor_and_onion(ed: Editor):
    await ed.call("set_scene_camera", scene=SCENE, **CAMERA)
    before_entities = await entity_count(ed)
    idle = await scene_shot(ed, "00_idle")
    await setup_posing(ed)
    vp = await viewport(ed)
    ed.check(vp.get("posing"), f"the Scene view poses: {vp}")
    for name in END_HANDLES + JOINT_DOTS:
        ed.check_in(name, vp["handles"], "handles")
    ed.check(vp["floor"], "the floor disc shows")
    ed.check_eq(vp["ghosts"], ["previous"], "the onion skin shows the previous pose")
    ed.check(vp["contacts"], f"a foot on the floor gets a contact ring: {vp}")
    ed.check(vp["balanced"] is not None, "the balance marker shows by default")
    posing = await scene_shot(ed, "01_posing_handles_floor_onion")
    view = vp["view"]
    hips = vp["handles"]["hips"]
    ed.check(near(pixel(posing, view, hips), HIPS_YELLOW, 70),
             f"the hips cube is yellow: {pixel(posing, view, hips)} at {hips}")
    ed.check(not near(pixel(idle, view, hips), HIPS_YELLOW, 70), f"no cube when idle: {pixel(idle, view, hips)}")
    ed.check(count_near(posing, HIPS_YELLOW, 40) > count_near(idle, HIPS_YELLOW, 40), "the handle colours appear")
    # nothing of it is saved or listed
    ed.check(not scene_modified(await ed.call("list_scenes")), "posing does not mark the scene modified")
    ed.check_eq(await entity_count(ed), before_entities, "no GameObjects added (ghosts and overlays are the editor's)")


async def test_ik_drag_commits_one_undo_step(ed: Editor):
    await setup_posing(ed)
    vp = await viewport(ed)
    hand = vp["handles"]["left_hand"]
    before = await angles(ed, 1)
    other = await angles(ed, 0)
    r = await hook(ed, handle="left_hand", drag=drag_path(hand, 40, -60))
    ed.check(r["viewport_input"]["pressed"], f"the hand handle took the press: {r['viewport_input']}")
    after = await angles(ed, 1)
    ed.check(after != before, "the drag changed the pose")
    moved = [k for k in after if k.startswith("left_") and "arm" in k and abs(after.get(k, 0) - before.get(k, 0)) > 1]
    ed.check(moved, f"the left arm bent (IK): {before} -> {after}")
    ed.check_eq(await angles(ed, 0), other, "the other poses are untouched")
    moved_hand = (await viewport(ed))["handles"]["left_hand"]
    ed.check(abs(moved_hand[0] - hand[0]) + abs(moved_hand[1] - hand[1]) > 8, f"the hand handle followed: {hand} -> {moved_hand}")
    await scene_shot(ed, "02_after_hand_drag")
    ed.check(not scene_modified(await ed.call("list_scenes")), "a drag does not mark the scene modified")
    # the whole drag is one undo step
    await undo_project(ed)
    ed.check_eq(await angles(ed, 1), before, "one undo restores the pose")
    await redo_project(ed)
    ed.check_eq(await angles(ed, 1), after, "redo applies the drag again")
    await undo_project(ed)


async def test_rings_stop_at_limits_and_snap(ed: Editor):
    await setup_posing(ed)
    r = await hook(ed, joint="left_lower_arm")
    vp = r["state"]["viewport"]
    ed.check_eq(vp["active_joint"], "left_lower_arm", "the clicked joint is active")
    for ring in ("ring_l", "ring_u", "ring_f", "ring_free"):
        ed.check_in(ring, vp["handles"], "rings of the active joint")
    await scene_shot(ed, "03_rings_active_joint")
    start = await angles(ed, 1)
    base = start.get("left_lower_arm.bend", 0.0)
    # turn the elbow ring far both ways: it stops at the limit (0..155) instead of passing it
    results = []
    committed = 0
    for direction in (1, -1):
        previous = await angles(ed, 1)
        vp = await viewport(ed)
        ring = vp["handles"]["ring_l"]
        center = vp["handles"]["left_lower_arm"]
        # around the ring's centre, a quarter turn and more, in small steps
        dx, dy = ring[0] - center[0], ring[1] - center[1]
        import math
        radius = math.hypot(dx, dy)
        a0 = math.atan2(dy, dx)
        path = [[center[0] + radius * math.cos(a0 + direction * math.radians(15 * i)),
                 center[1] + radius * math.sin(a0 + direction * math.radians(15 * i))] for i in range(1, 15)]
        r = await hook(ed, handle="ring_l", drag=path)
        ed.check(r["viewport_input"]["pressed"], f"the ring took the press: {r['viewport_input']}")
        now = await angles(ed, 1)
        bend = now.get("left_lower_arm.bend", 0.0)
        ed.check(-0.01 <= bend <= 155.01, f"the elbow stays within its limits: {bend}")
        results.append(bend)
        committed += now != previous   # a drag stopped at once by the limit commits nothing
        if now != previous and committed == 1:
            await scene_shot(ed, "04_ring_dragged_limit_ticks")
    ed.check(any(abs(b - base) > 5 for b in results), f"a ring drag turned the elbow: {base} -> {results}")
    # Ctrl: 5 degree steps (from a straight elbow, the L ring turns the bend alone)
    for _ in range(committed):
        await undo_project(ed)
    ed.check_eq(await angles(ed, 1), start, "the ring drags undo")
    vp = await viewport(ed)
    ring = vp["handles"]["ring_l"]
    center = vp["handles"]["left_lower_arm"]
    import math
    radius = math.hypot(ring[0] - center[0], ring[1] - center[1])
    a0 = math.atan2(ring[1] - center[1], ring[0] - center[0])
    snapped = None
    for direction in (1, -1):
        path = [[center[0] + radius * math.cos(a0 + direction * math.radians(7 * i)),
                 center[1] + radius * math.sin(a0 + direction * math.radians(7 * i))] for i in range(1, 6)]
        await hook(ed, handle="ring_l", drag=path, ctrl=True)
        bend = (await angles(ed, 1)).get("left_lower_arm.bend", 0.0)
        if abs(bend - base) > 1:
            snapped = bend
            break
    ed.check(snapped is not None, "a snapped ring drag turned the elbow")
    ed.check(abs(snapped / 5 - round(snapped / 5)) < 0.02, f"Ctrl snaps to 5 degrees: {snapped}")
    await undo_project(ed)
    ed.check_eq(await angles(ed, 1), start, "the snapped drag undoes")


async def test_hips_drag_keeps_feet_planted(ed: Editor):
    await setup_posing(ed, index=2)
    vp = await viewport(ed)
    hips = vp["handles"]["hips"]
    feet_before = [vp["handles"]["left_foot"], vp["handles"]["right_foot"]]
    before = await pose_data(ed, 2)
    r = await hook(ed, handle="hips", drag=drag_path(hips, 0, 25, 5))
    ed.check(r["viewport_input"]["pressed"], "the hips cube took the press")
    after = await pose_data(ed, 2)
    a, b = after.get("angles", {}), before.get("angles", {})
    knees = sum(abs(a.get(k, 0) - b.get(k, 0)) for k in ("left_lower_leg.bend", "right_lower_leg.bend"))
    ed.check(knees > 5, f"lowering the hips bends the knees: {b} -> {a}")
    feet = after["metrics"]["feet"]
    ed.check(feet["left"]["contact"] or feet["right"]["contact"], f"the pose stays on the floor: {after['metrics']}")
    vp = await viewport(ed)
    for name, start in zip(("left_foot", "right_foot"), feet_before):
        now = vp["handles"][name]
        ed.check(abs(now[0] - start[0]) <= 1.5 and abs(now[1] - start[1]) <= 1.5,
                 f"the {name} stays where it was: {start} -> {now}")
    ed.check(vp["handles"]["hips"][1] > hips[1] + 10, f"the hips went down: {hips} -> {vp['handles']['hips']}")
    await scene_shot(ed, "05_hips_lowered")
    await undo_project(ed)


async def test_global_edit_ghost_and_apply(ed: Editor):
    await setup_posing(ed)
    await hook(ed, onion={"previous": False})
    no_ghost = await scene_shot(ed, "06_before_global_edit_no_onion")
    r = await hook(ed, global_edit="begin")
    ed.check(r["viewport_input"]["global_edit"], "Global Edit began")
    vp = r["state"]["viewport"]
    ed.check(vp["global_edit"], "the session is in Global Edit")
    ed.check_eq(vp["ghosts"], ["global_edit"], "the orange baseline replaces the onion skin")
    # the baseline coincides with the model: the model wins (depth-tested, pushed back)
    unchanged = await scene_shot(ed, "06a_global_edit_nothing_changed")
    fraction = changed_fraction(no_ghost, unchanged)
    ed.check(fraction < 0.002, f"an unchanged baseline ghost is hidden by the model ({fraction:.4%} of the pixels changed)")
    poses_before = [await angles(ed, i) for i in range(3)]
    hand = vp["handles"]["left_hand"]
    shoulder = vp["handles"]["left_upper_arm"]
    await hook(ed, handle="left_hand", drag=drag_path(hand, 45, -50))
    shot = await scene_shot(ed, "06_global_edit_baseline_ghost")
    # where the hand was, the baseline's orange forearm shows over the background now
    old_forearm = [(hand[0] * 2 + shoulder[0]) / 3, (hand[1] * 2 + shoulder[1]) / 3]
    view = vp["view"]
    ed.check(max_warmth(shot, view, hand) > 25 or max_warmth(shot, view, old_forearm) > 25,
             f"the orange ghost shows where the arm was: {max_warmth(shot, view, hand)}, {max_warmth(shot, view, old_forearm)}")
    ed.check(changed_fraction(no_ghost, shot) > 0.002, "the change shows")
    ed.check((await viewport(ed))["global_edit"], "a drag keeps Global Edit on")
    r = await hook(ed, global_edit="apply_all")
    ed.check(r["viewport_input"]["global_edit"], "applied")
    vp = r["state"]["viewport"]
    ed.check(not vp["global_edit"], "Global Edit ends after applying")
    ed.check_eq(vp["ghosts"], [], "the onion skin is back (switched off here)")
    poses_after = [await angles(ed, i) for i in range(3)]
    for i in (0, 2):
        ed.check(poses_after[i] != poses_before[i], f"pose {i} got the change: {poses_before[i]} -> {poses_after[i]}")
    # one step: the apply; one more: the drag
    await undo_project(ed)
    for i in (0, 2):
        ed.check_eq(await angles(ed, i), poses_before[i], f"one undo restores pose {i}")
    await undo_project(ed)
    ed.check_eq(await angles(ed, 1), poses_before[1], "the drag undoes too")
    await hook(ed, onion={"previous": True})


async def test_next_pose_and_preview_ghosts(ed: Editor):
    await setup_posing(ed)
    r = await hook(ed, onion={"previous": True, "next": True})
    await scene_shot(ed, "08_onion_previous_and_next")
    ed.check_eq((await viewport(ed))["ghosts"], ["previous", "next"], "both onion ghosts")
    # the robot in front of the hero hides the ghosts too (they are depth-tested against the scene)
    await ed.call("set_scene_camera", scene=SCENE, position=[2.6, 1.1, 0.9], target=[-0.75, 0.8, 0.0])
    await scene_shot(ed, "10_ghosts_behind_the_robot")
    await ed.call("set_scene_camera", scene=SCENE, **CAMERA)
    await hook(ed, onion={"previous": True, "next": False}, ghost_during_preview=True)
    r = await window_state(ed, {"play": True})
    await window_state(ed, {"play": False})
    await window_state(ed, {"play": True})
    vp = await viewport(ed)
    ed.check(vp["previewing"], f"previewing: {vp}")
    ed.check_eq(vp["ghosts"], ["preview"], "the key pose being left as a ghost")
    await window_state(ed, {"play": False})
    await hook(ed, ghost_during_preview=False)


async def test_preview_hides_handles(ed: Editor):
    await setup_posing(ed)
    r = await window_state(ed, {"play": True})
    vp = r["state"]["viewport"]
    ed.check(vp["previewing"] and not vp["posing"], f"previewing: {vp}")
    ed.check_eq(vp["handles"], {}, "no handles while previewing")
    ed.check_eq(vp["handle_count"], 0, "no handles while previewing")
    await scene_shot(ed, "07_preview")
    r = await window_state(ed, {"play": False})
    ed.check(r["state"]["viewport"]["posing"], "stop poses again")


async def test_clicks_off_the_model_leave_posing(ed: Editor):
    await setup_posing(ed)
    vp = await viewport(ed)
    # a click on the hero's body (between the spine and hips handles, off to the side): still posing
    spine, hips = vp["handles"]["spine"], vp["handles"]["hips"]
    body = [(spine[0] + hips[0]) / 2 + 10, (spine[1] + hips[1]) / 2]
    r = await hook(ed, click=body)
    ed.check(not r["viewport_input"]["left_posing"], f"a click on the model keeps posing: {r['viewport_input']}")
    ed.check_eq(r["state"]["mode"], "posing", "posing")
    # empty sky, with nothing selected before or after: posing ends anyway
    r = await hook(ed, click=[12, 12])
    ed.check(r["viewport_input"]["left_posing"], f"a click on empty space leaves posing: {r['viewport_input']}")
    ed.check_eq(r["state"]["mode"], "idle", "idle")
    ed.check(not (await viewport(ed)).get("ghosts"), "the ghosts went")


async def test_closing_the_window_restores_bones(ed: Editor):
    await setup_posing(ed)
    posed = await bone_transform(ed, "mixamorig:LeftArm")
    r = await hook(ed, close_window=True)
    ed.check(r["viewport_input"].get("closed"), "the window closed")
    rest = await bone_transform(ed, "mixamorig:LeftArm")
    ed.check(rest != posed, f"the bones went back: {posed} -> {rest}")
    vp = await viewport(ed)
    ed.check(not vp.get("posing") and not vp.get("ghosts"), f"no posing or ghosts after closing: {vp}")
    await window_state(ed)   # open again for the next tests


async def test_exit_paths_restore_bones_and_drop_ghosts(ed: Editor):
    await setup_posing(ed)
    # the rest transform of a posed bone, as the scene has it while idle
    await window_state(ed, {"index": 1})
    posed = await bone_transform(ed, "mixamorig:LeftArm")
    entities = await entity_count(ed)

    # saving the scene while posing writes the rest bones (the file a save while idle writes)
    # and keeps posing
    await window_state(ed, {"play": True})
    await window_state(ed, {"play": False})
    await ed.call("select_entities", scene=SCENE, entities=["Robot"])   # leaves posing
    ed.check_eq((await window_state(ed))["state"]["mode"], "idle", "selecting another object after a preview leaves posing")
    await ed.call("save_scene", scene=SCENE)
    idle_file = (ed.project / f"{SCENE}.scene").read_text(encoding="utf-8")
    await ed.call("select_entities", scene=SCENE, entities=[])
    await window_state(ed, {"index": 1})
    vp = await viewport(ed)
    ed.check(vp.get("posing"), f"posing again: {vp}")
    await ed.call("save_scene", scene=SCENE)
    vp = await viewport(ed)
    ed.check(vp["posing"] and vp["ghosts"], f"still posing after a save: {vp}")
    ed.check(not scene_modified(await ed.call("list_scenes")), "saved and unmodified")
    saved = (ed.project / f"{SCENE}.scene").read_text(encoding="utf-8")
    if saved != idle_file:
        (Path(ed.scratch.root) / "idle.scene").write_text(idle_file, encoding="utf-8")
        (Path(ed.scratch.root) / "posing.scene").write_text(saved, encoding="utf-8")
    ed.check(authored(saved) == authored(idle_file),
             f"saving while posing writes the rest bones (diff {ed.scratch.root}/idle.scene posing.scene)")

    # play mode: the session goes idle, the bones go back, the ghosts go
    await ed.call("play", action="start")
    await ed.call("wait", frames=5)
    vp = await viewport(ed)
    ed.check(not vp.get("posing") and not vp.get("ghosts"), f"no posing or ghosts in play mode: {vp}")
    await ed.call("play", action="stop")
    await window_state(ed)
    rest = await bone_transform(ed, "mixamorig:LeftArm")
    ed.check(rest != posed, "the bone was posed before and is at rest now")
    ed.check_eq(await entity_count(ed), entities, "no ghost GameObjects in the scene")

    # selecting another object leaves posing (bones back, ghosts dropped)
    await window_state(ed, {"index": 1})
    ed.check((await viewport(ed)).get("posing"), "posing again")
    await ed.call("select_entities", scene=SCENE, entities=["Robot"])
    await window_state(ed)
    r = await window_state(ed)
    ed.check_eq(r["state"]["mode"], "idle", "another selection leaves posing")
    vp = r["state"].get("viewport") or {}
    ed.check(not vp.get("ghosts"), f"the ghosts are gone: {vp}")
    ed.check_eq(await bone_transform(ed, "mixamorig:LeftArm"), rest, "the bones are back at rest")
    ed.check("mixamorig:LeftArm" in saved, "the saved scene has the bones")

