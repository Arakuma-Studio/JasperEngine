"""The physics world settings in the Inspector, driven like a human (xdotool in the harness's Xvfb;
docs/specs/S96-physics-gaps.md REQ-43): with nothing selected a 2D scene's Physics section has Points
Per Meter and Max Rotation Per Step rows and a World foldout whose rows are the solver's tuning; a
click on one of them is one undo step. A Rigidbody 2D has an Allow Fast Rotation checkbox, one undo
step too. Screenshots of both go to the scratch directory (and to JASPER_EVIDENCE_DIR when set)."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from harness import Editor
import ui_input

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Physics World Inspector"}
TIMEOUT = 600
INSPECTOR = "Inspector"


async def shot(ed: Editor, name: str) -> None:
    path = ed.scratch.root / name
    await ed.screenshot("editor", path=str(path), max_width=1600)
    evidence = os.environ.get("JASPER_EVIDENCE_DIR")
    if evidence:
        Path(evidence).mkdir(parents=True, exist_ok=True)
        shutil.copy(path, Path(evidence) / name)


async def test_scene_world_foldout_and_fast_rotation(ed: Editor):
    ed.require_tools("find_ui_item", "select_entities", "focus_window", "get_scene_settings", "undo")
    sid = (await ed.call("create_scene", name="WorldRows", type="2d"))["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("select_entities", entities=[])
    await ed.call("focus_window", window="inspector")
    ui = ui_input.Ui(ed)               # skips without xdotool or the harness's display
    await ui.frames(10)
    ui.focus()

    await ui.reveal("##physics_2d_points_per_meter", INSPECTOR)
    await ui.reveal("##physics_2d_max_rotation", INSPECTOR)
    ed.check(await ui.find("##physics_2d_warm_starting", INSPECTOR, frames=4) is None, "the World foldout starts closed")
    await ui.click_revealed("World##physics_2d_world", INSPECTOR)
    await ui.reveal("##physics_2d_contact_hertz", INSPECTOR)
    await ui.click_revealed("##physics_2d_warm_starting", INSPECTOR)
    got = await ed.call("get_scene_settings", scene=sid, names=["physics_2d_warm_starting"])
    ed.check_eq(got["settings"]["physics_2d_warm_starting"], False, "the Warm Starting checkbox turned it off")
    await shot(ed, "s96-scene-physics-2d-world.png")
    await ed.call("undo")
    got = await ed.call("get_scene_settings", scene=sid, names=["physics_2d_warm_starting"])
    ed.check_eq(got["settings"]["physics_2d_warm_starting"], True, "one undo step")

    wheel = (await ed.call("create_entity", scene=sid, name="Wheel", type="rigidbody2d"))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=wheel, body_type="dynamic", add=[{"shape": "circle", "radius": 32}])
    await ed.call("select_entities", entities=[wheel])
    await ui.frames(10)
    await ui.click_revealed("##checkbox_allowFastRotation", INSPECTOR)
    props = await ed.call("get_entity", scene=sid, entity=wheel, components=["rigidbody2d"], properties=["allowFastRotation"])
    ed.check(props["components"]["rigidbody2d"]["allowFastRotation"] is True, f"the Allow Fast Rotation checkbox set it: {props}")
    await shot(ed, "s96-rigidbody2d-allow-fast-rotation.png")
    await ed.call("undo")
    props = await ed.call("get_entity", scene=sid, entity=wheel, components=["rigidbody2d"], properties=["allowFastRotation"])
    ed.check(props["components"]["rigidbody2d"]["allowFastRotation"] is False, f"one undo step: {props}")
