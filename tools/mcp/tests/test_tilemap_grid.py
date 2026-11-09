"""edit_tilemap and get_tilemap on grid tilemaps (docs/specs/S50-grid-tilemap.md R75, R76, R79,
R83, R84, AC10; editor/mcp/tools/TilemapGridTools.cpp): create each layout; paint ASCII; a terrain
lake read back through include_terrain and a stable image; region; missing_tiles; every typed
error with its fix; rollback on a failing later op; preview writes nothing and adds no undo step;
the same operation gives the same cells as the brush rules.
"""

from __future__ import annotations

import hashlib

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "TilemapGrid"}
TIMEOUT = 300

TILES_PNG = "assets/art/tiles.png"
TILESET = "assets/art/overworld.tileset"


def write_sheet(ed: Editor, relative: str, tile: int, columns: int, rows: int) -> None:
    """A sheet of opaque tiles, each with its own colour (index in the red channel)"""
    width, height = tile * columns, tile * rows
    data = bytearray()
    for y in range(height):
        for x in range(width):
            index = (y // tile) * columns + (x // tile)
            data.extend([(40 + index * 13) & 255, (200 - index * 7) & 255, index * 3 & 255, 255])
    path = ed.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(width, height, bytes(data)))


async def scene(ed: Editor, name: str) -> int:
    return (await ed.call("create_scene", name=name, type="2d"))["scene"]["id"]


async def errors_of(ed: Editor, tool: str, args: dict) -> dict:
    result = await ed.call_raw(tool, args, timeout=120)
    ed.check(result.is_error, f"{tool} {args} should fail: {result.text[:400]}")
    structured = result.structured if isinstance(result.structured, dict) else {}
    errors = structured.get("errors") or []
    ed.check(bool(errors), f"typed errors: {result.text[:400]} {structured}")
    ed.check(all(e.get("fix") for e in errors), f"every error names its fix: {errors}")
    return errors[0]


async def undo_count(ed: Editor, sid: int) -> int:
    history = await ed.call("get_undo_history", scene=sid, scope="scene", limit=200)
    return len(history["entries"])


async def test_setup(ed: Editor):
    await ed.call("tool_groups", enable=["tilemaps"])
    write_sheet(ed, TILES_PNG, 16, 4, 4)
    # a sides_16 terrain over the 4x4 sheet: the fixture the terrain and lake tests paint with
    made = await ed.call("edit_tileset", path=TILESET, output=True, sources=[{"texture": TILES_PNG, "tile_size": [16, 16], "skip_empty": False}],
                         terrain_sets=[{"name": "Ground", "mode": "sides", "terrains": [{"name": "Water", "color": "#3a6fd8"}], "template": "sides_16", "origin": 0}])
    ed.check_eq(made["report"]["verdict"], "PASS", f"the fixture tileset's coverage: {made['report']}")


async def test_create_each_layout(ed: Editor):
    sid = await scene(ed, "Layouts")
    for layout in ("rectangle", "isometric", "isometric_staggered", "hexagon"):
        made = await ed.call("edit_tilemap", scene=sid, create={"name": f"G_{layout}"}, layout=layout, tileset=TILESET,
                             ops=[{"op": "paint", "grid": "ab\nb.", "legend": {"a": 0, "b": 1}}])
        ed.check(made["created"], f"{layout}: created")
        ed.check_eq(made["grid"]["lines"], ["ab", "b."], f"{layout}: the ASCII of the touched region")
        ed.check_eq(made["changed_cells"], 3, f"{layout}: changed cells")
        read = await ed.call("get_tilemap", scene=sid, entity=made["grid"]["id"])
        ed.check_eq(read["layout"], layout, "the Grid's layout")
        ed.check_eq(len(read["layers"]), 1, "one Tilemap Layer")
        ed.check_eq(read["layers"][0]["name"], "Tilemap", "the child's name")
    tree = await ed.call("find_entities", scene=sid, name="G_rectangle")
    ed.check_eq(tree["count"], 1, "the Grid keeps the given name")


async def test_create_entity_tilemap_is_one_undo_step(ed: Editor):
    sid = await scene(ed, "CreateEntity")
    before = await undo_count(ed, sid)
    made = await ed.call("create_entity", scene=sid, type="tilemap", name="Level")
    ed.check_eq((await undo_count(ed, sid)), before + 1, "one undo step for the Grid and its layer")
    ed.check_eq(made["entity"]["name"], "Level", "the given name is the Grid's")
    ed.check("children" in made and made["children"][0]["name"] == "Tilemap", f"the child: {made.get('children')}")
    await ed.call("undo", scene=sid)
    ed.check_eq((await ed.call("find_entities", scene=sid, name="Level"))["count"], 0, "undo removes the Grid and the layer")


