"""Materials and lighting (docs/specs/S11-materials-lighting.md): the built-in Lit's extended
lobes through the material `pbr` object and the Catalog paths (partial merges, range errors
naming the field), get_material_variant (defines, maps, the 3-map budget), set_light_unit
(the image stays the same), Lit Quality (new scenes High, saved; old scenes Standard,
unchanged), the direct-light energy compensation and the specular anti-aliasing of High, hex
tiling against repetition, Lua, the presets, and the Khronos sample models: each loads,
draws, and draws differently with its lobes off; with a golden per model in
engine/tests/render/golden/s11/ (JASPER_TEST_UPDATE_GOLDEN=1 writes them on the GL server).
Re-saving every scene of tools/fixtures with this build gives the same bytes."""

from __future__ import annotations

import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

import harness
import pngdiff
from harness import Editor, find_key

CI = True
EDITOR = {"create": True, "name": "Materials PBR"}
# the Khronos sample models' tests (test_khronos_<model>) each make their own scene and use nothing
# the file's other tests leave, nor do those use theirs: the runner may give each an editor of its
# own (run_tests.py --split-seconds, tools/mcp/shard.py); the other tests share one, in order
TESTS_INDEPENDENT = ("each Khronos model test makes its own scene", ["test_khronos_*"])
TIMEOUT = 5400

REPO = harness.REPO
KHRONOS = REPO / "engine" / "tests" / "render" / "assets" / "khronos"
# (JASPER_S11_GOLDEN_DIR: compare with the goldens of another directory)
GOLDEN_DIR = Path(os.environ.get("JASPER_S11_GOLDEN_DIR") or REPO / "engine" / "tests" / "render" / "golden" / "s11")
UPDATE_GOLDEN = "1" in (os.environ.get("JASPER_TEST_UPDATE_GOLDEN"), os.environ.get("JASPER_RENDER_UPDATE_GOLDEN"))
GOLDEN_SIZE = (480, 270)

M = "submeshes[0].material."
STATE: dict = {}


# ------------------------------------------------------------------ images

def png(width: int, height: int, pixel) -> bytes:
    """An RGBA PNG whose pixel(x, y) gives (r, g, b, a)."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixel(x, y))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def luminance(img, x: int, y: int) -> float:
    w = img[0]
    i = (y * w + x) * 4
    rgba = img[2]
    return 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2]


def mean_luminance(img, box) -> float:
    x0, y0, x1, y1 = box
    total = 0.0
    count = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            total += luminance(img, x, y)
            count += 1
    return total / max(count, 1)


def centre_box(img, fraction: float = 0.1):
    w, h = img[0], img[1]
    dx, dy = int(w * fraction), int(h * fraction)
    return (w // 2 - dx, h // 2 - dy, w // 2 + dx, h // 2 + dy)


def differing_fraction(a, b, tolerance: int = 24) -> float:
    ok, fraction, _ = pngdiff.compare(a, b, tolerance, 1.0)
    return fraction


# ------------------------------------------------------------------ editor helpers

async def shot(ed: Editor, frames: int = 30, view: str = "game"):
    if frames:
        await ed.call("wait", frames=frames, timeout_seconds=900)
    r = await ed.call_raw("screenshot", {"view": view, "max_width": 0}, timeout=300)
    ed.check(not r.is_error and r.images, f"no {view} screenshot: {r.text[:300]}")
    return pngdiff.decode(r.images[0][0])


async def make_entity(ed: Editor, name: str, type_: str, **args):
    made = await ed.call("create_entity", name=name, type=type_, **args)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def material_of(ed: Editor, eid, submesh: int = 0) -> dict:
    """a submesh's material as get_entity reads it (the lobes away from their defaults in pbr)"""
    name = f"submeshes[{submesh}].material"
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=[name], detail="values")
    return next(iter(got["components"].values()))[name]


async def entities_with(ed: Editor, component: str) -> list:
    found = await ed.call("find_entities", component=component)
    return [find_key(e, "entity_id", "id") for e in found.get("entities", [])]


async def variant(ed: Editor, eid, submesh: int = 0) -> dict:
    return await ed.call("get_material_variant", entity=eid, submesh=submesh)


async def submesh_count(ed: Editor, eid) -> int:
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=["numSubmeshes"], detail="values")
    return next(iter(got["components"].values())).get("numSubmeshes") or 1


async def ready_all(ed: Editor, eid) -> list:
    """every submesh's variant, once built"""
    return [await ready_variant(ed, eid, k) for k in range(await submesh_count(ed, eid))]


