"""generate_sprite_normals for S78 WP-B (docs/specs/S78-sprite-normals-improvements.md R8-R13, R21, R22), on a new
project with inputs made here (a 32 px brick tile between 1 px dark gaps, a flat tile, a crate):

- preset stones: gaps 0.3, dome, wrap; params.gaps overrides it, the written recipe records it; bad gaps values are
  refused naming params.gaps(.share);
- gaps_unmatched on a flat tile, with a fix naming gaps.share;
- params.light_steps bands the sheet's lit columns (another image), is echoed in sheet_legend, is no setting, and is
  refused outside 0..16; without it the sheet takes the selected scene's Light Steps;
- boxy on a crate with the default preset (its fix: props, or bevel with a radius), not with props;
- the write step (next) and the sheet legend (sheet_legend) are in structuredContent.
"""

from __future__ import annotations

import json
import struct
import zlib

from harness import Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "SpriteNormalsStones"}
TIMEOUT = 300

ART = "assets/ground"


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png(width: int, height: int, pixel) -> bytes:
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def bricks(x: int, y: int) -> tuple[int, int, int, int]:
    """four rows of 7 px bricks between 1 px dark gaps, each row shifted, seamless at 32 px"""
    widths = [[7, 9, 7, 5], [9, 5, 7, 7], [11, 5, 7, 5], [5, 9, 9, 5]]
    shift = [0, 4, 9, 2]
    row = y // 8
    if y % 8 == 7:
        return (38, 34, 44, 255)
    edge = shift[row]
    for w in widths[row]:
        edge += w
        if (x - shift[row]) % 32 == (edge - shift[row]) % 32:
            return (38, 34, 44, 255)
        edge += 1
    v = 182 if y % 8 == 0 else 150
    return (v, v - 6, v - 16, 255)


def write_inputs(ed: Editor) -> None:
    folder = ed.project / ART
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "bricks.png").write_bytes(png(32, 32, bricks))
    (folder / "flat.png").write_bytes(png(32, 32, lambda x, y: (120, 110, 100, 255)))
    (folder / "crate.png").write_bytes(png(32, 32, lambda x, y: (160, 110, 60, 255) if 6 <= x < 26 and 6 <= y < 26 else (0, 0, 0, 0)))


async def gen(ed: Editor, **args):
    r = await ed.call_raw("generate_sprite_normals", args, timeout=300)
    if r.is_error:
        fail(f"generate_sprite_normals {args} failed: {r.text}")
    return r


async def gen_error(ed: Editor, **args) -> list[dict]:
    r = await ed.call_raw("generate_sprite_normals", args, timeout=300)
    check(r.is_error, f"{args} should fail: {r.text[:400]}")
    errors = (r.structured or {}).get("errors") or []
    check(errors and all(e.get("fix") for e in errors), f"{args}: errors with fixes: {r.structured}")
    return errors


def kinds(v: dict) -> list[str]:
    return [w["kind"] for w in v["warnings"]]


def warning(v: dict, kind: str) -> dict:
    for w in v["warnings"]:
        if w["kind"] == kind:
            return w
    fail(f"no {kind} warning: {kinds(v)}")
    return {}


def recipe(data: bytes) -> dict:
    pos = 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if kind == b"iTXt" and body.startswith(b"jasper.spritenormals\x00"):
            return json.loads(body[len(b"jasper.spritenormals\x00") + 4:].decode("utf-8"))
        pos += 12 + length
    fail("no jasper.spritenormals chunk")
    return {}


async def test_stones_preset_gaps_and_the_structured_next(ed: Editor):
    ed.require_tools("generate_sprite_normals")
    write_inputs(ed)
    r = await gen(ed, source=f"{ART}/bricks.png", preset="stones")
    v = r.structured
    s = v["settings"]
    check((s["preset"], s["edges"], s["profile"], s["gaps"]) == ("stones", "wrap", "dome", {"share": 0.3}), f"stones: {s}")
    check("gaps_unmatched" not in kinds(v) and "no_alpha" not in kinds(v), f"bricks: {kinds(v)}")
    # the write step and the legend where the agent reads them
    check("output" in v.get("next", "") and f"{ART}/bricks_normal.png" in v["next"], f"next: {v.get('next')}")
    legend = v.get("sheet_legend") or {}
    check(len(legend.get("columns", [])) == 6 and legend["columns"][0] == "colour" and legend["columns"][2] == "lit from the top-left",
          f"legend columns: {legend}")
    check(legend.get("light_elevation_deg") == 34 and legend.get("light_steps") == 0, f"legend: {legend}")
    # an override, written: the recipe records it
    r = await gen(ed, source=f"{ART}/bricks.png", preset="stones", params={"gaps": {"share": 0.2}}, output=True)
    check(r.structured["settings"]["gaps"] == {"share": 0.2}, f"override: {r.structured['settings']}")
    check("next" not in r.structured, f"nothing left to do for an image source: {r.structured.get('next')}")
    written = recipe((ed.project / ART / "bricks_normal.png").read_bytes())
    check(written["settings"]["gaps"] == {"share": 0.2} and written["settings"]["preset"] == "stones", f"recipe: {written['settings']}")
    # gaps on another preset, and off on stones
    s = (await gen(ed, source=f"{ART}/bricks.png", preset="tile", params={"gaps": {"share": 0.3}})).structured["settings"]
    check(s["gaps"] == {"share": 0.3} and s["edges"] == "wrap", f"tile with gaps: {s}")
    s = (await gen(ed, source=f"{ART}/bricks.png", preset="stones", params={"gaps": {"share": 0}})).structured["settings"]
    check(s["gaps"] == {"share": 0}, f"stones without gaps: {s}")
    check("gaps" not in (await gen(ed, source=f"{ART}/bricks.png", preset="tile")).structured["settings"], "tile: no gaps key")