async def test_paint_and_flips_and_region(ed: Editor):
    sid = await scene(ed, "Paint")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Map"}, tileset=TILESET,
                         ops=[{"op": "paint", "grid": ["abc", "d.a"], "legend": {"a": 0, "b": 1, "c": 2, "d": 3}, "at": [-1, 4]},
                              {"op": "set", "cells": [[5, 0, 1, "xt"], [6, 0, 2], [5, 0, None]]}])
    layer = made["tilemap"]["id"]
    ed.check_eq(made["grid"]["columns"], [-1, 6], "the touched columns")
    got = await ed.call("get_tilemap", scene=sid, entity=layer, region=[-1, 4, 1, 5])
    ed.check_eq(got["grid"]["lines"], ["abc", "d.a"], "the top line is the highest row, negative x included")
    ed.check_eq(got["region"], [-1, 4, 1, 5], "the region returned")
    tiles = await ed.call("get_tilemap", scene=sid, entity=layer, region=[6, 0, 6, 0], include_tiles=True)
    ed.check_eq(tiles["cells"], [[6, 0, 2, 0]], "cells as [x, y, tile, flags]")
    await ed.call("edit_tilemap", scene=sid, entity=layer, ops=[{"op": "set", "cells": [[5, 0, 1, "xt"]]}])
    flipped = await ed.call("get_tilemap", scene=sid, entity=layer, region=[5, 0, 5, 0], include_tiles=True)
    x, y, tile, flags = flipped["cells"][0]
    ed.check_eq((x, y, tile), (5, 0, 1), "the flipped cell")
    ed.check(flags & (1 << 24) and flags & (1 << 26), f"flip x and transpose bits: {flags:#x}")


async def test_terrain_lake_reads_back_and_image_is_stable(ed: Editor):
    sid = await scene(ed, "Lake")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Lake"}, tileset=TILESET,
                         ops=[{"op": "terrain", "terrain": "Water", "from": [2, 2], "to": [8, 6]}])
    ed.check(made["changed_cells"] >= 35, f"the lake's cells: {made['changed_cells']}")
    ed.check_eq(made["warnings"], [], f"no cell without a tile: {made['warnings']}")
    layer = made["tilemap"]["id"]
    got = await ed.call("get_tilemap", scene=sid, entity=layer, include_terrain=True, region=[2, 2, 8, 6])
    ed.check(all(line == "W" * 7 for line in got["terrain"]["lines"]), f"the terrain plane: {got['terrain']}")
    ed.check_eq(got["terrain"]["legend"], {"W": "Water"}, "the terrain legend")

    first = await ed.call_raw("get_tilemap", {"scene": sid, "entity": layer, "image": True, "scale": 2, "region": [2, 2, 8, 6]}, timeout=60)
    second = await ed.call_raw("get_tilemap", {"scene": sid, "entity": layer, "image": True, "scale": 2, "region": [2, 2, 8, 6]}, timeout=60)
    ed.check(first.images and second.images, "an image each")
    ed.check_eq(hashlib.sha256(first.images[0][0]).hexdigest(), hashlib.sha256(second.images[0][0]).hexdigest(), "the same image bytes (R83)")
    width, height, _ = pngdiff.decode(first.images[0][0])
    ed.check_eq((width, height), (7 * 16 * 2, 5 * 16 * 2), "the region at scale 2")

    other = await ed.call("edit_tilemap", scene=sid, create={"name": "Lake2"}, tileset=TILESET,
                          ops=[{"op": "terrain", "terrain": "Water", "from": [2, 2], "to": [8, 6]}])
    again = await ed.call_raw("get_tilemap", {"scene": sid, "entity": other["tilemap"]["id"], "image": True, "scale": 2, "region": [2, 2, 8, 6]}, timeout=60)
    ed.check_eq(hashlib.sha256(again.images[0][0]).hexdigest(), hashlib.sha256(first.images[0][0]).hexdigest(), "the same arguments give the same image")


