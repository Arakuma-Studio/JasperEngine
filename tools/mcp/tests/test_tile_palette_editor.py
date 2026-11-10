"""S50 AC13 (docs/specs/S50-grid-tilemap.md R63-R70): the Tile Palette and the Tileset Editor,
driven like a person (xdotool in the harness's Xvfb, aimed where find_ui_item and the scene
screenshot's view_rect_in_window say things are drawn) on an isolated editor: a Grid created from
GameObject > 2D Object > Tilemap, tiles chosen in the palette, paint, box, flood, erase, pick,
terrain painting, Esc cancelling a stroke, the undo step names; the Tileset Editor painting
peering values and applying a template. The screenshots go to $JE_EVIDENCE_DIR (else the scratch
directory) for docs/specs/evidence/s50/.
"""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path

import harness
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


async def close_tileset_editor(ed: Editor) -> None:
    """The Tileset Editor's title-bar close button, when its window is open; then it is gone, so a
    Scene-view screenshot after it shows the view whatever ran before"""
    items = [i for i in (await ed.call("find_ui_item", label="#CLOSE", window=EDITOR_WINDOW)).get("items", []) if i.get("visible", True)]
    if not items:
        return
    await ui(ed).click(ui(ed).centre(items[0]["rect"]))
    await ui_input.frames(ed, 4)
    left = (await ed.call("find_ui_item", label="#CLOSE", window=EDITOR_WINDOW)).get("items", [])
    ed.check(not left, f"the Tileset Editor closed: {left}")


async def tool(ed: Editor, name: str) -> None:
    await click_item(ed, f"###tool_{name}")


async def rest_frames(ed: Editor, seconds: float = 3.0) -> int:
    """The frames the editor ran in `seconds` with no input: at rest it idles (EditorFrame), a scene
    redrawn every frame keeps it at full rate"""
    await ed.call("editor_status")   # starts the frame window
    await asyncio.sleep(seconds)
    return (await ed.call("editor_status"))["frame_ms"]["frames"]


async def project_steps(ed: Editor, name: str) -> int:
    """The project history's steps named `name` (the Tileset Editor's writes)"""
    got = await ed.call("get_undo_history", scope="project", limit=500)
    return sum(1 for entry in got["entries"] if entry["name"].startswith(name))


async def drag_held(ed: Editor, start: tuple[int, int], end: tuple[int, int], steps: int = 10) -> int:
    """A drag a frame per step: how many times the .tileset changed on disk before the release"""
    path = ed.project / TILESET
    seen, writes = path.read_bytes(), 0
    await ui(ed).press(start)
    for k in range(1, steps + 1):
        ui(ed).xdo("mousemove", str(int(start[0] + (end[0] - start[0]) * k / steps)), str(int(start[1] + (end[1] - start[1]) * k / steps)))
        await ui_input.frames(ed, 2)
        now = path.read_bytes()
        writes += now != seen
        seen = now
    await ui(ed).release()
    return writes


async def type_held(ed: Editor, text: str) -> int:
    """Types into the active field a key at a time, then Return: how many times the .tileset
    changed on disk before the Return"""
    path = ed.project / TILESET
    seen, writes = path.read_bytes(), 0
    await ui(ed).key("ctrl+a")
    for char in text:
        await ui(ed).type(char)
        await ui_input.frames(ed, 2)
        now = path.read_bytes()
        writes += now != seen
        seen = now
    await ui(ed).key("Return")
    return writes


def tile_doc(ed: Editor, tile: int) -> dict:
    doc = json.loads((ed.project / TILESET).read_text())
    return next((t for t in doc["tiles"] if t["id"] == tile), {})


