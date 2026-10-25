"""Stochastic Tiling (includes/stochastic.glsl, Mikkelsen's hex-tiling) in the editor, on the
stochastic-tiling fixture (tools/fixtures/stochastic-tiling, saved with the feature off): the
terrain layers' five settings through set_terrain_maps (round trip through get_entity,
set_properties paths, undo, schema and range errors that say how to fix them), the library
shaders' STOCHASTIC feature (list_shaders rows gated by the feature, check_shader of its variants),
the built-in Lit material's (set_properties paths and material objects, range errors, the 'Sto'
bits of its colour, depth and G-buffer variants in get_build_info and generate_shaders, a prefab
override, the check command's warnings), screenshots with the feature off and on (the picture
changes, keeps its mean colour and has no black or NaN pixels; Lit also with SSR's G-buffer views
and an alpha-masked shadow caster), a save and reload of the scene with the feature on, and the
old scene re-saved byte-identically once the feature is toggled back off."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

import harness
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


# ---------------------------------------------------------------- the built-in Lit material

LIT = ("LitGround", "LitBox", "LitFoliage")
MESH_STO = 1 << 5
DEPTH_STO = 1 << 14
GBUFFER_STO = 1 << 10


async def lit_values(ed: Editor, sid, eid) -> dict:
    got = await ed.call("get_entity", scene=sid, entity=eid, components=["mesh"], properties=[M + f for f in FIELDS], detail="values")
    values = next(iter(got["components"].values()))
    return {f: values[M + f] for f in FIELDS}


async def lit_on(ed: Editor, sid, on: bool = True, **values):
    """Stochastic Tiling of the Lit meshes on (or off), with values; the others at their defaults"""
    for name in LIT:
        eid = await entity(ed, sid, name)
        fields = {M + k: v for k, v in DEFAULTS.items()}
        fields.update({M + k: v for k, v in values.items()})
        fields[M + "stochasticTiling"] = on
        await ed.call("set_properties", scene=sid, entity=eid, values={"mesh": fields})


async def test_lit_material_settings(ed: Editor):
    sid = await scene_id(ed)
    ground = await entity(ed, sid, "LitGround")
    ed.check_eq(await lit_values(ed, sid, ground), DEFAULTS, "defaults")

    await ed.call("set_properties", scene=sid, entity=ground, values={"mesh": {
        M + "stochasticTiling": True, M + "stochasticCellSize": 1.5, M + "stochasticBlendSharpness": 0.6,
        M + "stochasticContrast": 0.3, M + "stochasticRotation": 0.25}})
    got = await lit_values(ed, sid, ground)
    ed.check(got["stochasticTiling"] is True, f"{got!r}")
    for field, want in (("stochasticCellSize", 1.5), ("stochasticBlendSharpness", 0.6), ("stochasticContrast", 0.3),
                        ("stochasticRotation", 0.25)):
        ed.check(close(got[field], want), f"{field}: {got[field]} != {want}")

    # the whole material as an object reads them, and takes them
    whole = await ed.call("get_entity", scene=sid, entity=ground, components=["mesh"], properties=["submeshes[0].material"],
                          detail="values")
    material = next(iter(whole["components"].values()))["submeshes[0].material"]
    ed.check(material.get("stochasticTiling") is True and close(material["stochasticCellSize"], 1.5)
             and close(material["stochasticRotation"], 0.25), f"material object: {material!r}")
    await ed.call("set_properties", scene=sid, entity=ground, values={"mesh": {"submeshes[0].material": {"stochasticContrast": 0.8}}})
    ed.check(close((await lit_values(ed, sid, ground))["stochasticContrast"], 0.8), "a material object sets it")
    await ed.call("undo", scene=sid)

    # out of range: the error names the field, its range and what it does
    cases = [
        ({M + "stochasticCellSize": 8}, ["stochasticCellSize", "0.25", "4", "default 1"]),
        ({M + "stochasticCellSize": 0.1}, ["stochasticCellSize", "0.25"]),
        ({M + "stochasticBlendSharpness": 1.5}, ["stochasticBlendSharpness", "0 to 1"]),
        ({M + "stochasticRotation": -0.5}, ["stochasticRotation", "180"]),
        ({M + "stochasticTiling": "yes"}, ["stochasticTiling"]),
        ({"submeshes[0].material": {"stochasticContrast": 2}}, ["stochasticContrast", "0 to 1"]),
    ]
    for values, needles in cases:
        err = await ed.call("set_properties", scene=sid, entity=ground, values={"mesh": values}, expect_error=True)
        for needle in needles:
            ed.check_in(needle, err, f"error of {values}")

    # one undo step back to the defaults; then the file holds none of them (defaults are not written)
    await ed.call("undo", scene=sid)
    ed.check_eq(await lit_values(ed, sid, ground), DEFAULTS, "undo restores the defaults")
    await ed.call("save_scene", scene=sid)
    saved = yaml.safe_load((ed.project / SCENE_FILE).read_text())
    node = next(e for e in saved["entities"] if e.get("name") == "LitGround")
    material = node["components"]["mesh"]["submeshes"][0]["material"]
    ed.check(not [k for k in material if k.startswith("stochastic")], f"a material with the feature off writes none of its keys: {material!r}")


async def test_lit_variants(ed: Editor):
    """the colour, depth (the alpha-masked foliage's clip) and G-buffer variants of the Lit meshes
    carry 'Sto' once it is on, and compile"""
    sid = await scene_id(ed)
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": True})
    await ed.call("edit_build_scenes", action="add", scene=sid)
    before = {s["name"] for s in (await ed.call("get_build_info", include_shaders=True))["shaders"]}
    ed.check(not any("Sto" in n for n in before), f"no Sto variant while it is off: {sorted(n for n in before if 'Sto' in n)}")

    await lit_on(ed, sid)
    await ed.call("wait", frames=10)
    shaders = (await ed.call("get_build_info", include_shaders=True))["shaders"]
    # display names, "Mesh (Uv1, Puc, Sto, ...)" (a library shader's has its reference in brackets)
    built_in = {s["name"]: s for s in shaders if "[" not in s["name"]}

    def bits(name):
        return name[name.find("(") + 1:-1].split(", ") if "(" in name else []

    names = set(built_in)
    mesh = sorted(n for n in names if n.startswith("Mesh (") and "Sto" in bits(n))
    depth = sorted(n for n in names if n.startswith("Depth (") and "Sto" in bits(n))
    gbuffer = sorted(n for n in names if n.startswith("GBuffer (") and "Sto" in bits(n))
    ed.check(mesh, f"Lit colour variants with Sto: {sorted(names)}")
    ed.check(any("Ams" in bits(n) for n in mesh), f"the foliage's alpha-masked variant: {mesh}")
    ed.check(depth and all("Ams" in bits(n) and "Tex" in bits(n) for n in depth), f"its alpha clip depth variant: {sorted(names)}")
    ed.check(gbuffer, f"G-buffer variants with Sto: {sorted(names)}")
    # the library shaders and the terrain never take Lit's bit
    ed.check(not any("Ter" in bits(n) for n in mesh), f"no terrain variant takes it: {mesh}")
    ed.check(not any("Sto" in s["name"] for s in shaders if "[" in s["name"]), "no library shader variant has Lit's bit")

    keys = [built_in[n]["key"] for n in mesh + depth + gbuffer]
    out = ed.scratch.root / "sto-shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary", shaders=keys,
                      timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    written = {p.stem.rsplit("_glsl", 1)[0] for p in out.rglob("*.sdat")}
    expected = {{"Mesh": "mesh", "Depth": "depth", "GBuffer": "gbuffer"}[n.split(" ")[0]] + "_" + "".join(bits(n))
                for n in mesh + depth + gbuffer}
    ed.check(expected <= written, f"compiled {sorted(expected)}: {sorted(written)}")
    await lit_on(ed, sid, on=False)
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": False})


def variant_bits(name: str) -> list[str]:
    """the property names of a display name, "Mesh [shaders/x] (Uv1, Puc, Sto)" -> [Uv1, Puc, Sto]"""
    return name[name.rfind("(") + 1:-1].split(", ") if name.endswith(")") else []


def old_style_fork(source: str) -> str:
    """Remove the documented opt-in directive; the fork must then use legacy variants.

    Keep the shader's declarations intact so unrelated layout or identifier changes do not
    break the compatibility fixture.
    """
    old, count = re.subn(r"^[ \t]*#[ \t]*define[ \t]+LIT_STOCHASTIC_PARAMS\b[^\n]*", "", source, flags=re.M)
    assert count == 1, "the current fork defines the documented Stochastic Tiling opt-in"
    return old


async def foliage_variants(ed: Editor, fork: str) -> dict:
    """the variants the build collects for the alpha-masked leaf card (the only Lit mesh with
    Stochastic Tiling on, drawn with the fork): its colour variants (the fork's), and the
    alpha-clip depth and G-buffer variants (built-in shaders)"""
    await ed.call("wait", frames=10)
    shaders = (await ed.call("get_build_info", include_shaders=True))["shaders"]
    names = [s["name"] for s in shaders]
    return {
        "keys": {s["name"]: s["key"] for s in shaders},
        "mesh": sorted(n for n in names if n.startswith(f"Mesh [{fork}] (")),
        "depth": sorted(n for n in names if n.startswith("Depth (") and "Ams" in variant_bits(n) and "Tex" in variant_bits(n)),
        "gbuffer": sorted(n for n in names if n.startswith("GBuffer (")),
        "sto": sorted(n for n in names if "Sto" in variant_bits(n)),
    }


async def test_lit_fork_passes_agree(ed: Editor):
    """a fork of Lit without the documented Stochastic Tiling opt-in (no LIT_STOCHASTIC_PARAMS) on the alpha-masked
    leaf card with the feature on: its colour, depth (the shadow's alpha clip) and G-buffer variants
    agree, none with 'Sto', and `check` says the fork predates the feature; the same fork brought up
    to date (as fork_shader writes it now) takes 'Sto' in all three, and they compile"""
    ed.require_tools("fork_shader", "write_file", "get_build_info", "generate_shaders")
    sid = await scene_id(ed)
    await lit_on(ed, sid, on=False)
    foliage = await entity(ed, sid, "LitFoliage")
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": True})
    try:
        await ed.call("edit_build_scenes", action="add", scene=sid)
    except Exception:
        pass  # already in the build (test_lit_variants)
    before = await foliage_variants(ed, "none")
    ed.check(not before["sto"], f"no Sto variant while it is off: {before['sto']}")

    forked = await ed.call("fork_shader", scene=sid, entity=foliage, name="OldLeaves")
    fork, frag = forked["custom_shader"], forked["frag_path"]
    current = (ed.project / frag).read_text()
    ed.check(re.search(r"^[ \t]*#[ \t]*define[ \t]+LIT_STOCHASTIC_PARAMS\b", current, re.M),
             "a fork made now names the values")
    w = await ed.call("write_file", path=frag, content=old_style_fork(current), timeout=300)
    ed.check(all(c.get("ok", True) for c in w.get("shader_check", [])), f"the old-style fork compiles: {w.get('shader_check')!r}")
    await ed.call("set_properties", scene=sid, entity=foliage, values={"mesh": {M + "stochasticTiling": True}})

    old = await foliage_variants(ed, fork)
    ed.check(old["mesh"], f"the fork's colour variants: {sorted(old['keys'])}")
    ed.check(any("Ams" in variant_bits(n) for n in old["mesh"]), f"its alpha-masked colour variant: {old['mesh']}")
    ed.check(old["depth"], f"its alpha clip depth variant (the spot light's shadow): {sorted(old['keys'])}")
    ed.check(old["gbuffer"], f"G-buffer variants (SSR): {sorted(old['keys'])}")
    ed.check(not old["sto"], f"an old fork: no pass takes Sto (colour, depth and G-buffer agree): {old['sto']}")

    # check: the fork predates the feature, with a fix
    await ed.call("save_scene", scene=sid)
    data = run_check_json(ed.project)
    found = [f for f in data["findings"] if f.get("code") == "stochastic-no-effect" and "LitFoliage" in f["message"]]
    ed.check(len(found) == 1, f"one stochastic-no-effect finding for the leaf card: {data['findings']}")
    if found:
        f = found[0]
        ed.check_in(f"the material's custom shader {fork} predates Stochastic Tiling", f["message"])
        ed.check_in("update the fork (see docs) or turn the feature off", f["message"])
        ed.check(f["severity"] == "warning" and f["path"] == SCENE_FILE, f"{f!r}")
        ed.check("LIT_STOCHASTIC_PARAMS" in f.get("suggestion", ""), f"the suggestion says how: {f!r}")

    # the fork brought up to date: Sto in every pass
    await ed.console_since(None)
    w = await ed.call("write_file", path=frag, content=current, timeout=300)
    ed.check(all(c.get("ok", True) for c in w.get("shader_check", [])), f"the fork compiles: {w.get('shader_check')!r}")
    new = await foliage_variants(ed, fork)
    sto_mesh = [n for n in new["mesh"] if "Sto" in variant_bits(n)]
    sto_depth = [n for n in new["depth"] if "Sto" in variant_bits(n)]
    sto_gbuffer = [n for n in new["gbuffer"] if "Sto" in variant_bits(n)]
    ed.check(sto_mesh and any("Ams" in variant_bits(n) for n in sto_mesh), f"the fork's alpha-masked colour variant has Sto: {new['mesh']}")
    ed.check(sto_depth, f"its alpha clip depth variant has Sto: {new['depth']}")
    ed.check(sto_gbuffer, f"its G-buffer variant has Sto: {new['gbuffer']}")
    await ed.call("save_scene", scene=sid)
    data = run_check_json(ed.project)
    ed.check(not [f for f in data["findings"] if f.get("code") == "stochastic-no-effect" and "LitFoliage" in f["message"]],
             f"no finding once the fork names the values: {data['findings']}")

    # the fork's Sto variant builds where the editor draws it (generate_shaders builds no forks:
    # they need the project), the built-in depth and G-buffer ones through generate_shaders
    await ed.call("configure_game_view", aspect="16:9")
    await ed.call("set_main_camera", scene=sid, entity=await entity(ed, sid, "LitCam"))
    shots = ed.scratch.root / "shots" if ed.scratch else Path("/tmp/stochastic-shots")
    await ed.screenshot(view="game", path=shots / "LitCam-fork-sto.png", max_width=960)
    problems = [e for e in await ed.console_since() if e.get("type") == "error" or "OldLeaves" in e.get("message", "")]
    ed.check(not problems, f"the fork's Sto variants build and draw: {problems[:5]!r}")
    keys = [new["keys"][n] for n in sto_depth + sto_gbuffer]
    out = ed.scratch.root / "sto-fork-shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary", shaders=keys,
                      timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"the depth and G-buffer Sto variants compile: {r}")

    # back as it was (later tests take the fixture's scene)
    await ed.call("set_properties", scene=sid, entity=foliage, values={"mesh": {M + "stochasticTiling": False, "customShader": ""}})
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": False})
    await ed.call("save_scene", scene=sid)


async def lit_shots(ed: Editor, tag: str, shots: Path, settings=None) -> bytes:
    sid = await scene_id(ed)
    if settings:
        await ed.call("set_scene_settings", scene=sid, settings=settings)
    await ed.call("set_main_camera", scene=sid, entity=await entity(ed, sid, "LitCam"))
    return await ed.screenshot(view="game", path=shots / f"LitCam-{tag}.png", max_width=960)


async def test_lit_screenshots_off_and_on(ed: Editor):
    sid = await scene_id(ed)
    shots = ed.scratch.root / "shots" if ed.scratch else Path("/tmp/stochastic-shots")
    await ed.call("configure_game_view", aspect="16:9")
    await lit_on(ed, sid, on=False)
    off = await lit_shots(ed, "off", shots)
    # SSR: the G-buffer's base colour and roughness (composite debug views 5 and 3)
    gbuffer_off = await lit_shots(ed, "gbuffer-albedo-off", shots, {"ssr_enabled": True, "ssr_debug_mode": 5})
    rough_off = await lit_shots(ed, "gbuffer-roughness-off", shots, {"ssr_debug_mode": 3})
    ssr_off = await lit_shots(ed, "ssr-off", shots, {"ssr_debug_mode": 0})
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": False})

    await lit_on(ed, sid)
    on = await lit_shots(ed, "on", shots)
    gbuffer_on = await lit_shots(ed, "gbuffer-albedo-on", shots, {"ssr_enabled": True, "ssr_debug_mode": 5})
    rough_on = await lit_shots(ed, "gbuffer-roughness-on", shots, {"ssr_debug_mode": 3})
    ssr_on = await lit_shots(ed, "ssr-on", shots, {"ssr_debug_mode": 0})
    await ed.call("set_scene_settings", scene=sid, settings={"ssr_enabled": False})
    # with Rotation (normal maps turned back per cell)
    await lit_on(ed, sid, stochasticRotation=1.0)
    rotated = await lit_shots(ed, "on-rotation", shots)

    for tag, a, b in (("colour", off, on), ("G-buffer base colour", gbuffer_off, gbuffer_on),
                      ("G-buffer roughness", rough_off, rough_on), ("SSR", ssr_off, ssr_on), ("rotation", on, rotated)):
        ok, fraction, _ = pngdiff.compare(pngdiff.decode(a), pngdiff.decode(b), channel_tolerance=24, max_fraction=0.02)
        ed.check(not ok and fraction > 0.03, f"{tag}: Stochastic Tiling changes the picture ({fraction:.3f} of the pixels)")
    for tag, a, b in (("colour", off, on), ("G-buffer base colour", gbuffer_off, gbuffer_on), ("SSR", ssr_off, ssr_on),
                      ("rotation", off, rotated)):
        mean_off, black_off, _ = ground_stats(a)
        mean_on, black_on, magenta = ground_stats(b)
        ed.check(black_on <= black_off + 0.002, f"{tag}: no new black pixels ({black_off:.4f} -> {black_on:.4f})")
        ed.check(magenta == 0, f"{tag}: no NaN-coloured pixels")
        for c in range(3):
            ed.check(abs(mean_on[c] - mean_off[c]) < 12, f"{tag}: the mean colour stays ({mean_off} -> {mean_on})")
    problems = [e for e in await ed.console_since(None) if e.get("type") == "error"]
    ed.check(not problems, f"console errors: {problems[:5]!r}")
    await lit_on(ed, sid, on=False)


async def test_lit_save_reload_and_prefab_override(ed: Editor):
    sid = await scene_id(ed)
    box = await entity(ed, sid, "LitBox")
    await ed.call("set_properties", scene=sid, entity=box, values={"mesh": {
        M + "stochasticTiling": True, M + "stochasticCellSize": 0.75, M + "stochasticRotation": 0.5}})
    await ed.call("save_scene", scene=sid)
    saved = yaml.safe_load((ed.project / SCENE_FILE).read_text())
    node = next(e for e in saved["entities"] if e.get("name") == "LitBox")
    material = node["components"]["mesh"]["submeshes"][0]["material"]
    ed.check(material.get("stochasticTiling") is True and close(material["stochasticCellSize"], 0.75)
             and close(material["stochasticRotation"], 0.5), f"saved material: {material!r}")
    ed.check("stochasticContrast" not in material and "stochasticBlendSharpness" not in material, "defaults are not written")
    keys = saved.get("shaderKeys", [])
    ed.check(any(k.get("type") == "mesh" and k.get("properties", 0) & MESH_STO and k.get("keyVersion") == 2 for k in keys),
             f"the scene's Sto key carries keyVersion 2: {keys}")
    ed.check(not any(k.get("properties", 0) & MESH_STO and k.get("type") == "mesh" and "keyVersion" not in k for k in keys),
             "no unversioned MESH bit 5")

    # reload from disk: close it (another scene open first: the last one cannot close) and reopen
    if len((await ed.call("list_scenes"))["scenes"]) < 2:
        await ed.call("create_scene", name="Other Lit", path="Other Lit.scene")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    await ed.call("open_scene", scene=SCENE_FILE)
    sid = next(s["id"] for s in (await ed.call("list_scenes"))["scenes"] if s.get("name") == "Main Scene")
    box = await entity(ed, sid, "LitBox")
    got = await lit_values(ed, sid, box)
    ed.check(got["stochasticTiling"] is True and close(got["stochasticCellSize"], 0.75) and close(got["stochasticRotation"], 0.5)
             and close(got["stochasticContrast"], 0.45), f"reloaded: {got!r}")

    # a prefab of it: an instance's edit is an override of those fields alone, kept through a reload
    ed.require_tools("create_prefab", "instantiate_prefab", "get_prefab_info")
    await ed.call("create_prefab", scene=sid, entity=box, path="prefabs/LitBox.prefab")
    made = await ed.call("instantiate_prefab", scene=sid, prefab="prefabs/LitBox.prefab", position=[-108, 1.5, 3], name="LitBox B")
    b = made["instance"]["id"]
    ed.check(close((await lit_values(ed, sid, b))["stochasticCellSize"], 0.75), "the instance has the prefab's")
    await ed.call("set_properties", scene=sid, entity=b, values={"mesh": {M + "stochasticContrast": 0.2}})
    info = await ed.call("get_prefab_info", scene=sid, entity=b, usage=False)
    names = set()
    for entry in info["overrides"]:
        names.update(entry.get("properties") or [])
    ed.check(M + "stochasticContrast" in names, f"an override: {sorted(names)}")
    ed.check(M + "stochasticCellSize" not in names, f"only the edited field: {sorted(names)}")
    await ed.call("save_scene", scene=sid)
    await ed.call("close_scene", scene=sid, unsaved="discard")
    await ed.call("open_scene", scene=SCENE_FILE)
    sid = next(s["id"] for s in (await ed.call("list_scenes"))["scenes"] if s.get("name") == "Main Scene")
    got = await lit_values(ed, sid, await entity(ed, sid, "LitBox B"))
    ed.check(close(got["stochasticContrast"], 0.2) and close(got["stochasticCellSize"], 0.75) and got["stochasticTiling"] is True,
             f"the instance after a reload: {got!r}")
    ed.check(close((await lit_values(ed, sid, await entity(ed, sid, "LitBox")))["stochasticContrast"], 0.45), "the source is untouched")


def run_check_json(project: Path) -> dict:
    tmp = Path(tempfile.mkdtemp(prefix="jasper-sto-check-"))
    try:
        copy = tmp / "project"
        shutil.copytree(project, copy, ignore=shutil.ignore_patterns(".git"))
        cmd = harness.editor_command() + ["check", "--project", str(copy), "--shaders", "--format", "json"]
        r = subprocess.run(cmd, cwd=str(copy), capture_output=True, text=True, timeout=600)
        try:
            return json.loads(r.stdout)
        except json.JSONDecodeError:
            raise AssertionError(f"check printed no JSON (exit {r.returncode}): {r.stdout[-2000:]} {r.stderr[-2000:]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


async def test_check_rule(ed: Editor):
    """`check --shaders`: a tilted normal map under Rotation (Lit material, terrain layer, library
    shader) and Stochastic Tiling with no effect"""
    sid = await scene_id(ed)
    tilted = "assets/textures/pebbles_normal_tilted.png"
    clean = run_check_json(ed.project)
    ed.check(not [f for f in clean["findings"] if f.get("code", "").startswith("stochastic-")], f"none on the fixture: {clean['findings']}")

    ground = await entity(ed, sid, "LitGround")
    box = await entity(ed, sid, "LitBox")
    await ed.call("set_properties", scene=sid, entity=ground, values={"mesh": {
        M + "stochasticTiling": True, M + "stochasticRotation": 0.5, M + "normalTexture": tilted}})
    # the same map without Rotation is fine
    await ed.call("set_properties", scene=sid, entity=box, values={"mesh": {
        M + "stochasticTiling": True, M + "stochasticRotation": 0.0, M + "normalTexture": tilted}})
    terrain = await entity(ed, sid, "Ground")
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, "stochastic_tiling": True, "rotation": 1.0,
                                                                         "normal": tilted}])
    quad = await entity(ed, sid, "SimpleLitQuad")
    await ed.call("set_properties", scene=sid, entity=quad, values={"mesh": {
        M + "shaderFeatures": {"STOCHASTIC": True}, M + "shaderParams.stochasticRotation": 0.25,
        M + "shaderTextures": {"u_normalTexture": tilted}}})
    # Lit's switch on a library shader material: no effect
    stripes = await entity(ed, sid, "UnlitStripes")
    await ed.call("set_properties", scene=sid, entity=stripes, values={"mesh": {M + "stochasticTiling": True}})
    await ed.call("save_scene", scene=sid)

    data = run_check_json(ed.project)
    found = [f for f in data["findings"] if f.get("code", "").startswith("stochastic-")]
    tilt = [f for f in found if f["code"] == "stochastic-tilted-normal-map"]
    none = [f for f in found if f["code"] == "stochastic-no-effect"]
    ed.check(all(f["severity"] == "warning" and f["tool"] == "shader" and f["path"] == SCENE_FILE and f.get("suggestion") for f in found),
             f"warnings at the scene, with a fix: {found}")
    ed.check(any("LitGround" in f["message"] and "pebbles_normal_tilted.png" in f["message"] for f in tilt), f"the Lit material: {tilt}")
    ed.check(any("terrain layer 0" in f["message"] for f in tilt), f"the terrain layer: {tilt}")
    ed.check(any("SimpleLitQuad" in f["message"] for f in tilt), f"the library shader material: {tilt}")
    ed.check(not any("LitBox" in f["message"] for f in tilt), f"no warning without Rotation: {tilt}")
    ed.check(any("UnlitStripes" in f["message"] and "library:unlit" in f["message"] for f in none), f"no effect: {none}")
    ed.check(data["errors"] == 0, f"no errors: {data['findings']}")