async def all_ready(ed: Editor, entities, seconds: float = 240.0) -> list:
    """every submesh variant of the entities, once built (or failed), within a deadline: the
    variants that failed or are still building"""
    pending = []
    for eid in entities:
        pending += [(eid, k) for k in range(await submesh_count(ed, eid))]
    deadline = time.monotonic() + seconds
    failed = []
    last = {}
    while pending and time.monotonic() < deadline:
        still = []
        for eid, k in pending:
            v = await variant(ed, eid, k)
            if v["failed"]:
                failed.append(v)
            elif not v["ready"]:
                still.append((eid, k))
                last[(eid, k)] = v
        pending = still
        if pending:
            await ed.call("wait", frames=10, timeout_seconds=900)
    return failed + [{"entity": eid, "submesh": k, "ready": False, "reference": last[(eid, k)].get("reference"),
                      "state": last[(eid, k)].get("state")} for eid, k in pending]


async def ready_variant(ed: Editor, eid, submesh: int = 0) -> dict:
    """The submesh's variant once it is built (a new one compiles while the old one draws)."""
    v = {}
    for _ in range(120):
        v = await variant(ed, eid, submesh)
        if v["ready"] or v["failed"]:
            return v
        await ed.call("wait", frames=10, timeout_seconds=900)
    return v


def write_project_png(ed: Editor, rel: str, data: bytes) -> str:
    path = ed.project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return rel


async def new_scene(ed: Editor, name: str):
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}.scene", close_previous=True, unsaved="discard")
    sid = made["scene"]["id"]
    STATE["scene"] = sid
    return sid


async def scene_camera(ed: Editor):
    """the scene's main camera (a new scene draws through the default camera, no GameObject)"""
    cams = await entities_with(ed, "CameraComponent")
    if not cams:
        cams = [await make_entity(ed, "Camera", "camera")]
        await ed.call("set_main_camera", entity=cams[0])
    return cams[0]


async def camera_at(ed: Editor, position, target):
    cam = await scene_camera(ed)
    await ed.call("set_transform", entity=cam, position=list(position))
    await ed.call("set_properties", entity=cam, values={"camera": {"useTarget": True, "target": list(target)}})


async def sun(ed: Editor):
    lights = await entities_with(ed, "LightComponent")
    ed.check(lights, "the scene has a sun")
    return lights[0]


async def without_sky(ed: Editor):
    for sky in await entities_with(ed, "SkyboxComponent"):
        await ed.call("delete_entities", entity=sky)


# ------------------------------------------------------------------ values and tools

async def test_new_scene_is_high_and_saved(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "get_material_variant", "set_light_unit",
                     "create_material", "update_material", "list_material_presets", "apply_material_preset",
                     "get_scene_settings", "set_scene_settings", "screenshot", "eval_lua", "play")
    await new_scene(ed, "Materials")

    s = (await ed.call("get_scene_settings", names=["lit_quality", "physical_light_ev100"]))["settings"]
    ed.check_eq(s["lit_quality"], "high", "a new scene shades at High")
    ed.check_eq(s["physical_light_ev100"], 15, "EV100 default")
    await ed.call("save_scene")
    text = (ed.project / "scenes/Materials.scene").read_text()
    ed.check("litQuality: high" in text, "High is written")
    await ed.check_error("set_scene_settings", settings={"physical_light_ev100": 40}, contains="-6 to 24")
    await ed.check_error("set_scene_settings", settings={"lit_quality": "ultra"}, contains="standard")


async def test_pbr_values_merge_and_validate(ed: Editor):
    eid = await make_entity(ed, "Coated", "sphere")
    STATE["sphere"] = eid
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "pbr.clearcoat.enabled": True, M + "pbr.clearcoat.factor": 1.0}})
    material = await material_of(ed, eid)
    ed.check(material and material.get("pbr", {}).get("clearcoat", {}).get("enabled") is True, f"read back: {material!r}")
    ed.check_eq(material["pbr"]["clearcoat"]["factor"], 1, "factor")
    ed.check_eq(material["emissiveStrength"], 1, "emissiveStrength read")

    # a whole material with a partial pbr object merges into the lobes it has
    await ed.call("set_properties", entity=eid, values={"mesh": {"submeshes[0].material": {"pbr": {"clearcoat": {"roughness": 0.25}, "sheen": {"enabled": True}}}}})
    material = await material_of(ed, eid)
    ed.check_eq(material["pbr"]["clearcoat"]["factor"], 1, "kept by the merge")
    ed.check_eq(material["pbr"]["clearcoat"]["roughness"], 0.25, "merged")
    ed.check(material["pbr"]["sheen"]["enabled"], "sheen on")

    # a bad range is an error naming the field, and changes nothing
    await ed.check_error("set_properties", entity=eid, values={"mesh": {"submeshes[0].material": {"pbr": {"clearcoat": {"roughness": 2}}}}},
                         contains="pbr.clearcoat.roughness")
    await ed.check_error("set_properties", entity=eid, values={"mesh": {"submeshes[0].material": {"pbr": {"lacquer": {}}}}}, contains="unknown lobe")

    # a .material file: create with lobes, update merges, null clears
    await ed.call("create_material", path="materials/paint.material", values={"pbr": {"clearcoat": {"enabled": True, "factor": 0.8}}})
    await ed.call("update_material", path="materials/paint.material", values={"pbr": {"specular": {"enabled": True, "ior": 1.45}}, "emissiveStrength": 3})
    text = (ed.project / "materials/paint.material").read_text()
    ed.check("clearcoat:" in text and "ior: 1.45" in text and "emissiveStrength: 3" in text, f"saved: {text}")
    await ed.check_error("update_material", path="materials/paint.material", values={"pbr": {"specular": {"ior": 0.5}}}, contains="pbr.specular.ior")
    await ed.call("update_material", path="materials/paint.material", values={"pbr": None})
    ed.check("pbr:" not in (ed.project / "materials/paint.material").read_text(), "pbr null clears the lobes")
    await ed.call("set_properties", entity=eid, values={"mesh": {"submeshes[0].material": {"pbr": None}}})