async def probe(ed: Editor, name: str, frames: int = 60) -> list:
    """The UI probe rect [x0, y0, x1, y1] of `name` once the editor draws it whole"""
    for _ in range(frames // 2):
        rect = ed.ui_items().get(name)
        if rect:
            return rect
        await ui_input.frames(ed, 2)
    ed.fail(f"the UI probe never listed {name!r}: {sorted(ed.ui_items())[:40]}")


def overlay_cells(ed: Editor) -> int | None:
    """How many cells the Scene tool's grid overlay outlined in the last frame (its probe name)"""
    for name in ed.ui_items():
        if name.startswith("tilemap.overlay.cells."):
            return int(name.rsplit(".", 1)[1])
    return None


async def layer_alpha(ed: Editor, layer: int) -> float:
    """The alpha the layer draws with: its tint times Focus On's dim (TilemapSystem)"""
    got = await ed.call("get_entity", scene=STATE["scene"], entity=layer, components=["mesh"])
    color = harness.find_key(got, "baseColorFactor", "submeshes[0].material.baseColorFactor")
    ed.check(isinstance(color, list) and len(color) == 4, f"the layer's colour: {json.dumps(got)[:600]}")
    return float(color[3])


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
    # what the editor draws at rest with no tile tool up (test_scene_view_rests_with_a_tool_up)
    STATE["rest_frames"] = await rest_frames(ed)


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
    got = await ed.call("get_tilemap", scene=STATE["scene"], entity=STATE["layer"], include_terrain=True, region=[10, 2, 10, 2])
    terrain = got.get("terrain", {})
    ed.check_eq(terrain.get("lines"), ["W"], f"the clicked cell's terrain plane holds Water: {terrain}")
    ed.check_eq(terrain.get("legend"), {"W": "Water"}, "W is Water")
    ed.check_eq((await undo_names(ed))[-1], "Paint Terrain", "Paint Terrain's step")
    await click_item(ed, "None")
    await shot(ed, "scene-terrain")


async def test_each_layout_from_the_menu_and_its_overlay(ed: Editor):
    """R63: each menu row makes its layout; R65: the overlay is drawn on it (the screenshots, AC13),
    before the Tileset Editor's window opens over the Scene view"""
    sid = STATE["scene"]
    await close_tileset_editor(ed)
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


async def test_tileset_editor_peering_and_template(ed: Editor):
    try:
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
    finally:
        # the window floats over the Scene view: a later test's screenshots must not show it
        await close_tileset_editor(ed)


async def close_window(ed: Editor, window: str) -> None:
    """A floating window's title-bar close button, when it is open"""
    items = [i for i in (await ed.call("find_ui_item", label="#CLOSE", window=window)).get("items", []) if i.get("visible", True)]
    if items:
        await ui(ed).click(ui(ed).centre(items[0]["rect"]))
        await ui_input.frames(ed, 4)


async def test_tileset_editor_fields_commit_once(ed: Editor):
    """R67, R70: the fields read the document every frame; one held across frames (a drag, a
    slider, typing) shows its edited value until it is let go, then writes the file once, one undo
    step (a drag never wrote; a slider, a colour or a keystroke wrote the file every frame, which
    the undo history merged into one step)"""
    path = ed.project / TILESET
    doc = json.loads(path.read_text())
    doc["property_schema"] = [{"name": "speed", "type": "float"}, {"name": "tint", "type": "color", "default": "#808080ff"}]
    path.write_text(json.dumps(doc, indent=2))
    await ed.call("sync_files", paths=[TILESET])
    try:
        await ed.call("run_command", id="window.tilesetEditor", payload=TILESET)
        await ui_input.frames(ed, 6)
        tiles = (await ui(ed).reveal("##tiles", EDITOR_WINDOW))["rect"]
        step = CELL * 2 + 1
        await ui(ed).click(ui(ed).at(tiles, (step * 2.5, step * 0.5)))   # tile 2
        await ui_input.frames(ed, 4)
        # each field's outcome, checked together at the end: one field failing does not hide the next
        wrong: list[str] = []

        def expect(ok: bool, what: str) -> None:
            if not ok:
                wrong.append(what)

        # Probability: a drag
        before = await project_steps(ed, "Set Probability")
        item = (await ui(ed).reveal("Probability##probability", EDITOR_WINDOW))["rect"]
        start = ui(ed).at(item, (item["width"] * 0.3, item["height"] / 2))
        writes = await drag_held(ed, start, (start[0] + 60, start[1]))
        await ui_input.frames(ed, 4)
        expect(writes == 0, f"the probability drag wrote the file {writes} time(s) while held")
        expect(tile_doc(ed, 2).get("probability", 1.0) > 1.2, f"the drag wrote the probability: {tile_doc(ed, 2)}")
        expect((n := await project_steps(ed, "Set Probability") - before) == 1, f"one undo step for the drag: {n}")

        # Offset and Navigation Area: typed (each keystroke wrote)
        for label, text, name, key, want in (("Offset##offset", "12", "Set Tile Offset", "offset", [12, 0]),
                                             ("Navigation Area##navArea", "34", "Set Navigation Area", "nav_area", 34)):
            before = await project_steps(ed, name)
            x0, y0, x1, y1 = await probe(ed, f"tilesetEditor.{label}")
            await ui(ed).click(ui(ed).at([x0 + (x1 - x0) * 0.1, (y0 + y1) / 2]))
            writes = await type_held(ed, text)
            await ui_input.frames(ed, 4)
            expect(writes == 0, f"typing {label} wrote the file {writes} time(s) before Return")
            expect(tile_doc(ed, 2).get(key) == want, f"{label} typed: {tile_doc(ed, 2).get(key)}")
            expect((n := await project_steps(ed, name) - before) == 1, f"one undo step for typing {label}: {n}")

        # Alpha Threshold: a slider dragged
        before = await project_steps(ed, "Set Alpha Threshold")
        item = (await ui(ed).reveal("Alpha Threshold##threshold", EDITOR_WINDOW))["rect"]
        y = item["height"] / 2
        writes = await drag_held(ed, ui(ed).at(item, (item["width"] * 0.1, y)), ui(ed).at(item, (item["width"] * 0.4, y)))
        await ui_input.frames(ed, 4)
        expect(writes == 0, f"the slider wrote the file {writes} time(s) while held")
        threshold = tile_doc(ed, 2).get("collision", {}).get("threshold", 128)
        expect(threshold != 128, f"the slider wrote the threshold: {tile_doc(ed, 2)}")
        expect((n := await project_steps(ed, "Set Alpha Threshold") - before) == 1, f"one undo step for the slider: {n}")

        # a Float property dragged, a Color property's red dragged
        before = await project_steps(ed, "Set Tile Property")
        item = (await ui(ed).reveal("speed##prop", EDITOR_WINDOW))["rect"]
        start = ui(ed).at(item, (item["width"] * 0.3, item["height"] / 2))
        writes = await drag_held(ed, start, (start[0] + 60, start[1]))
        await ui_input.frames(ed, 4)
        expect(writes == 0, f"the float drag wrote the file {writes} time(s) while held")
        speed = tile_doc(ed, 2).get("properties", {}).get("speed", 0.0)
        expect(speed > 10.0, f"the drag wrote the float property: {tile_doc(ed, 2)}")
        expect((n := await project_steps(ed, "Set Tile Property") - before) == 1, f"one undo step for the float drag: {n}")
        x0, y0, x1, y1 = await probe(ed, "tilesetEditor.tint##prop")
        start = ui(ed).at([x0 + (x1 - x0) * 0.04, (y0 + y1) / 2])
        writes = await drag_held(ed, start, (start[0] + 40, start[1]))
        await ui_input.frames(ed, 4)
        expect(writes == 0, f"the colour drag wrote the file {writes} time(s) while held")
        tint = tile_doc(ed, 2).get("properties", {}).get("tint", "#808080ff")
        expect(tint.lower() != "#808080ff", f"the drag wrote the colour: {tile_doc(ed, 2)}")
        expect((n := await project_steps(ed, "Set Tile Property") - before) == 2, f"one undo step for the colour drag: {n}")
        await shot(ed, "tileset-editor-fields")
        ed.check(not wrong, f"fields that did not commit once: {wrong}; tile 2: {tile_doc(ed, 2)}")
    finally:
        await ui(ed).release_all()
        await close_tileset_editor(ed)


async def test_scene_view_rests_with_a_tool_up(ed: Editor):
    """R66: the tool's overlay is ImGui's, drawn every frame anyway: with a tile tool up and nothing
    changing, the scene is not redrawn every frame, so the editor idles as it does without a tool"""
    rest = STATE.get("rest_frames")
    ed.check(rest is not None, "the first test measured the editor at rest")
    await ed.call("select_scene", scene=STATE["scene"])
    await tool(ed, "Paint")
    # the pointer at rest off the Scene view
    focus = await ui(ed).find("##FocusOn", PALETTE)
    await ui(ed).move(ui(ed).centre(focus["rect"]))
    await ui_input.frames(ed, 6)
    up = await rest_frames(ed)
    print(f"     [frames] 3 s at rest: {rest} without a tool, {up} with the Paint tool up")
    ed.check(up <= rest * 1.4 + 5, f"with a tool up the editor rests too: {up} frames in 3 s, {rest} without one")


async def test_overlay_is_capped_off_the_view(ed: Editor):
    """R65: on a far zoom the diamond and hex overlay outlines a capped block of cells (81 x 81),
    also while the pointer has not been over the view (it outlined every cell the view covers)"""
    sid = STATE["scene"]
    try:
        # the active layer is the last layout's (hexagonal, flat top)
        await ed.call("set_scene_camera", scene=sid, center=[64, 64], zoom=4)
        focus = await ui(ed).find("##FocusOn", PALETTE)
        await ui(ed).move(ui(ed).centre(focus["rect"]))
        # a new tool, which has not seen the pointer
        await ed.call("run_command", id="tilePalette.paint")
        await ui_input.frames(ed, 8)
        count = overlay_cells(ed)
        print(f"     [overlay] {count} cells outlined at zoom 4 with the pointer off the view")
        ed.check(count is not None and 0 < count <= 81 * 81, f"the overlay is capped: {count} cells")
        await shot(ed, "overlay-far-zoom")
    finally:
        await ed.call("set_scene_camera", scene=sid, center=list(STATE["center"]), zoom=ZOOM)


async def test_focus_on_dims_the_others_and_clears(ed: Editor):
    """R65, R70: Focus On Layer dims the scene's other layers; once the focused layer is deleted no
    layer stays dimmed. The Palette's Create Layer... is one step named as the Inspector's Add Layer"""
    sid = STATE["scene"]
    first = STATE["layer"]
    try:
        await ed.call("select_entities", scene=sid, entities=[STATE["grid"]])
        await ui_input.frames(ed, 4)
        await click_item(ed, "##ActiveTilemap")
        await ui(ed).click_item("Create Layer...")
        await ui_input.frames(ed, 4)
        created = (await undo_names(ed))[-1]
        second = (await ed.call("get_selection", scene=sid))["entities"][0]["id"]
        ed.check(second != first, f"a new layer is selected: {second}")
        await ed.call("edit_tilemap", scene=sid, entity=second, ops=[{"op": "set", "cells": [[20, 20, 1]]}])
        await tool(ed, "Paint")
        await click_item(ed, "##FocusOn")
        await ui(ed).click_item("Layer", "##Combo")
        await ui_input.frames(ed, 6)
        ed.check(abs(await layer_alpha(ed, first) - 0.35) < 0.01, "Focus On Layer dims the other layer")
        ed.check(abs(await layer_alpha(ed, second) - 1.0) < 0.01, "and not the focused one")
        await shot(ed, "focus-on-layer")
        await ed.call("delete_entities", scene=sid, entities=[second])
        await ui_input.frames(ed, 6)
        ed.check(abs(await layer_alpha(ed, first) - 1.0) < 0.01, "the focused layer deleted, the other one is undimmed")
        ed.check_eq(created, "Add Tilemap Layer", "the Palette's Create Layer... step")
    finally:
        await click_item(ed, "##FocusOn")
        await ui(ed).click_item("None", "##Combo")
        await ui_input.frames(ed, 4)


async def test_a_stroke_never_lands_in_another_scene(ed: Editor):
    """R66, R70: a tool in another scene's view refuses the Active Tilemap of this one: its stroke
    edited this scene's layer and went to the other scene's undo history"""
    sid = STATE["scene"]
    await ed.call("select_scene", scene=sid)
    await ed.call("select_entities", scene=sid, entities=[STATE["layer"]])
    await ui_input.frames(ed, 4)
    await click_item(ed, "Tiles")
    await choose_tile(ed, 1, 0)
    before = await cells(ed)
    # the Palette follows the selected scene's Grid: closed, the Active Tilemap stays this scene's
    await close_window(ed, PALETTE)
    other = (await ed.call("create_scene", name="Other", type="2d"))["scene"]["id"]
    try:
        await ed.call("select_scene", scene=other)
        await ui_input.frames(ed, 6)
        await ed.call("run_command", id="tilePalette.paint")
        view = (await ed.call_raw("screenshot", {"view": "scene", "scene": other, "max_width": 0})).structured["view_rect_in_window"]
        await ui(ed).click(ui(ed).at(view, (view["width"] / 2, view["height"] / 2)))
        await ui_input.frames(ed, 4)
        ed.check_eq(sorted(await cells(ed)), sorted(before), "the first scene's layer is untouched")
        steps = (await ed.call("get_undo_history", scene=other, scope="scene", limit=50))["entries"]
        ed.check(not any(entry["name"] == "Paint Tiles" for entry in steps), f"no stroke in the other scene's history: {steps}")
    finally:
        await ed.call("select_scene", scene=sid)
        await ed.call("run_command", id="window.tilePalette")
