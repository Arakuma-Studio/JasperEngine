"""S50's editor UI around the Tile Palette (docs/specs/S50-grid-tilemap.md R63, R67, R68, R69, R71,
R11/S05 hot reload), driven like a person (xdotool in the harness's Xvfb) on an isolated editor:
the Grid Inspector's fields with undo, its layer list (Add Layer, remove, drag to reorder = Order in
Layer); the Layer Inspector's fields and Clear; a .tmj dropped in the Hierarchy opens the import
preview and Import is one undo step; Create Tileset from Assets > Create > 2D (its command) and from
a texture's context menu; a .tileset's Project window thumbnail is its contact sheet; a .tileset
edited on disk refreshes the pool and the drawn layer.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pngdiff
import ui_input
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "TilemapEditorUi", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 900

CELL = 16
ZOOM = 0.5
CENTER = (64.0, 64.0)
TILESET = "assets/art/overworld.tileset"
STATE: dict = {}


def colour(index: int, inverted: bool = False) -> tuple[int, int, int]:
    """The test sheets' tile colours: the index in the red channel (tiles_b.png the inverse)"""
    rgb = ((40 + index * 13) & 255, (200 - index * 7) & 255, index * 3 & 255)
    return tuple(255 - c for c in rgb) if inverted else rgb