async def test_get_material_variant_reports_maps(ed: Editor):
    eid = STATE["sphere"]
    v = await ready_variant(ed, eid)
    ed.check_eq(v["reference"], "builtin:lit#USE_LIT_QUALITY", "no lobe: only the High Lit Quality")
    for name in ("cc", "spec", "trans", "sheen"):
        write_project_png(ed, f"textures/{name}.png", png(4, 4, lambda x, y: (200, 120, 60, 255)))
    await ed.call("set_properties", entity=eid, values={"mesh": {
        M + "pbr.clearcoat.enabled": True, M + "pbr.clearcoat.map": "textures/cc.png",
        M + "pbr.specular.enabled": True, M + "pbr.specular.map": "textures/spec.png",
        M + "pbr.transmission.enabled": True, M + "pbr.transmission.map": "textures/trans.png",
        M + "pbr.sheen.enabled": True, M + "pbr.sheen.map": "textures/sheen.png"}})
    v = await ready_variant(ed, eid)
    ed.check(not v["failed"], f"the variant builds: {v!r}")
    for define in ("PBR_CLEARCOAT", "PBR_SPECULAR", "PBR_TRANSMISSION", "PBR_SHEEN", "PBR_CLEARCOAT_MAP", "PBR_SPECULAR_MAP", "PBR_TRANSMISSION_MAP"):
        ed.check(define in v["defines"], f"{define} in {v['defines']}")
    ed.check("PBR_SHEEN_MAP" not in v["defines"], "the fourth map is over the budget")
    ed.check_eq(v["extensionMaps"]["used"], ["clearcoat", "specular", "transmission"], "kept by priority")
    ed.check_eq(v["extensionMaps"]["dropped"], ["sheen"], "dropped")
    ed.check_eq(v["extensionMaps"]["max"], 3, "budget")
    ed.check(v["basename"].startswith("lit_"), f"basename {v['basename']}")
    await ed.call("set_properties", entity=eid, values={"mesh": {"submeshes[0].material": {"pbr": None}}})


async def test_lobe_changes_the_image(ed: Editor):
    eid = STATE["sphere"]
    await camera_at(ed, (0, 0.5, 3.0), (0, 0, 0))
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "baseColorFactor": [0.6, 0.05, 0.05, 1], M + "roughnessFactor": 0.9, M + "metallicFactor": 0}})
    plain = await shot(ed)
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "pbr.clearcoat.enabled": True, M + "pbr.clearcoat.factor": 1.0, M + "pbr.clearcoat.roughness": 0.02}})
    await ready_variant(ed, eid)
    coated = await shot(ed)
    ed.check(differing_fraction(plain, coated) > 0.005, "the clear coat shows")
    await ed.call("set_properties", entity=eid, values={"mesh": {M + "pbr.clearcoat.enabled": False}})
    await ready_variant(ed, eid)
    again = await shot(ed)
    ed.check(differing_fraction(plain, again, 8) < 0.002, "off again draws as before")


async def test_light_units_preserve(ed: Editor):
    eid = STATE["sphere"]
    point = await make_entity(ed, "Lamp", "point_light", position=[1.0, 1.0, 1.5])
    await ed.call("set_properties", entity=point, values={"light": {"intensity": 3.0, "range": 10}})
    before = await shot(ed)
    sun_id = await sun(ed)
    out = await ed.call("set_light_unit", entity=sun_id, unit="lux")
    ed.check(out["lights"][0]["unit"] == "Lux", f"sun in lux: {out!r}")
    await ed.call("set_light_unit", entity=point, unit="candela")
    ed.check(pngdiff.compare(before, await shot(ed))[0], "lux and candela draw the same")
    await ed.call("set_light_unit", entity=point, unit="lumen")
    after = await shot(ed)
    ok, fraction, _ = pngdiff.compare(before, after)
    ed.check(ok, f"lumen draws the same ({fraction * 100:.2f}% differ)")
    got = find_key(await ed.call("get_entity", entity=point), "intensityUnit")
    ed.check_eq(got, "lumen", "the unit is saved on the light")
    # a unit the type cannot take is its neighbour
    out = await ed.call("set_light_unit", entity=sun_id, unit="candela")
    ed.check_eq(out["lights"][0]["unit"], "Lux", "a directional light takes lux")
    # the EV100 moves physical lights only
    await ed.call("set_scene_settings", settings={"physical_light_ev100": 13})
    darker = await shot(ed)
    ed.check(mean_luminance(darker, centre_box(darker)) > mean_luminance(after, centre_box(after)), "a lower EV100 draws physical lights brighter")
    await ed.call("set_scene_settings", settings={"physical_light_ev100": 15})
    await ed.call("set_light_unit", entities=[sun_id, point], unit="engine")
    await ed.call("delete_entities", entity=point)


