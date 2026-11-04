"""The anim layers windows driven like a human (upstream anim layers spec 9, 11.4, 14, on S21's Animator window and
asset Inspector: docs/specs/S21-animation.md "Upstream absorption"), with xdotool in the harness's Xvfb on a
scratch copy of tools/fixtures/pose-animation:

- the Inspector's Layers table: + Add Layer, the weight slider and a clip row's Layer combo, each one scene undo step;
- the Avatar Mask asset Inspector: upstream's version 1 .mask clicked in the Project window, a body part clicked on
  the figure writes the file as version 2 (a project undo step restores the original text);
- the Animator window (focus_window animator) on an Animator whose controller edit_animator_controller made:
  right-click the canvas -> Create State > Empty, Make Transition to another state, + Condition in the Inspector,
  Ctrl+Z / Ctrl+Y with the file checked through get_animation {controller}; a state drag is one undo step; in play
  mode a parameter's checkbox changes the runtime (get_animator) and not the file.

Widgets are found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h), the Project window's
files by their "project.file.<name>" rects (as test_physics_2d_inspector.py does). Screenshots go to
$JASPER_SHOTS_DIR when set (else the scratch directory).
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
from pathlib import Path

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
CI_EXCLUDED = ("long-running (about 5 minutes on a loaded server: the windows driven with real X input on the pose-animation rig); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 900

CONTROLLER = "assets/anim/window.controller"
MASK = "assets/aaa_masks/arm.mask"
S: dict = {}


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def move(self, x: float, y: float):
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await asyncio.sleep(0.25)

    async def click(self, x: float, y: float, button: int = 1, double: bool = False):
        await self.move(x, y)
        self.xdo("click", "--repeat", "2" if double else "1", "--delay", "80", str(button))
        await asyncio.sleep(0.5)

    async def drag(self, a, b, steps: int = 8):
        await self.move(*a)
        self.xdo("mousedown", "1")
        await asyncio.sleep(0.2)
        for i in range(1, steps + 1):
            self.xdo("mousemove", str(int(a[0] + (b[0] - a[0]) * i / steps)), str(int(a[1] + (b[1] - a[1]) * i / steps)))
            await asyncio.sleep(0.08)
        await asyncio.sleep(0.2)
        self.xdo("mouseup", "1")
        await asyncio.sleep(0.5)

    async def focus_x(self):
        """the editor's X window focused, for xdotool's keys"""
        ids = self.xdo("search", "--onlyvisible", "--name", "JasperEngine").split()
        self.ed.check(ids, "the editor's X window")
        self.xdo("windowfocus", "--sync", ids[-1])
        await self.frames(6)

    async def key(self, combo: str):
        *modifiers, last = combo.split("+")
        for modifier in modifiers:
            self.xdo("keydown", "--clearmodifiers", modifier)
        self.xdo("keydown", last)
        self.xdo("keyup", last)
        for modifier in reversed(modifiers):
            self.xdo("keyup", modifier)
        await self.frames(8)

    async def wheel(self, at, down: bool, notches: int):
        await self.move(*at)
        for _ in range(notches):
            self.xdo("click", "5" if down else "4")
        await asyncio.sleep(0.3)

    async def project_file(self, name: str, folders=("assets", "aaa_masks")):
        """a file of the Project window (its probe rect), opening the folders on the way"""
        await self.ed.call("focus_window", window="project")
        for _ in range(12):
            await self.frames(4)
            items = self.ed.ui_items()
            rect = items.get("project.file." + name)
            if rect:
                return rect
            for folder in reversed(folders):
                if "project.file." + folder in items:
                    await self.click(*center(items["project.file." + folder]), double=True)
                    break
        self.ed.fail(f"the Project window never lists {name}: {sorted(self.ed.ui_items())}")

    async def frames(self, n: int = 3):
        await self.ed.call("wait", frames=n)

    async def item(self, name: str) -> list[float]:
        for _ in range(30):
            await self.frames()
            rect = self.ed.ui_items().get(name)
            if rect:
                return rect
        self.ed.fail(f"{name} is never drawn: {sorted(self.ed.ui_items())}")

    async def scroll_to(self, name: str, anchor: str) -> list[float]:
        """a widget of a scrolled panel: the wheel over the panel (at `anchor`, a widget of it) until it shows"""
        at = center(await self.item(anchor))
        for _ in range(40):
            await self.frames()
            rect = self.ed.ui_items().get(name)
            if rect:
                return rect
            await self.wheel(at, True, 2)
        self.ed.fail(f"{name} is never drawn: {sorted(self.ed.ui_items())}")

    async def reveal(self, name: str, top: bool = True) -> list[float]:
        """an Inspector widget, scrolled near the top of the panel"""
        await self.frames()
        panel = await self.ed.ui_rect("inspector")
        self.ed.check(panel, "the inspector is drawn")
        at = ((panel[0] + panel[2]) / 2, (panel[1] + panel[3]) / 2)
        if top:
            await self.wheel(at, False, 80)
        for _ in range(80):
            await self.frames()
            rect = self.ed.ui_items().get(name)
            if rect:
                for _ in range(40):
                    if rect[1] < panel[1] + 60:
                        break
                    await self.wheel(at, True, 1)
                    await self.frames()
                    moved = self.ed.ui_items().get(name)
                    if not moved or moved[1] == rect[1]:
                        break
                    rect = moved
                return rect
            await self.wheel(at, True, 2)
        self.ed.fail(f"{name} is never drawn: {sorted(self.ed.ui_items())}")


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def shot(ed: Editor, name: str):
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    await ed.call("wait", frames=4)
    await ed.screenshot("editor", path=str(shots / f"animator-{name}.png"), max_width=1600)


async def player(ed: Editor, entity: str) -> dict:
    got = await ed.call("get_entity", entity=entity, components=["animationplayer"])
    comps = got.get("components", {})
    return comps.get("animationplayer", comps)


async def controller(ed: Editor) -> dict:
    return await ed.call("get_animation", controller=CONTROLLER)


def controller_states(got: dict) -> list:
    return got["layers"][0]["state_machine"]["states"]


def transitions(got: dict) -> list:
    """(from, to) of the base layer's state transitions, by source state"""
    return [(s["name"], t["to"]) for s in controller_states(got) for t in s["transitions"]]


