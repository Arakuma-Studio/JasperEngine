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

import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import harness
import pngdiff
from harness import Editor, find_key

CI = True
EDITOR = {"create": True, "name": "Materials PBR"}
TIMEOUT = 2400

REPO = harness.REPO
KHRONOS = REPO / "engine" / "tests" / "render" / "assets" / "khronos"
GOLDEN_DIR = REPO / "engine" / "tests" / "render" / "golden" / "s11"
UPDATE_GOLDEN = os.environ.get("JASPER_TEST_UPDATE_GOLDEN") == "1"
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
    await ed.call("wait", frames=frames)
    r = await ed.call_raw("screenshot", {"view": view, "max_width": 0}, timeout=300)
    ed.check(not r.is_error and r.images, f"no {view} screenshot: {r.text[:300]}")
    return pngdiff.decode(r.images[0][0])


async def make_entity(ed: Editor, name: str, type_: str, **args):
    made = await ed.call("create_entity", name=name, type=type_, **args)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def entities_with(ed: Editor, component: str) -> list:
    found = await ed.call("find_entities", component=component)
    return [find_key(e, "entity_id", "id") for e in found.get("entities", [])]


async def variant(ed: Editor, eid, submesh: int = 0) -> dict:
    return await ed.call("get_material_variant", entity=eid, submesh=submesh)


async def ready_variant(ed: Editor, eid, submesh: int = 0) -> dict:
    """The submesh's variant once it is built (a new one compiles while the old one draws)."""
    v = {}
    for _ in range(120):
        v = await variant(ed, eid, submesh)
        if v["ready"] or v["failed"]:
            return v
        await ed.call("wait", frames=10)
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


async def camera_at(ed: Editor, position, target):
    cams = await entities_with(ed, "CameraComponent")
    ed.check(cams, "the scene has a camera")
    await ed.call("set_transform", entity=cams[0], position=list(position))
    await ed.call("set_properties", entity=cams[0], values={"camera": {"target": list(target)}})


async def sun(ed: Editor):
    lights = await entities_with(ed, "LightComponent")
    ed.check(lights, "the scene has a sun")
    return lights[0]


async def without_sky(ed: Editor):
    for sky in await entities_with(ed, "SkyboxComponent"):
        await ed.call("delete_entities", entity=sky)


# ------------------------------------------------------------------ values and tools

async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "get_material_variant", "set_light_unit",
                     "create_material", "update_material", "list_material_presets", "apply_material_preset",
                     "get_scene_settings", "set_scene_settings", "screenshot", "eval_lua", "play")
    await new_scene(ed, "Materials")


async def test_new_scene_is_high_and_saved(ed: Editor):
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
    got = await ed.call("get_entity", entity=eid)
    material = find_key(got, "material")
    ed.check(material and material.get("pbr", {}).get("clearcoat", {}).get("enabled") is True, f"read back: {material!r}")
    ed.check_eq(material["pbr"]["clearcoat"]["factor"], 1, "factor")
    ed.check_eq(material["emissiveStrength"], 1, "emissiveStrength read")

    # a whole material with a partial pbr object merges into the lobes it has
    await ed.call("set_properties", entity=eid, values={"mesh": {"submeshes[0].material": {"pbr": {"clearcoat": {"roughness": 0.25}, "sheen": {"enabled": True}}}}})
    material = find_key(await ed.call("get_entity", entity=eid), "material")
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


