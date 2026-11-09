# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S84 AC25 (R42): the Library strip in a real editor window, clicked with the harness's xdotool
on its own Xvfb: a Looks tile applies with one undo step named after the look, Ctrl+Z restores it,
a 3D look on a 2D layer converts it (its tile's badge), and the three tabs draw. Screenshots of the
three tabs go to the test's kept output (JASPER_TEST_WRITE_ACTUAL).
"""

import os
from pathlib import Path

import ui_input
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Library UI", "xvfb": "harness"}
TIMEOUT = 900

PATH = "assets/ui.titleart"


async def open_designer(ed: Editor, helper: ui_input.Ui, tab: str, query: str) -> None:
    await ed.call("focus_window", window="title_designer", title_designer={"path": PATH, "select": "Title", "panel": "library",
                                                                           "library": {"tab": tab, "query": query}})
    await helper.frames(8)


async def step_name(ed: Editor) -> str:
    history = await ed.call("get_undo_history", scope="project", limit=1)
    steps = history.get("steps") or history.get("entries") or []
    return steps[0].get("name", "") if steps else ""


async def title_look(ed: Editor) -> dict:
    got = await ed.call("get_title", path=PATH)
    return next(l for l in got["layers"] if l.get("name") == "Title")


async def click_tile(helper: ui_input.Ui) -> None:
    tile = await helper.reveal("##tile")
    await helper.click(helper.centre(tile["rect"]))
    await helper.frames(8)


async def test_tile_click(ed: Editor):
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=PATH, create={"text": "HERO"}, bake="never")
    helper = ui_input.Ui(ed)
    await open_designer(ed, helper, "looks", "Classic Gold")
    await click_tile(helper)
    ed.check_eq(await step_name(ed), "Apply Look Classic Gold", "one undo step named after the look")
    layer = await title_look(ed)
    ed.check_eq(layer.get("look", {}).get("id"), "metal/classic-gold", f"the look applied: {layer.get('look')}")
    await helper.key("ctrl+z")
    await helper.frames(8)
    layer = await title_look(ed)
    ed.check_eq(layer.get("look", {}).get("id", ""), "", f"Ctrl+Z restored it: {layer.get('look')}")


async def test_badge_converts_to_3d(ed: Editor):
    helper = ui_input.Ui(ed)
    await open_designer(ed, helper, "looks", "Chrome 3D")
    await click_tile(helper)
    layer = await title_look(ed)
    ed.check("three_d" in layer, f"the tile's badge said so and the layer is 3D: {sorted(layer)}, last step '{await step_name(ed)}'")
    await helper.key("ctrl+z")
    await helper.frames(8)


async def test_three_tabs_draw(ed: Editor):
    helper = ui_input.Ui(ed)
    out = os.environ.get("JASPER_TEST_WRITE_ACTUAL")
    for tab in ("looks", "color_schemes", "templates"):
        await open_designer(ed, helper, tab, "")
        items = (await ed.call("find_ui_item", label="##tile")).get("items", [])
        ed.check(items, f"{tab}: the tab shows tiles")
        shot = await ed.screenshot("title_designer", max_width=0)
        ed.check(len(shot) > 1000, f"{tab}: a screenshot")
        if out:
            directory = Path(out) / "title-library-ui"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / f"{tab}.png").write_bytes(shot)