def transition(got: dict, source: str, index: int = 0) -> dict:
    return next(s for s in controller_states(got) if s["name"] == source)["transitions"][index]


async def test_setup(ed: Editor):
    for name, x in (("win_idle", 0.0), ("win_run", 0.4)):
        r = await ed.call_raw("edit_animation", {"clip": f"assets/anim/{name}.anim", "create": {}, "wrap": "loop", "tracks": [
            {"path": "Hero/mixamorig:Hips", "channel": "position", "keys": [{"time": 0, "value": [0, 0.9, 0]}, {"time": 1, "value": [x, 0.9, 0]}]}]})
        ed.check(not r.is_error, r.text[:400])
    r = await ed.call_raw("edit_animator_controller", {"path": CONTROLLER, "create": True, "ops": [
        {"op": "add_parameter", "name": "Speed", "type": "float"},
        {"op": "add_parameter", "name": "Go", "type": "bool"},
        {"op": "add_state", "name": "Idle", "clip": "assets/anim/win_idle.anim", "position": [300, 60]},
        {"op": "add_state", "name": "Run", "clip": "assets/anim/win_run.anim", "position": [560, 60]},
        {"op": "add_transition", "from": "Idle", "to": "Run", "conditions": [{"parameter": "Go", "mode": "if"}], "has_exit_time": False}]})
    ed.check(not r.is_error, r.text[:600])
    # the player of the Inspector tests: clips and no controller
    await ed.call("set_model", path="assets/models/hero_animated.gltf", create={"name": "Layered", "position": [2, 0, 0]})
    await ed.call("add_component", entity="Layered", component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": "assets/anim/win_idle.anim", "name": "Idle"}, {"clip": "assets/anim/win_run.anim", "name": "Run"}]}})
    # the Animator window's Animator: the controller
    await ed.call("add_component", entity="Hero", component="animator", values={"animator": {"controller": CONTROLLER}})
    # a mask alone in the first folder of the assets (the Project window is clicked at fixed places)
    r = await ed.call_raw("write_file", {"path": MASK, "content": json.dumps(
        {"format": "jasperengine.mask", "version": 1, "default": 0, "humanoid": {"right_arm": 1}})})
    ed.check(not r.is_error, r.text[:300])
    await ed.call("sync_files")


async def test_inspector_layers_table(ed: Editor):
    ui = Ui(ed)
    await ed.call("select_entities", entities=["Layered"])
    await ed.call("focus_window", window="inspector")
    header = await ui.reveal("player.layers")
    if "player.addLayer" not in ed.ui_items():
        await ui.click(*center(header))         # "Layers (1)" starts collapsed
    rect = await ui.reveal("player.addLayer")
    await shot(ed, "01-layers-table")
    await ui.click(*center(rect))
    got = await player(ed, "Layered")
    ed.check_eq([l["name"] for l in got["layers"]], ["Base Layer", "New Layer"], f"+ Add Layer: {json.dumps(got)[:500]}")

    # the weight slider: a click near its left end sets a weight below 1 (one scene step)
    slider = await ui.reveal("player.layer.1.weight")
    await ui.click(slider[0] + (slider[2] - slider[0]) * 0.25, center(slider)[1])
    got = await player(ed, "Layered")
    weight = got["layers"][1]["weight"]
    ed.check(0.05 < weight < 0.6, f"the slider set the weight: {got['layers']}")
    await shot(ed, "02-layer-weight")
    await ed.call("undo", scope="scene")
    ed.check_eq((await player(ed, "Layered"))["layers"][1].get("weight", 1), 1, "undo restores the weight")

    # a clip row's Layer combo
    combo = await ui.reveal("player.clip.1.layer")
    await ui.click(*center(combo))
    option = await ui.item("player.clip.1.layerOption.1")
    await shot(ed, "03-clip-layer-combo")
    await ui.click(*center(option))
    got = await player(ed, "Layered")
    ed.check_eq(got["clips"][1].get("layer", 0), 1, f"the entry plays on New Layer: {got['clips']}")
    await ed.call("undo", scope="scene")
    ed.check_eq((await player(ed, "Layered"))["clips"][1].get("layer", 0), 0, "undo puts it back on the base")
    await ed.call("undo", scope="scene")
    ed.check_eq(len((await player(ed, "Layered")).get("layers", [{}])), 1, "undo removes the added layer")


async def test_mask_asset_inspector(ed: Editor):
    ui = Ui(ed)
    await ed.call("focus_window", window="project")
    await ed.call("wait", frames=5)
    mask = ed.project / MASK
    before = mask.read_text()
    # the mask in the Project window (its probe rect): a click shows it in the Inspector
    for _ in range(4):
        rect = await ui.project_file("arm.mask")
        await ui.click(*center(rect))
        await ed.call("wait", frames=10)
        if "asset.mask.figure" in ed.ui_items():
            break
    ed.check("asset.mask.figure" in ed.ui_items(), f"the Avatar Mask Inspector: {sorted(ed.ui_items())}")
    await shot(ed, "04-mask-inspector")
    head = ed.ui_items().get("asset.mask.part.head")
    ed.check(head, f"the figure's head: {sorted(ed.ui_items())}")
    await ui.click(*center(head))
    await ed.call("wait", frames=5)
    data = json.loads(mask.read_text())
    ed.check_eq(data["version"], 2, f"saved as version 2: {data}")
    ed.check_eq(data["body_parts"].get("head"), 1, f"the head toggled on: {data}")
    ed.check_eq((data["body_parts"].get("right_arm"), data["body_parts"].get("right_hand")), (1, 1),
                "the other parts stay (upstream's arm also weighs the hand)")
    await shot(ed, "05-mask-head-on")
    await ed.call("undo", scope="project")
    await ed.call("wait", frames=5)
    ed.check_eq(mask.read_text(), before, "project undo restores the file")


async def test_animator_window_edits(ed: Editor):
    ui = Ui(ed)
    await ed.call("select_entities", entities=["Hero"])
    r = await ed.call("focus_window", window="animator")
    ed.check(r.get("shown"), f"the Animator window shows: {r}")
    await ui.focus_x()
    await ed.call("wait", frames=10)
    path = await ui.item("animator.path")
    canvas = await ui.item("animator.canvas")
    await shot(ed, "06-window")
    states = [ed.ui_items().get(f"animator.state.{i}") for i in range(2)]
    ed.check(all(states), f"both states are drawn: {sorted(ed.ui_items())}")
    boxes = states + [ed.ui_items().get("animator.any_state")]
    # the Idle -> Run arrow's middle is no empty spot either
    mid = ((center(states[0])[0] + center(states[1])[0]) / 2, (center(states[0])[1] + center(states[1])[1]) / 2)
    boxes.append([mid[0] - 1, mid[1] - 1, mid[0] + 1, mid[1] + 1])

    # an empty spot of the canvas: the farthest from every box
    def free_spot():
        best, score = None, -1.0
        for fx in (0.1, 0.3, 0.5, 0.7, 0.9):
            for fy in (0.15, 0.5, 0.85):
                x = canvas[0] + (canvas[2] - canvas[0]) * fx
                y = canvas[1] + (canvas[3] - canvas[1]) * fy
                d = min(((x - (b[0] + b[2]) / 2) ** 2 + (y - (b[1] + b[3]) / 2) ** 2) ** 0.5 for b in boxes if b)
                if d > score:
                    best, score = (x, y), d
        return best

    # right-click the canvas -> Create State > Empty
    spot = free_spot()
    await ui.click(*spot, button=3)
    menu = await ui.item("animator.menu.create_state")
    await ui.move(*center(menu))
    empty = await ui.item("animator.menu.create_empty")
    await shot(ed, "07-create-state-menu")
    await ui.click(*center(empty))
    got = await controller(ed)
    names = [s["name"] for s in controller_states(got)]
    ed.check_eq(names, ["Idle", "Run", "New State"], f"Create State > Empty: {names}")

    # right-click the new state -> Make Transition, then click Idle
    new_state = await ui.item("animator.state.2")
    await ui.click(*center(new_state), button=3)
    item = await ui.item("animator.menu.make_transition")
    await ui.click(*center(item))
    idle = await ui.item("animator.state.0")
    await ui.click(*center(idle))
    got = await controller(ed)
    made = transitions(got)
    ed.check_eq(made, [("Idle", "Run"), ("New State", "Idle")], f"Make Transition: {made}")

    # the new transition is selected: + Condition in the Inspector
    await ed.call("focus_window", window="inspector")
    add = await ui.scroll_to("animator.transition.add_condition", "animator.transition.title")
    await shot(ed, "08-transition-panel")
    await ui.click(*center(add))
    got = await controller(ed)
    conditions = transition(got, "New State")["conditions"]
    ed.check_eq([c["parameter"] for c in conditions], ["Speed"], f"a condition on the first parameter: {conditions}")
    text = (ed.project / CONTROLLER).read_text()
    ed.check('"parameter": "Speed"' in text, "the file has it")

    # Ctrl+Z / Ctrl+Y with the window focused: the project history
    await ed.call("focus_window", window="animator")
    await ui.move(*spot)
    await ui.key("ctrl+z")
    got = await controller(ed)
    ed.check_eq(transition(got, "New State")["conditions"], [], "Ctrl+Z removes the condition")
    await ui.key("ctrl+z")
    got = await controller(ed)
    ed.check_eq(len(transitions(got)), 1, "Ctrl+Z again removes the transition")
    await ui.key("ctrl+y")
    await ui.key("ctrl+y")
    ed.check_eq((ed.project / CONTROLLER).read_text(), text, "Ctrl+Y twice gives the file back")

    # a state drag is one undo step
    run = await ui.item("animator.state.1")
    start = next(s["position"] for s in controller_states(await controller(ed)) if s["name"] == "Run")
    await ui.drag(center(run), (center(run)[0] + 60, center(run)[1] + 30))
    moved = next(s["position"] for s in controller_states(await controller(ed)) if s["name"] == "Run")
    ed.check(moved != start, f"the drag moved Run: {start} -> {moved}")
    await shot(ed, "09-dragged")
    await ui.key("ctrl+z")
    back = next(s["position"] for s in controller_states(await controller(ed)) if s["name"] == "Run")
    ed.check_eq(back, start, "one Ctrl+Z puts it back")
    S["text"] = (ed.project / CONTROLLER).read_text()
    ed.check(path, "the header names the controller")


async def test_animator_window_live_parameters(ed: Editor):
    ui = Ui(ed)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.3)
        await ed.call("select_entities", entities=["Hero"])
        await ed.call("focus_window", window="animator")
        await ui.focus_x()
        # the Parameters tab (a click on an unfocused window may only focus it: again until it shows)
        for _ in range(3):
            tab = await ui.item("animator.tab.parameters")
            await ui.click(*center(tab))
            await ui.frames(6)
            if "animator.parameter.1.value" in ed.ui_items():
                break
        # the running value's checkbox (drawn once the window has the running Animator)
        go = await ui.item("animator.parameter.1.live")
        await shot(ed, "10-live-parameters")
        # (the first click on the checkbox may only give the window the mouse: one more if the
        # running value did not change)
        for attempt in range(2):
            await ui.click(*center(await ui.item("animator.parameter.1.live")))
            await ed.call("wait", game_seconds=0.3)
            live = (await ed.call("get_animator", entity="Hero"))["live"]
            if live["parameters"]["Go"] in (True, 1):
                break
        ed.check(live["parameters"]["Go"] in (True, 1), f"the checkbox set the runtime's Go: {live['parameters']} "
                 f"(file unchanged: {(ed.project / CONTROLLER).read_text() == S['text']})")
        # (on a loaded machine the click lands late in that wait: the transition has only just begun)
        for _ in range(50):
            if live["layers"][0]["state"] == "Base Layer.Run":
                break
            await ed.call("wait", game_seconds=0.1)
            live = (await ed.call("get_animator", entity="Hero"))["live"]
        ed.check_eq(live["layers"][0]["state"], "Base Layer.Run", f"Go moved Idle to Run: {live}")
        await shot(ed, "11-live-run")
        ed.check_eq((ed.project / CONTROLLER).read_text(), S["text"], "the file is unchanged")
    finally:
        await ed.call("play", action="stop")


async def test_delete_goes_to_the_window_not_the_scene(ed: Editor):
    """S61 AC2: Delete with a state selected removes the state, never the selected Hero."""
    ui = Ui(ed)
    await ed.call("select_entities", entities=["Hero"])
    heroes = len(harness.find_key(await ed.call("find_entities", name="Hero*"), "entities") or [])
    await ed.call("focus_window", window="animator")
    await ui.focus_x()
    await ed.call("wait", frames=10)
    names = [state["name"] for state in controller_states(await controller(ed))]
    ed.check(len(names) > 1, f"states to delete: {names}")
    last = await ui.item(f"animator.state.{len(names) - 1}")
    await ui.click(*center(last))
    await ui.frames(6)
    await ui.key("Delete")
    await ui.frames(10)
    ed.check_eq([state["name"] for state in controller_states(await controller(ed))], names[:-1], "Delete removed the selected state")
    ed.check_eq(len(harness.find_key(await ed.call("find_entities", name="Hero*"), "entities") or []), heroes, "Hero survives")
    await ed.call("undo", scope="project")
    await ui.frames(10)
    ed.check_eq([state["name"] for state in controller_states(await controller(ed))], names, "one undo step")