async def test_lit_quality_energy(ed: Editor):
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
    """Specular AA: a high-frequency normal map under a sharp highlight, the camera dollying
    slowly; the frame-to-frame luminance change of the highlight's pixels is at least 40% lower
    at High than at Standard."""
    await new_scene(ed, "SpecAA")
    await without_sky(ed)
    rng = random.Random(7)
    noise = [[(rng.randint(0, 255), rng.randint(0, 255)) for _ in range(256)] for _ in range(256)]

    def normal(x, y):
        nx, ny = noise[y][x]
        return (64 + nx // 2, 64 + ny // 2, 255, 255)

    write_project_png(ed, "textures/noise_normal.png", png(256, 256, normal))
    plane = await make_entity(ed, "Bumpy", "plane", scale=[4, 1, 4])
    await ed.call("set_properties", entity=plane, values={"mesh": {
        M + "normalTexture": "textures/noise_normal.png", M + "roughnessFactor": 0.15, M + "metallicFactor": 1.0,
        M + "baseColorFactor": [0.9, 0.9, 0.9, 1]}})
    sun_id = await sun(ed)
    await ed.call("set_properties", entity=sun_id, values={"light": {"direction": [0, -1, -0.6], "intensity": 3.0, "shadows": False}})

    shimmer = {}
    for quality in ("standard", "high"):
        await ed.call("set_scene_settings", settings={"lit_quality": quality})
        await ready_variant(ed, plane)
        frames = []
        for i in range(30):
            await camera_at(ed, (0, 4.0 - i * 0.01, 4.0 - i * 0.01), (0, 0, 0))
            frames.append(await shot(ed, frames=2))
        w, h = frames[0][0], frames[0][1]
        # the highlight: the brightest tenth of the first frame's pixels
        samples = [(x, y) for y in range(0, h, 3) for x in range(0, w, 3)]
        first = sorted(samples, key=lambda p: -luminance(frames[0], *p))[:max(len(samples) // 10, 1)]
        change = 0.0
        for a, b in zip(frames, frames[1:]):
            change += sum((luminance(a, x, y) - luminance(b, x, y)) ** 2 for x, y in first) / len(first)
        shimmer[quality] = change / (len(frames) - 1)
    ed.check(shimmer["high"] <= 0.6 * shimmer["standard"],
             f"High shimmers at least 40% less: {shimmer['high']:.1f} vs Standard {shimmer['standard']:.1f}")
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
    cams = await entities_with(ed, "CameraComponent")
    await ed.call("set_properties", entity=cams[0], values={"camera": {"type": "orthographic"}})
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
            local mesh = object('Ball')
            local m = mesh:getMaterial()
            m.baseColorFactor = Vector4(0.6, 0.05, 0.05, 1)
            m.roughnessFactor = 0.9
            m.clearcoatEnabled = true
            m.clearcoatFactor = 1
            m.clearcoatRoughness = 0.02
            mesh:setMaterial(m)
            local light = object('{sun_name}', 'Light')
            light.intensityUnit = LightUnit.LUX
            scene.litQuality = LitQuality.HIGH
            local again = mesh:getMaterial()
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
            await ed.call("wait", frames=10)
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
    material = find_key(await ed.call("get_entity", entity=car), "material")
    ed.check(material["pbr"]["clearcoat"]["enabled"], f"Car Paint applies a clear coat: {material!r}")
    glass = await make_entity(ed, "Glass", "sphere")
    await ed.call("apply_material_preset", entity=glass, preset="clear_glass")
    v = await ready_variant(ed, glass)
    ed.check("PBR_TRANSMISSION" in v["defines"] and "PBR_VOLUME" in v["defines"], f"glass: {v!r}")


# ------------------------------------------------------------------ the Khronos samples

MODELS = {
    # model: (camera position, target), the lobe(s) the lobe-off twin turns off
    "ClearCoatTest": ((0, 0, 12), (0, 0, 0), ["clearcoat"]),
    "SheenTestGrid": ((0, 0, 9), (0, 0, 0), ["sheen"]),
    "AnisotropyStrengthTest": ((0, 0, 16), (0, 0, 0), ["anisotropy"]),
    "AnisotropyRotationTest": ((0, 0, 14), (0, 0, 0), ["anisotropy"]),
    "IridescenceDielectricSpheres": ((0, 0, 22), (0, 0, 0), ["iridescence"]),
    "IridescenceMetallicSpheres": ((0, 0, 22), (0, 0, 0), ["iridescence"]),
    "TransmissionTest": ((0, 0.2, 3.5), (0, 0.2, 0), ["transmission"]),
    "TransmissionRoughnessTest": ((0, 0, 9), (0, 0, 0), ["transmission"]),
    "AttenuationTest": ((0, 0, 12), (0, 0, 0), ["transmission", "volume"]),
    "SpecularTest": ((0, 0, 9), (0, 0, 0), ["specular"]),
    "IORTestGrid": ((0, 0, 11), (0, 0, 0), ["specular", "transmission"]),
    "EmissiveStrengthTest": ((0, 0, 12), (0, 0, 0), []),
    "MetalRoughSpheresNoTextures": ((0, 0, 0.6), (0, 0, 0), []),
}


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
        got = await ed.call("get_entity", entity=eid)
        count = len(find_key(got, "submeshes") or []) or 1
        values = {}
        for s in range(count):
            for lobe in lobes:
                values[f"submeshes[{s}].material.pbr.{lobe}.enabled"] = False
            values[f"submeshes[{s}].material.emissiveStrength"] = 1.0
        if values:
            await ed.call("set_properties", entity=eid, values={"mesh": values})


async def test_khronos_models(ed: Editor):
    missing_goldens = []
    for model, (position, target, lobes) in MODELS.items():
        source = model_file(model)
        dest = ed.project / "models" / model
        shutil.copytree(source.parent, dest, dirs_exist_ok=True)
        await new_scene(ed, f"Khronos{model}")
        cursor = await ed.console_since("last")
        made = await ed.call("create_entity", name=model, type="model", model=f"models/{model}/{source.name}")
        root = find_key(made, "entity_id", "id")
        await camera_at(ed, position, target)
        await ed.call("wait", frames=60)
        meshes = [e for e in await entities_with(ed, "MeshComponent")]
        # every material's variant built
        for eid in meshes[:8]:
            v = await ready_variant(ed, eid)
            ed.check(not v["failed"], f"{model}: a variant failed: {v!r}")
        img = await shot(ed, frames=30)
        ed.check(not pngdiff.flat(img), f"{model} draws something")
        errors = [l for l in await ed.console_since(cursor) if l.get("level") == "error"]
        ed.check(not errors, f"{model}: errors: {errors[:3]}")

        golden = GOLDEN_DIR / f"{model}.png"
        small = pngdiff.downscale(img, *GOLDEN_SIZE)
        if UPDATE_GOLDEN:
            GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
            golden.write_bytes(pngdiff.encode(small))
        elif golden.is_file():
            ok, fraction, diff = pngdiff.compare(small, pngdiff.load(golden))
            if not ok:
                (ed.scratch.root / f"{model}.actual.png").write_bytes(pngdiff.encode(small))
                (ed.scratch.root / f"{model}.diff.png").write_bytes(diff or b"")
                ed.keep = True
            ed.check(ok, f"{model}: {fraction * 100:.2f}% of the pixels differ from {golden.relative_to(REPO)}")
        else:
            missing_goldens.append(model)

        # the lobe-off twin differs from it by more than 2% of the pixels
        off_lobes = lobes or []
        if model == "EmissiveStrengthTest":
            off_lobes = []
        if off_lobes or model == "EmissiveStrengthTest":
            await lobes_off(ed, meshes, off_lobes)
            await ed.call("wait", frames=10)
            for eid in meshes[:8]:
                await ready_variant(ed, eid)
            twin = pngdiff.downscale(await shot(ed, frames=30), *GOLDEN_SIZE)
            fraction = differing_fraction(small, twin)
            ed.check(fraction > 0.02, f"{model}: its lobe(s) {off_lobes or ['emissive strength']} change "
                                      f"{fraction * 100:.2f}% of the pixels (more than 2%)")
    ed.check(not missing_goldens, f"no golden for {missing_goldens} (JASPER_TEST_UPDATE_GOLDEN=1 writes them)")


# ------------------------------------------------------------------ old files

def test_legacy_resave_identical_sync():
    """Every scene of tools/fixtures, loaded and saved by this build, keeps its bytes (no key
    of the lobes, units or Lit Quality is written for a file that had none)."""
    fixtures = REPO / "tools" / "fixtures"
    changed = []
    for project in sorted(p for p in fixtures.iterdir() if (p / "project.yaml").is_file()):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / project.name
            shutil.copytree(project, copy)
            before = {p.relative_to(copy): p.read_bytes() for p in copy.rglob("*.scene")}
            r = subprocess.run(harness.editor_command() + ["scene-dump", "--project", str(copy), "--save"],
                               capture_output=True, text=True, timeout=600)
            assert r.returncode == 0, f"{project.name}: scene-dump failed: {r.stderr[-2000:]}"
            for rel, data in before.items():
                after = (copy / rel).read_bytes()
                if after != data:
                    changed.append(f"{project.name}/{rel}")
    assert not changed, f"re-saved scenes differ: {changed}"


async def test_legacy_resave_identical(ed: Editor):
    test_legacy_resave_identical_sync()
