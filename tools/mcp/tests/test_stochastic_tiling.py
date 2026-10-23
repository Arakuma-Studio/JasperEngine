"""Stochastic Tiling (includes/stochastic.glsl, Mikkelsen's hex-tiling) in the editor, on the
stochastic-tiling fixture (tools/fixtures/stochastic-tiling, saved by an editor without the
feature): the terrain layers' five settings through set_terrain_maps (round trip through
get_entity, set_properties paths, undo, schema and range errors that say how to fix them), the
library shaders' STOCHASTIC feature (list_shaders rows gated by the feature, check_shader of its
variants), screenshots with the feature off and on (the picture changes, keeps its mean colour and
has no black or NaN pixels), a save and reload of the scene with the feature on, and the old scene
re-saved byte-identically once the feature is toggled back off."""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

from harness import Editor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402

REPO = HERE.parents[2]
FIXTURE = REPO / "tools" / "fixtures" / "stochastic-tiling"
EDITOR = {"project": str(FIXTURE)}
TIMEOUT = 900

SCENE_FILE = "Main Scene.scene"
M = "submeshes[0].material."
L = "surfaceLayers[0]."
FIELDS = ("stochasticTiling", "stochasticCellSize", "stochasticBlendSharpness", "stochasticContrast", "stochasticRotation")
DEFAULTS = {"stochasticTiling": False, "stochasticCellSize": 1.0, "stochasticBlendSharpness": 0.4,
            "stochasticContrast": 0.45, "stochasticRotation": 0.0}
# the library shaders with the feature, and a variant of each with other features
LIBRARY = {
    "simple_lit": "NORMAL_MAP,EMISSION,STOCHASTIC",
    "unlit": "ALPHA_CLIP,STOCHASTIC",
    "toon": "RAMP,STOCHASTIC",
    "triplanar": "TOP_BOTTOM,NOISE_MASK,STOCHASTIC",
    "vertex_lit": "EMISSION,STOCHASTIC",
    "fake_lit": "EMISSION,MATCAP,STOCHASTIC",
}


def close(a, b, eps=1e-4) -> bool:
    # shader parameters read back as [x, y, z, w]
    a = a[0] if isinstance(a, list) else a
    return abs(float(a) - float(b)) <= eps


async def scene_id(ed: Editor):
    return (await ed.call("list_scenes"))["scenes"][0]["id"]


async def entity(ed: Editor, sid, name: str):
    return (await ed.call("find_entities", scene=sid, name=name))["entities"][0]["id"]


async def layer_values(ed: Editor, sid, terrain) -> dict:
    got = await ed.call("get_entity", scene=sid, entity=terrain, components=["terrain"],
                        properties=[L + f for f in FIELDS], detail="values")
    values = next(iter(got["components"].values()))
    return {f: values[L + f] for f in FIELDS}


async def material_values(ed: Editor, sid, eid) -> dict:
    got = await ed.call("get_entity", scene=sid, entity=eid, components=["mesh"],
                        properties=[M + "shaderFeatures", M + "shaderParams"], detail="values")
    return next(iter(got["components"].values()))


async def test_old_scene_resaves_byte_identical(ed: Editor):
    """the fixture was saved without the feature: toggling it on and back off and saving gives the
    same bytes (defaults are not written, for terrain layers and materials alike)"""
    original = (ed.project / SCENE_FILE).read_bytes()
    sid = await scene_id(ed)
    terrain = await entity(ed, sid, "Ground")
    quad = await entity(ed, sid, "SimpleLitQuad")
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, "stochastic_tiling": True}])
    await ed.call("set_properties", scene=sid, entity=quad, values={"mesh": {M + "shaderFeatures": {"STOCHASTIC": True}}})
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, "stochastic_tiling": False}])
    # (the object form writes "!STOCHASTIC" for a feature turned off: the list restores the old value)
    await ed.call("set_properties", scene=sid, entity=quad, values={"mesh": {M + "shaderFeatures": ["NORMAL_MAP"]}})
    await ed.call("save_scene", scene=sid)
    resaved = (ed.project / SCENE_FILE).read_bytes()
    if resaved != original:
        a, b = original.decode().splitlines(), resaved.decode().splitlines()
        diff = [f"{i + 1}: {x!r} -> {y!r}" for i, (x, y) in enumerate(zip(a, b)) if x != y][:10]
        ed.fail(f"the re-saved scene differs ({len(a)} -> {len(b)} lines): {diff}")


