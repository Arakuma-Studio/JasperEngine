# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""S77 R32-R35, R57 (docs/specs/S77-clip-quality.md): the constraint fields soften, hingeAxis, twist
and aimOffset through the whole path, on a scratch copy of the pose-animation fixture:

- old data keeps its bytes: at their defaults the fields are not written;
- set_properties and undo, the Inspector's rows with their Unity-like names;
- the runtime: get_animator's `reached`, control_animator's live fields (play mode only), Lua's
  isIKReached and setIK* through eval_lua.
"""

from __future__ import annotations

import json

import harness
from harness import Editor, REPO

CI = True
TIMEOUT = 600
EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}

CONTROLLER = "assets/anim/ik.controller"
IDLE = "assets/models/hero_animated.gltf#Idle"
ARM = ["left_upper_arm", "left_lower_arm", "left_hand"]
NEW_KEYS = ("soften", "hingeAxis", "twist", "aimOffset")
ANIMATOR = "object('Hero'):getAnimator()"


def close(a, b, eps=1e-4) -> bool:
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) <= eps


async def fields(ed: Editor, tool: str = "get_entity") -> dict:
    """The new fields of both constraints, flat: {"ik[0].soften": ...}"""
    names = [f"ik[0].{k}" for k in ("soften", "hingeAxis", "twist")] + ["ik[1].aimOffset"]
    args = {"entity": "Hero", "components": ["animator"], "properties": names}
    if tool == "get_runtime_entity":
        args["include_script_state"] = False
    got = await ed.call(tool, **args)
    return got["components"]["animator"]


def holds(ed: Editor, got: dict, soften, axis, twist, offset, what: str) -> None:
    ed.check(close(got["ik[0].soften"], soften) and close(got["ik[0].hingeAxis"], axis) and close(got["ik[0].twist"], twist)
             and close(got["ik[1].aimOffset"], offset), f"{what}: {json.dumps(got)[:500]}")


async def scene_text(ed: Editor) -> str:
    await ed.call("save_scene", all=True)
    return "\n".join(p.read_text(errors="replace") for p in ed.project.rglob("*.scene"))


async def test_setup_old_data_keeps_its_bytes(ed: Editor):
    await ed.call("edit_animator_controller", path=CONTROLLER, create=True,
                  ops=[{"op": "add_state", "name": "Idle", "clip": IDLE, "default": True, "position": [300, 100]}])
    await ed.call("add_component", entity="Hero", component="animator", values={"animator": {
        "controller": CONTROLLER,
        "ik": [{"name": "Arm", "type": "two_bone", "bones": ARM, "targetPosition": [0.2, 1.2, 0.2]},
               {"name": "Look", "type": "look_at", "bones": ["neck", "head"], "targetPosition": [0, 1.5, 3], "clampAngle": 180}]}})
    holds(ed, await fields(ed), 0, [0, 0, 0], 0, [0, 0, 0], "the fields default to off")
    text = await scene_text(ed)
    ed.check("clampAngle:" in text, "the scene holds the constraints")
    for key in NEW_KEYS:
        ed.check(f"{key}:" not in text, f"{key} at its default is not written: old scenes keep their bytes")


async def test_set_properties_undo_and_save(ed: Editor):
    await ed.call("set_properties", entity="Hero", values={"animator": {
        "ik[0].soften": 0.05, "ik[0].twist": 15, "ik[0].hingeAxis": [0, 0, 1], "ik[1].aimOffset": [0, 0.1, 0.08]}})
    holds(ed, await fields(ed), 0.05, [0, 0, 1], 15, [0, 0.1, 0.08], "the fields are set")
    text = await scene_text(ed)
    for key in NEW_KEYS:
        ed.check(f"{key}:" in text, f"{key} away from its default is saved")
    # one set_properties, one undo step
    await ed.call("undo")
    holds(ed, await fields(ed), 0, [0, 0, 0], 0, [0, 0, 0], "undo restores all of them")
    await ed.call("redo")
    holds(ed, await fields(ed), 0.05, [0, 0, 1], 15, [0, 0.1, 0.08], "redo")
    await ed.call("undo")
    # a Clamped field is not refused out of its range (the solver and Lua's setter clamp it)
    r = await ed.call_raw("set_properties", {"entity": "Hero", "values": {"animator": {"ik[0].soften": 3}}})
    ed.check(not r.is_error, f"soften 3 is accepted: {r.text[:300]}")
    await ed.call("set_properties", entity="Hero", values={"animator": {"ik[0].soften": 0}})


async def test_inspector_rows(ed: Editor):
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("wait", frames=3)
    layout = await ed.call("get_inspector_layout", entities=["Hero"])
    rows = {row["path"]: row for row in harness.find_key(layout, "rows", "layout") if row.get("component", "").lower() == "animator" or "ik[" in row.get("path", "")}
    for path, label in (("ik[0].soften", "Soften"), ("ik[0].hingeAxis", "Hinge Axis"), ("ik[0].twist", "Twist"), ("ik[1].aimOffset", "Aim Offset")):
        ed.check(path in rows and rows[path]["label"] == label and rows[path]["visible"], f"{path} shows as {label}: {rows.get(path)}")


async def test_runtime(ed: Editor):
    # edit mode: the stored fields are refused, with where to set them
    await ed.call("control_animator", entity="Hero", preview="start")
    r = await ed.call_raw("control_animator", {"entity": "Hero", "ik": {"Arm": {"soften": 0.1}}})
    ed.check("set_properties" in r.text, f"edit mode points at set_properties: {r.text[:500]}")
    holds(ed, await fields(ed), 0, [0, 0, 0], 0, [0, 0, 0], "and changes nothing")
    await ed.call("control_animator", entity="Hero", preview="stop")

    async def reached() -> list:
        return [c["reached"] for c in (await ed.call("get_animator", entity="Hero"))["live"]["ik"]]

    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.2)
        # halfway from the shoulder to the hand is in the arm's reach at any size of model, also
        # while the idle moves the shoulder (the hand itself is at full stretch toward the setup's target)
        hand = (await ed.call("eval_lua", code=f"local a = {ANIMATOR} local s, h = a:getBonePosition('left_upper_arm'), a:getBonePosition('left_hand') "
                                               "return (s.x + h.x) / 2, (s.y + h.y) / 2, (s.z + h.z) / 2"))["values"]
        await ed.call("control_animator", entity="Hero", ik={"Arm": {"target": hand}})
        await ed.call("wait", game_seconds=0.1)
        values = (await ed.call("eval_lua", code=f"local a = {ANIMATOR} return a:isIKReached('Arm'), a:isIKReached('Look'), a:isIKReached('Nobody')"))["values"]
        ed.check_eq(values, [True, True, False], "isIKReached: the arm and the aim reached, an unknown name not")
        ed.check_eq(await reached(), [True, True], "get_animator reads the same")
        # out of reach: not reached, by both
        far = [hand[0] + 100, hand[1] + 100, hand[2] + 100]
        await ed.call("control_animator", entity="Hero", ik={"Arm": {"target": far}})
        await ed.call("wait", game_seconds=0.1)
        ed.check_eq((await reached())[0], False, "beyond the arm")
        await ed.call("control_animator", entity="Hero", ik={"Arm": {"target": hand}})
        await ed.call("wait", game_seconds=0.1)
        ed.check_eq((await reached())[0], True, "back in reach")
        # the tool's live fields reach the game's copy and the solver: all of the reach eases
        await ed.call("control_animator", entity="Hero", ik={"Arm": {"soften": 1, "twist": 30, "hinge_axis": [0, 0, 1]}, "Look": {"aim_offset": [0, 0.1, 0]}})
        await ed.call("wait", game_seconds=0.1)
        holds(ed, await fields(ed, "get_runtime_entity"), 1, [0, 0, 1], 30, [0, 0.1, 0], "control_animator set the game's copy")
        ed.check_eq((await reached())[0], False, "fully soft: the arm stops short")
        # Lua's setters put them back
        await ed.call("eval_lua", code=f"local a = {ANIMATOR} a:setIKSoften('Arm', 0) a:setIKTwist('Arm', 0) "
                                       "a:setIKHingeAxis('Arm', Vector3(0, 0, 0)) a:setIKAimOffset('Look', Vector3(0, 0, 0))")
        await ed.call("wait", game_seconds=0.1)
        holds(ed, await fields(ed, "get_runtime_entity"), 0, [0, 0, 0], 0, [0, 0, 0], "Lua set them back")
        ed.check_eq((await reached())[0], True, "and the arm reaches again")
    finally:
        await ed.call("play", action="stop")
    holds(ed, await fields(ed), 0, [0, 0, 0], 0, [0, 0, 0], "play left the scene as it was")
