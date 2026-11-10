"""edit_tileset (docs/specs/S50-grid-tilemap.md R77, R79, AC11; editor/mcp/tools/TilesetTools.cpp):
the tilemaps group is hidden until enabled, a preview writes nothing and returns the report and a
contact sheet, output writes one undo step, templates assign peering values, the coverage check
answers PASS/WARN/FAIL, and every property and peering error is typed.
"""

from __future__ import annotations

import json

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Tilesets"}
TIMEOUT = 300

TEXTURE = "assets/art/ground.png"
PATH = "assets/art/ground.tileset"


def sheet(ed: Editor, relative: str, tile: int, columns: int, rows: int) -> None:
    width, height = tile * columns, tile * rows
    data = bytes((30 + (y // tile) * 40 + (x // tile) * 10) & 255 if i == 0 else (120 + i * 30) & 255 for y in range(height) for x in range(width) for i in range(3))
    # the loop above writes RGB; add the alpha here (opaque tiles)
    rgba = bytearray()
    for i in range(0, len(data), 3):
        rgba += data[i:i + 3] + b"\xff"
    path = ed.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(width, height, bytes(rgba)))


async def fails(ed: Editor, args: dict, kind: str) -> dict:
    result = await ed.call_raw("edit_tileset", args, timeout=120)
    ed.check(result.is_error, f"edit_tileset {args} should fail with {kind}: {result.text[:400]}")
    errors = (result.structured or {}).get("errors") or []
    ed.check(errors and errors[0]["kind"] == kind, f"kind {kind}: {result.text[:400]}")
    ed.check(all(e.get("fix") for e in errors), f"every error names its fix: {errors}")
    return errors[0]


async def test_group_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_tileset" not in default and "import_tilemap" not in default, "tilemaps is hidden by default")
    await ed.call("tool_groups", enable=["tilemaps"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_tileset" in listed and "import_tilemap" in listed, "listed once tool_groups enables tilemaps")


async def test_preview_then_output_one_undo_step(ed: Editor):
    sheet(ed, TEXTURE, 16, 4, 4)
    target = ed.project / PATH
    args = {"path": PATH, "sources": [{"texture": TEXTURE, "tile_size": [16, 16], "skip_empty": False}],
            "property_schema": [{"name": "damage", "type": "int", "default": 0}],
            "tiles": [{"id": 5, "name": "lava", "probability": 0.5, "properties": {"damage": 3}, "collision": {"mode": "full"},
                       "animation": [{"tile": 5, "ms": 120}, {"tile": 6, "ms": 120}]}],
            "stamps": [{"name": "pair", "cells": [[0, 0, 1], [1, 0, 2, "x"]]}]}
    preview = await ed.call_raw("edit_tileset", args, timeout=60)
    ed.check(not preview.is_error, preview.text[:400])
    ed.check(not target.exists(), "a preview writes nothing")
    ed.check(preview.images, "the contact sheet image")
    ed.check(preview.structured["created"], "the preview says it would create it")
    ed.check("output: true" in preview.structured.get("next", ""), f"the preview says how to write: {preview.structured.get('next')}")

    written = await ed.call("edit_tileset", {**args, "output": True})
    ed.check(target.exists(), "output writes the file")
    stored = json.loads(target.read_text())
    tile = next(t for t in stored["tiles"] if t["id"] == 5)
    ed.check_eq(tile["name"], "lava", "the edited tile's name")
    ed.check_eq(tile["properties"], {"damage": 3}, "the declared property")
    ed.check(written.get("undo"), f"the undo step: {written.get('undo')}")
    await ed.call("undo", scope="project")
    ed.check(not target.exists(), "one undo removes the new file")
    await ed.call("redo", scope="project")
    ed.check(target.exists(), "redo restores it")


async def test_edits_by_name_and_range(ed: Editor):
    edited = await ed.call("edit_tileset", path=PATH, output=True, tiles=[{"tile": "lava", "name": "magma"}, {"range": [0, 2], "probability": 0.25}])
    ed.check(edited.get("written"), "written")
    stored = json.loads((ed.project / PATH).read_text())
    names = {t["id"]: t.get("name") for t in stored["tiles"]}
    ed.check_eq(names[5], "magma", "the tile selected by name")
    probabilities = [t["probability"] for t in stored["tiles"] if t["id"] <= 2]
    ed.check_eq(probabilities, [0.25, 0.25, 0.25], "the tiles selected by range")


async def test_template_coverage_pass_warn(ed: Editor):
    ok = await ed.call("edit_tileset", path=PATH, output=True,
                       terrain_sets=[{"name": "Ground", "mode": "sides", "terrains": [{"name": "Grass", "color": "#5fa04e"}], "template": "sides_16", "origin": 0}])
    ed.check_eq(ok["report"]["verdict"], "PASS", f"sides_16 covers every mask: {ok['report']}")
    stored = json.loads((ed.project / PATH).read_text())
    template_tile = next(t for t in stored["tiles"] if t["id"] == 5)
    ed.check("terrain" in template_tile, f"the template assigned peering: {template_tile}")
    check = await ed.call("edit_tileset", path=PATH, check=True)
    ed.check_eq(check["verdict"], "PASS", f"check: {check}")
    ed.check("terrain_sets" in check and "tiles" in check, "only the report comes back")

    # a blob set with no tiles of its own has every mask missing
    warn = await ed.call("edit_tileset", path=PATH, output=True, terrain_sets=[{"name": "Empty", "mode": "blob", "terrains": ["Water"]}])
    ed.check_eq(warn["report"]["verdict"], "WARN", f"masks without a tile: {warn['report']}")
    empty = next(s for s in warn["report"]["terrain_sets"] if s["name"] == "Empty")
    ed.check(empty["missing_count"] > 0 and empty["required"] > 0, f"the counts: {empty}")


async def test_check_fail_and_typed_errors(ed: Editor):
    # a file written by hand with a property the schema does not declare: validate is the FAIL
    broken = "assets/art/broken.tileset"
    path = ed.project / broken
    path.write_text(json.dumps({
        "version": 1, "name": "broken", "tile_size": [16, 16],
        "sources": [{"id": 0, "kind": "texture", "texture": TEXTURE, "tile_size": [16, 16], "skip_empty": False}],
        "tiles": [{"id": 0, "source": 0, "at": [0, 0], "properties": {"undeclared": 1}}],
        "next_id": 1,
    }))
    report = await ed.call("edit_tileset", path=broken, check=True)
    ed.check_eq(report["verdict"], "FAIL", f"the validity problem: {report}")
    ed.check(report["problems"] and report["problems"][0]["kind"] == "unknown_property", f"typed: {report['problems']}")

    await fails(ed, {"path": PATH, "tiles": [{"id": 1, "properties": {"nope": 1}}]}, "unknown_property")
    kind = await fails(ed, {"path": PATH, "terrain_sets": [{"name": "Ground"}], "tiles": [{"id": 1, "terrain": {"set": "Ground", "peering": {"q": "any"}}}]}, "invalid_peering")
    ed.check("n" in kind["fix"], f"the fix lists the directions: {kind['fix']}")
    await fails(ed, {"path": PATH, "tiles": [{"tile": "ghost", "name": "x"}]}, "unknown_tile")
    await fails(ed, {"path": "assets/art/new.tileset", "name": "new"}, "tileset_missing")


async def test_sources_are_written_with_their_guid(ed: Editor):
    """R12: a written tileset names each image by path and GUID, so a moved image still resolves"""
    sheet(ed, "assets/art/guid.png", 16, 2, 1)
    await ed.call("edit_tileset", path="assets/art/guid.tileset", output=True, sources=[{"texture": "assets/art/guid.png", "tile_size": [16, 16]}])
    stored = json.loads((ed.project / "assets/art/guid.tileset").read_text())
    info = await ed.call("get_asset_info", path="assets/art/guid.png")
    ed.check(info.get("guid"), f"the image has a GUID: {info}")
    source = stored["sources"][0]["texture"]
    ed.check(isinstance(source, dict) and source.get("path", "").endswith("art/guid.png"), f"the source's path: {source}")
    ed.check_eq(source.get("guid"), info["guid"], "and its GUID")


async def test_contact_sheet_columns_and_scale_are_checked(ed: Editor):
    """columns and scale are integers in their range, refused before anything is written"""
    before = (ed.project / PATH).read_bytes()
    for field, value in (("columns", 0), ("columns", 300), ("scale", 9), ("scale", 2.5), ("scale", "2")):
        error = await fails(ed, {"path": PATH, "output": True, "name": "renamed", field: value}, "invalid_value")
        ed.check_eq(error.get("field"), field, f"{field}={value!r}: {error}")
        ed.check("integer from" in error["fix"], f"the fix gives the range: {error['fix']}")
    ed.check_eq((ed.project / PATH).read_bytes(), before, "nothing was written")
    ok = await ed.call_raw("edit_tileset", {"path": PATH, "columns": 2, "scale": 1}, timeout=60)
    ed.check(not ok.is_error and ok.images, f"in range: {ok.text[:300]}")