async def test_terrain_layer_settings(ed: Editor):
    sid = await scene_id(ed)
    terrain = await entity(ed, sid, "Ground")
    ed.check_eq(await layer_values(ed, sid, terrain), DEFAULTS, "defaults")

    # parameters given while the layer is off are kept, with a note
    out = await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, "cell_size": 2.0}])
    ed.check(any("stochastic_tiling" in n for n in out.get("notes", [])), f"a note on the inactive setting: {out!r}")
    ed.check(close((await layer_values(ed, sid, terrain))["stochasticCellSize"], 2.0), "cell_size stored while off")
    await ed.call("undo", scene=sid)

    out = await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{
        "index": 0, "stochastic_tiling": True, "cell_size": 1.5, "blend_sharpness": 0.6, "contrast": 0.3, "rotation": 0.25}])
    ed.check_eq(len(out["stochastic_tiling"]), 1, f"the layers with the feature: {out!r}")
    row = out["stochastic_tiling"][0]
    ed.check(row["index"] == 0 and close(row["cell_size"], 1.5) and close(row["rotation"], 0.25), f"{row!r}")
    got = await layer_values(ed, sid, terrain)
    ed.check(got["stochasticTiling"] is True, f"{got!r}")
    for field, want in (("stochasticCellSize", 1.5), ("stochasticBlendSharpness", 0.6), ("stochasticContrast", 0.3),
                        ("stochasticRotation", 0.25)):
        ed.check(close(got[field], want), f"{field}: {got[field]} != {want}")

    # the Catalog paths (Inspector, set_properties) reach the same fields
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {L + "stochasticRotation": 0.5}})
    ed.check(close((await layer_values(ed, sid, terrain))["stochasticRotation"], 0.5), "set_properties path")
    await ed.call("undo", scene=sid)
    ed.check(close((await layer_values(ed, sid, terrain))["stochasticRotation"], 0.25), "undo of the path edit")

    # the whole set_terrain_maps call is one undo step
    await ed.call("undo", scene=sid)
    ed.check_eq(await layer_values(ed, sid, terrain), DEFAULTS, "undo restores the defaults")
    await ed.call("redo", scene=sid)
    ed.check((await layer_values(ed, sid, terrain))["stochasticTiling"] is True, "redo")
    await ed.call("undo", scene=sid)


async def test_terrain_layer_errors(ed: Editor):
    sid = await scene_id(ed)
    terrain = await entity(ed, sid, "Ground")
    cases = [
        ({"cell_size": 8}, ["cell_size", "0.25", "4"]),
        ({"cell_size": 0.1}, ["cell_size", "0.25"]),
        ({"blend_sharpness": 1.5}, ["blend_sharpness", "1"]),
        ({"contrast": -0.2}, ["contrast", "0"]),
        ({"rotation": "half"}, ["rotation"]),
        ({"stochastic_tiling": "yes"}, ["stochastic_tiling"]),
    ]
    for extra, needles in cases:
        err = await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, **extra}], expect_error=True)
        for needle in needles:
            ed.check_in(needle, err, f"error of {extra}")
    ed.check_eq(await layer_values(ed, sid, terrain), DEFAULTS, "a refused call changes nothing")


async def test_library_feature(ed: Editor):
    listed = {s["shader"]: s for s in (await ed.call("list_shaders", detail=True))["shaders"]}
    for name in LIBRARY:
        shader = listed["library:" + name]
        features = {f["define"]: f for f in shader["features"]}
        ed.check("STOCHASTIC" in features, f"{name}: features {list(features)}")
        ed.check_eq(features["STOCHASTIC"]["label"], "Stochastic Tiling", name)
        params = {p["member"]: p for p in shader["properties"]}
        for member, label, default in (("stochasticCellSize", "Cell Size", 1.0), ("stochasticBlendSharpness", "Blend Sharpness", 0.4),
                                       ("stochasticContrast", "Contrast", 0.45), ("stochasticRotation", "Rotation", 0.0)):
            ed.check(member in params, f"{name}: no {member}")
            ed.check_eq(params[member]["feature"], "STOCHASTIC", f"{name}.{member} gating")
            ed.check_eq(params[member]["label"], label, f"{name}.{member}")
            got = params[member]["default"]
            ed.check(close(got, default), f"{name}.{member} default {got}")

    for name, features in LIBRARY.items():
        for variant in ("STOCHASTIC", features):
            result = await ed.call("check_shader", shader=f"library:{name}#{variant}", timeout=600)
            failed = [r for r in result["results"] if not r["ok"]]
            ed.check(not failed, f"library:{name}#{variant}: {failed!r}")


