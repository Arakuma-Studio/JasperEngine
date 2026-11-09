"""S52 AC11 (docs/specs/S52-sprite-instancing.md R30-R33) on the harness' display: Project Settings >
Tags and Layers > Sorting Layers through its widgets (add with the name field and +, rename inline,
reorder with the arrows, remove with -, a used layer asking first), each one undo step of the
project; the renderers' Sorting Layer and Order in Layer rows edit two selected sprites at once
and undo as one step; Add Component > Sorting Group, then undo. Screenshots of the Tags and Layers
panel and of a sorted scene go to JASPER_SHOTS_DIR, else the test's scratch folder (evidence,
outside the repository).
"""

from __future__ import annotations

import os
from pathlib import Path

import ui_input
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "SortingLayersEditor", "xvfb": "harness", "ui_probe": True}

S: dict = {}


async def layers(ed: Editor) -> list:
    return (await ed.call("get_project_settings", keys=["tags_and_layers.sorting_layers"]))["settings"]["tags_and_layers.sorting_layers"]


def as_rect(probe: list) -> dict:
    """A UI probe rectangle ([x0, y0, x1, y1], window pixels) as find_ui_item's {x, y, width, height}"""
    return {"x": probe[0], "y": probe[1], "width": probe[2] - probe[0], "height": probe[3] - probe[1]}


