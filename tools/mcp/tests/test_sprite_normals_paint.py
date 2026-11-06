"""The paint layer of generate_sprite_normals (editor/spritenormals/SpriteNormalsPaint.cpp, sprite normals Phase 5 spec
4 and 7.4), on a new project with inputs made here (a shaded ball, a two-frame sheet, sources that are resized or get a
hole later, a Tilemap with two tileset textures):

- a preview with paint: structuredContent paint.ops[i].selected / changed, painted_pixels, painted_pct (also in the
  metrics), the contact sheet with the magenta outline in the normal column and its caption; nothing written;
- a write with paint: the PNG holds IHDR, the recipe (with "paint": {"painted": N}) and the paint chunk in that order;
  the painted pixels are the paint; get_asset_info's sprite_normals.paint and view_asset's "Painted: N px";
- the regenerate arguments (paint_mode keep) keep the paint over other settings; paint_mode replace starts over,
  clear drops the chunk; a preview with keep composites the existing file's paint;
- every error and warning kind: invalid op enum, tilt 90, 257 ops, paint_mode, clear with ops (invalid_paint),
  paint_size_mismatch (nothing written; clear writes), paint_empty_selection, paint_layout_changed,
  paint_hidden_pixels, paint on a Tilemap with two tileset textures (invalid_paint);
- one project undo of a write restores the previous file, paint included;
- `check --normals` reports normals-paint-unreadable for a hand-corrupted chunk and normals-paint-size for a paint
  layer of another size;
"""

from __future__ import annotations

import base64
import json
import math
import struct
import subprocess
import zlib
from pathlib import Path

import harness
from harness import Editor, TestFailure
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "SpriteNormalsPaint"}
CI = True
TIMEOUT = 900

SPR = "assets/sprites"
BALL = f"{SPR}/ball.png"
SHEET = f"{SPR}/sheet.png"
FLAT = [{"op": "flatten", "where": {"rect": [8, 8, 6, 6]}},
        {"op": "angle", "where": {"pixels": [[16, 16]]}, "direction": {"azimuth_deg": 180, "tilt_deg": 45}}]
S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


# ---------------------------------------------------------------------------- images and chunks

def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_rgba(width: int, height: int, rows: list[bytes]) -> bytes:
    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def ball_rows(width: int = 32, height: int = 32, cell: int = 32, hole: tuple[int, int] | None = None) -> list[bytes]:
    """Shaded balls (light from the top-left) on transparency, one per `cell` columns"""
    r = min(cell, height) * 0.42
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            dx, dy = (x % cell) + 0.5 - cell / 2, y + 0.5 - height / 2
            if math.hypot(dx, dy) > r or (x, y) == hole:
                row += bytes(4)
                continue
            shade = max(0.0, min(1.0, 0.75 - 0.5 * (dx + dy) / (2 * r)))
            row += bytes((int(60 + 180 * shade), int(40 + 120 * shade), int(30 + 60 * shade), 255))
        rows.append(bytes(row))
    return rows


def chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    out, pos = [], 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        out.append((data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]))
        pos += 12 + length
    return out


def text_chunk(path: Path, keyword: str) -> dict | None:
    key = keyword.encode() + b"\x00"
    for kind, data in chunks(path.read_bytes()):
        if kind == b"iTXt" and data.startswith(key):
            return json.loads(data[len(key) + 4:].decode("utf-8"))
    return None


def chunk_order(path: Path) -> list[str]:
    out = []
    for kind, data in chunks(path.read_bytes()):
        out.append(kind.decode() + (":" + data.split(b"\x00")[0].decode() if kind == b"iTXt" else ""))
    return out


def pixel(path: Path, x: int, y: int) -> tuple[int, int, int, int]:
    w, _, channels, rows = decode_png(path.read_bytes())
    return tuple(rows[y][x * channels:x * channels + 4])


def files_under(project: Path) -> set[str]:
    # (the assets' S04 .meta sidecars aside: the asset database writes them for every file)
    return {p.relative_to(project).as_posix() for p in (project / "assets").rglob("*") if p.is_file() and p.suffix != ".meta"}


async def gen(ed: Editor, **args):
    r = await ed.call_raw("generate_sprite_normals", args, timeout=600)
    if r.is_error:
        fail(f"generate_sprite_normals {args} failed: {r.text}")
    return r


