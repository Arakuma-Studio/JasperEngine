"""S50 AC13 (docs/specs/S50-grid-tilemap.md R63-R70): the Tile Palette and the Tileset Editor,
driven like a person (xdotool in the harness's Xvfb, aimed where find_ui_item and the scene
screenshot's view_rect_in_window say things are drawn) on an isolated editor: a Grid created from
GameObject > 2D Object > Tilemap, tiles chosen in the palette, paint, box, flood, erase, pick,
terrain painting, Esc cancelling a stroke, the undo step names; the Tileset Editor painting
peering values and applying a template. The screenshots go to $JE_EVIDENCE_DIR (else the scratch
directory) for docs/specs/evidence/s50/.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pngdiff
import ui_input
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "TilePaletteEditor", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 900

CELL = 16
ZOOM = 0.5
TILESET = "assets/art/overworld.tileset"
STATE: dict = {}
PALETTE = "Tile Palette"
EDITOR_WINDOW = "Tileset Editor"


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "s50"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def ui(ed: Editor) -> ui_input.Ui:
    if "ui" not in STATE:
        STATE["ui"] = ui_input.for_editor(ed)
    return STATE["ui"]


async def click_item(ed: Editor, label: str, window: str | None = PALETTE) -> None:
    await ui(ed).click_revealed(label, window)


def write_sheet(ed: Editor) -> None:
    """A 4 x 4 sheet of opaque 16 px tiles, each with its own colour (index in the red channel)"""
    tile, cols, rows = CELL, 4, 4
    data = bytearray()
    for y in range(tile * rows):
        for x in range(tile * cols):
            index = (y // tile) * cols + (x // tile)
            data.extend([(40 + index * 13) & 255, (200 - index * 7) & 255, index * 3 & 255, 255])
    path = ed.project / "assets" / "art" / "tiles.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(tile * cols, tile * rows, bytes(data)))


async def cells(ed: Editor) -> list:
    got = await ed.call("get_tilemap", scene=STATE["scene"], entity=STATE["layer"], include_tiles=True, limit=4000)
    return [tuple(c[:3]) for c in got.get("cells", [])]


async def tile_at(ed: Editor, x: int, y: int) -> int | None:
    for cx, cy, tile in await cells(ed):
        if cx == x and cy == y:
            return tile
    return None


async def undo_names(ed: Editor) -> list[str]:
    got = await ed.call("get_undo_history", scene=STATE["scene"], scope="scene", limit=200)
    return [entry["name"] for entry in reversed(got["entries"])]


async def scene_rect(ed: Editor) -> dict:
    """The Scene view's rect, read once: each test asks, so one failing does not break the next"""
    if "rect" not in STATE:
        result = await ed.call_raw("screenshot", {"view": "scene", "scene": STATE["scene"], "max_width": 0})
        STATE["rect"] = result.structured["view_rect_in_window"]
    return STATE["rect"]


def cell_point(ed: Editor, rect: dict, col: float, row: float) -> tuple[int, int]:
    """The window pixel of a cell's centre: the Scene view's centre shows CENTER at 0.5 world
    units per pixel"""
    wx = (col + 0.5) * CELL
    wy = (row + 0.5) * CELL
    x = rect["width"] / 2 + (wx - STATE["center"][0]) / ZOOM
    y = rect["height"] / 2 - (wy - STATE["center"][1]) / ZOOM
    return ui(ed).at(rect, (x, y))


async def choose_tile(ed: Editor, col: int, row: int) -> None:
    item = await ui(ed).reveal("##grid", PALETTE)
    rect = item.get("rect", item)
    scale = (CELL * 2 + 1)   # the palette's 2x zoom and its one-pixel gap
    await ui(ed).click(ui(ed).at(rect, ((col + 0.5) * scale, (row + 0.5) * scale)))


async def tool(ed: Editor, name: str) -> None:
    await click_item(ed, f"###tool_{name}")


async def test_create_grid_and_palette(ed: Editor):
    if not ed._xvfb or not ed._xvfb.display:
        skip("needs the harness Xvfb")
    await ed.call("tool_groups", enable=["tilemaps", "editor_ux", "assets_pipeline"])
    write_sheet(ed)
    made = await ed.call("edit_tileset", path=TILESET, output=True, sources=[{"texture": "assets/art/tiles.png", "tile_size": [CELL, CELL], "skip_empty": False}],
                         terrain_sets=[{"name": "Ground", "mode": "sides", "terrains": [{"name": "Water", "color": "#3a6fd8"}], "template": "sides_16", "origin": 0}])
    ed.check_eq(made["report"]["verdict"], "PASS", "the fixture tileset's coverage")
    sid = (await ed.call("create_scene", name="Palette", type="2d"))["scene"]["id"]
    STATE["scene"] = sid
    await ed.call("select_scene", scene=sid)
    await ed.call("run_command", id="gameobject.create.2DObjectTilemap.rectangular")
    selection = await ed.call("get_selection", scene=sid)
    ed.check(len(selection["entities"]) == 1, f"the created Tilemap is selected: {selection}")
    layer = selection["entities"][0]["id"]
    STATE["layer"] = layer
    got = await ed.call("get_entity", scene=sid, entity=layer)
    ed.check("tilemaplayer" in got["components"], f"a Tilemap Layer was created: {list(got['components'])}")
    parent = got.get("parent")
    STATE["grid"] = parent if isinstance(parent, int) else parent["id"] if parent else None
    grid = await ed.call("get_entity", scene=sid, entity=STATE["grid"])
    ed.check("grid" in grid["components"], f"its parent is the Grid: {grid}")
    names = await undo_names(ed)
    ed.check(len([n for n in names if "Tilemap" in n or "Grid" in n]) == 1, f"one undo step for Grid and child: {names}")
    await ed.call("set_properties", scene=sid, entity=layer, values={"tilemaplayer": {"tileset": TILESET}})
    await ed.call("run_command", id="window.tilePalette")
    STATE["center"] = (64.0, 64.0)
    await ed.call("set_scene_camera", scene=sid, center=[64, 64], zoom=ZOOM)
    ui(ed).focus()
    await ui_input.frames(ed, 6)
    await shot(ed, "palette-window")