async def test_gaps_errors_name_the_field(ed: Editor):
    write_inputs(ed)
    for params, field in [({"gaps": {"share": 1.5}}, "params.gaps.share"), ({"gaps": {"share": -0.1}}, "params.gaps.share"),
                          ({"gaps": {"amount": 0.3}}, "params.gaps"), ({"gaps": 0.3}, "params")]:
        errors = await gen_error(ed, source=f"{ART}/bricks.png", params=params)
        check(errors[0]["kind"] == "invalid_settings" and errors[0].get("field") == field, f"{params}: {errors}")
    errors = await gen_error(ed, source=f"{ART}/bricks.png", params={"gaps": {"share": 1.5}})
    check("0.9" in errors[0]["message"], f"the range is named: {errors}")


async def test_gaps_unmatched_on_a_flat_tile(ed: Editor):
    write_inputs(ed)
    v = (await gen(ed, source=f"{ART}/flat.png", preset="stones")).structured
    w = warning(v, "gaps_unmatched")
    check("gaps.share" in w["fix"] and "1 of 1 cell" in w["message"], f"gaps_unmatched: {w}")


async def test_light_steps_band_the_sheet(ed: Editor):
    write_inputs(ed)
    smooth = await gen(ed, source=f"{ART}/bricks.png", preset="stones")
    stepped = await gen(ed, source=f"{ART}/bricks.png", preset="stones", params={"light_steps": 6})
    check(stepped.structured["sheet_legend"]["light_steps"] == 6, f"echoed: {stepped.structured['sheet_legend']}")
    check("light_steps" not in stepped.structured["settings"], f"not a setting of the map: {stepped.structured['settings']}")
    check(smooth.images and stepped.images and smooth.images[0][0] != stepped.images[0][0], "the lit columns change")
    check("Light Steps 6" in stepped.text, f"the caption says so: {stepped.text[-300:]}")
    for bad in (17, -1, 2.5, "6"):
        errors = await gen_error(ed, source=f"{ART}/bricks.png", params={"light_steps": bad})
        check(errors[0]["kind"] == "invalid_settings" and errors[0].get("field") == "params.light_steps", f"{bad}: {errors}")


async def test_light_steps_default_to_the_scenes(ed: Editor):
    # without params.light_steps the sheet lights as the selected scene's Light Steps; 0 overrides it
    write_inputs(ed)
    await ed.call("set_scene_settings", settings={"light_2d_steps": 4})
    try:
        scened = await gen(ed, source=f"{ART}/bricks.png", preset="stones")
        check(scened.structured["sheet_legend"]["light_steps"] == 4, f"the scene's: {scened.structured['sheet_legend']}")
        check("Light Steps 4" in scened.text, f"the caption says so: {scened.text[-300:]}")
        smooth = await gen(ed, source=f"{ART}/bricks.png", preset="stones", params={"light_steps": 0})
        check(smooth.structured["sheet_legend"]["light_steps"] == 0, f"0 overrides: {smooth.structured['sheet_legend']}")
    finally:
        await ed.call("set_scene_settings", settings={"light_2d_steps": 0})


async def test_boxy_crate(ed: Editor):
    write_inputs(ed)
    v = (await gen(ed, source=f"{ART}/crate.png")).structured
    w = warning(v, "boxy")
    check(w["fix"] == "Use preset props, or profile bevel with radius 1.", f"boxy fix: {w}")
    check("boxy" not in kinds((await gen(ed, source=f"{ART}/crate.png", preset="props")).structured), "props: no boxy")
    v = (await gen(ed, source=f"{ART}/crate.png", params={"profile": "bevel", "radius": 1})).structured
    check("boxy" not in kinds(v), f"bevel: {kinds(v)}")