async def test_furnace(ed: Editor):
    """The direct lights' multiscatter compensation of High: a rough white metal lit by the sun
    is brighter at High than at Standard (single scattering loses energy there), a smooth one
    hardly changes. The environment's split sum already compensates (Standard and High alike),
    so the scene has no sky and no ambient: the direct light alone."""
    await new_scene(ed, "Energy")
    await without_sky(ed)
    await ed.call("set_scene_settings", settings={"global_illumination_intensity": 0})
    await camera_at(ed, (0, 0, 3.0), (0, 0, 0))
    sun_id = await sun(ed)
    await ed.call("set_properties", entity=sun_id, values={"light": {"direction": [0, 0, -1], "intensity": 2.0, "shadows": False}})
    ball = await make_entity(ed, "Metal", "sphere")
    ratios = {}
    for roughness in (1.0, 0.2):
        await ed.call("set_properties", entity=ball, values={"mesh": {M + "baseColorFactor": [1, 1, 1, 1], M + "metallicFactor": 1, M + "roughnessFactor": roughness}})
        lit = {}
        for quality in ("standard", "high"):
            await ed.call("set_scene_settings", settings={"lit_quality": quality})
            await ready_variant(ed, ball)
            img = await shot(ed)
            lit[quality] = mean_luminance(img, centre_box(img, 0.06))
        ratios[roughness] = lit["high"] / max(lit["standard"], 1e-3)
    ed.check(ratios[1.0] >= 1.15, f"a rough metal gets its energy back at High ({ratios[1.0]:.3f}x)")
    ed.check(abs(ratios[0.2] - 1.0) <= 0.08, f"a smooth one hardly changes ({ratios[0.2]:.3f}x)")
    await ed.call("set_scene_settings", settings={"lit_quality": "high"})


async def test_spec_aa_shimmer(ed: Editor):
    """Specular AA: small, very smooth metal spheres under the sun on black, the camera panning
    by a fraction of a pixel per frame. A sphere's normal turns by degrees from one pixel to the
    next while the GGX lobe is a fraction of one, so at Standard how much of its highlight a
    sphere shows depends on where the pixels fall: its light flickers as it moves. High widens
    the lobe by the normal's screen-space variance. The flicker, each sphere's light (the sum
    over a box around it) from frame to frame relative to its mean, drops by at least 40%."""
    await new_scene(ed, "SpecAA")
    await without_sky(ed)
    await ed.call("set_scene_settings", settings={"global_illumination_intensity": 0, "background_color": [0, 0, 0, 1]})
    spacing, distance, yfov = 0.6, 7.0, 0.75
    centres = [((col - 2.5) * spacing, (row - 1.5) * spacing) for row in range(4) for col in range(6)]
    balls = [await make_entity(ed, f"Ball{i}", "sphere", position=[x, y, 0], scale=[0.2, 0.2, 0.2])
             for i, (x, y) in enumerate(centres)]
    await ed.call("set_properties", entities=balls, values={"mesh": {
        M + "roughnessFactor": 0.04, M + "metallicFactor": 1.0, M + "baseColorFactor": [0.9, 0.9, 0.9, 1]}})
    cam = await scene_camera(ed)
    await ed.call("set_properties", entity=cam, values={"camera": {"yfov": yfov}})
    sun_id = await sun(ed)
    await ed.call("set_properties", entity=sun_id, values={"light": {"direction": [-0.3, -0.4, -1], "intensity": 1.0, "shadows": False}})

    flicker = {}
    for quality in ("standard", "high"):
        await ed.call("set_scene_settings", settings={"lit_quality": quality})
        await ready_variant(ed, balls[0])
        energy = [[] for _ in centres]
        for i in range(30):
            pan = i * 0.0021
            await camera_at(ed, (pan, 0, distance), (pan, 0, 0))
            img = await shot(ed, frames=2)
            w, h = img[0], img[1]
            scale = (h / 2) / (distance * math.tan(yfov / 2))   # pixels per unit at the spheres
            half = int(spacing * 0.4 * scale)
            for k, (x, y) in enumerate(centres):
                cx, cy = int(w / 2 + (x - pan) * scale), int(h / 2 - y * scale)
                energy[k].append(sum(luminance(img, px, py) for py in range(cy - half, cy + half, 2)
                                     for px in range(cx - half, cx + half, 2)))
        values = []
        for e in energy:
            mean = sum(e) / len(e)
            if mean > 1.0:
                values.append(math.sqrt(sum((v - mean) ** 2 for v in e) / len(e)) / mean)
        ed.check(values, f"{quality}: the spheres show a highlight")
        flicker[quality] = sum(values) / len(values)
    ed.check(flicker["high"] <= 0.6 * flicker["standard"],
             f"High flickers at least 40% less: {flicker['high']:.4f} vs Standard {flicker['standard']:.4f}")
    await ed.call("set_scene_settings", settings={"lit_quality": "high"})


