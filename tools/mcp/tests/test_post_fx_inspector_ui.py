"""The S13 post FX in the editor's UI, driven with real mouse input (docs/specs/S13-post-fx.md R15,
R18, AC19): xdotool works the rects editor_status reports (ui.project_window), the assertions read
the editor's state back, screenshots of each step go to build/test-results/post-fx-inspector/.

- the Project window lists a .cube as a colour LUT (its icon), and its context menu's "Use as Color
  Grading LUT" (what a double-click does: the harness's software-rendered editor draws a frame in
  about 250 ms, too slow for ImGui's 0.3 s double-click) makes it the open scene's Color Grading LUT
  in one named undo step ("Assign Color LUT look_display.cube");
  the Inspector (nothing selected: the scene's Post-processing) then shows LUT Input Encoding with
  what Auto resolves to; one undo restores the scene's LUT
- with VHS on, FXAA's row reads "inactive while VHS is on" (screenshot); presets apply as one step
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "post-processing", "xvfb": "harness"}
TIMEOUT = 600
CI = True

SHOTS = REPO / "build" / "test-results" / "post-fx-inspector"
STATE: dict = {}
CUBE = "look_display.cube"


def xdo(ed: Editor, *args: str) -> str:
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout.strip()


def window(ed: Editor) -> str:
    if "window" not in STATE:
        ids = xdo(ed, "search", "--name", "JasperEngine").split()
        if not ids:
            ed.fail("no editor window on the harness display")
        STATE["window"] = ids[-1]
        xdo(ed, "windowactivate", "--sync", STATE["window"])
    return STATE["window"]


async def frames(ed: Editor, count: int = 2) -> None:
    for _ in range(count):
        await ed.call("editor_status")


async def ui(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]


async def wait_ui(ed: Editor, predicate, what: str, timeout: float = 15.0) -> dict:
    state = None
    for _ in range(int(timeout / 0.25)):
        state = await ui(ed)
        if predicate(state):
            return state
        await asyncio.sleep(0.25)
    ed.fail(f"{what}; ui: {state}")


def centre(rect: list) -> tuple[str, str]:
    x, y, w, h = rect
    return str(int(x + w / 2)), str(int(y + h / 2))


async def click(ed: Editor, rect: list) -> None:
    x, y = centre(rect)
    xdo(ed, "mousemove", "--window", window(ed), x, y)
    await frames(ed)
    xdo(ed, "mousedown", "1")
    await frames(ed)
    xdo(ed, "mouseup", "1")
    await frames(ed)


async def click_right(ed: Editor, rect: list) -> None:
    x, y = centre(rect)
    xdo(ed, "mousemove", "--window", window(ed), x, y)
    await frames(ed)
    xdo(ed, "mousedown", "3")
    await frames(ed)
    xdo(ed, "mouseup", "3")
    await frames(ed)


def menu_item(state: dict, label: str) -> list | None:
    for item in state["project_window"]["context_menu"]:
        if item["label"] == label:
            return item["rect"]
    return None


def tree_node(state: dict, path: str) -> dict | None:
    for node in state["project_window"]["tree"]:
        if node["path"] == path:
            return node
    return None


def entry(state: dict, name: str) -> list | None:
    for item in state["project_window"]["entries"]:
        if item["name"] == name:
            return item["rect"]
    return None


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


async def open_luts(ed: Editor) -> dict:
    await ed.call("focus_window", window="project")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if state["project_window"]["directory"] != "assets/luts":
        if not tree_node(state, "assets/luts"):
            x, y = tree_node(state, "assets")["arrow"]
            xdo(ed, "mousemove", "--window", window(ed), str(int(x)), str(int(y)))
            await frames(ed)
            xdo(ed, "click", "1")
            await frames(ed, 3)
            state = await wait_ui(ed, lambda s: tree_node(s, "assets/luts"), "assets opened in the tree")
        await click(ed, tree_node(state, "assets/luts")["rect"])
        state = await wait_ui(ed, lambda s: s["project_window"]["directory"] == "assets/luts" and entry(s, CUBE),
                              "assets/luts shown")
    return state


async def lut(ed: Editor, sid: int) -> dict:
    got = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    return {"lut_texture": got.get("lut_texture", ""), "color_grading_enabled": got.get("color_grading_enabled", False)}


async def test_cube_assigns_the_lut(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    sid = (await ed.call("list_scenes"))["selected_scene_id"]
    await ed.call("select_entities", entities=[])
    ed.check_eq(await lut(ed, sid), {"lut_texture": "", "color_grading_enabled": False}, "no LUT at first")

    state = await open_luts(ed)
    names = [item["name"] for item in state["project_window"]["entries"]]
    ed.check({"look_display.cube", "look_acescct.cube"} <= set(names), f"the .cube files listed: {names}")
    ed.check(not any(n.endswith(".meta") for n in names), f"metas listed: {names}")
    await shot(ed, "project-luts")

    await click_right(ed, entry(state, CUBE))
    state = await wait_ui(ed, lambda s: menu_item(s, "Use as Color Grading LUT"), "the .cube's context menu")
    await shot(ed, "project-lut-menu")
    await click(ed, menu_item(state, "Use as Color Grading LUT"))
    for _ in range(40):
        if (await lut(ed, sid))["lut_texture"]:
            break
        await asyncio.sleep(0.25)
    ed.check_eq(await lut(ed, sid), {"lut_texture": "assets/luts/look_display.cube", "color_grading_enabled": True},
                "the menu assigns the LUT")
    status = await ed.call("editor_status")
    ed.check("Assign Color LUT look_display.cube" in (status["undo"]["undo_name"] or ""), f"one named step: {status['undo']}")
    await frames(ed, 4)
    await shot(ed, "inspector-lut-encoding")

    await ed.call("undo")
    ed.check_eq(await lut(ed, sid), {"lut_texture": "", "color_grading_enabled": False}, "one undo restores the scene")


async def test_vhs_marks_fxaa_inactive(ed: Editor):
    sid = (await ed.call("list_scenes"))["selected_scene_id"]
    await ed.call("select_entities", entities=[])
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {"fxaa_enabled": True, "vhs_enabled": True}})
    status = await ed.call("get_post_processing_status", scene=sid)
    ed.check("VHS" in status["uber_features"] and "FXAA" not in status["uber_features"], f"VHS drops FXAA: {status['uber_features']}")
    await frames(ed, 4)
    await shot(ed, "inspector-vhs-fxaa-inactive")
    await ed.call("apply_post_processing_preset", scene=sid, preset="film")
    ed.check("Apply Film Preset" in ((await ed.call("editor_status"))["undo"]["undo_name"] or ""), "the preset step is named")
    await ed.call("undo")
    await ed.call("undo")