async def test_missing_tiles(ed: Editor):
    sid = await scene(ed, "Missing")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Map"}, tileset=TILESET,
                         ops=[{"op": "set", "cells": [[0, 0, 3], [1, 0, 15]]}])
    layer = made["tilemap"]["id"]
    # a smaller tileset assigned over the same cells: id 15 no longer exists there
    write_sheet(ed, "assets/art/small.png", 16, 2, 1)
    small = "assets/art/small.tileset"
    await ed.call("edit_tileset", path=small, output=True, sources=[{"texture": "assets/art/small.png", "tile_size": [16, 16], "skip_empty": False}])
    await ed.call("edit_tilemap", scene=sid, entity=layer, tileset=small)
    got = await ed.call("get_tilemap", scene=sid, entity=layer, missing_tiles=True)
    ed.check_eq(got["missing_count"], 2, "both cells are outside the small tileset")
    ed.check([0, 0, 3] in got["missing_tiles"] and [1, 0, 15] in got["missing_tiles"], f"the cells: {got['missing_tiles']}")


async def test_typed_errors(ed: Editor):
    sid = await scene(ed, "Errors")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Rect"}, tileset=TILESET,
                         ops=[{"op": "set", "cells": [[0, 0, 0]]}])
    entity = made["grid"]["id"]
    layer = made["tilemap"]["id"]
    base = {"scene": sid, "entity": layer}

    kind = await errors_of(ed, "edit_tilemap", {**base, "ops": [{"op": "fill", "tile": "nope", "from": [0, 0], "to": [1, 1]}]})
    ed.check_eq(kind["kind"], "unknown_tile", f"unknown_tile: {kind}")
    ed.check("use one of" in kind["fix"], f"the fix lists tiles: {kind['fix']}")
    kind = await errors_of(ed, "edit_tilemap", {**base, "ops": [{"op": "terrain", "terrain": "Lava", "from": [0, 0], "to": [1, 1]}]})
    ed.check_eq(kind["kind"], "unknown_terrain", f"unknown_terrain: {kind}")
    ed.check("Water" in kind["fix"], f"the fix names terrains: {kind['fix']}")
    kind = await errors_of(ed, "edit_tilemap", {"scene": sid, "entity": entity, "layer": "Nope", "ops": [{"op": "set", "cells": [[0, 0, 0]]}]})
    ed.check_eq(kind["kind"], "unknown_layer", f"unknown_layer: {kind}")
    kind = await errors_of(ed, "edit_tilemap", {"scene": sid, "create": {"name": "Hex"}, "layout": "hexagon", "tileset": TILESET,
                                                "ops": [{"op": "set", "cells": [[0, 0, 0, "t"]]}]})
    ed.check_eq(kind["kind"], "layout_mismatch", f"layout_mismatch (hexagon transpose): {kind}")
    kind = await errors_of(ed, "edit_tilemap", {"scene": sid, "create": {"name": "NoFile"}, "tileset": "assets/art/none.tileset"})
    ed.check_eq(kind["kind"], "tileset_missing", f"tileset_missing: {kind}")
    kind = await errors_of(ed, "get_tilemap", {"scene": sid, "entity": entity, "region": [0, 0, 2000, 2000]})
    ed.check_eq(kind["kind"], "region_too_large", f"region_too_large: {kind}")

    await ed.call("set_properties", scene=sid, entity=entity, values={"grid": {"linked": True}})
    kind = await errors_of(ed, "edit_tilemap", {"scene": sid, "entity": entity, "ops": [{"op": "set", "cells": [[0, 0, 0]]}]})
    ed.check_eq(kind["kind"], "locked_linked", f"locked_linked: {kind}")
    await ed.call("set_properties", scene=sid, entity=entity, values={"grid": {"linked": False}})

    # a legacy Free Tilemap, off the grid: tiles from two cell sizes
    legacy = await ed.call("edit_tilemap", scene=sid, create={"name": "Free"}, texture=TILES_PNG,
                           rects=[{"name": "a", "rect": [0, 0, 16, 16]}], cell_size=[16, 16],
                           ops=[{"op": "set", "cells": [[0, 0, "a"], [1, 0, "a"]]}])
    await ed.call("edit_tilemap", scene=sid, entity=legacy["tilemap"]["id"], cell_size=[10, 10], ops=[{"op": "set", "cells": [[2, 2, "a"]]}])
    kind = await errors_of(ed, "edit_tilemap", {"scene": sid, "entity": legacy["tilemap"]["id"], "ops": [{"op": "convert"}]})
    ed.check_eq(kind["kind"], "not_grid_aligned", f"not_grid_aligned: {kind}")


