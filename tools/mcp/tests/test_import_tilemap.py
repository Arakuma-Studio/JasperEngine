"""import_tilemap (docs/specs/S50-grid-tilemap.md R78, R79, R81, AC8 part; editor/mcp/tools/TilesetTools.cpp):
a preview writes nothing and reports the plan; output writes the tilesets and GameObjects in one undo
step; a parse failure is typed; a linked map re-imports on a file change and keeps a user's component.
"""

from __future__ import annotations

import json

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "ImportTilemap"}
TIMEOUT = 300


def tiled_map(data: list[int]) -> dict:
    return {
        "type": "map", "version": "1.10", "orientation": "orthogonal", "renderorder": "right-down",
        "width": 2, "height": 2, "tilewidth": 16, "tileheight": 16, "infinite": False,
        "tilesets": [{"firstgid": 1, "name": "tiles", "image": "tiles.png", "imagewidth": 32, "imageheight": 16,
                      "tilewidth": 16, "tileheight": 16, "tilecount": 2, "columns": 2, "margin": 0, "spacing": 0}],
        "layers": [{"type": "tilelayer", "name": "Ground", "width": 2, "height": 2, "x": 0, "y": 0, "opacity": 1, "visible": True, "data": data}],
    }


def setup_map(ed: Editor) -> str:
    folder = ed.project / "assets/maps"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "tiles.png").write_bytes(pngdiff.encode(32, 16, bytes([200, 100, 50, 255] * (32 * 16))))
    (folder / "level.tmj").write_text(json.dumps(tiled_map([1, 2, 0, 1])))
    return "assets/maps/level.tmj"


async def scene(ed: Editor, name: str) -> int:
    return (await ed.call("create_scene", name=name, type="2d"))["scene"]["id"]


async def test_preview_writes_nothing(ed: Editor):
    await ed.call("tool_groups", enable=["tilemaps"])
    file = setup_map(ed)
    sid = await scene(ed, "Preview")
    report = await ed.call("import_tilemap", file=file, scene=sid)
    ed.check(report["preview"], "a preview")
    ed.check_eq(report["counts"]["grids"], 1, "one Grid")
    ed.check_eq(report["counts"]["layers"], 1, "one layer")
    ed.check_eq(report["counts"]["tiles"], 3, "the cells with a tile")
    ed.check("output: true" in report["next"], f"next: {report['next']}")
    ed.check_eq((await ed.call("find_entities", scene=sid, name="Ground"))["count"], 0, "no GameObject")
    ed.check(not list((ed.project / "assets/maps").glob("*.tileset")), "no tileset written")


async def test_output_is_one_undo_step(ed: Editor):
    file = "assets/maps/level.tmj"
    sid = await scene(ed, "Output")
    made = await ed.call("import_tilemap", file=file, scene=sid, output=True)
    ed.check_eq(len(made["grids"]), 1, "the Grid made")
    ed.check_eq(len(made["layers"]), 1, "its layer")
    tilesets = list((ed.project / "assets/maps").glob("*.tileset"))
    ed.check(tilesets, "the tileset was written")
    ed.check(all(ts.name in " ".join(made["tileset_files"]) for ts in tilesets), f"listed: {made['tileset_files']}")
    grid_id = made["grids"][0]["id"]
    read = await ed.call("get_tilemap", scene=sid, entity=grid_id, include_tiles=True)
    ed.check_eq(read["tiles"], 3, "the imported cells read back")
    await ed.call("undo", scene=sid)
    ed.check_eq((await ed.call("find_entities", scene=sid, name=made["grids"][0]["name"]))["count"], 0, "one undo removes the Grid")
    ed.check(not list((ed.project / "assets/maps").glob("*.tileset")), "and the tileset file written in the step")


async def test_parse_error_is_typed(ed: Editor):
    sid = await scene(ed, "Parse")
    (ed.project / "assets/maps/broken.tmj").write_text("{ not json")
    result = await ed.call_raw("import_tilemap", {"file": "assets/maps/broken.tmj", "scene": sid, "output": True}, timeout=60)
    ed.check(result.is_error, result.text[:400])
    errors = (result.structured or {}).get("errors") or []
    ed.check(errors and errors[0]["kind"] == "parse", f"typed parse: {result.text[:400]}")
    ed.check(errors[0].get("fix"), f"the fix: {errors}")


async def test_linked_reimport_keeps_a_user_component(ed: Editor):
    file = "assets/maps/linked.tmj"
    folder = ed.project / "assets/maps"
    (folder / "linked.tmj").write_text(json.dumps(tiled_map([1, 1, 1, 1])))
    sid = await scene(ed, "Linked")
    made = await ed.call("import_tilemap", file=file, scene=sid, output=True, linked=True)
    grid = made["grids"][0]["id"]
    layer = made["layers"][0]["id"]
    await ed.call("add_component", scene=sid, entity=layer, component="tilemapcollider2d")
    await ed.check_error("edit_tilemap", scene=sid, entity=grid, ops=[{"op": "set", "cells": [[0, 0, 0]]}], contains="locked_linked")

    (folder / "linked.tmj").write_text(json.dumps(tiled_map([2, 2, 2, 0])))
    await ed.call("sync_files", paths=[file])
    read = await ed.call("get_tilemap", scene=sid, entity=grid)
    for _ in range(60):
        if read["tiles"] == 3:
            break
        await ed.call("wait", frames=5)
        read = await ed.call("get_tilemap", scene=sid, entity=grid)
    ed.check_eq(read["tiles"], 3, "the file change re-imported the cells")
    kept = await ed.call("get_entity", scene=sid, entity=layer, detail="summary")
    ed.check("tilemapcollider2d" in [c.lower() for c in kept["components"]] or "tilemap_collider_2d" in [c.lower() for c in kept["components"]],
             f"the user's component stays: {kept['components']}")