def write_sheet(ed: Editor, relative: str, inverted: bool = False) -> None:
    """A 4 x 4 sheet of opaque 16 px tiles, each with its own colour"""
    data = bytearray()
    for y in range(CELL * 4):
        for x in range(CELL * 4):
            data.extend([*colour((y // CELL) * 4 + (x // CELL), inverted), 255])
    path = ed.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(CELL * 4, CELL * 4, bytes(data)))


def tiled_map(data: list[int]) -> dict:
    return {
        "type": "map", "version": "1.10", "orientation": "orthogonal", "renderorder": "right-down",
        "width": 2, "height": 2, "tilewidth": 16, "tileheight": 16, "infinite": False,
        "tilesets": [{"firstgid": 1, "name": "tiles", "image": "tiles.png", "imagewidth": 32, "imageheight": 16,
                      "tilewidth": 16, "tileheight": 16, "tilecount": 2, "columns": 2, "margin": 0, "spacing": 0}],
        "layers": [{"type": "tilelayer", "name": "Ground", "width": 2, "height": 2, "x": 0, "y": 0, "opacity": 1, "visible": True, "data": data}],
    }


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "s50"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def ui(ed: Editor) -> ui_input.Ui:
    return ui_input.for_editor(ed)


async def undo_names(ed: Editor, sid: int) -> list[str]:
    got = await ed.call("get_undo_history", scene=sid, scope="scene", limit=200, include_redo=False)
    return [entry["name"] for entry in reversed(got["entries"])]


async def component(ed: Editor, entity: int, name: str) -> dict:
    return (await ed.call("get_entity", scene=STATE["scene"], entity=entity, components=[name]))["components"][name]


async def layers(ed: Editor) -> list[dict]:
    """The Grid's layers, {id, order} in draw order"""
    got = await ed.call("get_tilemap", scene=STATE["scene"], entity=STATE["grid"])
    rows = [{"id": layer["entity"]["id"], "order": layer["order_in_layer"]} for layer in got["layers"]]
    return sorted(rows, key=lambda row: row["order"])


async def type_into(ed: Editor, label: str, text: str, window: str = "Inspector") -> None:
    """A drag field's text input (double click), its text replaced"""
    item = await ui(ed).reveal(label, window)
    await ui(ed).double_click(ui(ed).centre(item["rect"]))
    await ui(ed).key("ctrl+a")
    await ui(ed).type(text)
    await ui(ed).key("Return")
    await ui_input.frames(ed, 4)


async def wait_ui(ed: Editor, predicate, what: str, frames: int = 160) -> dict:
    state = None
    for _ in range(frames // 2):
        state = (await ed.call("editor_status"))["ui"]
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}; ui: {json.dumps(state)[:800]}")


def tree_node(state: dict, path: str) -> dict | None:
    return next((node for node in state["project_window"]["tree"] if node["path"] == path), None)


def entry(state: dict, name: str) -> list | None:
    return next((item["rect"] for item in state["project_window"]["entries"] if item["name"] == name), None)


def rect_centre(rect: list) -> tuple[float, float]:
    x, y, w, h = rect
    return (x + w / 2, y + h / 2)


async def open_folder(ed: Editor, folder: str) -> dict:
    """The Project window showing `folder` (a child of assets), through its tree"""
    await ed.call("focus_window", window="project")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if not tree_node(state, folder):
        await ui(ed).click(ui(ed).at(tree_node(state, "assets")["arrow"]))
        state = await wait_ui(ed, lambda s: tree_node(s, folder), f"{folder} in the tree")
    await ui(ed).click(ui(ed).at(rect_centre(tree_node(state, folder)["rect"])))
    return await wait_ui(ed, lambda s: s["project_window"]["directory"].rstrip("/").endswith(folder.split("/")[-1]), f"{folder} shown")


async def dialog(ed: Editor, frames: int = 200) -> dict | None:
    for _ in range(frames // 3):
        found = (await ed.call("editor_status")).get("dialog")
        if found:
            return found
        await ui_input.frames(ed, 3)
    return None


async def keep_created_name(ed: Editor) -> None:
    """A created asset starts its rename (as in Unity): cancelled, the name it was given stays"""
    shown = await dialog(ed, frames=60)
    ed.check(shown and shown.get("title") == "Rename File", f"the created file's rename: {shown}")
    await ed.call("respond_dialog", choice="cancel")
    for _ in range(40):
        if not (await ed.call("editor_status")).get("dialog"):
            return
        await ui_input.frames(ed, 3)
    ed.fail(f"the rename dialog stayed open: {(await ed.call('editor_status')).get('dialog')}")


def menu_item(state: dict, label: str) -> list | None:
    return next((item["rect"] for item in state["project_window"]["context_menu"] if item["label"] == label), None)


async def test_setup(ed: Editor):
    if not ed._xvfb or not ed._xvfb.display:
        skip("needs the harness Xvfb")
    await ed.call("tool_groups", enable=["tilemaps", "editor_ux", "assets_pipeline"])
    write_sheet(ed, "assets/art/tiles.png")
    write_sheet(ed, "assets/art/tiles_b.png", inverted=True)
    await ed.call("sync_files", full=False)
    await ed.call("edit_tileset", path=TILESET, output=True, sources=[{"texture": "assets/art/tiles.png", "tile_size": [CELL, CELL], "skip_empty": False}])
    sid = (await ed.call("create_scene", name="Inspectors", type="2d"))["scene"]["id"]
    STATE["scene"] = sid
    await ed.call("select_scene", scene=sid)
    await ed.call("run_command", id="gameobject.create.2DObjectTilemap.rectangular")
    layer = (await ed.call("get_selection", scene=sid))["entities"][0]["id"]
    STATE["layer"] = layer
    parent = (await ed.call("get_entity", scene=sid, entity=layer)).get("parent")
    STATE["grid"] = parent if isinstance(parent, int) else parent["id"]
    await ed.call("set_properties", scene=sid, entity=layer, values={"tilemaplayer": {"tileset": TILESET}})
    await ed.call("set_scene_camera", scene=sid, center=list(CENTER), zoom=ZOOM)
    ui(ed).focus()


async def test_grid_inspector_fields_and_layers(ed: Editor):
    """R68: Cell Size edits with undo; Add Layer (one step, the next Order in Layer), dragging a row
    onto another reorders = Order in Layer (one step), the row's x deletes its layer"""
    sid, grid = STATE["scene"], STATE["grid"]
    await ed.call("select_entities", scene=sid, entities=[grid])
    await ed.call("focus_window", window="inspector")
    await ui_input.frames(ed, 4)
    before = (await component(ed, grid, "grid"))["cellSize"]
    await type_into(ed, "##input_x_cellSize", "24")
    size = (await component(ed, grid, "grid"))["cellSize"]
    ed.check_eq(size[0] if isinstance(size, list) else size["x"], 24, f"Cell Size typed: {size}")
    await ed.call("undo", scene=sid)
    ed.check_eq((await component(ed, grid, "grid"))["cellSize"], before, "one undo restores Cell Size")

    first = (await layers(ed))[0]
    await ui(ed).click_revealed("+ Add Layer", "Inspector")
    await ui_input.frames(ed, 4)
    rows = await layers(ed)
    ed.check_eq(len(rows), 2, f"Add Layer added one: {rows}")
    added = next(row for row in rows if row["id"] != first["id"])
    ed.check_eq(added["order"], first["order"] + 1, "the added layer draws above")
    ed.check_eq((await undo_names(ed, sid))[-1], "Add Tilemap Layer", "Add Layer's step")
    await ed.call("select_entities", scene=sid, entities=[grid])
    await ui_input.frames(ed, 4)
    await shot(ed, "grid-inspector")

    # the upper row dragged onto the lower one: it takes its place, below
    steps = len(await undo_names(ed, sid))
    upper = await ui(ed).reveal(f"(Order {added['order']})", "Inspector")
    lower = await ui(ed).reveal(f"(Order {first['order']})", "Inspector")
    await ui(ed).drag(ui(ed).centre(upper["rect"]), ui(ed).centre(lower["rect"]), steps=12)
    await ui_input.frames(ed, 4)
    rows = await layers(ed)
    ed.check_eq([row["id"] for row in rows], [added["id"], first["id"]], f"the dragged layer draws first now: {rows}")
    ed.check_eq(len(await undo_names(ed, sid)), steps + 1, "the reorder is one step")
    await ed.call("undo", scene=sid)
    ed.check_eq([row["id"] for row in await layers(ed)], [first["id"], added["id"]], "one undo restores the order")

    # the x of the added layer's row deletes it; undo brings it back
    rows_drawn = [i for i in (await ed.call("find_ui_item", label="##removeLayer", window="Inspector"))["items"] if i.get("visible", True)]
    ed.check_eq(len(rows_drawn), 2, f"each row has its x: {rows_drawn}")
    await ui(ed).click(ui(ed).centre(sorted(rows_drawn, key=lambda i: i["rect"]["y"])[1]["rect"]))
    await ui_input.frames(ed, 4)
    ed.check_eq([row["id"] for row in await layers(ed)], [first["id"]], "the x deleted the layer")
    await ed.call("undo", scene=sid)
    ed.check_eq(len(await layers(ed)), 2, "undo restores it")


async def test_layer_inspector_fields_and_clear(ed: Editor):
    """R69: Animation Speed edits with undo; Clear erases the cells as one "Erase Tiles" step"""
    sid, layer = STATE["scene"], STATE["layer"]
    await ed.call("edit_tilemap", scene=sid, entity=layer, ops=[{"op": "set", "cells": [[0, 0, 1], [1, 0, 2], [2, 2, 1]]}])
    await ed.call("select_entities", scene=sid, entities=[layer])
    await ed.call("focus_window", window="inspector")
    await ui_input.frames(ed, 4)
    await type_into(ed, "##input_float_animationSpeed", "2.5")
    ed.check_eq((await component(ed, layer, "tilemaplayer"))["animationSpeed"], 2.5, "Animation Speed typed")
    await ed.call("undo", scene=sid)
    ed.check_eq((await component(ed, layer, "tilemaplayer"))["animationSpeed"], 1.0, "one undo restores it")
    await shot(ed, "layer-inspector")

    await ui(ed).click_revealed("Clear", "Inspector")
    await ui_input.frames(ed, 4)
    got = await ed.call("get_tilemap", scene=sid, entity=layer)
    ed.check_eq(got["tiles"], 0, "Clear erased the cells")
    ed.check_eq((await undo_names(ed, sid))[-1], "Erase Tiles", "Clear's step")
    await ed.call("undo", scene=sid)
    ed.check_eq((await ed.call("get_tilemap", scene=sid, entity=layer))["tiles"], 3, "one undo restores them")


async def test_map_dropped_in_the_hierarchy_opens_the_import_preview(ed: Editor):
    """R46, R63: a .tmj dropped in the Hierarchy shows the plan; Import makes it, one undo step"""
    sid = STATE["scene"]
    folder = ed.project / "assets/maps"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "tiles.png").write_bytes(pngdiff.encode(32, 16, bytes([200, 100, 50, 255] * (32 * 16))))
    (folder / "level.tmj").write_text(json.dumps(tiled_map([1, 2, 0, 1])))
    await ed.call("sync_files", full=False)
    await ed.call("select_entities", scene=sid, entities=[])
    hierarchy = (await ed.call("focus_window", window="hierarchy"))["rect"]
    state = await open_folder(ed, "assets/maps")
    state = await wait_ui(ed, lambda s: entry(s, "level.tmj"), "level.tmj listed")
    steps = len(await undo_names(ed, sid))
    target = (hierarchy["x"] + hierarchy["width"] / 2, hierarchy["y"] + hierarchy["height"] * 0.85)
    await ui(ed).drag(ui(ed).at(rect_centre(entry(state, "level.tmj"))), ui(ed).at(target), steps=16)
    shown = await dialog(ed)
    ed.check(shown, "the drop opened the import preview")
    ed.check("1 Grid(s), 1 layer(s)" in shown.get("message", "") and "3 tile(s)" in shown.get("message", ""), f"the plan: {shown}")
    await shot(ed, "import-preview")
    await ed.call("respond_dialog", choice="ok")
    for _ in range(40):
        if not (await ed.call("editor_status")).get("dialog"):
            break
        await ui_input.frames(ed, 3)
    names = await undo_names(ed, sid)
    ed.check_eq(len(names), steps + 1, f"Import is one undo step: {names[-3:]}")
    ed.check(names[-1].startswith("Import"), f"the step: {names[-1]}")
    grid = (await ed.call("get_selection", scene=sid))["entities"][0]["id"]
    ed.check_eq((await ed.call("get_tilemap", scene=sid, entity=grid))["tiles"], 3, "the map's cells")
    # Ctrl+Z, the timeline's undo: the Grid's selection, then the import
    for _ in range(3):
        await ed.call("undo", scope="timeline")
        if (await ed.call_raw("get_entity", {"scene": sid, "entity": grid})).is_error:
            break
    ed.check((await ed.call_raw("get_entity", {"scene": sid, "entity": grid})).is_error, "undo removes the imported Grid")
    ed.check_eq(len(await undo_names(ed, sid)), steps, "and the one step")


async def test_create_tileset_from_the_menu_and_a_texture(ed: Editor):
    """R63: Assets > Create > 2D > Tileset (its command) writes "New Tileset.tileset" in the Project
    window's folder; a texture's context menu Create Tileset slices it and opens the Tileset Editor"""
    state = await open_folder(ed, "assets/art")
    await ed.call("run_command", id="assets.createTileset")
    made = ed.project / "assets/art/New Tileset.tileset"
    for _ in range(40):
        if made.exists():
            break
        await ui_input.frames(ed, 3)
    ed.check(made.exists(), "Create > 2D > Tileset wrote New Tileset.tileset")
    await keep_created_name(ed)
    ed.check_eq(json.loads(made.read_text())["name"], "New Tileset", "an empty tileset")

    state = await wait_ui(ed, lambda s: entry(s, "tiles_b.png"), "tiles_b.png listed")
    await ui(ed).click(ui(ed).at(rect_centre(entry(state, "tiles_b.png"))), button="3")
    state = await wait_ui(ed, lambda s: menu_item(s, "Create Tileset"), "the texture's context menu offers Create Tileset")
    await ui(ed).click(ui(ed).at(rect_centre(menu_item(state, "Create Tileset"))))
    await ui_input.frames(ed, 6)
    await keep_created_name(ed)
    sliced = ed.project / "assets/art/tiles_b.tileset"
    ed.check(sliced.exists(), "the texture's Create Tileset wrote tiles_b.tileset")
    ed.check_eq(len(json.loads(sliced.read_text())["tiles"]), 16, "one tile per 16 px cell")
    opened = [i for i in (await ed.call("find_ui_item", label="#CLOSE", window="Tileset Editor"))["items"] if i.get("visible", True)]
    ed.check(opened, "the Tileset Editor opened it")
    await shot(ed, "create-tileset")
    await ui(ed).click(ui(ed).centre(opened[0]["rect"]))
    await ui_input.frames(ed, 4)


async def test_tileset_thumbnail_is_its_contact_sheet(ed: Editor):
    """R71: a .tileset's Project window thumbnail shows its tiles, not the file icon"""
    await ed.call("edit_tileset", path="assets/sets/sheet.tileset", output=True,
                  sources=[{"texture": "assets/art/tiles.png", "tile_size": [CELL, CELL], "skip_empty": False}])
    thumbs = ed.project / ".jasperengine" / "thumbs"
    before = {p.name for p in thumbs.glob("*")} if thumbs.exists() else set()
    await open_folder(ed, "assets/sets")
    sheet = None
    for _ in range(120):
        for path in (thumbs.glob("*") if thumbs.exists() else []):
            if path.name in before or path.suffix.lower() != ".png":
                continue
            width, height, rgba = pngdiff.load(path)
            seen = {tuple(rgba[i:i + 3]) for i in range(0, len(rgba), 4)}
            if colour(0) in seen and colour(15) in seen:
                sheet = (path, width, height)
        if sheet:
            break
        await ui_input.frames(ed, 4)
    ed.check(sheet, f"a thumbnail with the first and last tiles' colours was written: {sorted(p.name for p in thumbs.glob('*'))}")
    await shot(ed, "tileset-thumbnail")


async def test_tileset_edited_on_disk_reloads_the_layer(ed: Editor):
    """S05 hot reload of a .tileset (editor/reload/handlers/Tilesets.cpp): the pool resolves the file
    again, the layer reads the new tile names and draws the new texture"""
    sid, layer = STATE["scene"], STATE["layer"]
    await ed.call("edit_tilemap", scene=sid, entity=layer, ops=[{"op": "set", "cells": [[2, 2, 1]]}])
    await ed.call("select_entities", scene=sid, entities=[])
    await ed.call("focus_window", window="scene")
    await ui_input.frames(ed, 6)

    async def pixel() -> tuple[int, int, int]:
        result = await ed.call_raw("screenshot", {"view": "scene", "scene": sid, "max_width": 0})
        rect = result.structured["view_rect_in_window"]
        width, height, rgba = pngdiff.decode(result.images[0][0])
        scale = width / rect["width"]
        x = rect["width"] / 2 + ((2 + 0.5) * CELL - CENTER[0]) / ZOOM
        y = rect["height"] / 2 - ((2 + 0.5) * CELL - CENTER[1]) / ZOOM
        i = (int(y * scale) * width + int(x * scale)) * 4
        return tuple(rgba[i:i + 3])

    def near(a, b) -> bool:
        return all(abs(int(p) - int(q)) <= 6 for p, q in zip(a, b))

    drawn = await pixel()
    ed.check(near(drawn, colour(1)), f"tile 1 is drawn from tiles.png: {drawn} vs {colour(1)}")
    path = ed.project / TILESET
    doc = json.loads(path.read_text())
    source = doc["sources"][0]
    texture = source["texture"]
    old = texture["path"] if isinstance(texture, dict) else texture
    source["texture"] = old.replace("tiles.png", "tiles_b.png")
    for tile in doc["tiles"]:
        if tile["id"] == 1:
            tile["name"] = "reloaded"
    path.write_text(json.dumps(doc, indent=2))
    await ed.call("sync_files", paths=[TILESET])
    legend = {}
    for _ in range(60):
        got = await ed.call("get_tilemap", scene=sid, entity=layer, region=[2, 2, 2, 2])
        legend = got["grid"].get("legend", {})
        if "reloaded" in legend.values():
            break
        await ui_input.frames(ed, 4)
    ed.check("reloaded" in legend.values(), f"the pool read the edited file: {legend}")
    drawn = None
    for _ in range(30):
        drawn = await pixel()
        if near(drawn, colour(1, inverted=True)):
            break
        await ui_input.frames(ed, 4)
    ed.check(near(drawn, colour(1, inverted=True)), f"the layer draws tiles_b.png now: {drawn} vs {colour(1, inverted=True)}")
    await shot(ed, "tileset-reloaded")
