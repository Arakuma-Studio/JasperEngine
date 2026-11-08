"""S87 SVG export (docs/specs/S87-title-localisation-interchange.md R41, AC21; jasper://docs/title-art
"Import and export"): export_title format svg of a document with text, rims (round and miter), a
linear gradient and a shape mask, turned and moved, rendered by LunaSVG (diff_images reads .svg)
matches export_title png of the same document; what has no SVG form is listed by layer; strict
fails; one file per language.
"""

import re

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title SVG Export"}
TIMEOUT = 900

S: dict = {}

GRADIENT = {"stops": [{"pos": 0, "color": "#ffd040", "alpha": 1, "mid": 0.5}, {"pos": 1, "color": "#c03010", "alpha": 1, "mid": 0.5}]}


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = "assets/titles/vector.titleart"
    await ed.call("edit_title", path=path, create={"text": "SVG", "canvas": [1024, 512]}, bake="never")
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "set", "layer": "Title", "values": {"size": 220, "transform.rotation": 8, "transform.position": [40, -20],
                                                    "face.fill.kind": "linear", "face.fill.angle": 90, "face.fill.gradient": GRADIENT}},
        {"op": "add_rim", "layer": "Title", "values": {"width": 4, "join": "miter", "material.fill.color": "#202020"}},
        {"op": "add_rim", "layer": "Title", "values": {"width": 6, "join": "round", "material.fill.color": "#ffffff"}},
        {"op": "add_mask", "layer": "Title", "type": "shape", "values": {"shape.shape": "ellipse", "shape.size": 300, "shape.aspect": 2.4}},
    ])
    S["path"] = path


async def test_svg_matches_png(ed: Editor):
    """AC21: the SVG's raster equals the PNG export within max channel 48 and 0.5 % of pixels over 8."""
    await setup(ed)
    svg = await ed.call("export_title", path=S["path"], format="svg", options={"overwrite": True})
    ed.check(svg.get("ok"), f"exported: {svg}")
    ed.check_eq(svg["files"], ["Recordings/Title Art/vector/vector.svg"], "the default folder and name")
    png = await ed.call("export_title", path=S["path"], format="png", options={"overwrite": True, "dither": False})
    ed.check_eq(svg["frames"], png["frames"], "the same frame for the same document")
    text = (ed.project / svg["files"][0]).read_text()
    frame = next(iter(svg["frames"].values()))
    ed.check(f'viewBox="{frame[0]} {frame[1]} {frame[2]} {frame[3]}"' in text, f"the viewBox is the frame: {frame}")
    ed.check("<linearGradient" in text and "<mask" in text, "the gradient and the shape mask")
    ed.check('stroke-linejoin="round"' in text and 'stroke-linejoin="miter"' in text, "both rim joins")
    ed.check("transform=" not in re.sub(r"gradientTransform=\"[^\"]*\"", "", text), "transforms are baked into the coordinates")
    diff = await ed.call("diff_images", a=svg["files"][0], b=png["files"][0], tolerance=8, max_fraction=0.005, max_diff=48, max_mean=255)
    ed.check(diff.get("ok"), f"the SVG renders as the PNG: {diff}")


async def test_unrepresentable_listed_and_strict(ed: Editor):
    """Effects and pixel layers are left out by layer; strict fails with the list."""
    await setup(ed)
    path = "assets/titles/glow.titleart"
    await ed.call("edit_title", path=path, create={"text": "GLOW"}, bake="never")
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "add_effect", "layer": "Title", "type": "outer_glow"}])
    r = await ed.call("export_title", path=path, format="svg", options={"overwrite": True})
    ed.check(any("Title: outer_glow effect left out" in w for w in r.get("warnings", [])), f"the effect named by layer: {r}")
    msg = await ed.check_error("export_title", path=path, format="svg", options={"strict": True, "overwrite": True})
    ed.check("svg_unrepresentable" in msg and "outer_glow" in msg, f"strict lists what has no SVG form: {msg}")
    bad = await ed.check_error("export_title", path=path, format="svg", options={"embed_raster": "yes"})
    ed.check("bad_option" in bad, f"embed_raster is a bool: {bad}")


async def test_svg_per_language(ed: Editor):
    """One SVG per language, `.<code>` before the extension."""
    await setup(ed)
    path = "assets/titles/words.titleart"
    await ed.call("edit_title", path=path, create={"text": "WIN"}, bake="never")
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_language", "code": "de"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"}])
    r = await ed.call("export_title", path=path, format="svg", languages="all", options={"overwrite": True})
    ed.check_eq(sorted(r["files"]), ["Recordings/Title Art/words/words.de.svg", "Recordings/Title Art/words/words.en.svg"], "a file per language")
    ed.check(r["frames"]["de"] != r["frames"]["en"], "each trimmed to its own text")
