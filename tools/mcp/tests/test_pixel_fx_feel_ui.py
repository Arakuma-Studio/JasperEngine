"""S48 R42 (docs/specs/S48-pixel-fx.md AC24, the game-feel rows): the Inspector shows Hit Stop Exempt (Add
Component > Pixel Art), the Camera Shaker's Pixel Snap and Tick Rate (disabled on a perspective camera), and
the Pose Frame Rate row on an Animation Player. Screenshots go to $JASPER_EVIDENCE_DIR when set."""

from __future__ import annotations

import os
from pathlib import Path

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel FX Feel"}
TIMEOUT = 600

S: dict = {}


async def keep_shot(ed: Editor, name: str) -> None:
    folder = os.environ.get("JASPER_EVIDENCE_DIR")
    shot = await ed.call_raw("screenshot", {"view": "editor"})
    ed.check(not shot.is_error and shot.images, f"an editor screenshot: {shot.text}")
    if folder and shot.images:
        Path(folder).mkdir(parents=True, exist_ok=True)
        (Path(folder) / f"{name}.png").write_bytes(shot.images[0][0])


async def test_setup(ed: Editor):
    scene = (await ed.call("create_scene", name="Feel", type="2D"))["scene"]["id"]
    S["scene"] = scene
    cam = (await ed.call("create_entity", scene=scene, type="camera", name="Pixel Camera"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=cam, component="camerashaker")
    hero = (await ed.call("create_entity", scene=scene, name="Hero"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=hero, component="hitstopexempt")
    await ed.call("add_component", scene=scene, entity=hero, component="animationplayer")
    S.update(cam=cam, hero=hero)
    await ed.call("select_scene", scene=scene)


async def test_shaker_rows(ed: Editor):
    ui = Ui(ed)
    await ed.call("select_entities", scene=S["scene"], entities=[S["cam"]])
    await ed.call("focus_window", window="inspector")
    await ui.frames(30)
    await keep_shot(ed, "s48-camera-shaker-pixel-rows")
    ed.check(await ui.find("Pixel Snap", frames=600), "the Camera Shaker's Pixel Snap row")
    ed.check(await ui.find("Tick Rate"), "the Camera Shaker's Tick Rate row")


async def test_hit_stop_exempt_and_pose_frame_rate(ed: Editor):
    ui = Ui(ed)
    await ed.call("select_entities", scene=S["scene"], entities=[S["hero"]])
    await ed.call("focus_window", window="inspector")
    await ui.frames(30)
    await keep_shot(ed, "s48-hit-stop-exempt-pose-frame-rate")
    ed.check(await ui.find("Hit Stop Exempt", frames=600), "the Hit Stop Exempt component")
    ed.check(await ui.find("Include Children"), "its Include Children row")
    ed.check(await ui.find("Pose Frame Rate"), "the Animation Player's Pose Frame Rate row")
    await ed.call("set_properties", scene=S["scene"], entity=S["hero"], values={"animationplayer": {"poseFrameRate": 12}})
    await ui.frames()
    await keep_shot(ed, "s48-pose-frame-rate-12")
