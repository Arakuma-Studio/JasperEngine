# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""S74 R36, R37, R49, R53, AC17 (docs/specs/S74-clip-quality.md): an Animator's live debug numbers,
on a scratch copy of the pose-animation fixture in an isolated editor:

- get_animator's `live` gains ik[i] weight, reach_error_mm, lock_drift_mm and root_motion speed,
  yaw, accumulated position and yaw, slide and its contact source;
- the Animator Inspector's IK (live) table and Root Motion line draw the same numbers (find_ui_item
  reads each drawn value; the game is paused so both read one update);
- set_scene_camera's display takes show_ik, show_root_motion and show_foot_trails, and the Scene
  view draws them (screenshots to JE_EVIDENCE_DIR, else the test's scratch folder);
- without a Foot IK or Foot Lock the contact source is "none" and no slide is reported (R37).
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import ui_input
from harness import Editor, REPO, skip

CI = True
TIMEOUT = 600
EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}

CONTROLLER = "assets/anim/debug.controller"
WALK = "assets/models/hero_animated.gltf#Walk"
ARM = ["left_upper_arm", "left_lower_arm", "left_hand"]
LEG = ["left_upper_leg", "left_lower_leg", "left_foot"]
FLAGS = ("show_ik", "show_root_motion", "show_foot_trails")


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def fmt(key: str, value: float) -> str:
    """formatAnimatorDebugField (editor/animation/AnimatorDebug.cpp), from the value the JSON holds"""
    units = {"weight": "{:.2f}", "reach_error_mm": "{:.1f} mm", "pelvis_offset_mm": "{:.1f} mm", "lock_drift_mm": "{:.1f} mm",
             "speed_mps": "{:.2f} m/s", "yaw_deg_s": "{:.1f} deg/s", "foot_speed_mps": "{:.2f} m/s", "slide_mm_s": "{:.0f} mm/s"}
    return units[key].format(value)


def text_of(item: dict) -> str:
    return item["label"].split("##")[0]


async def live(ed: Editor) -> dict:
    return (await ed.call("get_animator", entity="Hero"))["live"]


async def test_setup(ed: Editor):
    await ed.call("edit_animator_controller", path=CONTROLLER, create=True, ops=[
        {"op": "add_parameter", "name": "Contact", "type": "float", "default": 0},
        {"op": "add_state", "name": "Walk", "clip": WALK, "default": True, "position": [300, 100]}])
    await ed.call("add_component", entity="Hero", component="animator", values={"animator": {
        "controller": CONTROLLER, "applyRootMotion": True,
        "ik": [{"name": "Arm", "type": "two_bone", "bones": ARM, "targetPosition": [0.2, 40, 0.2]},
               {"name": "Lock", "type": "foot_lock", "bones": LEG, "contactParameter": "Contact"}]}})
    display = (await ed.call("set_scene_camera"))["display"]
    for flag in FLAGS:
        ed.check(display.get(flag) is False, f"{flag} is off by default: {display}")


async def test_live_fields_equal_the_inspector(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    ui = ui_input.for_editor(ed)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.3)
        await ed.call("control_animator", entity="Hero", parameters={"Contact": 1})
        await ed.call("wait", game_seconds=0.3)
        await ed.call("play", action="pause")
        state = await live(ed)
        arm, lock = state["ik"]
        ed.check(arm["weight"] == 1 and arm["reached"] is False and arm["reach_error_mm"] > 1000,
                 f"the arm's target is 40 m up: not reached, the miss in mm: {arm}")
        ed.check(lock["locked"] is True and lock["lock_drift_mm"] >= 0 and lock["weight"] > 0,
                 f"the foot locked on contact, its drift in mm: {lock}")
        rm = state["root_motion"]
        for key in ("speed_mps", "yaw_deg_s", "accumulated_yaw", "slide_mm_s", "foot_speed_mps"):
            ed.check(isinstance(rm.get(key), (int, float)), f"root_motion.{key}: {rm}")
        ed.check(len(rm["accumulated_position"]) == 3, f"accumulated_position: {rm}")
        ed.check(rm["contact_source"] == "foot_goals", f"the Foot Lock is the contact: {rm}")

        await ed.call("select_entities", entities=["Hero"])
        await ed.call("wait", frames=5)
        cells = {"##ik_weight_0": fmt("weight", arm["weight"]),
                 "##ik_reach_0": fmt("reach_error_mm", arm["reach_error_mm"]),
                 "##ik_weight_1": fmt("weight", lock["weight"]),
                 "##ik_detail_1": "Lock Drift " + fmt("lock_drift_mm", lock["lock_drift_mm"]),
                 "##rm_speed": "Speed " + fmt("speed_mps", rm["speed_mps"]),
                 "##rm_yaw": "Yaw " + fmt("yaw_deg_s", rm["yaw_deg_s"]),
                 "##rm_foot_speed": "Foot Speed " + fmt("foot_speed_mps", rm["foot_speed_mps"]),
                 "##rm_slide": "Slide " + fmt("slide_mm_s", rm["slide_mm_s"])}
        await ed.screenshot("editor", path=str(evidence(ed) / "ac17-inspector-selected.png"))
        # a ## id is found only while in view; out of view, its whole label (the tool's value) scrolls it there
        cells["##ik_state_0"] = "Out of Reach"
        for label, expected in cells.items():
            items = (await ed.call("find_ui_item", label=label, window="Inspector")).get("items", [])
            if not items or not items[0].get("visible"):
                # below the fold: its whole label, the tool's value, scrolls it into view (or is not drawn)
                await ui.reveal(expected + label, "Inspector")
                items = (await ed.call("find_ui_item", label=label, window="Inspector")).get("items", [])
            ed.check(items, f"{label} is drawn in the Inspector")
            ed.check_eq(text_of(items[0]), expected, f"the Inspector's {label} is the tool's value")
        await ed.call("wait", frames=4)
        await ed.screenshot("editor", path=str(evidence(ed) / "ac17-inspector-live.png"))

        # the overlays: on through the tool, drawn by the Scene view
        out = await ed.call("set_scene_camera", display={flag: True for flag in FLAGS}, frame_all=True)
        for flag in FLAGS:
            ed.check(out["display"][flag] is True, f"{flag} on: {out['display']}")
        await ed.call("play", action="resume")
        await ed.call("wait", game_seconds=1.0)
        await ed.call("play", action="pause")
        await ed.call("frame_entity", entity="Hero")
        await ed.screenshot("scene", path=str(evidence(ed) / "ac17-overlays-on.png"))
        await ed.call("set_scene_camera", display={flag: False for flag in FLAGS})
        await ed.screenshot("scene", path=str(evidence(ed) / "ac17-overlays-off.png"))
    finally:
        await ed.call("play", action="stop")


async def test_no_feet_no_contact(ed: Editor):
    await ed.call("set_properties", entity="Hero", values={"animator": {"ik": [
        {"name": "Arm", "type": "two_bone", "bones": ARM, "targetPosition": [0.2, 40, 0.2]}]}})
    await ed.call("control_animator", entity="Hero", preview="start", advance=0.5)
    try:
        rm = (await live(ed))["root_motion"]
        ed.check(rm["contact_source"] == "none", f"no Foot IK or Foot Lock: no guess (R37): {rm}")
        ed.check("slide_mm_s" not in rm and "foot_speed_mps" not in rm, f"and no slide: {rm}")
    finally:
        await ed.call("control_animator", entity="Hero", preview="stop")