async def test_rollback_on_a_failing_later_op(ed: Editor):
    sid = await scene(ed, "Rollback")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Map"}, tileset=TILESET, ops=[{"op": "set", "cells": [[0, 0, 0]]}])
    layer = made["tilemap"]["id"]
    before = await ed.call("get_tilemap", scene=sid, entity=layer, include_tiles=True)
    undo_before = await undo_count(ed, sid)
    await ed.check_error("edit_tilemap", scene=sid, entity=layer, contains="legend",
                         ops=[{"op": "clear"}, {"op": "paint", "grid": "X", "legend": {}}])
    after = await ed.call("get_tilemap", scene=sid, entity=layer, include_tiles=True)
    ed.check_eq(after["cells"], before["cells"], "a failing later op leaves the cells as they were")
    ed.check_eq(await undo_count(ed, sid), undo_before, "and no undo entry")


async def test_preview_writes_nothing(ed: Editor):
    sid = await scene(ed, "Preview")
    write_sheet(ed, "assets/art/preview.png", 16, 2, 1)
    undo_before = await undo_count(ed, sid)
    result = await ed.call_raw("edit_tilemap", {"scene": sid, "create": {"name": "Ghost"}, "texture": "assets/art/preview.png",
                                                 "tileset": {"tile_size": [16, 16], "names": ["a", "b"]}, "preview": True,
                                                 "ops": [{"op": "fill", "tile": "a", "from": [0, 0], "to": [2, 1]}]}, timeout=60)
    ed.check(not result.is_error, result.text[:400])
    ed.check(result.images, "the preview's image")
    ed.check_eq(result.structured["changed_cells"], 6, "the preview's result")
    ed.check_eq(await undo_count(ed, sid), undo_before, "no undo entry")
    ed.check_eq((await ed.call("find_entities", scene=sid, name="Ghost"))["count"], 0, "no GameObject")
    ed.check(not (ed.project / "assets/art/preview.tileset").exists(), "no tileset file")


async def test_slice_writes_tileset_and_assigns_it(ed: Editor):
    sid = await scene(ed, "Slice")
    write_sheet(ed, "assets/art/slice.png", 16, 2, 1)
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Map"}, texture="assets/art/slice.png",
                         tileset={"columns": 2, "rows": 1, "names": ["grass", "stone"]},
                         ops=[{"op": "paint", "grid": "ab", "legend": {"a": "grass", "b": "stone"}}])
    ed.check((ed.project / "assets/art/slice.tileset").exists(), "the slice wrote <texture>.tileset")
    ed.check_eq(made["tile_rects"], 2, "both tiles")
    read = await ed.call("get_tilemap", scene=sid, entity=made["tilemap"]["id"])
    ed.check_eq(read["texture"], "assets/art/slice.png", "the texture of the tileset")
    ed.check_eq(read["layers"][0]["tileset"], "assets/art/slice.tileset", "assigned to the layer")
    await ed.call("undo", scene=sid)
    ed.check_eq((await ed.call("find_entities", scene=sid, name="Map"))["count"], 0, "one undo removes the whole create")
    ed.check(not (ed.project / "assets/art/slice.tileset").exists(), "undo removes the tileset file written in the step")


async def test_add_layer_and_layer_targeting(ed: Editor):
    sid = await scene(ed, "Layers")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Map"}, tileset=TILESET,
                         ops=[{"op": "add_layer", "name": "Over"}, {"op": "set", "layer": "Over", "cells": [[0, 0, 1]]},
                              {"op": "set", "cells": [[1, 1, 2]]}])
    read = await ed.call("get_tilemap", scene=sid, entity=made["grid"]["id"])
    ed.check_eq([layer["name"] for layer in read["layers"]], ["Tilemap", "Over"], "the layers in hierarchy order")
    ed.check_eq(read["layers"][0]["cells"], 1, "the default layer got its op")
    ed.check_eq(read["layers"][1]["cells"], 1, "the added layer got its op")
    ed.check(read["layers"][1]["order_in_layer"] > read["layers"][0]["order_in_layer"], f"the next Order in Layer: {read['layers']}")
    over = await ed.call("get_tilemap", scene=sid, entity=made["grid"]["id"], layer="Over", include_tiles=True)
    ed.check_eq(over["cells"], [[0, 0, 1, 0]], "layer by name")


