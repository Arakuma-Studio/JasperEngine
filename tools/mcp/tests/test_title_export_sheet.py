"""S87 AC22 end to end (docs/specs/S87-title-localisation-interchange.md R42): export_title's sheet,
atlas and sequence formats over S85's rendered frames (`registerTitleAnimExportFrames`): the frame
count and times, `@WxH` cell naming, the per-language sets, and the title really moving between
frames (the frames come from S85's sampling, not one still repeated).
"""

import json

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Sheets"}
TIMEOUT = 900

DOC = "assets/titles/sheet.titleart"
# the animation moves the title across the canvas: a sheet of a repeated still fails the "moves" checks
OPS = [
    {"op": "add_language", "code": "de"},
    {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"},
    {"op": "add_animation", "name": "Move", "duration": 0.5},
    {"op": "set_track", "animation": "Move", "layer": "Title", "field": "transform.position", "keys": [[0, [-150, 0]], [0.5, [150, 0]]]},
]


async def setup(ed: Editor) -> None:
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "WIN", "canvas": [512, 256]}, bake="never", ops=OPS)


def centroid_x(rgba: bytes, width: int, height: int) -> float:
    total = 0.0
    weight = 0.0
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            a = rgba[(y * width + x) * 4 + 3]
            if a:
                total += x * a
                weight += a
    return total / max(1.0, weight)


async def test_sheet_atlas_sequence_of_a_moving_title(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=DOC, format="sheet", options={"fps": 10, "columns": 3, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check_eq(r.get("frame_count"), 6, f"a once animation of 0.5 s at 10 fps has 6 frames (0 ... 0.5): {r}")
    pngs = [f for f in r["files"] if f.endswith(".png")]
    jsons = [f for f in r["files"] if f.endswith(".sheet.json")]
    ed.check(len(pngs) == 1 and "@" in pngs[0] and "x" in pngs[0].rsplit("@", 1)[1], f"one sheet image named @WxH: {r['files']}")
    ed.check_eq(len(jsons), 1, f"one JSON: {r['files']}")
    sheet = json.loads((ed.project / jsons[0]).read_text())
    frames = sheet["frames"]
    ed.check_eq(len(frames), 6, "a JSON frame per time")
    data = (ed.project / pngs[0]).read_bytes()
    width, height, rgba = pngdiff.decode(data)
    w = frames[0]["sourceSize"]["w"]
    h = frames[0]["sourceSize"]["h"]
    ed.check_eq((width, height), (w * 3, h * 2), "a uniform grid of columns 3, rows 2")
    cells = []
    for i, frame in enumerate(frames):
        x = frame["frame"]["x"]
        y = frame["frame"]["y"]
        cell = bytearray()
        for row in range(h):
            start = ((y + row) * width + x) * 4
            cell += rgba[start:start + w * 4]
        cells.append(bytes(cell))
        ed.check(frame["duration"] > 0, f"frame {i} has a duration: {frame}")
    ed.check(cells[0] != cells[5], "the first and last frame differ")
    ed.check(sum(1 for a, b in zip(cells[0], cells[5]) if a != b) > w * h * 0.02, "the title moved")
    first, last = centroid_x(cells[0], w, h), centroid_x(cells[5], w, h)
    ed.check(last > first + w * 0.2, f"the ink travels from left to right: {first:.0f} -> {last:.0f} (cell {w})")
    middle = centroid_x(cells[3], w, h)
    ed.check(first < middle < last, f"frame 3 (0.3 s) sits between: {first:.0f} {middle:.0f} {last:.0f}")

    # the atlas: trimmed frames packed, sourceSize and spriteSourceSize, a page and its JSON
    r = await ed.call("export_title", path=DOC, format="atlas", options={"fps": 10, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    pages = [f for f in r["files"] if "_page" in f and f.endswith(".png")]
    jsons = [f for f in r["files"] if f.endswith(".atlas.json")]
    ed.check(pages and len(jsons) == 1, f"pages and JSON: {r['files']}")
    atlas = json.loads((ed.project / jsons[0]).read_text())
    ed.check_eq(len(atlas["frames"]), 6, "six atlas frames")
    first_source = next(iter(atlas["frames"].values()))["sourceSize"]
    ed.check_eq((first_source["w"], first_source["h"]), (w, h), "every frame keeps the shared source frame")
    ed.check(any(f["trimmed"] for f in atlas["frames"].values()), "trimmed to their ink")

    # the sequence: one PNG per frame, on the shared frame
    r = await ed.call("export_title", path=DOC, format="sequence", options={"fps": 10, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    files = sorted(f for f in r["files"] if f.endswith(".png"))
    ed.check_eq(len(files), 6, f"six numbered files: {r['files']}")
    sizes = {pngdiff.decode((ed.project / f).read_bytes())[:2] for f in files}
    ed.check_eq(sizes, {(w, h)}, "every image on the shared frame")

    # per language: each set rendered with its own text and frame
    r = await ed.call("export_title", path=DOC, format="sheet", languages="all", options={"fps": 10, "columns": 3, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check_eq(sorted(r["frames"]), ["de", "en"], "a frame per language")
    ed.check(r["frames"]["de"] != r["frames"]["en"], f"each language its own frame: {r['frames']}")
    named = [f for f in r["files"] if f.endswith(".png")]
    ed.check_eq(len(named), 2, f"a sheet per language: {r['files']}")
    ed.check(any(".de" in f for f in named), f"the language in the name: {named}")


async def test_a_title_without_an_animation_says_so(ed: Editor):
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = "assets/titles/still.titleart"
    await ed.call("edit_title", path=path, create={"text": "STILL"}, bake="never")
    r = await ed.call_raw("export_title", {"path": path, "format": "sheet", "options": {"overwrite": True}})
    ed.check(r.is_error, f"a still title has no frames: {r.text}")
    ed.check("no_animation" in r.text or "no animation" in r.text, r.text)