async def item(ed: Editor, helper: ui_input.Ui, name: str, frames: int = 120) -> dict:
    for _ in range(frames // 4):
        items = ed.ui_items()
        if name in items:
            return as_rect(items[name])
        await helper.frames(4)
    ed.fail(f"the UI item {name} never showed: {sorted(i for i in ed.ui_items() if 'sort' in i)}")


async def click(ed: Editor, helper: ui_input.Ui, name: str) -> None:
    await helper.frames(2)
    await helper.click(helper.centre(await item(ed, helper, name)))
    await helper.frames(4)


async def reveal_in_inspector(ed: Editor, helper: ui_input.Ui, name: str) -> dict:
    """A probed Inspector widget, the Inspector scrolled until it is inside the panel"""
    panel = await ed.ui_rect("inspector")
    ed.check(panel, "the Inspector is drawn")
    at = helper.centre(as_rect(panel))
    for _ in range(60):
        await helper.frames(2)
        rect = ed.ui_items().get(name)
        if rect and rect[1] >= panel[1] and rect[3] <= panel[3]:
            return as_rect(rect)
        down = not rect or rect[1] > (panel[1] + panel[3]) / 2
        await helper.wheel(at, 2 if down else -2)
    ed.fail(f"{name} never shows in the Inspector: {sorted(i for i in ed.ui_items() if 'sort' in i)}")


async def shot(ed: Editor, name: str) -> None:
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    png = await ed.screenshot("editor", path=str(shots / f"{name}.png"), max_width=0)
    ed.check(png and len(png) > 1000, f"{name}: the editor window draws")


async def open_tags_and_layers(ed: Editor, helper: ui_input.Ui) -> None:
    await ed.call("run_command", id="edit.projectSettings")
    category = await helper.reveal("Tags and Layers")
    await helper.click(helper.centre(category["rect"]))
    await helper.frames(6)
    await helper.reveal("Sorting Layers")


async def close_settings(ed: Editor) -> None:
    for _ in range(4):
        status = await ed.call("editor_status")
        dialog = status.get("dialog")
        if not dialog:
            return
        await ed.call_raw("respond_dialog", {"choice": "cancel" if "cancel" in dialog.get("choices", []) else "ok"})


async def test_sorting_layers_list(ed: Editor):
    helper = ui_input.Ui(ed)
    made = await ed.call("create_scene", name="Sorted", type="2D", path="scenes/Sorted")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera")
    await ed.call("set_main_camera", scene=S["scene"], entity="Main Camera")
    await open_tags_and_layers(ed, helper)
    await shot(ed, "tags-and-layers-open")
    try:
        # add: type a name, press +
        await click(ed, helper, "sorting_layers.new_name")
        await helper.type("Foreground")
        await helper.frames(2)
        await click(ed, helper, "sorting_layers.add")
        ed.check_eq(await layers(ed), ["Default", "Foreground"], "added after the others")
        await ed.call("undo", scope="project")
        ed.check_eq(await layers(ed), ["Default"], "the add undone")
        await ed.call("redo", scope="project")
        ed.check_eq(await layers(ed), ["Default", "Foreground"], "redone")
        await shot(ed, "tags-and-layers-sorting-layers")

        # rename inline (the id stays: a renderer on it stays on it)
        await ed.call("create_entity", scene=S["scene"], type="sprite", name="Hero", position=[0, 0, 0])
        await ed.call("set_properties", scene=S["scene"], entity="Hero", values={"mesh": {"sortingLayer": "Foreground"}})
        await click(ed, helper, "sorting_layers.name.1")
        await helper.key("ctrl+a")
        await helper.type("Front")
        await helper.key("Return")
        await helper.frames(4)
        ed.check_eq(await layers(ed), ["Default", "Front"], "renamed")
        hero = await ed.call("get_entity", scene=S["scene"], entity="Hero", properties=["sortingLayer"])
        ed.check_eq(hero["components"]["mesh"]["sortingLayer"]["name"], "Front", "the renderer keeps its layer (by id)")
        await ed.call("undo", scope="project")
        ed.check_eq(await layers(ed), ["Default", "Foreground"], "the rename undone")

        # reorder with the arrows
        await click(ed, helper, "sorting_layers.up.1")
        ed.check_eq(await layers(ed), ["Foreground", "Default"], "moved up")
        await ed.call("undo", scope="project")
        ed.check_eq(await layers(ed), ["Default", "Foreground"], "the move undone")

        # remove: a used layer asks first, its renderers then draw on Default
        await click(ed, helper, "sorting_layers.remove.1")
        ed.check_eq(await layers(ed), ["Default", "Foreground"], "a used layer is not removed at once")
        # (the question draws under the row, at the panel's end: scrolled to)
        await helper.wheel(helper.centre(await item(ed, helper, "sorting_layers.up.0")), 10)
        await click(ed, helper, "sorting_layers.confirm_remove")
        ed.check_eq(await layers(ed), ["Default"], "removed once confirmed")
        await ed.call("undo", scope="project")
        ed.check_eq(await layers(ed), ["Default", "Foreground"], "the removal undone")
    finally:
        await close_settings(ed)


async def test_renderer_rows_multi_edit(ed: Editor):
    helper = ui_input.Ui(ed)
    if "scene" not in S:
        made = await ed.call("create_scene", name="Sorted", type="2D", path="scenes/Sorted")
        S["scene"] = made["scene"]["id"]
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Default", "Foreground"]})
    a = await ed.call("create_entity", scene=S["scene"], type="sprite", name="A", position=[0, 0, 0])
    b = await ed.call("create_entity", scene=S["scene"], type="sprite", name="B", position=[10, 10, 0])
    await ed.call("select_entities", entities=[a["entity"]["id"], b["entity"]["id"]])
    await ed.call("focus_window", window="inspector")
    await helper.frames(6)
    combo = await reveal_in_inspector(ed, helper, "sorting.sorting_layer")
    await helper.click(helper.centre(combo))
    await helper.frames(4)
    choice = await helper.reveal("Foreground", "##Combo_00")
    await helper.click(helper.centre(choice["rect"]))
    await helper.frames(4)
    for name in ("A", "B"):
        got = await ed.call("get_entity", scene=S["scene"], entity=name, properties=["sortingLayer"])
        ed.check_eq(got["components"]["mesh"]["sortingLayer"]["name"], "Foreground", f"{name}: both selected sprites take it")
    await shot(ed, "sprite-sorting-rows")
    await ed.call("undo", scene=S["scene"])
    for name in ("A", "B"):
        got = await ed.call("get_entity", scene=S["scene"], entity=name, properties=["sortingLayer"])
        ed.check_eq(got["components"]["mesh"]["sortingLayer"]["name"], "Default", f"{name}: one undo step for both")


async def test_add_sorting_group(ed: Editor):
    helper = ui_input.Ui(ed)
    if "scene" not in S:
        made = await ed.call("create_scene", name="Sorted", type="2D", path="scenes/Sorted")
        S["scene"] = made["scene"]["id"]
    made = await ed.call("create_entity", scene=S["scene"], type="empty", name="Group")
    await ed.call("select_entities", entities=[made["entity"]["id"]])
    await ed.call("focus_window", window="inspector")
    await helper.frames(6)
    add = await helper.reveal("Add Component", "Inspector")
    await helper.click(helper.centre(add["rect"]))
    await helper.frames(4)
    await helper.type("Sorting Group")
    await helper.frames(4)
    # Enter adds the best match of the search
    await helper.key("Return")
    await helper.frames(4)
    got = await ed.call("get_entity", scene=S["scene"], entity="Group")
    ed.check("sortinggroup" in got["components"], f"Add Component > Sorting Group: {list(got['components'])}")
    await shot(ed, "sorting-group-inspector")
    await ed.call("undo", scene=S["scene"])
    got = await ed.call("get_entity", scene=S["scene"], entity="Group")
    ed.check("sortinggroup" not in got["components"], f"undone: {list(got['components'])}")