async def test_paint_erase_pick(ed: Editor):
    await choose_tile(ed, 1, 0)   # tile 1 (the sheet's second tile of the top row)
    rect = await scene_rect(ed)
    await ui(ed).click(cell_point(ed, rect, 2, 2))
    ed.check_eq(await tile_at(ed, 2, 2), 1, "Paint wrote the chosen tile")
    names = await undo_names(ed)
    ed.check_eq(names[-1], "Paint Tiles", f"the stroke's step: {names}")
    await shot(ed, "scene-painted")

    # Shift erases with Paint
    await ui(ed).click(cell_point(ed, rect, 3, 2), modifier="shift")
    await ui(ed).click(cell_point(ed, rect, 2, 2), modifier="shift")
    ed.check_eq(await tile_at(ed, 2, 2), None, "Shift erased the cell")
    ed.check_eq((await undo_names(ed))[-1], "Erase Tiles", "Erase Tiles")

    await ui(ed).click(cell_point(ed, rect, 2, 2))
    await tool(ed, "Eraser")
    await ui(ed).click(cell_point(ed, rect, 2, 2))
    ed.check_eq(await tile_at(ed, 2, 2), None, "the Eraser tool erased it")

    # the Picker takes a cell's tile as the brush and hands over to Paint
    await tool(ed, "Paint")
    await choose_tile(ed, 2, 1)   # tile 6
    await ui(ed).click(cell_point(ed, rect, 5, 5))
    ed.check_eq(await tile_at(ed, 5, 5), 6, "painted tile 6")
    await choose_tile(ed, 0, 0)
    await tool(ed, "Picker")
    await ui(ed).click(cell_point(ed, rect, 5, 5))     # picks tile 6
    await ui(ed).click(cell_point(ed, rect, 6, 5))     # Paint, with the picked tile
    ed.check_eq(await tile_at(ed, 6, 5), 6, "the picked tile was painted")
    # Ctrl picks from any tool
    await tool(ed, "Paint")
    await choose_tile(ed, 0, 0)
    await ui(ed).click(cell_point(ed, rect, 5, 5), modifier="ctrl")  # picks tile 6 again
    await ui(ed).click(cell_point(ed, rect, 7, 5))
    ed.check_eq(await tile_at(ed, 7, 5), 6, "Ctrl picked the cell's tile")
    await shot(ed, "scene-picked")


async def test_box_and_flood(ed: Editor):
    rect = await scene_rect(ed)
    await tool(ed, "Box")
    await choose_tile(ed, 3, 0)   # tile 3
    await ui(ed).drag(cell_point(ed, rect, 1, 1), cell_point(ed, rect, 3, 3))
    tiles = await cells(ed)
    box = {(x, y) for x in range(1, 4) for y in range(1, 4)}
    ed.check(box.issubset({(x, y) for x, y, t in tiles if t == 3}), f"the box filled 3 x 3: {tiles}")
    ed.check_eq((await undo_names(ed))[-1], "Box Fill", "Box Fill's step")
    await shot(ed, "scene-box")

    await tool(ed, "Flood")
    await choose_tile(ed, 0, 1)   # tile 4
    await ui(ed).click(cell_point(ed, rect, 2, 2))
    tiles = await cells(ed)
    ed.check(box.issubset({(x, y) for x, y, t in tiles if t == 4}), f"the flood replaced the box's tile: {tiles}")
    ed.check_eq((await undo_names(ed))[-1], "Flood Fill", "Flood Fill's step")


async def test_esc_cancels_a_stroke(ed: Editor):
    rect = await scene_rect(ed)
    await tool(ed, "Paint")
    await choose_tile(ed, 1, 1)   # tile 5
    before = await cells(ed)
    names = await undo_names(ed)
    start, end = cell_point(ed, rect, 9, 9), cell_point(ed, rect, 12, 9)
    await ui(ed).press(start)
    await ui(ed).move(end)
    ed.check((10, 9, 5) in await cells(ed) or (11, 9, 5) in await cells(ed), "the stroke previews live")
    await ui(ed).key("Escape")
    await ui(ed).release()
    await ui_input.frames(ed, 4)
    ed.check_eq(sorted(await cells(ed)), sorted(before), "Esc restored the cells")
    ed.check_eq(await undo_names(ed), names, "and left no undo step")