def ground_stats(png: bytes, top: float = 0.5):
    """mean colour, and the share of black and of saturated-magenta (NaN-like) pixels, of the lower
    part of a screenshot"""
    width, height, rgba = pngdiff.decode(png)
    total, black, magenta = 0, 0, 0
    sums = [0, 0, 0]
    for y in range(int(height * top), height, 2):
        row = y * width * 4
        for x in range(0, width, 2):
            r, g, b = rgba[row + x * 4], rgba[row + x * 4 + 1], rgba[row + x * 4 + 2]
            total += 1
            sums[0] += r
            sums[1] += g
            sums[2] += b
            if r < 4 and g < 4 and b < 4:
                black += 1
            if r > 250 and g < 5 and b > 250:
                magenta += 1
    return [s / total for s in sums], black / total, magenta / total


async def test_screenshots_off_and_on(ed: Editor):
    sid = await scene_id(ed)
    terrain = await entity(ed, sid, "Ground")
    cams = {name: await entity(ed, sid, name) for name in ("TerrainCam", "PlaneCam")}
    shots = ed.scratch.root / "shots" if ed.scratch else Path("/tmp/stochastic-shots")
    await ed.call("configure_game_view", aspect="16:9")

    async def take(tag):
        images = {}
        for name, eid in cams.items():
            await ed.call("set_main_camera", scene=sid, entity=eid)
            images[name] = await ed.screenshot(view="game", path=shots / f"{name}-{tag}.png", max_width=960)
        return images

    off = await take("off")
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, "stochastic_tiling": True}])
    for name in ("SimpleLitQuad", "TriplanarBox", "UnlitStripes"):
        eid = await entity(ed, sid, name)
        await ed.call("set_properties", scene=sid, entity=eid, values={"mesh": {M + "shaderFeatures": {"STOCHASTIC": True}}})
    on = await take("on")

    for name in cams:
        ok, fraction, _ = pngdiff.compare(pngdiff.decode(off[name]), pngdiff.decode(on[name]), channel_tolerance=24, max_fraction=0.02)
        ed.check(not ok and fraction > 0.05, f"{name}: Stochastic Tiling changes the picture ({fraction:.3f} of the pixels)")
        mean_off, black_off, _ = ground_stats(off[name])
        mean_on, black_on, magenta = ground_stats(on[name])
        ed.check(black_on <= black_off + 0.002, f"{name}: no new black pixels ({black_off:.4f} -> {black_on:.4f})")
        ed.check(magenta == 0, f"{name}: no NaN-coloured pixels")
        for c in range(3):
            ed.check(abs(mean_on[c] - mean_off[c]) < 12, f"{name}: the mean colour stays ({mean_off} -> {mean_on})")
    problems = [e for e in await ed.console_since(None) if e.get("type") == "error"]
    ed.check(not problems, f"console errors: {problems[:5]!r}")


async def test_save_and_reload(ed: Editor):
    sid = await scene_id(ed)
    terrain = await entity(ed, sid, "Ground")
    quad = await entity(ed, sid, "SimpleLitQuad")
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{
        "index": 0, "stochastic_tiling": True, "cell_size": 0.75, "contrast": 0.6, "rotation": 1.0}])
    await ed.call("set_properties", scene=sid, entity=quad, values={"mesh": {
        M + "shaderFeatures": {"STOCHASTIC": True}, M + "shaderParams.stochasticCellSize": 2.0}})
    await ed.call("save_scene", scene=sid)

    saved = yaml.safe_load((ed.project / SCENE_FILE).read_text())
    ground = next(e for e in saved["entities"] if e.get("name") == "Ground")
    layer = ground["components"]["terrain"]["surfaceLayers"][0]
    ed.check(layer.get("stochasticTiling") is True and close(layer["stochasticCellSize"], 0.75)
             and close(layer["stochasticContrast"], 0.6) and close(layer["stochasticRotation"], 1.0), f"saved layer: {layer!r}")
    ed.check("stochasticBlendSharpness" not in layer, "a default parameter is not written")

    # reload from disk: close it (another scene open first: the last one cannot close) and reopen
    await ed.call("create_scene", name="Other", path="Other.scene")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    await ed.call("open_scene", scene=SCENE_FILE)
    sid = next(s["id"] for s in (await ed.call("list_scenes"))["scenes"] if s.get("name") == "Main Scene")
    terrain = await entity(ed, sid, "Ground")
    quad = await entity(ed, sid, "SimpleLitQuad")
    got = await layer_values(ed, sid, terrain)
    ed.check(got["stochasticTiling"] is True and close(got["stochasticCellSize"], 0.75) and close(got["stochasticRotation"], 1.0)
             and close(got["stochasticBlendSharpness"], 0.4), f"reloaded layer: {got!r}")
    material = await material_values(ed, sid, quad)
    ed.check("STOCHASTIC" in material[M + "shaderFeatures"], f"reloaded features: {material!r}")
    ed.check(close(material[M + "shaderParams"]["stochasticCellSize"], 2.0), f"reloaded params: {material!r}")