async def gen_error(ed: Editor, **args) -> dict:
    r = await ed.call_raw("generate_sprite_normals", args, timeout=600)
    check(r.is_error, f"{args} should fail: {r.text[:400]}")
    errors = (r.structured or {}).get("errors") or []
    check(errors and all({"kind", "message", "fix"} <= set(e) for e in errors), f"{args}: errors [{{kind, message, fix}}]: {r.structured}")
    return {"kinds": [e["kind"] for e in errors], "fields": [e.get("field") for e in errors], "text": r.text, "data": r.structured}


def kinds(v: dict) -> list[str]:
    return [w["kind"] for w in v.get("warnings", [])]


# ---------------------------------------------------------------------------- 1. preview

async def test_preview_with_paint(ed: Editor):
    ed.require_tools("generate_sprite_normals")
    spr = ed.project / SPR
    spr.mkdir(parents=True, exist_ok=True)
    (spr / "ball.png").write_bytes(png_rgba(32, 32, ball_rows()))
    (spr / "sheet.png").write_bytes(png_rgba(64, 32, ball_rows(64, 32, 32)))
    before = files_under(ed.project)
    r = await gen(ed, source=BALL, paint=FLAT)
    v = r.structured
    check(files_under(ed.project) == before, "a preview writes nothing")
    p = v.get("paint")
    check(isinstance(p, dict), f"paint object: {v.keys()}")
    check(p["mode"] == "keep" and p["kept_from"] is None, f"mode / kept_from: {p}")
    check(p["ops"] == [{"op": "flatten", "selected": 36, "changed": 36}, {"op": "angle", "selected": 1, "changed": 1}], f"ops: {p['ops']}")
    check(p["painted_pixels"] == 37 and p["hidden_pixels"] == 0 and p["painted_pct"] > 0, f"counts: {p}")
    check(v["metrics"]["painted_pct"] == p["painted_pct"], f"metrics.painted_pct: {v['metrics']}")
    check(any(line.startswith("Painted: 37 px") for line in v["assessment"]), f"assessment line: {v['assessment']}")
    check("magenta outline: painted pixels" in r.text and "Paint (keep)" in r.text, f"caption and summary: {r.text}")
    # the sheet: magenta only in the normal column (tile 2 of 6)
    w, h, channels, rows = decode_png(r.images[0][0])
    columns = set()
    for y in range(h):
        for x in range(w):
            if tuple(rows[y][x * channels:x * channels + 3]) == (255, 0, 255):
                columns.add(x * 6 // w)
    check(columns == {1}, f"the magenta outline is in the normal column only: {columns}")
    # without paint: no outline, no caption
    plain = await gen(ed, source=BALL)
    check("magenta" not in plain.text and plain.structured["paint"]["painted_pixels"] == 0, f"no paint: {plain.text}")


# ---------------------------------------------------------------------------- 2. write

async def test_write_with_paint(ed: Editor):
    r = await gen(ed, source=BALL, output=True, paint=FLAT)
    out = ed.project / SPR / "ball_normal.png"
    check(r.structured["written"] and out.is_file(), "written")
    order = chunk_order(out)
    check(order[:3] == ["IHDR", "iTXt:jasper.spritenormals", "iTXt:jasper.spritenormals.paint"], f"chunk order: {order}")
    rec = text_chunk(out, "jasper.spritenormals")
    check(rec.get("paint") == {"painted": 37}, f"recipe paint: {rec.get('paint')}")
    paint = text_chunk(out, "jasper.spritenormals.paint")
    check(list(paint)[:5] == ["format", "version", "size", "painted", "encoding"] and paint["size"] == [32, 32] and paint["painted"] == 37,
          f"paint chunk: {list(paint)}")
    check(base64.b64decode(paint["data"]).startswith(b"\x89PNG"), "an embedded PNG")
    check(pixel(out, 10, 10) == (128, 128, 255, 255), f"flattened: {pixel(out, 10, 10)}")
    check(pixel(out, 16, 16) == (37, 128, 218, 255), f"angle 180/45: {pixel(out, 16, 16)}")
    info = await ed.call("get_asset_info", path=f"{SPR}/ball_normal.png")
    check(info["sprite_normals"].get("paint") == {"painted": 37, "size": [32, 32]}, f"get_asset_info paint: {info['sprite_normals'].get('paint')}")
    check(info["sprite_normals"]["regenerate"].get("paint_mode") == "keep", f"regenerate keeps paint: {info['sprite_normals']['regenerate']}")
    view = await ed.call_raw("view_asset", {"path": f"{SPR}/ball_normal.png"})
    check(not view.is_error and "Painted: 37 px" in view.text, f"view_asset: {view.text}")
    S["regenerate"] = info["sprite_normals"]["regenerate"]
    S["painted_bytes"] = out.read_bytes()


# ---------------------------------------------------------------------------- 3. regenerate, replace, clear

async def test_regenerate_keeps_paint(ed: Editor):
    out = ed.project / SPR / "ball_normal.png"
    regen = dict(S["regenerate"])
    regen["params"] = {**regen.get("params", {}), "strength": 4}
    r = await gen(ed, **regen)
    p = r.structured["paint"]
    check(p["kept_from"] == f"{SPR}/ball_normal.png" and p["painted_pixels"] == 37 and p["ops"] == [], f"kept: {p}")
    check(pixel(out, 10, 10) == (128, 128, 255, 255) and pixel(out, 16, 16) == (37, 128, 218, 255), "the paint over the new strength")
    check(text_chunk(out, "jasper.spritenormals")["settings"]["strength"] == 4, "the new settings")
    check(text_chunk(out, "jasper.spritenormals.paint")["painted"] == 37, "the chunk again")
    # a preview with keep composites the file's paint
    prev = await gen(ed, source=BALL)
    check(prev.structured["paint"]["kept_from"] == f"{SPR}/ball_normal.png" and prev.structured["paint"]["painted_pixels"] == 37,
          f"preview keeps: {prev.structured['paint']}")
    # keep adds, replace starts over
    await gen(ed, source=BALL, output=True, overwrite=True, paint=[{"op": "flatten", "where": {"pixels": [[20, 20]]}}])
    check(text_chunk(out, "jasper.spritenormals.paint")["painted"] == 38, "keep adds")
    await gen(ed, source=BALL, output=True, overwrite=True, paint_mode="replace", paint=[{"op": "flatten", "where": {"pixels": [[20, 20]]}}])
    check(text_chunk(out, "jasper.spritenormals.paint")["painted"] == 1, "replace starts over")
    r = await gen(ed, source=BALL, output=True, overwrite=True, paint_mode="clear")
    check(r.structured["paint"]["mode"] == "clear" and text_chunk(out, "jasper.spritenormals.paint") is None, "clear drops the chunk")
    check("paint" not in text_chunk(out, "jasper.spritenormals"), "and the recipe's paint")


# ---------------------------------------------------------------------------- 4. errors and warnings

async def test_errors_and_warnings(ed: Editor):
    before = files_under(ed.project)
    cases = [
        ({"paint": [{"op": "paint", "where": {"frame": 0}}]}, "paint[0].op", "angle, flatten, smooth or erase"),
        ({"paint": [{"op": "angle", "where": {"frame": 0}, "direction": {"azimuth_deg": 0, "tilt_deg": 90}}]}, "paint[0].direction.tilt_deg", "85"),
        ({"paint": [{"op": "flatten", "where": {"frame": 0}}] * 257}, "paint", "257"),
        ({"paint_mode": "sometimes"}, "paint_mode", "keep, replace or clear"),
        ({"paint_mode": "clear", "paint": [{"op": "erase", "where": {"frame": 0}}]}, "paint", "clear"),
        ({"paint": [{"op": "flatten", "where": {"frame": 2}}]}, "paint[0].where.frame", "frame"),
        ({"paint": {"op": "flatten"}}, "paint", "array"),
    ]
    for args, field, needle in cases:
        got = await gen_error(ed, source=BALL, **args)
        check("invalid_paint" in got["kinds"] and field in got["fields"] and needle in got["text"], f"{args}: {got['kinds']} {got['fields']} {got['text'][:500]}")
    check(files_under(ed.project) == before, "errors write nothing")

    # an empty selection is a warning
    r = await gen(ed, source=BALL, paint=[{"op": "flatten", "where": {"rect": [0, 0, 2, 2]}}])
    check("paint_empty_selection" in kinds(r.structured) and r.structured["paint"]["ops"][0]["selected"] == 0, f"{r.structured['warnings']}")

    # a source resized after painting: paint_size_mismatch, nothing written; clear writes
    spr = ed.project / SPR
    (spr / "grow.png").write_bytes(png_rgba(32, 32, ball_rows()))
    await gen(ed, source=f"{SPR}/grow.png", output=True, paint=[{"op": "flatten", "where": {"rect": [10, 10, 4, 4]}}])
    grown = spr / "grow_normal.png"
    kept = grown.read_bytes()
    (spr / "grow.png").write_bytes(png_rgba(48, 32, ball_rows(48, 32, 48)))
    got = await gen_error(ed, source=f"{SPR}/grow.png", output=True, overwrite=True)
    check("paint_size_mismatch" in got["kinds"] and "16 painted pixels for a 32x32 image" in got["text"] and "48x32" in got["text"]
          and 'paint_mode: "clear"' in got["text"], f"size mismatch: {got['text']}")
    check(grown.read_bytes() == kept, "nothing written")
    got = await gen_error(ed, source=f"{SPR}/grow.png")
    check("paint_size_mismatch" in got["kinds"], "a preview with keep says it too")
    await gen(ed, source=f"{SPR}/grow.png", output=True, overwrite=True, paint_mode="clear")
    check(text_chunk(grown, "jasper.spritenormals.paint") is None, "written without paint")

    # frames changed with the same size: paint_layout_changed (the overlay kept)
    await gen(ed, source=SHEET, frames={"tile_size": 32}, output=True, paint=[{"op": "flatten", "where": {"frame": 1}}])
    r = await gen(ed, source=SHEET, frames="none")
    check("paint_layout_changed" in kinds(r.structured) and r.structured["paint"]["painted_pixels"] > 0, f"layout: {r.structured['warnings']}")

    # a painted pixel that turned transparent: paint_hidden_pixels
    (spr / "holey.png").write_bytes(png_rgba(32, 32, ball_rows()))
    await gen(ed, source=f"{SPR}/holey.png", output=True, paint=[{"op": "flatten", "where": {"pixels": [[16, 16], [17, 16]]}}])
    (spr / "holey.png").write_bytes(png_rgba(32, 32, ball_rows(hole=(16, 16))))
    r = await gen(ed, source=f"{SPR}/holey.png")
    check("paint_hidden_pixels" in kinds(r.structured) and r.structured["paint"]["hidden_pixels"] == 1, f"hidden: {r.structured['paint']}")

    # paint on a Tilemap with two tileset textures: one call per output
    for name, shade in (("ground_a.png", 90), ("ground_b.png", 180)):
        rows = [b"".join(bytes((shade, 140, (x * y) % 90, 255)) for x in range(32)) for y in range(16)]
        (spr / name).write_bytes(png_rgba(32, 16, rows))
    await ed.call("edit_tilemap", create={"name": "Ground"}, texture=f"{SPR}/ground_a.png", tileset={"tile_size": [16, 16]},
                  rects=[{"name": "b0", "rect": [0, 0, 16, 16], "submesh": 1}],
                  ops=[{"op": "fill", "tile": 0, "from": [0, 0], "to": [3, 0]}, {"op": "set", "cells": [[0, 1, "b0"], [1, 1, "b0"]]}])
    await ed.call("wait", frames=3)
    await ed.call("set_properties", entity="Ground", values={"mesh": {"numSubmeshes": 2}})
    await ed.call("set_properties", entity="Ground", values={"mesh": {"submeshes[1].material.baseColorTexture": f"{SPR}/ground_b.png"}})
    await ed.call("wait", frames=3)
    got = await gen_error(ed, source="Ground", frames={"tile_size": 16}, paint=[{"op": "flatten", "where": {"frame": 0}}])
    check("invalid_paint" in got["kinds"] and "tileset" in got["text"], f"Tilemap: {got['text']}")


# ---------------------------------------------------------------------------- 5. undo

async def test_undo_restores_the_previous_paint(ed: Editor):
    out = ed.project / SPR / "ball_normal.png"
    await gen(ed, source=BALL, output=True, overwrite=True, paint_mode="replace", paint=FLAT)
    first = out.read_bytes()
    await gen(ed, source=BALL, output=True, overwrite=True, paint=[{"op": "erase", "where": {"rect": [8, 8, 3, 6]}}])
    check(text_chunk(out, "jasper.spritenormals.paint")["painted"] == 37 - 18, "erased 18")
    await ed.call("undo", scope="project")
    check(out.read_bytes() == first and text_chunk(out, "jasper.spritenormals.paint")["painted"] == 37, "undo puts the previous paint back")
    await ed.call("redo", scope="project")
    check(text_chunk(out, "jasper.spritenormals.paint")["painted"] == 19, "redo")


# ---------------------------------------------------------------------------- 7. check --normals

def run_check(project: Path) -> list[dict]:
    cmd = harness.editor_command() + ["check", "--project", str(project), "--normals", "--format", "json"]
    r = subprocess.run(cmd, cwd=str(project), capture_output=True, text=True, timeout=300)
    try:
        data = json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"check printed no JSON (exit {r.returncode}): {r.stdout[-1500:]} {r.stderr[-1500:]}")
        return []
    return [f for f in data["findings"] if f.get("code", "").startswith("normals-paint")]


