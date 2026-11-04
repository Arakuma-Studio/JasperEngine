"""Navigation in the editor's UI (docs/specs/S53-navigation-fixes.md), driven like a human in the
harness's Xvfb:

- R21, AC24 (a guard: the keys already worked, see the spec's Deviations): the Inspector's Nav
  Grid "Paint" button arms the brush, then the Terrain brush commands (terrain.brushLarger /
  terrain.brushSmaller, bound to ] and [) resize it by one cell (1-64). Before the brush is armed
  nothing takes the keys, so the commands are disabled. The brush size is read from the
  Inspector's slider (UIProbe item "inspector.navBrushSize.<n>").
- R3, AC6: after a failed bake the Inspector shows "Bake failed: <reason>" and the Navigation
  window's Surfaces tab "Bake failed"; a good bake clears both. The editor screenshots of both
  states (navigation-bake-failed.png, navigation-bake-cleared.png) are for the manual check: in the
  scratch directory, and in build/navigation-ui-shots when it exists (a container's scratch dies
  with it).

Runs in a new empty project (EDITOR create).
"""

from __future__ import annotations

import shutil
from pathlib import Path

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Navigation UI", "ui_probe": True}
TIMEOUT = 600

SHOTS = Path(__file__).resolve().parents[3] / "build" / "navigation-ui-shots"


async def editor_shot(ed: Editor, name: str) -> bytes:
    path = ed.scratch.root / name
    png = await ed.screenshot("editor", path=path)
    if SHOTS.is_dir():
        shutil.copy(path, SHOTS / name)
    return png


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

    # the brush starts one cell across: ] makes it two
    await ui.click_revealed("Paint", window="Inspector")
    await ui.frames(4)
    await ed.call("run_command", id="terrain.brushLarger")
    await ui.frames(4)
    ed.check_eq(await brush_size(ed, ui), 2, "] grows the nav-grid brush by a cell")
    await ed.call("run_command", id="terrain.brushSmaller")
    await ed.call("run_command", id="terrain.brushSmaller")
    await ui.frames(4)
    ed.check_eq(await brush_size(ed, ui), 1, "[ shrinks it, never below one cell")

    await ui.click_revealed("Stop Painting", window="Inspector")
    await ui.frames(4)
    await ed.check_error("run_command", id="terrain.brushLarger", contains="disabled")


async def test_bake_failure_shown(ed: Editor):
    ui = Ui(ed)
    scene = (await ed.call("create_scene", name="NavShown", type="3D"))["scene"]["id"]
    ground = (await ed.call("create_entity", scene=scene, type="plane", name="Ground", scale=[2, 1, 2]))["entity_id"]
    await ed.call("add_component", scene=scene, entity=ground, component="navmeshsurface",
                  values={"navmeshsurface": {"runtimeCarving": True, "overrideTileSize": True, "tileSize": 300}})
    message = await ed.call("bake_navigation", scene=scene, mode="full", expect_error=True)
    ed.check("at most 255 cells" in message, message)
    await ed.call("select_entities", scene=scene, entities=[ground])
    await ed.call("run_command", id="window.navigation")
    ui.focus()
    await ui.click_item("Surfaces", window="Navigation")
    # the Inspector scrolled to the surface's Bake / Clear row, where the failure line is (by the
    # button's whole label, its hammer icon included: found while scrolled away too)
    await ui.reveal("\uf6e3 Bake", window="Inspector")
    await ui.frames(4)
    await editor_shot(ed, "navigation-bake-failed.png")

    await ed.call("set_properties", scene=scene, entity=ground, values={"navmeshsurface": {"overrideTileSize": False}})
    await ed.call("bake_navigation", scene=scene, mode="full")
    await ui.frames(4)
    await editor_shot(ed, "navigation-bake-cleared.png")
    last = (await ed.call("get_navigation_info", scene=scene))["surfaces"][0]["last_bake"]
    ed.check(last["ok"], f"{last}")
