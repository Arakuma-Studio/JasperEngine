"""S87 SVG export (docs/specs/S87-title-localisation-interchange.md R41, AC21; jasper://docs/title-art
"Import and export"): export_title format svg, rendered by LunaSVG (diff_images reads .svg), against
export_title png of the same document: text, rims (round and miter), a linear gradient and a
shape mask at AC21's limits, and all of them turned with contrasting rims at wider, measured
limits; what has no SVG form is listed by layer; strict fails; one file per language.
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


async def document(ed: Editor, name: str, values: dict, extra_ops: list) -> str:
    path = f"assets/titles/{name}.titleart"
    await ed.call("edit_title", path=path, create={"text": "SVG", "canvas": [1024, 512]}, bake="never")
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "set", "layer": "Title", "values": values}] + extra_ops)
    return path


async def compare(ed: Editor, path: str, max_diff: int, max_fraction: float) -> tuple:
    """The SVG's LunaSVG raster against the PNG export of the same frame."""
    svg = await ed.call("export_title", path=path, format="svg", options={"overwrite": True})
    ed.check(svg.get("ok"), f"exported: {svg}")
    png = await ed.call("export_title", path=path, format="png", options={"overwrite": True, "dither": False})
    ed.check_eq(svg["frames"], png["frames"], "the same frame for the same document")
    diff = await ed.call("diff_images", a=svg["files"][0], b=png["files"][0], tolerance=8, max_fraction=max_fraction, max_diff=max_diff, max_mean=255)
    ed.check(diff.get("ok"), f"the SVG renders as the PNG of {path}: {diff}")
    return svg, (ed.project / svg["files"][0]).read_text()


def rims(first: str, second: str) -> list:
    return [{"op": "add_rim", "layer": "Title", "values": {"width": 4, "join": "miter", "material.fill.color": first}},
            {"op": "add_rim", "layer": "Title", "values": {"width": 6, "join": "round", "material.fill.color": second}}]


MASK = {"op": "add_mask", "layer": "Title", "type": "shape", "values": {"shape.shape": "ellipse", "shape.size": 150, "shape.aspect": 3}}


async def test_rims_and_mask_match_png(ed: Editor):
    """AC21's limits (max channel 48, 0.5 % of pixels over 8): round and miter rims and a shape mask that cuts the text."""
    await setup(ed)
    path = await document(ed, "rims", {"size": 220, "face.fill.color": "#ffffff"}, rims("#ffffff", "#ffffff") + [MASK])
    _, text = await compare(ed, path, 48, 0.005)
    ed.check('stroke-linejoin="round"' in text and 'stroke-linejoin="miter"' in text, "both rim joins")
    ed.check("<mask" in text, "the shape mask")


async def test_gradient_matches_png(ed: Editor):
    """AC21's limits on a linear gradient face."""
    await setup(ed)
    path = await document(ed, "gradient", {"size": 220, "face.fill.kind": "linear", "face.fill.angle": 90, "face.fill.gradient": GRADIENT}, [])
    _, text = await compare(ed, path, 48, 0.005)
    ed.check("<linearGradient" in text, "the gradient")


async def test_turned_contrasting_title_registers(ed: Editor):
    """Everything at once, turned and moved, with rims of contrasting colours: a registration guard with wider limits.

    Measured causes of the difference (S87 Deviations), none of them in the outlines (the least-squares
    shift of the SVG's raster against the PNG is under 0.01 px, flat and turned): the engine blends
    edges in linear light and SVG in sRGB (a black-white edge differs by 60 at half coverage), and
    the renderer resamples a turned layer's mask (max 84 on white text alone).
    """
    await setup(ed)
    path = await document(ed, "turned", {"size": 220, "transform.rotation": 8, "transform.position": [40, -20], "face.fill.kind": "linear",
                                         "face.fill.angle": 90, "face.fill.gradient": GRADIENT}, rims("#202020", "#ffffff"))
    svg, text = await compare(ed, path, 110, 0.11)
    frame = next(iter(svg["frames"].values()))
    ed.check(f'viewBox="{frame[0]} {frame[1]} {frame[2]} {frame[3]}"' in text, f"the viewBox is the frame: {frame}")
    ed.check_eq(svg["files"], ["Recordings/Title Art/turned/turned.svg"], "the default folder and name")
    ed.check(" transform=" not in re.sub(r'gradientTransform="[^"]*"', "", text), "transforms are baked into the coordinates")


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


async def test_embed_raster_uses_the_renderer(ed: Editor):
    """embed_raster: a layer with no vector form comes from the real renderer, as a PNG in the SVG."""
    await setup(ed)
    path = "assets/titles/raster.titleart"
    await ed.call("edit_title", path=path, create={"text": "RASTER", "canvas": [512, 256]}, bake="never",
                  ops=[{"op": "add_layer", "type": "generator", "generator": "fill"}])
    plain = await ed.call("export_title", path=path, format="svg", options={"overwrite": True})
    ed.check(any("left out" in w for w in plain.get("warnings", [])), f"without it the generator is left out: {plain}")
    ed.check("<image" not in (ed.project / plain["files"][0]).read_text(), "and nothing is embedded")
    r = await ed.call("export_title", path=path, format="svg", options={"overwrite": True, "embed_raster": True})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check(not any("left out" in w for w in r.get("warnings", [])), f"the layer is no longer left out: {r}")
    text = (ed.project / r["files"][0]).read_text()
    ed.check_in("<image", text, "a raster element")
    ed.check_in("data:image/png;base64,", text, "with the rendered PNG inside")
    png = await ed.call("export_title", path=path, format="png", options={"overwrite": True, "dither": False})
    ed.check_eq(r["frames"], png["frames"], "the same frame")
    diff = await ed.call("diff_images", a=r["files"][0], b=png["files"][0], tolerance=8, max_fraction=0.02, max_diff=64, max_mean=255)
    ed.check(diff.get("ok"), f"the SVG renders as the PNG: {diff}")

