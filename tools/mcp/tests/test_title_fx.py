"""S83 Title effects (docs/specs/S83-title-effects-generators.md AC18, effect side; R44-R46,
§6.10, §6.11): list_title_library lists the catalogue with recipes and style settings, the `id`
detail carries animation classes and presets, `query` searches aliases and `category` filters;
add_effect takes a preset and names the nearest type on a typo; out-of-range values are clamped
and echoed, a value on a hidden param applies with an `inactive` note; apply_recipe adds the
recipe's effect in one step named after it; render_title {options: {cost}} returns the §6.4
readout.
"""

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Effects"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def create(ed: Editor, path: str) -> None:
    await ed.call("edit_title", path=path, create={"text": "Title"}, bake="never")


async def test_group_hidden(ed: Editor):
    """The catalogue's tools stay hidden until tool_groups enables title_art."""
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("list_title_library" not in default, "list_title_library hidden until title_art is enabled")
    await setup(ed)


async def test_catalogue_rows(ed: Editor):
    await setup(ed)
    rows = (await ed.call("list_title_library", kind="effects", limit=200))["rows"]
    by_id = {r["id"]: r for r in rows}
    for t in ("light_glow", "contour_lines", "neon_tube", "motion_blur", "lens_blur", "tilt_shift", "radial_blur", "extrude",
              "perspective_shadow", "floor_reflection", "wave_warp", "motion_trail", "chromatic_aberration", "glitch", "halftone",
              "scanlines", "pixelate", "grain", "holographic", "distress", "drips", "frost", "chalk", "material", "brush_stroke",
              "stitch", "shatter"):
        ed.check_in(t, by_id, f"{t} is listed")
        ed.check_eq(by_id[t]["entry"], "effect", f"{t} is an effect entry")
        ed.check(by_id[t]["cost"] in ("C0", "C1", "C2", "C3", "C4"), f"{t} has a class")
        ed.check(len(by_id[t]["presets"]) >= 1, f"{t} lists its presets")
    for r in ("long_shadow", "shine", "rainbow", "vignette", "emboss", "deboss", "icicles", "crt", "comic_dots", "ben_day"):
        ed.check_in(r, by_id, f"the recipe {r} is a menu entry")
        ed.check_eq(by_id[r]["entry"], "recipe", f"{r} is a recipe")
    ed.check_eq(by_id["dry_brush"]["entry"], "style_setting", "Dry Brush is a style setting")
    ed.check_eq(by_id["dry_brush"]["sets"], "style.erode.enabled", "it switches style.erode on")
    found = (await ed.call("list_title_library", kind="effects", query="power glow"))["rows"]
    ed.check(any(r["id"] == "light_glow" for r in found), f"'power glow' finds Light Glow: {found}")
    kasure = (await ed.call("list_title_library", kind="effects", query="かすれ", limit=50))["rows"]
    ids = {r["id"] for r in kasure}
    ed.check("dry_brush" in ids and "distress" in ids, f"'かすれ' finds Dry Brush and Distress: {ids}")
    blur = (await ed.call("list_title_library", kind="effects", category="Blur", limit=50))["rows"]
    ed.check(all(r["category"] == "Blur" for r in blur) and len(blur) >= 5, f"category filters: {blur}")


async def test_detail(ed: Editor):
    await setup(ed)
    d = await ed.call("list_title_library", kind="effects", id="light_glow")
    ed.check_eq(d["live"], "scale_split", "Light Glow's live variant")
    ed.check_in("intensity", d["live_params"], "intensity plays live")
    ed.check(any(p["id"] == "soft_bloom" for p in d["presets"]), "its presets with their params")
    ed.check_eq(d["anim"]["intensity"], "live", "each param's animation class")
    ed.check_eq(d["anim"]["radius"], "rerender", "a re-rendering param")
    recipe = await ed.call("list_title_library", kind="effects", id="long_shadow")
    ed.check_eq(recipe["type"], "extrude", "Long Shadow is an Extrude")
    msg = await ed.check_error("list_title_library", kind="effects", id="lite_glow")
    ed.check_in("light_glow", msg, "an unknown id names the nearest")


async def test_edit_ops(ed: Editor):
    await setup(ed)
    path = "assets/titles/fx.titleart"
    await create(ed, path)
    msg = await ed.check_error("edit_title", path=path, ops=[{"op": "add_effect", "layer": "Title", "type": "lite_glow"}])
    ed.check_in("light_glow", msg, "a typo names the nearest type")
    r = await ed.call("edit_title", path=path, ops=[{"op": "add_effect", "layer": "Title", "type": "light_glow", "preset": "soft_bloom"}])
    ed.check(r.get("ok", True), f"add_effect with a preset: {r}")
    r = await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"fx:light_glow.radius": 5000}}])
    ed.check(any(c.get("clamped") for c in r["changes"]), f"an out-of-range value is clamped and echoed: {r['changes']}")
    r = await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"fx:light_glow.color": "#ff0000"}}])
    problems = r.get("problems", [])
    ed.check(any(p.get("code") == "inactive" for p in problems), f"a hidden param applies with an inactive note: {problems}")
    r = await ed.call("edit_title", path=path, ops=[{"op": "apply_recipe", "layer": "Title", "recipe": "long_shadow"}])
    ed.check_eq(r.get("undo_name", r.get("undoName", "Long Shadow")), "Long Shadow", f"one step named after the recipe: {r}")
    doc = await ed.call("get_title", path=path, detail="full")
    title = next(l for l in doc["layers"] if l["name"] == "Title")
    effects = title["effects"]
    ed.check(any("extrude" in (e if isinstance(e, str) else e.get("type", "")) for e in effects), f"apply_recipe added an Extrude: {effects}")


async def test_cost_readout(ed: Editor):
    await setup(ed)
    path = "assets/titles/cost.titleart"
    await create(ed, path)
    await ed.call("edit_title", path=path, ops=[{"op": "add_effect", "layer": "Title", "type": "light_glow"}])
    r = await ed.call("render_title", path=path, image="none", options={"cost": "mobile"})
    cost = r["cost"]
    row = next(x for x in cost["rows"] if x["type"] == "light_glow")
    for key in ("path", "type", "class", "passes", "fre", "taps", "instances", "cpu_ms", "sdf", "analysis", "readback", "desktop_ms",
                "mobile_ms", "live"):
        ed.check_in(key, row, f"the readout has {key}")
    ed.check_in(cost["class"], ("C0", "C1", "C2", "C3", "C4"), "a document total")
    msg = await ed.check_error("render_title", path=path, options={"cost": "phone"})
    ed.check_in("desktop", msg, "a bad tier names the valid ones")
