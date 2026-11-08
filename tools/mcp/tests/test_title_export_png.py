"""S87 Title exports (docs/specs/S87-title-localisation-interchange.md R35-R40, R43, AC19, AC22 names,
AC23; jasper://docs/title-art "Import and export"): export_title's png format (trimmed frames, padding,
round_to, the sRGB chunks and no iCCP, per-language files, parts with their manifest, a ZIP bundle),
the strings formats (csv, xliff), bad options refused before anything renders, `out` inside the
project, and output_exists.
"""

import json
import struct
import zipfile

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Exports"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = "assets/titles/out.titleart"
    await ed.call("edit_title", path=path, create={"text": "WIN", "canvas": [1024, 512]}, bake="never")
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_rim", "layer": "Title", "values": {"width": 6}},
        {"op": "add_language", "code": "de"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"},
    ])
    S["path"] = path


def chunk_types(data: bytes) -> list:
    out, pos = [], 8
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        out.append(data[pos + 4:pos + 8].decode("latin-1"))
        pos += 12 + length
    return out


async def test_png_trimmed_and_tagged(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="png", options={"padding": 4, "round_to": 8, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    files = r["files"]
    ed.check_eq(files, ["Recordings/Title Art/out/out.png"], "the default folder and name")
    data = (ed.project / files[0]).read_bytes()
    ed.check_eq(chunk_types(data)[:5], ["IHDR", "sRGB", "gAMA", "cHRM", "pHYs"], "sRGB-tagged the PNG way")
    ed.check("iCCP" not in chunk_types(data), "no ICC profile beside sRGB")
    width, height, rgba = pngdiff.decode(data)
    ed.check_eq((width % 8, height % 8), (0, 0), "sides rounded to round_to")
    ed.check(width < 1024, "trimmed to the content")
    # no fully opaque pixel touches the edge: the padding holds
    edge = [rgba[(y * width + x) * 4 + 3] for y in (0, height - 1) for x in range(width)]
    ed.check(max(edge) < 255, "the padding keeps the ink off the edge")


async def test_png_languages_and_zip(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="png", languages="all", options={"zip": True, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check_eq(r["files"], ["Recordings/Title Art/out/out_png.zip"], "only the ZIP is listed")
    with zipfile.ZipFile(ed.project / r["files"][0]) as z:
        names = sorted(z.namelist())
    ed.check_eq(names, ["out.de.png", "out.en.png"], "the R40 names inside")
    ed.check(set(r["frames"]) == {"en", "de"}, "a frame per language")
    ed.check(r["frames"]["de"] != r["frames"]["en"], "each trimmed to its own text")


async def test_png_parts(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="png", options={"parts": True, "split_titles": True, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    manifest = json.loads((ed.project / "Recordings/Title Art/out/out.parts.json").read_text())
    ed.check_eq(manifest["format"], "jasperengine.titleart.parts", "the manifest's format")
    parts = [p["part"] for p in manifest["parts"]]
    ed.check_eq(parts, ["rim_1", "face", "fx"], "rim N, the face, then fx")
    ed.check_eq([p["hidden"] for p in manifest["parts"]], [True, True, False], "only fx shows")


async def test_strings_formats(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="csv", options={"overwrite": True})
    csv = (ed.project / r["files"][0]).read_bytes()
    ed.check(csv.startswith(b"\xef\xbb\xbf"), "UTF-8 with BOM")
    ed.check_in(b"out.title,", csv, "the key")
    ed.check_in(b"GEWINN", csv, "the German text")
    r = await ed.call("export_title", path=S["path"], format="xliff", options={"overwrite": True})
    ed.check_eq(r["files"], ["Recordings/Title Art/out/out.de.xlf"], "one XLIFF per target language")
    ed.check_in("target-language=\"de\"", (ed.project / r["files"][0]).read_text(), "its target language")


async def test_refusals(ed: Editor):
    await setup(ed)
    msg = await ed.check_error("export_title", path=S["path"], format="png", options={"scale": 99})
    ed.check_in("bad_option", msg, "out of range")
    msg = await ed.check_error("export_title", path=S["path"], format="png", options={"columns": 2})
    ed.check_in("bad_option", msg, "a key of another format")
    msg = await ed.check_error("export_title", path=S["path"], format="png", out="/tmp/elsewhere")
    ed.check_in("out_outside_project", msg, "MCP writes only inside the project")
    await ed.call("export_title", path=S["path"], format="png", options={"overwrite": True})
    msg = await ed.check_error("export_title", path=S["path"], format="png")
    ed.check_in("output_exists", msg, "an existing file needs overwrite")
    msg = await ed.check_error("export_title", path=S["path"], format="png", languages=["ko"])
    ed.check_in("no language 'ko'", msg, "a language the title lacks")
