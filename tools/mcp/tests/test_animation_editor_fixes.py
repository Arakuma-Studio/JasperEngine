"""Animation clips: the final editor review's fixes that need the real editor (FINAL-review-editor.md), on the
pose fixture project (Hero, Robot), driven through the Animation window's test hook (focus_window
{window: "animation", animation: {...}}, JASPER_MCP_TEST_HOOKS=1) and REAL mouse clicks (xdotool on the harness'
Xvfb, widgets found through the UI probe, Editor(ui_probe=True)):

- I2: a right-click on a track row's label opens the row menu (Retarget..., Add Key at Playhead, Remove Property),
  and its Retarget... opens the Retarget popup;
- I9: the entity-mode preview owns every root it writes: an Animation on the Hero that nests an Animation of the
  Robot is stopped by a clip preview of the Robot (PreviewOwner);
- Minor 4: undoing Create New Clip shows the empty state again, not the deleted clip.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 600

ROBOT_CLIP = "assets/anim/fx2_robot.anim"
ROBOT_WALK = {
    "format": "jasperengine.anim", "version": 1, "guid": "5f2e7d9c0b1a43e8a6c4d2b0e9f81726", "frame_rate": 30, "wrap": "loop",
    "tracks": [{"path": "", "channel": "position", "keys": [[0, [0.75, 0, 0]], [1, [0.75, 0.5, 0]]]}],
}


async def hook(ed: Editor, **animation) -> dict:
    r = await ed.call("focus_window", window="animation", animation=animation)
    out = r.get("viewport_input") if isinstance(r, dict) else None
    ed.check(isinstance(out, dict), f"the hook returns the window state: {r}")
    return out


def xdo(ed: Editor, *args: str) -> str:
    if not shutil.which("xdotool"):
        harness.skip("xdotool is not installed")
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def click(ed: Editor, rect, button: str = "1"):
    x, y = center(rect)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.3)
    xdo(ed, "click", button)
    await ed.call("wait", frames=6)


async def test_setup(ed: Editor):
    ed.require_tools("focus_window", "write_file", "edit_animation", "add_component", "preview_animation", "undo")
    anim = ed.project / "assets" / "anim"
    anim.mkdir(parents=True, exist_ok=True)
    (ed.project / ROBOT_CLIP).write_text(json.dumps(ROBOT_WALK, indent=2) + "\n")
    await ed.call("sync_files")


async def test_row_menu_opens_on_a_real_right_click(ed: Editor):
    """I2: the row label's right-click opens the menu (it was opened in the row's ID scope and never showed)"""
    st = await hook(ed, entity="Hero", create_clip="assets/anim/fx2_rows.anim")
    ed.check_eq(st["clip"], "assets/anim/fx2_rows.anim", f"created: {st}")
    st = await hook(ed, add_property={"path": "", "channel": "position"})
    ed.check(any(r.get("path") == "" and r.get("channel") == "position" for r in st["rows"]), f"a root position row: {st['rows']}")
    label = await ed.ui_rect("clip.rowLabel.0")
    ed.check(label, f"the row label is drawn: {sorted(ed.ui_items())}")
    ed.check("clip.rowMenu.retarget" not in ed.ui_items(), "no menu before the click")
    await click(ed, label, "3")
    item = await ed.ui_rect("clip.rowMenu.retarget", timeout=5)
    ed.check(item, f"the row menu is open after a real right-click: {sorted(ed.ui_items())}")
    await click(ed, item)
    title = await ed.ui_rect("clip.rebind.title", timeout=5)
    ed.check(title, f"Retarget... opens the Retarget popup: {sorted(ed.ui_items())}")
    xdo(ed, "key", "Escape")
    await ed.call("wait", frames=6)


async def test_entity_preview_owns_every_root_it_writes(ed: Editor):
    """I9: an entity Animation on the Hero that also plays an Animation of the Robot owns the Robot too: a clip
    preview of the Robot stops it (before, only the Hero was owned and both previews wrote the Robot)"""
    await ed.call("edit_animation", target="Robot", name="FxRobotSpin", tweens=[{"type": "scale", "to": [1.5, 1.5, 1.5], "duration": 1.0}])
    await ed.call("edit_animation", target="Hero", name="FxHeroMix", tweens=[{"type": "position", "to": [0, 0.5, 0], "duration": 1.0}],
                  add_actions=[{"action": "FxRobotSpin", "start_time": 0}])
    await ed.call("add_component", entity="Robot", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": ROBOT_CLIP}]}})
    st = await hook(ed, entity_animation="FxHeroMix", entity_play=True)
    await ed.call("wait", seconds=0.3)
    ed.check((await hook(ed, entity_animation="FxHeroMix"))["entity_preview"], "the entity preview runs")
    await ed.call("preview_animation", animation="Robot", action="seek", time=0.2)
    st = await hook(ed, entity_animation="FxHeroMix")
    ed.check(not st["entity_preview"], f"the clip preview of the Robot stopped the entity preview that writes it: {st}")
    await ed.call("preview_animation", animation="Robot", action="stop")


async def test_undone_create_shows_the_empty_state(ed: Editor):
    """Minor 4: Create New Clip undone (the file and the player go): the window shows its empty state again"""
    await ed.call("create_entity", type="cube", name="FxBox")
    st = await hook(ed, entity="FxBox")
    ed.check(st["empty_state"], f"the empty state first: {st}")
    st = await hook(ed, create_clip="assets/anim/fx2_box.anim")
    ed.check_eq(st["clip"], "assets/anim/fx2_box.anim", f"created: {st}")
    await ed.call("undo", scope="scene")
    await ed.call("wait", frames=6)
    st = await hook(ed)
    ed.check(not (ed.project / "assets/anim/fx2_box.anim").exists(), "the file went with the undo")
    ed.check(st["clip"] is None and st["empty_state"], f"the deleted clip is no longer shown: {st}")