def replace_paint(path: Path, text: str) -> None:
    """The paint chunk's text replaced (its CRC right: only the content is damaged)"""
    key = b"jasper.spritenormals.paint\x00"
    data = path.read_bytes()
    out, pos = data[:8], 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        out += chunk(kind, key + b"\x00\x00\x00\x00" + text.encode()) if kind == b"iTXt" and body.startswith(key) else data[pos:pos + 12 + length]
        pos += 12 + length
    path.write_bytes(out)


async def test_check_reports_unreadable_and_mismatched_paint(ed: Editor):
    spr = ed.project / SPR
    for name in ("broken.png", "small.png"):
        (spr / name).write_bytes(png_rgba(32, 32, ball_rows()))
        await gen(ed, source=f"{SPR}/{name}", output=True, paint=FLAT)
    check(run_check(ed.project) == [], "readable paint: no finding")
    broken = spr / "broken_normal.png"
    paint = text_chunk(broken, "jasper.spritenormals.paint")
    paint["data"] = "!!" + paint["data"][2:]
    replace_paint(broken, json.dumps(paint, separators=(",", ":")))
    # a 16 x 16 layer in a 32 x 32 map
    tiny = png_rgba(16, 16, [bytes((128, 128, 255, 255)) * 16] * 16)
    small = {"format": "jasperengine.spritenormals.paint", "version": 1, "size": [16, 16], "painted": 256, "encoding": "png-base64",
             "data": base64.b64encode(tiny).decode()}
    replace_paint(spr / "small_normal.png", json.dumps(small, separators=(",", ":")))
    found = {(f["path"], f["code"]): f for f in run_check(ed.project)}
    unreadable = found.get((f"{SPR}/broken_normal.png", "normals-paint-unreadable"))
    check(unreadable and "base64" in unreadable["message"] and "paint_mode: clear" in unreadable["suggestion"], f"unreadable: {found}")
    size = found.get((f"{SPR}/small_normal.png", "normals-paint-size"))
    check(size and "16x16" in size["message"] and "32x32" in size["message"], f"size: {found}")
    # the cache is version 3 (2: the paint state, 3: island_side); a version 1 cache (entries without the paint state) is read again
    cache_file = ed.project / ".jasperengine" / "normals-check-cache.json"
    cache = json.loads(cache_file.read_text())
    check(cache["version"] == 3, f"cache version: {cache['version']}")
    for entry in cache["pngs"].values():
        entry.pop("paint_error", None)
        entry.pop("paint", None)
    cache["version"] = 1
    cache_file.write_text(json.dumps(cache))
    again = {(f["path"], f["code"]) for f in run_check(ed.project)}
    check((f"{SPR}/broken_normal.png", "normals-paint-unreadable") in again, f"a version 1 cache is not trusted: {again}")
    check(json.loads(cache_file.read_text())["version"] == 3, "rewritten as version 3")
    # the generator says it too, and the next write drops the unreadable chunk
    r = await gen(ed, source=f"{SPR}/broken.png", output=True, overwrite=True)
    check("paint_unreadable" in kinds(r.structured), f"paint_unreadable warning: {r.structured['warnings']}")
    check(text_chunk(broken, "jasper.spritenormals.paint") is None, "dropped")


# ---------------------------------------------------------------------------- the definition