async def test_terrain_painting(ed: Editor):
    rect = await scene_rect(ed)
    await click_item(ed, "Terrains")
    await click_item(ed, "Water")
    await ui(ed).click(cell_point(ed, rect, 10, 2))
    got = await ed.call("get_tilemap", scene=STATE["scene"], entity=STATE["layer"], include_terrain=True, include_tiles=True, limit=4000)
    ed.check("terrain" in got or any("Water" in str(v) for v in got.values()), f"the terrain plane was written: {list(got)}")
    ed.check_eq((await undo_names(ed))[-1], "Paint Terrain", "Paint Terrain's step")
    await click_item(ed, "None")
    await shot(ed, "scene-terrain")


async def test_tileset_editor_peering_and_template(ed: Editor):
    await ed.call("run_command", id="window.tilesetEditor", payload=TILESET)
    await ui_input.frames(ed, 6)
    await shot(ed, "tileset-editor")
    tiles = (await ui(ed).reveal("##tiles", EDITOR_WINDOW))["rect"]
    step = CELL * 2 + 1   # the grid's 2x zoom and its one-pixel gap

    def peering_n() -> object:
        doc = json.loads((ed.project / TILESET).read_text())
        return next(t for t in doc["tiles"] if t["id"] == 1)["terrain"]["peering"]["n"]

    # tile 1 of sides_16 has its north side: the painter turns that region to None
    original = peering_n()
    ed.check(original != "none", f"the fixture's tile 1 has a north side: {original}")
    await ui(ed).click(ui(ed).at(tiles, (step * 1.5, step * 0.5)))
    await ui_input.frames(ed, 4)
    await click_item(ed, "None##paintNone", EDITOR_WINDOW)
    canvas = (await ui(ed).reveal("##peeringCanvas", EDITOR_WINDOW))["rect"]
    await ui(ed).click(ui(ed).at(canvas, (canvas["width"] / 2, canvas["height"] / 6)))   # the 3 x 3 painter's north region
    await ui_input.frames(ed, 4)
    ed.check_eq(peering_n(), "none", "the painter wrote the peering value")
    await shot(ed, "tileset-editor-peering")

    # the template's origin is the selected tile; applying it from tile 0 restores tile 1's north
    tiles = (await ui(ed).reveal("##tiles", EDITOR_WINDOW))["rect"]
    await ui(ed).click(ui(ed).at(tiles, (step * 0.5, step * 0.5)))
    await ui_input.frames(ed, 4)
    await click_item(ed, "Apply Terrain Template...", EDITOR_WINDOW)
    dialog = None
    for _ in range(200):
        dialog = (await ed.call("editor_status")).get("dialog")
        if dialog:
            break
        await ui_input.frames(ed, 3)
    ed.check(dialog, "the template modal opened")
    ed.check("template" in {f["name"] for f in dialog.get("fields", [])}, f"its fields: {dialog}")
    await ed.call("respond_dialog", choice="ok", values={"template": "Sides 16 (4x4)", "terrain": "Water"})
    still_open = None
    for _ in range(40):
        still_open = (await ed.call("editor_status")).get("dialog")
        if not still_open:
            break
        await ui_input.frames(ed, 3)
    ed.check(not still_open, f"Apply closed the dialog: {still_open}")
    ed.check_eq(peering_n(), original, "the template assigned the peering values again")
    await shot(ed, "tileset-editor-template")


async def test_each_layout_from_the_menu_and_its_overlay(ed: Editor):
    """R63: each menu row makes its layout; R65: the overlay is drawn on it (the screenshots, AC13)"""
    sid = STATE["scene"]
    await ed.call("focus_window", window="scene")
    rect = await scene_rect(ed)
    await tool(ed, "Paint")
    for command, layout, axis in (("isometric", "isometric", None), ("isometricStaggered", "isometric_staggered", None),
                                  ("hexagonalPointyTop", "hexagon", "y"), ("hexagonalFlatTop", "hexagon", "x")):
        await ed.call("run_command", id=f"gameobject.create.2DObjectTilemap.{command}")
        layer = (await ed.call("get_selection", scene=sid))["entities"][0]["id"]
        made = await ed.call("get_entity", scene=sid, entity=layer)
        parent = made.get("parent")
        grid = (await ed.call("get_entity", scene=sid, entity=parent if isinstance(parent, int) else parent["id"]))["components"]["grid"]
        ed.check_eq(grid["layout"], layout, f"{command}'s Grid layout")
        if axis:
            ed.check_eq(grid["staggerAxis"], axis, f"{command}'s stagger axis")
        await ed.call("set_properties", scene=sid, entity=layer, values={"tilemaplayer": {"tileset": TILESET}})
        await ui_input.frames(ed, 4)
        await ui(ed).move(cell_point(ed, rect, 4, 4))
        await ui_input.frames(ed, 4)
        await shot(ed, f"overlay-{command}")
