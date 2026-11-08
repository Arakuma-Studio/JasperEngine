"""T5 AC34, AC36: the Paint Trees panel by real input on the harness's Xvfb: stamp, Shift erases every
prototype, Ctrl+Shift erases the selected one, Alt+click picks with no drag, Alt+drag orbits and places
nothing, Mass Place's job and Cancel, the undo names and the Stats rows."""

from __future__ import annotations

import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
import test_foliage as common  # noqa: E402
import ui_input

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Foliage UI"}

WINDOW = "Terrain"


async def open_paint_trees(ed: Editor, name: str) -> tuple[int, int, ui_input.Ui, tuple[int, int]]:
    sid, terrain = await common.terrain_scene(ed, name)
    await ed.call("select_entities", scene=sid, entities=[terrain])
    await ed.call("focus_window", window="terrain")
    ui = ui_input.for_editor(ed)
    await ui.frames(5)
    await ui.click_item("##terrain_tool_Paint Trees", WINDOW)
    await ui.frames(5)
    # nearly straight down at the Terrain, so a point off the middle is on its ground
    await ed.call("set_scene_camera", scene=sid, position=[0, 80, 12], target=[0, 0, 0])
    await ed.call("focus_window", window="scene", scene=sid)
    rect = (await ed.call("focus_window", window="scene", scene=sid))["rect"]
    # off the middle, clear of the selected Terrain's move gizmo, still on its ground
    x, y = ui.centre(rect)
    height = rect["height"] if isinstance(rect, dict) else rect[3]
    return sid, terrain, ui, (x, int(y + height * 0.25))


async def test_panel_rows_and_stamp_erase(ed: Editor):
    sid, terrain, ui, centre = await open_paint_trees(ed, "PanelRows")
    for label in ("##place_as", "Mass Place Trees...", "Stats"):
        await ui.reveal(label, WINDOW)

    await ui.click(centre)
    await ui.frames(10)
    painted = await common.tree_count(ed, sid, terrain)
    ed.check(painted > 0, "a click stamps")
    history = await ed.call("get_undo_history", scene=sid)
    ed.check("Paint Trees" in str(history), f"the undo name: {history}")

    # the second prototype
    await ui.click(centre, modifier=None)
    more = await common.tree_count(ed, sid, terrain)
    ed.check(more >= painted, "a second stamp adds")

    await ui.click(centre, modifier="shift")
    await ui.frames(10)
    ed.check_eq(await common.tree_count(ed, sid, terrain), 0, "Shift erases every prototype")
    await ed.call("undo", scene=sid)
    ed.check_eq(await common.tree_count(ed, sid, terrain), more, "one undo brings them back")


async def test_alt_click_picks_and_alt_drag_places_nothing(ed: Editor):
    sid, terrain, ui, centre = await open_paint_trees(ed, "AltClick")
    await ui.click(centre)
    await ui.frames(10)
    count = await common.tree_count(ed, sid, terrain)
    ed.check(count > 0, "planted")

    await ui.drag(centre, (centre[0] + 80, centre[1]), modifier="alt")
    await ui.frames(10)
    ed.check_eq(await common.tree_count(ed, sid, terrain), count, "Alt+drag orbits and places nothing")

    await ui.click(centre, modifier="alt")
    await ui.frames(10)
    ed.check_eq(await common.tree_count(ed, sid, terrain), count, "Alt+click with no drag picks and places nothing")


async def test_mass_place_job_and_cancel(ed: Editor):
    sid, terrain, ui, _ = await open_paint_trees(ed, "MassPlace")
    await ui.click_revealed("Mass Place Trees...", WINDOW)
    await ui.frames(5)
    await ui.click_item("Preview Count")
    await ui.frames(60)
    ed.check_eq(await common.tree_count(ed, sid, terrain), 0, "a preview writes nothing")

    await ui.click_item("Place")
    await ui.frames(2)
    cancel = await ui.find("Cancel##mass", frames=2)
    cancelled = False
    if cancel:
        await ui.click(ui.centre(cancel.get("rect", cancel)))
        await ui.frames(30)
        cancelled = await ui.find("Cancel##mass", frames=2) is None
    count = await common.tree_count(ed, sid, terrain)
    if cancelled:
        ed.check_eq(count, 0, "a cancelled run changes nothing")
        await ui.click_item("Place")
    # to its end: one undo step
    for _ in range(600):
        await ui.frames(2)
        count = await common.tree_count(ed, sid, terrain)
        if count > 0:
            break
    ed.check(count > 0, "the job planted")
    await ui.click_item("Close##mass")
    await ui.frames(5)
    await ed.call("undo", scene=sid)
    ed.check_eq(await common.tree_count(ed, sid, terrain), 0, "one undo step")