def autocorrelation(img, period: int, rows: range) -> float:
    """Mean normalized autocorrelation of luminance along x at a lag (pixels)."""
    w = img[0]
    total = 0.0
    count = 0
    for y in rows:
        line = [luminance(img, x, y) for x in range(w)]
        mean = sum(line) / w
        centred = [v - mean for v in line]
        var = sum(v * v for v in centred) / w
        if var < 1e-6:
            continue
        cov = sum(centred[x] * centred[x + period] for x in range(w - period)) / (w - period)
        total += cov / var
        count += 1
    return total / max(count, 1)


async def test_hex_repetition(ed: Editor):
    """Hex tiling: a plane with a texture repeated 64 times, seen from above with an orthographic
    view; the luminance autocorrelation at the texture's period falls by at least half."""
    await new_scene(ed, "HexFloor")
    await without_sky(ed)

    def tile(x, y):
        # a blob and a bar: a pattern whose repetition is unmistakable
        d = (x - 20) ** 2 + (y - 22) ** 2
        v = 230 if d < 90 else (40 if 40 <= x < 52 else 120 + (x * 7 + y * 3) % 30)
        return (v, v, v, 255)

    write_project_png(ed, "textures/tile.png", png(64, 64, tile))
    floor = await make_entity(ed, "Floor", "plane", scale=[8, 1, 8])
    await ed.call("set_properties", entity=floor, values={"mesh": {M + "baseColorTexture": "textures/tile.png", M + "roughnessFactor": 1.0,
                                                                     M + "metallicFactor": 0.0, "submeshes[0].textureRect": [0, 0, 64, 64]}})
    cam = await scene_camera(ed)
    await ed.call("set_properties", entity=cam, values={"camera": {"type": "orthographic"}})
    await camera_at(ed, (0, 10, 0.001), (0, 0, 0))
    plain = await shot(ed)
    w = plain[0]
    # the period: 64 repeats over the plane's width on screen (found from the image's own
    # autocorrelation peak near the expected lag)
    lags = range(4, max(8, w // 8))
    rows = range(plain[1] // 2 - 20, plain[1] // 2 + 20, 4)
    period = max(lags, key=lambda lag: autocorrelation(plain, lag, rows))
    before = autocorrelation(plain, period, rows)
    await ed.call("set_properties", entity=floor, values={"mesh": {M + "pbr.hex.enabled": True}})
    await ready_variant(ed, floor)
    hexed = await shot(ed)
    after = autocorrelation(hexed, period, rows)
    ed.check(after <= 0.5 * before, f"the repetition peak at {period}px falls by half: {before:.3f} -> {after:.3f}")


async def test_lua(ed: Editor):
    await new_scene(ed, "LuaLobes")
    await make_entity(ed, "Ball", "sphere")
    await camera_at(ed, (0, 0.5, 3.0), (0, 0, 0))
    sun_name = (await ed.call("find_entities", component="LightComponent"))["entities"][0]["name"]
    await ed.call("set_scene_settings", settings={"lit_quality": "standard"})
    await ed.call("save_scene")
    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        before = await shot(ed, frames=5)
        values = (await ed.call("eval_lua", code=f"""
            local mesh = object('Ball', 'Mesh')
            local m = mesh:getMaterial(0)
            m.baseColorFactor = Vector4(0.6, 0.05, 0.05, 1)
            m.roughnessFactor = 0.9
            m.clearcoatEnabled = true
            m.clearcoatFactor = 1
            m.clearcoatRoughness = 0.02
            mesh:setMaterial(0, m)
            local light = object('{sun_name}', 'Light')
            light.intensityUnit = LightUnit.LUX
            scene.litQuality = LitQuality.HIGH
            local again = mesh:getMaterial(0)
            return again.clearcoatEnabled, again.clearcoatFactor, light.intensityUnit == LightUnit.LUX, scene.litQuality == LitQuality.HIGH
        """))["values"]
        ed.check_eq(values, [True, 1, True, True], "set from Lua")
        runtime = await ed.call("find_entities", name="Ball", runtime=True)
        rid = find_key(runtime.get("entities", [{}])[0], "entity_id", "id")
        v = {}
        for _ in range(60):
            v = await variant(ed, rid)
            if "PBR_CLEARCOAT" in v["defines"] and v["ready"]:
                break
            await ed.call("wait", frames=10, timeout_seconds=900)
        ed.check("PBR_CLEARCOAT" in v["defines"] and "USE_LIT_QUALITY" in v["defines"], f"get_material_variant reports them: {v!r}")
        ed.check_eq(v["litQuality"], "high", "the scene's quality")
        after = await shot(ed, frames=5)
        ed.check(differing_fraction(before, after) > 0.01, "the screenshot changes")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_presets(ed: Editor):
    presets = {p["id"] for p in (await ed.call("list_material_presets"))["presets"]}
    for preset in ("car_paint", "velvet", "brushed_metal", "soap_bubble", "clear_glass", "tiled_floor"):
        ed.check(preset in presets, f"{preset} listed")
    await new_scene(ed, "Presets")
    car = await make_entity(ed, "Car", "sphere")
    await ed.call("apply_material_preset", entity=car, preset="car_paint")
    material = await material_of(ed, car)
    ed.check(material["pbr"]["clearcoat"]["enabled"], f"Car Paint applies a clear coat: {material!r}")
    glass = await make_entity(ed, "Glass", "sphere")
    await ed.call("apply_material_preset", entity=glass, preset="clear_glass")
    v = await ready_variant(ed, glass)
    ed.check("PBR_TRANSMISSION" in v["defines"] and "PBR_VOLUME" in v["defines"], f"glass: {v!r}")


# ------------------------------------------------------------------ the Khronos samples

MODELS = {
    # model: the lobe(s) the lobe-off twin turns off (the camera frames the model's bounds from +Z,
    # as the Khronos screenshots look at it)
    "ClearCoatTest": ["clearcoat"],
    "SheenTestGrid": ["sheen"],
    "AnisotropyStrengthTest": ["anisotropy"],
    "AnisotropyRotationTest": ["anisotropy"],
    "IridescenceDielectricSpheres": ["iridescence"],
    "IridescenceMetallicSpheres": ["iridescence"],
    "TransmissionTest": ["transmission"],
    "TransmissionRoughnessTest": ["transmission"],
    "AttenuationTest": ["transmission", "volume"],
    "SpecularTest": ["specular"],
    "IORTestGrid": ["specular", "transmission"],
    "EmissiveStrengthTest": [],
    "MetalRoughSpheresNoTextures": [],
}


def gltf_json(path: Path) -> dict:
    data = path.read_bytes()
    if path.suffix == ".glb":
        length = struct.unpack_from("<I", data, 12)[0]
        return json.loads(data[20:20 + length])
    return json.loads(data)


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def node_matrix(node: dict):
    if "matrix" in node:
        m = node["matrix"]  # column major
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    tx, ty, tz = node.get("translation", [0, 0, 0])
    qx, qy, qz, qw = node.get("rotation", [0, 0, 0, 1])
    sx, sy, sz = node.get("scale", [1, 1, 1])
    r = [[1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
         [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
         [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)]]
    return [[r[0][0] * sx, r[0][1] * sy, r[0][2] * sz, tx], [r[1][0] * sx, r[1][1] * sy, r[1][2] * sz, ty],
            [r[2][0] * sx, r[2][1] * sy, r[2][2] * sz, tz], [0, 0, 0, 1]]


def model_bounds(path: Path):
    """the world bounds of a glTF's default scene, from its POSITION accessors' min and max"""
    doc = gltf_json(path)
    lo, hi = [1e30] * 3, [-1e30] * 3
    identity = [[1 if i == j else 0 for j in range(4)] for i in range(4)]

    def visit(index, parent):
        node = doc["nodes"][index]
        world = mat_mul(parent, node_matrix(node))
        if "mesh" in node:
            for prim in doc["meshes"][node["mesh"]]["primitives"]:
                acc = doc["accessors"][prim["attributes"]["POSITION"]]
                mn, mx = acc["min"], acc["max"]
                for cx in (mn[0], mx[0]):
                    for cy in (mn[1], mx[1]):
                        for cz in (mn[2], mx[2]):
                            p = [sum(world[r][c] * v for c, v in enumerate((cx, cy, cz, 1))) for r in range(3)]
                            for k in range(3):
                                lo[k] = min(lo[k], p[k])
                                hi[k] = max(hi[k], p[k])
        for child in node.get("children", []):
            visit(child, world)

    for root in doc["scenes"][doc.get("scene", 0)]["nodes"]:
        visit(root, identity)
    return lo, hi


def framing(path: Path, yfov: float = 0.75, aspect: float = 16 / 9):
    """a camera on +Z that frames the model (5% margin): position, target"""
    lo, hi = model_bounds(path)
    centre = [(a + b) / 2 for a, b in zip(lo, hi)]
    half_w, half_h, half_d = [(b - a) / 2 for a, b in zip(lo, hi)]
    t = math.tan(yfov / 2)
    distance = max(half_h / t, half_w / (t * aspect)) * 1.05 + half_d
    return (centre[0], centre[1], centre[2] + distance), tuple(centre)


def model_file(model: str) -> Path:
    for sub in ("glTF-Binary", "glTF"):
        folder = KHRONOS / model / sub
        if folder.is_dir():
            for f in folder.iterdir():
                if f.suffix in (".glb", ".gltf"):
                    return f
    raise AssertionError(f"no glTF file for {model}")


async def lobes_off(ed: Editor, entity_ids, lobes) -> None:
    for eid in entity_ids:
        count = await submesh_count(ed, eid)
        values = {}
        for s in range(count):
            for lobe in lobes:
                values[f"submeshes[{s}].material.pbr.{lobe}.enabled"] = False
            values[f"submeshes[{s}].material.emissiveStrength"] = 1.0
        if values:
            await ed.call("set_properties", entity=eid, values={"mesh": values})


async def studio_sky(ed: Editor):
    """a 6-face studio environment on the scene's sky (a sky with a cubemap gives image based
    lighting): grey with a softbox on each face"""
    faces = []
    for name in ("px", "nx", "py", "ny", "pz", "nz"):
        def pixel(x, y, name=name):
            # every face alike (whichever way the faces are ordered): grey with a softbox
            v = 255 if 18 <= x < 46 and 18 <= y < 46 else 110
            return (v, v, v, 255)
        faces.append(write_project_png(ed, f"textures/studio_{name}.png", png(64, 64, pixel)))
    for sky in await entities_with(ed, "SkyboxComponent"):
        await ed.call("set_properties", entity=sky, values={"skybox": {"texture": {"cube": faces}}})


def _make_khronos_test(model: str, lobes: list[str]) -> None:
    """test_khronos_<model>: one Khronos sample model, a test of its own (TESTS_INDEPENDENT: each
    makes its own scene, so the runner may give each an editor of its own)."""

    async def khronos_model(ed: Editor):
        only = [m for m in os.environ.get("JASPER_S11_MODELS", "").split(",") if m]
        if only and model not in only:
            harness.skip(f"JASPER_S11_MODELS={','.join(only)} leaves {model} out")
        print(f"     [khronos] {model}", flush=True)
        position, target = framing(model_file(model))
        source = model_file(model)
        dest = ed.project / "models" / model
        shutil.copytree(source.parent, dest, dirs_exist_ok=True)
        await new_scene(ed, f"Khronos{model}")
        await ed.console_since("last", limit=1000)  # (moves the cursor past what came before)
        await studio_sky(ed)
        # the sun from over the camera's shoulder: each sphere and swatch shows its highlight
        await ed.call("set_properties", entity=await sun(ed), values={"light": {"direction": [-0.35, -0.45, -1], "intensity": 3.0}})
        made = await ed.call("create_entity", name=model, type="model", model=f"models/{model}/{source.name}")
        root = find_key(made, "entity_id", "id")
        await camera_at(ed, position, target)
        distance = math.dist(position, target)
        await ed.call("set_properties", entity=await scene_camera(ed),
                      values={"camera": {"nearClip": distance * 0.01, "farClip": distance * 20}})
        await ed.call("wait", frames=60, timeout_seconds=900)
        # the entities with submeshes, once the model has loaded (in the background)
        meshes = []
        for eid in await entities_with(ed, "MeshComponent"):
            r = await ed.call_raw("get_material_variant", {"entity": eid, "submesh": 0})
            if not r.is_error:
                meshes.append(eid)
        ed.check(meshes, f"{model} has meshes")
        # every material's variant built
        problems = await all_ready(ed, meshes)
        failed = [p for p in problems if p.get("failed")]
        ed.check(not failed, f"{model}: variants failed: {failed[:3]!r}")
        if problems:
            print(f"     [khronos] {model}: still building after the deadline: {problems[:3]!r}", flush=True)
        img = await shot(ed, frames=0)
        ed.check(not pngdiff.flat(img), f"{model} draws something")
        errors = [l for l in await ed.console_since("last", limit=1000) if l.get("level") == "error"]
        ed.check(not errors, f"{model}: errors: {errors[:3]}")

        golden = GOLDEN_DIR / f"{model}.png"
        small = pngdiff.downscale(img, *GOLDEN_SIZE)
        if UPDATE_GOLDEN:
            # (JASPER_S11_GOLDEN_OUT: write them there instead, e.g. outside the checkout)
            out = Path(os.environ.get("JASPER_S11_GOLDEN_OUT") or GOLDEN_DIR)
            out.mkdir(parents=True, exist_ok=True)
            (out / golden.name).write_bytes(pngdiff.encode(small))
        else:
            ed.check(golden.is_file(), f"no golden for {model} (JASPER_TEST_UPDATE_GOLDEN=1 writes it)")
            ok, fraction, diff = pngdiff.compare(small, pngdiff.load(golden))
            if not ok:
                (ed.scratch.root / f"{model}.actual.png").write_bytes(pngdiff.encode(small))
                (ed.scratch.root / f"{model}.diff.png").write_bytes(diff or b"")
                ed.keep = True
            ed.check(ok, f"{model}: {fraction * 100:.2f}% of the pixels differ from {golden}")

        # the lobe-off twin differs from it by more than 2% of the pixels
        off_lobes = lobes or []
        if model == "EmissiveStrengthTest":
            off_lobes = []
        if off_lobes or model == "EmissiveStrengthTest":
            await lobes_off(ed, meshes, off_lobes)
            await all_ready(ed, meshes)
            twin = await shot(ed, frames=0)
            # (a lobe can be subtle under the sky: a clear coat adds 4% reflection face on, 6 levels
            # of 255; the render is deterministic, so any change beyond rounding counts)
            fraction = differing_fraction(img, twin, 4)
            # (EmissiveStrengthTest's glowing cubes are 2% of its frame: 1% of it changes)
            ed.check(fraction > (0.01 if model == "EmissiveStrengthTest" else 0.02), f"{model}: its lobe(s) {off_lobes or ['emissive strength']} change "
                                      f"{fraction * 100:.2f}% of the pixels (more than 2%)")

    khronos_model.__name__ = khronos_model.__qualname__ = f"test_khronos_{model}"
    khronos_model.__doc__ = f"the Khronos sample {model}: its golden, and its lobe-off twin"
    globals()[khronos_model.__name__] = khronos_model


for _model, _lobes in MODELS.items():
    _make_khronos_test(_model, _lobes)


# ------------------------------------------------------------------ old files

def normalized_scene(data: bytes) -> list:
    """a scene file's lines without the format version line (S03 writes it on every save), its
    shaderKeys entries sorted (ordered by custom shader ids, which each session assigns anew)"""
    lines = data.decode("utf-8").split("\n")
    out, keys, entry, in_keys = [], [], [], False
    for line in lines:
        if line.startswith("formatVersion:"):
            continue
        top = line and not line.startswith(" ") and not line.startswith("-")
        if top:
            if in_keys:
                if entry:
                    keys.append("\n".join(entry))
                out.append("shaderKeys: " + repr(sorted(keys)))
                keys, entry = [], []
            in_keys = line.startswith("shaderKeys:")
            if in_keys:
                continue
        if in_keys:
            if line.startswith("  - ") and entry:
                keys.append("\n".join(entry))
                entry = []
            entry.append(line)
            continue
        out.append(line)
    return out


S11_KEYS = ("litQuality:", "physicalLightEV100:", "pbr:", "emissiveStrength:", "intensityUnit:",
            "useColorTemperature:", "colorTemperature:", "hexTiling:", "hexRotation:", "mobilePbrTier:")


def s11_keys(text: str) -> list:
    return sorted({k for line in text.split("\n") for k in S11_KEYS if line.strip().startswith(k)})


def test_legacy_resave_identical_sync():
    """Every scene of tools/fixtures, loaded and saved by this build: no key of the lobes, units,
    hex tiling or Lit Quality appears in a file that had none, and a second save writes the same
    text (shaderKeys as a set). A fixture saved by an older editor is first normalised by any save
    of today's build (keys other streams added, maxValues of what headless loads): that is not
    S11's, and Compat.CorpusRoundTrip (editor tests) checks those writes."""
    fixtures = REPO / "tools" / "fixtures"
    changed = []
    for project in sorted(p for p in fixtures.iterdir() if (p / "project.yaml").is_file()):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / project.name
            shutil.copytree(project, copy)
            original = {p.relative_to(copy): p.read_bytes() for p in copy.rglob("*.scene")}
            saves = []
            for _ in range(2):
                r = subprocess.run(harness.editor_command() + ["scene-dump", "--project", str(copy), "--save"],
                                   capture_output=True, text=True, timeout=600)
                assert r.returncode == 0, f"{project.name}: scene-dump failed: {r.stderr[-2000:]}"
                saves.append({rel: (copy / rel).read_bytes() for rel in original})
            for rel, data in original.items():
                before = s11_keys(data.decode("utf-8"))
                added = [k for k in s11_keys(saves[0][rel].decode("utf-8")) if k not in before]
                if added:
                    changed.append(f"{project.name}/{rel}: S11 keys written: {added}")
                a, b = normalized_scene(saves[1][rel]), normalized_scene(saves[0][rel])
                if a != b:
                    first = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                    changed.append(f"{project.name}/{rel}: the second save differs at line {first}: "
                                   f"{a[first] if first < len(a) else '<end>'!r} / {b[first] if first < len(b) else '<end>'!r}")
    assert not changed, f"re-saved scenes: {changed}"


async def test_legacy_resave_identical(ed: Editor):
    test_legacy_resave_identical_sync()
