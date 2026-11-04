"""The Animator window's canvas driven like a human (BF23 F17, F18, F19): xdotool in the harness's Xvfb, widgets found
through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h), on a new project with one controller.

- the window is begun once a frame (F17: App drew it twice, appending to itself);
- a box select keeps both states (F18: the window rebuilt the selection from one state each frame), a drag moves
  them as one undo step and Delete removes them as one;
- Esc while the mouse is down puts a dragged state back and adds no step (F19).

Screenshots go to $JASPER_SHOTS_DIR when set (else the scratch directory).
"""

from __future__ import annotations

import os
from pathlib import Path

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Animator Selection", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 600

CONTROLLER = "assets/anim/selection.controller"
ORIGINAL_STATES = ["A", "B", "C"]


def centre(ui: Ui, rect: list[float]) -> tuple[int, int]:
    """The screen point at a probe rect's centre (probe rects are [x0, y0, x1, y1] in window pixels)"""
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def shot(ed: Editor, name: str) -> None:
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    await ed.call("wait", frames=4)
    await ed.screenshot("editor", path=str(shots / f"animator-selection-{name}.png"), max_width=1600)


async def item(ui: Ui, name: str) -> list[float]:
    for _ in range(30):
        await ui.frames(2)
        rect = ui.ed.ui_items().get(name)
        if rect:
            return rect
    ui.ed.fail(f"{name} is never drawn: {sorted(ui.ed.ui_items())}")


async def states(ed: Editor) -> dict:
    got = await ed.call("get_animation", controller=CONTROLLER)
    return {s["name"]: s["position"] for s in got["layers"][0]["state_machine"]["states"]}


async def open_window(ed: Editor) -> Ui:
    await ed.call("select_entities", entities=["Rig"])
    shown = await ed.call("focus_window", window="animator")
    ed.check(shown.get("shown"), f"the Animator window shows: {shown}")
    ui = Ui(ed)
    await ui.frames(6)
    await item(ui, "animator.state.2")
    return ui


async def test_setup(ed: Editor):
    made = await ed.call_raw("edit_animator_controller", {"path": CONTROLLER, "create": True, "ops": [
        {"op": "add_state", "name": "A", "position": [300, 60]},
        {"op": "add_state", "name": "B", "position": [560, 60]},
        {"op": "add_state", "name": "C", "position": [300, 240]}]})
    ed.check(not made.is_error, made.text[:600])
    await ed.call("create_scene", name="Selection", type="3D", path="scenes/Selection")
    await ed.call("create_entity", name="Rig")
    await ed.call("add_component", entity="Rig", component="animator", values={"animator": {"controller": CONTROLLER}})
    await ed.call("sync_files")


async def test_window_is_begun_once_a_frame(ed: Editor):
    ui = await open_window(ed)
    await ui.frames(4)
    begins = ed.ui_probe_state().get("begins")
    ed.check(begins is not None, f"the probe reports begins: {sorted(ed.ui_probe_state())}")
    ed.check("Animator" not in begins, f"the Animator is begun once a frame: {begins}")
    ed.check_eq(begins, {}, "no panel is begun twice a frame")


async def test_box_select_drag_and_delete(ed: Editor):
    ui = await open_window(ed)
    a = await item(ui, "animator.state.0")
    b = await item(ui, "animator.state.1")
    canvas = await item(ui, "animator.canvas")
    # from the empty corner right of B and above it, over both boxes (below them and left of A: C and Entry stay out)
    start = ui.at([min(b[2] + 30, canvas[2] - 4), max(b[1] - 12, canvas[1] + 4)])
    end = ui.at([a[0] - 8, a[3] + 8])
    await ui.drag(start, end)
    await ui.frames(4)
    drawn = ed.ui_items()
    await shot(ed, "01-box-selected")
    ed.check("animator.state.0.selected" in drawn and "animator.state.1.selected" in drawn,
             f"A and B stay selected on the next frame: {[n for n in drawn if n.endswith('.selected')]}")
    ed.check("animator.state.2.selected" not in drawn, "C is outside the box")

    # one drag moves both, one undo step puts both back
    before = await states(ed)
    grab = centre(ui, a)
    await ui.drag(grab, (grab[0] + 40, grab[1] + 30))
    moved = await states(ed)
    ed.check(moved["A"] != before["A"] and moved["B"] != before["B"], f"both moved: {before} -> {moved}")
    ed.check_eq(moved["C"], before["C"], "C did not move")
    await shot(ed, "02-both-dragged")
    await ed.call("undo", scope="project")
    ed.check_eq(await states(ed), before, "one undo puts both back")
    await ed.call("redo", scope="project")
    await ui.frames(4)

    # Delete removes them both as one step (the X window takes the keys; the window keeps ImGui's focus)
    ui.focus()
    await ui.key("Delete")
    remaining = await states(ed)
    ed.check_eq(sorted(remaining), ["C"], "Delete removes the selected states")
    await shot(ed, "03-both-deleted")
    await ed.call("undo", scope="project")
    ed.check_eq(sorted(await states(ed)), ORIGINAL_STATES, "one undo restores both")
    await ui.release_all()


async def test_esc_cancels_a_state_drag(ed: Editor):
    ui = await open_window(ed)
    c = await item(ui, "animator.state.2")
    before = await states(ed)
    text = (ed.project / CONTROLLER).read_text()
    steps = len((await ed.call("get_undo_history", scope="project"))["entries"])
    grab = centre(ui, c)
    await ui.drag(grab, (grab[0] + 70, grab[1] - 40), release=False)
    await shot(ed, "04-dragging")
    await ui.key("Escape")
    await shot(ed, "05-esc-pressed")
    await ui.frames(2)
    await ui.release()
    await ui.frames(4)
    ed.check_eq(await states(ed), before, "the state is back where the drag began")
    ed.check_eq((ed.project / CONTROLLER).read_text(), text, "the file is unchanged")
    ed.check_eq(len((await ed.call("get_undo_history", scope="project"))["entries"]), steps, "no undo step was added")
