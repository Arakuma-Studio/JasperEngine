"""The nav-grid brush's [ and ] keys (docs/specs/S41-navigation-fixes.md R21, AC24), driven like a
human in the harness's Xvfb: the Inspector's Nav Grid "Paint" button arms the brush, then the
Terrain brush commands (terrain.brushLarger / terrain.brushSmaller, bound to ] and [) resize it
by one cell (1-64). Before the brush is armed nothing takes the keys, so the commands are
disabled. The brush size is read from the Inspector's slider (UIProbe item
"inspector.navBrushSize.<n>"). Runs in a new empty project (EDITOR create).
"""

from __future__ import annotations

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Navigation UI", "ui_probe": True}
TIMEOUT = 600


async def brush_size(ed: Editor, ui: Ui) -> int | None:
    """The size the Inspector's brush slider shows (None while it is not drawn whole)."""
    await ui.reveal("Size (cells)", window="Inspector")
    for _ in range(30):
        sizes = [int(name.rsplit(".", 1)[1]) for name in ed.ui_items() if name.startswith("inspector.navBrushSize.")]
        if sizes:
            return sizes[0]
        await ui.frames(2)
    return None


async def test_brush_size_keys(ed: Editor):
    ui = Ui(ed)
    scene = (await ed.call("create_scene", name="NavBrush", type="3D"))["scene"]["id"]
    grid = (await ed.call("create_entity", scene=scene, type="empty", name="Grid"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=grid, component="navgrid",
                  values={"navgrid": {"width": 8, "height": 8, "cellSize": 1}})
    await ed.call("select_entities", scene=scene, entities=[grid])
    ui.focus()
    await ui.frames(4)
    # no brush armed: nothing takes [ and ]
    await ed.check_error("run_command", id="terrain.brushLarger", contains="disabled")

    await ui.click_revealed("Paint", window="Inspector")
    ed.check_eq(await brush_size(ed, ui), 1, "the brush starts one cell across")
    ran = await ed.call("run_command", id="terrain.brushLarger")
    ed.check(ran is not None, f"{ran}")
    await ui.frames(4)
    ed.check_eq(await brush_size(ed, ui), 2, "] grows the nav-grid brush by a cell")
    await ed.call("run_command", id="terrain.brushSmaller")
    await ed.call("run_command", id="terrain.brushSmaller")
    await ui.frames(4)
    ed.check_eq(await brush_size(ed, ui), 1, "[ shrinks it, never below one cell")

    await ui.click_revealed("Stop Painting", window="Inspector")
    await ui.frames(4)
    await ed.check_error("run_command", id="terrain.brushLarger", contains="disabled")