def bresenham(ax, ay, bx, by):
    """The rectangle line of NavGridBrush/TilemapBrush: the expected cells of AC6's comparison"""
    dx, dy = abs(bx - ax), abs(by - ay)
    sx, sy = 1 if ax < bx else -1, 1 if ay < by else -1
    err = dx - dy
    cells = []
    x, y = ax, ay
    while True:
        cells.append((x, y))
        if (x, y) == (bx, by):
            return cells
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x += sx
        if e2 < dx:
            err += dx
            y += sy


async def test_brush_ops_equal_the_expected_cells(ed: Editor):
    """AC6, as far as MCP can reach: edit_tilemap's line and flood give the brush's documented cells,
    and random is the same map for the same arguments (TilemapBrushTest compares the C++ brush)."""
    sid = await scene(ed, "Brush")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Map"}, tileset=TILESET,
                         ops=[{"op": "line", "tile": 0, "from": [0, 0], "to": [6, 3]}])
    layer = made["tilemap"]["id"]
    tiles = await ed.call("get_tilemap", scene=sid, entity=layer, include_tiles=True, limit=500)
    ed.check_eq(sorted((c[0], c[1]) for c in tiles["cells"]), sorted(bresenham(0, 0, 6, 3)), "the line's cells")

    await ed.call("edit_tilemap", scene=sid, entity=layer, ops=[{"op": "clear"}, {"op": "fill", "tile": 1, "from": [0, 0], "to": [3, 3]},
                                                                  {"op": "set", "cells": [[1, 1, None], [2, 1, None]]},
                                                                  {"op": "flood", "tile": 2, "at": [0, 0]}])
    tiles = await ed.call("get_tilemap", scene=sid, entity=layer, include_tiles=True, limit=500)
    filled = sum(1 for c in tiles["cells"] if c[2] == 2)
    ed.check_eq(filled, 14, "the flood replaced the connected tile, not the two holes")

    ops = [{"op": "random", "tiles": [0, {"tile": 1, "weight": 3}], "from": [0, 0], "to": [9, 9], "seed": 7}]
    first = await ed.call("edit_tilemap", scene=sid, create={"name": "R1"}, tileset=TILESET, ops=ops)
    second = await ed.call("edit_tilemap", scene=sid, create={"name": "R2"}, tileset=TILESET, ops=ops)
    a = await ed.call("get_tilemap", scene=sid, entity=first["tilemap"]["id"], include_tiles=True, limit=500)
    b = await ed.call("get_tilemap", scene=sid, entity=second["tilemap"]["id"], include_tiles=True, limit=500)
    ed.check_eq(a["cells"], b["cells"], "the same random op gives the same cells")
    ed.check(len({c[2] for c in a["cells"]}) == 2, f"both tiles appear: {a['cells'][:5]}")


async def test_check_pixel_art_lists_a_bilinear_tile_page(ed: Editor):
    """AC10 (R74a): the validator reads each tile page through the layer's Mesh sampler"""
    made = await ed.call("create_scene", name="PixelCheck", type="2d", path="scenes/PixelCheck")
    sid = made["scene"]["id"]
    layer = (await ed.call("edit_tilemap", scene=sid, create={"name": "PixelGrid"}, tileset=TILESET,
                           ops=[{"op": "set", "cells": [[0, 0, 0]]}]))["tilemap"]["id"]
    sampler = "submeshes[0].material.baseColorTexture"
    await ed.call("set_properties", scene=sid, entity=layer,
                  values={"mesh": {sampler: {"path": TILES_PNG, "min_filter": "linear", "mag_filter": "linear"}}})
    scope = {"scenes": ["scenes/PixelCheck.scene"], "rules": ["filtering"]}
    result = await ed.call("check_pixel_art", scope, timeout=120)
    found = [f for f in result["findings"] if f["rule"] == "filtering" and f["target"].get("entity") == "/PixelGrid/Tilemap"]
    ed.check(found and found[0]["status"] == "FAIL", f"the bilinear tile page is listed: {result['findings']}")
    ed.check_eq(found[0]["measured"], {"min_filter": "linear", "mag_filter": "linear"}, "the sampler read")

    await ed.call("set_properties", scene=sid, entity=layer,
                  values={"mesh": {sampler: {"path": TILES_PNG, "min_filter": "nearest", "mag_filter": "nearest"}}})
    fixed = await ed.call("check_pixel_art", scope, timeout=120)
    rule = next(r for r in fixed["rules"] if r["id"] == "filtering")
    ed.check_eq(rule["status"], "PASS", f"nearest passes: {fixed['findings']}")
