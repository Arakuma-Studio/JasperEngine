#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes the Pachislop project fixtures of S87's importer tests (docs/specs/S87-title-localisation-
interchange.md AC14, AC15): editor/tests/titleart/fixtures/pachislop/<name>.pslot.json.

Each file is built here from the documented shapes of Pachislop's project model (src/model/types.ts,
src/io/project.ts: `{format: "pslot", version, project, fonts, assets}`, base64 blobs) at the version
its name gives, with the fields that version did not have left out, so the importer's migration
fills them. Pachislop itself is never run. Each fixture seeds a known set of items the importer
cannot map; PachislopImportTest checks the report lists exactly those.

The images are arithmetic (a gradient PNG, a two-colour GIF), the WebP blob is a RIFF/WEBP header
the importer must refuse, and the embedded font is the repository's OFL Oswald fixture
(engine/tests/core/titleart/fonts/Oswald-wght.ttf, OFL text beside it).

Usage: python3 tools/titleart/make_pachislop_fixtures.py [--out DIR]
"""

import argparse
import base64
import json
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "editor" / "tests" / "titleart" / "fixtures" / "pachislop"
FONT = ROOT / "engine" / "tests" / "core" / "titleart" / "fonts" / "Oswald-wght.ttf"


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def png(width: int, height: int) -> bytes:
    """An RGBA gradient PNG: red rises with x, green with y, opaque."""
    rows = b""
    for y in range(height):
        rows += b"\x00" + b"".join(bytes((x * 255 // max(1, width - 1), y * 255 // max(1, height - 1), 96, 255)) for x in range(width))

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def gif(width: int, height: int) -> bytes:
    """A two-colour GIF89a: left half colour 0, right half colour 1. LZW with a clear code before
    every pixel, so every code stays 3 bits wide (minimum code size 2)."""
    clear, end = 4, 5
    codes = []
    for y in range(height):
        for x in range(width):
            codes += [clear, 0 if x < width // 2 else 1]
    codes.append(end)
    bits, nbits, data = 0, 0, bytearray()
    for c in codes:
        bits |= c << nbits
        nbits += 3
        while nbits >= 8:
            data.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8
    if nbits:
        data.append(bits & 0xFF)
    blocks = b"".join(bytes((len(data[i:i + 255]),)) + bytes(data[i:i + 255]) for i in range(0, len(data), 255)) + b"\x00"
    header = b"GIF89a" + struct.pack("<HHBBB", width, height, 0x80, 0, 0)
    palette = bytes((255, 32, 32, 32, 32, 255))
    image = b"\x2c" + struct.pack("<HHHHB", 0, 0, width, height, 0) + b"\x02" + blocks
    return header + palette + image + b"\x3b"


def webp() -> bytes:
    """A WebP container header with a tiny VP8L payload: the importer refuses WebP by its magic."""
    payload = b"VP8L" + struct.pack("<I", 5) + b"\x2f\x00\x00\x00\x00" + b"\x00"
    return b"RIFF" + struct.pack("<I", 4 + len(payload)) + b"WEBP" + payload


SVG = (b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">'
       b'<path d="M10 10 L90 10 L50 90 Z" fill="#000000"/></svg>')


# ---- the model's defaults (src/model/defaults.ts), at the version each fixture is written in ----

def transform(**kw):
    t = {"x": 0, "y": 0, "z": 0, "rotX": 0, "rotY": 0, "rotZ": 0, "scale": 1, "opacity": 1}
    t.update(kw)
    return t


def layer_base(id_, name, **kw):
    l = {"id": id_, "name": name, "visible": True, "solo": False, "locked": False, "guide": False, "blend": "normal",
         "transform": transform(), "effects": [], "masks": []}
    l.update(kw)
    return l


def gradient(*stops):
    return {"stops": [{"pos": p, "color": c} for p, c in stops]}


def curve(*pts):
    return {"points": [{"x": x, "y": y, "corner": False} for x, y in pts]}


def solid(color):
    return {"kind": "solid", "color": color, "gradient": gradient((0, color), (1, color)), "angle": 90, "scale": 1, "offset": 0, "span": "layer"}


def material_v4(fill, bevel=None, reflection=None):
    """A Material2D as v4 wrote it: no opacity, refraction, glass, sheen, texture target / angle,
    gloss angle; a bevel without useGlobalLight, sizeMode or fit; a satin without useGlobalLight."""
    b = {"enabled": False, "profile": "smooth", "profileCurve": curve((0, 0), (0.5, 0.75), (1, 1)), "size": 3, "depth": 3, "soften": 0,
         "lightAngle": 120, "lightAltitude": 30, "gloss": "ring", "glossCurve": curve((0, 0), (1, 1)), "highlightColor": "#ffffff",
         "highlightOpacity": 0.85, "shadowColor": "#000000", "shadowOpacity": 0.6}
    if bevel:
        b.update(bevel)
    m = {"fill": fill, "bevel": b,
         "finish": {"enabled": False, "gradient": gradient((0, "#3a0c00"), (1, "#fff8d6")), "amount": 1},
         "pattern": {"kind": "none", "color": "#000000", "color2": "#ffffff", "opacity": 1, "scale": 12, "angle": 0, "seed": 1, "shade": 0.6, "relief": 0},
         "satin": {"enabled": False, "color": "#000000", "blend": "multiply", "opacity": 0.5, "angle": 19, "distance": 5, "size": 7,
                   "contour": curve((0, 0), (0.5, 1), (1, 0)), "invert": True},
         "texture": {"kind": "none", "amount": 0.3, "scale": 4, "blend": "overlay", "seed": 1},
         "innerGlow": {"enabled": False, "color": "#ffffff", "size": 2, "opacity": 0.6, "noise": 0},
         "gloss": {"enabled": False, "opacity": 0.55, "position": 0.45, "softness": 0.15, "curve": 0.25}}
    if reflection:
        m["reflection"] = reflection
    return m


def text_source_v4(text, font="dela-gothic-one", **kw):
    s = {"kind": "text", "text": text, "fontId": font, "size": 200, "tracking": 0, "lineHeight": 1.1, "align": "center", "vertical": False,
         "shear": 0, "chars": {}}
    s.update(kw)
    return s


def text_source_v11(text, font="dela-gothic-one", **kw):
    s = text_source_v4(text, font)
    s.update({"lines": {}, "hScale": 1, "vScale": 1, "tsume": 0, "tateChuYoko": True})
    s.update(kw)
    return s


def material_v11(fill):
    m = material_v4(fill)
    m["opacity"] = 1
    m["refraction"] = 0
    m["bevel"].update({"useGlobalLight": True, "sizeMode": "fixed", "fit": 70})
    m["satin"]["useGlobalLight"] = False
    m["texture"].update({"target": "color", "angle": 0})
    m["gloss"]["angle"] = 0
    m["reflection"] = {"enabled": False, "map": "highway", "amount": 1, "blur": 75, "rotation": 0, "colorize": 0}
    m["glass"] = {"enabled": False, "color": "#3cc8ff", "tint": 1, "opacity": 0.7, "edge": 0.7, "caustic": 0.7}
    m["sheen"] = {"enabled": False, "color": "#ffffff", "amount": 0.7, "position": -0.25, "width": 0.35}
    return m


def comp(version, **kw):
    c = {"width": 1280, "height": 720, "background": {"transparent": True, "color": "#000000"},
         "camera": {"fov": 30, "x": 0, "y": 0, "zOffset": 0, "rotX": 0, "rotY": 0, "rotZ": 0},
         "environment": {"preset": "studio", "intensity": 1, "rotation": 0, "tint": "#ffffff", "contrast": 1, "saturation": 1},
         "lights": [{"id": "l0000001", "type": "directional", "color": "#ffffff", "intensity": 2, "x": -600, "y": 800, "z": 1000}],
         "toneMapping": "neutral", "exposure": 1, "supersample": 3}
    if version >= 6:
        c["globalLight"] = {"angle": 120, "altitude": 30}
        c["dpi"] = 72
    c.update(kw)
    return c


def material3d(preset, color, **kw):
    m = {"preset": preset, "color": color, "gradientKind": "linear", "gradientAngle": 90, "gradientSpan": "layer", "gradientScale": 1,
         "gradientOffset": 0, "metalness": 1, "roughness": 0.3, "reflectivity": 1, "specBoost": 1, "clearcoat": 0, "iridescence": 0,
         "transmission": 0, "ior": 1.5, "dispersion": 0, "flakes": 0, "flakeScale": 0.6,
         "pattern": {"kind": "none", "color": "#000000", "color2": "#ffffff", "opacity": 1, "scale": 12, "angle": 0, "seed": 1, "shade": 0.6, "relief": 0},
         "emissive": "#000000", "emissiveIntensity": 0, "opacity": 1}
    m.update(kw)
    return m


def legacy_style3d(**kw):
    """A v4-v10 3D style: bevel rings (`bands`) and `backplates`, no mode / inflate / lighting fields."""
    s = {"depth": 10, "curveSegments": 8, "skew": {"x": 0, "y": 0}, "face": material3d("gold", "#ffcc33"),
         "side": material3d("gold", "#ffcc33"),
         "bands": [{"id": "ring0001", "width": 1.5, "depth": 1.5, "profile": "round", "segments": 6, "curve": 0, "material": material3d("gold", "#ffcc33")},
                   {"id": "ring0002", "width": 0.5, "depth": 0.5, "profile": "chamfer", "segments": 6, "curve": 0, "material": material3d("custom", "#3a1000")}],
         "backplates": [{"id": "plate001", "enabled": True, "grow": 4, "join": "round", "fillCounters": True,
                         "plate": {"kind": "box", "close": 0, "corner": 2, "slant": 8}, "depth": 7, "zGap": 1.5,
                         "bevel": {"width": 0.8, "depth": 1.2, "profile": "chamfer", "segments": 3, "curve": 0.2},
                         "face": material3d("custom", "#101010")}],
         "shadow": {"enabled": False, "color": "#000000", "opacity": 0.5, "softness": 0.5},
         "glint": {"enabled": False, "threshold": 0.8, "intensity": 1, "radius": 4, "streaks": 4, "length": 1, "angle": 0, "tint": "#ffffff"},
         "ao": {"enabled": False, "radius": 4, "intensity": 1}}
    s.update(kw)
    return s


def effect(id_, type_, enabled=True, **params):
    return {"id": id_, "type": type_, "enabled": enabled, "params": params}


def file_of(version, name, layers, comp_, fonts=(), assets=(), **project):
    p = {"version": version, "name": name, "comp": comp_, "layers": layers}
    p.update(project)
    return {"format": "pslot", "version": version, "project": p, "fonts": list(fonts), "assets": list(assets)}


# ---- the fixtures ----

def v4_text_rims():
    """v4: a 2D text title with two rims (no plates, no erode, no knockout), a v4 bevel and a
    dropShadow without useGlobalLight. Seeded: `solo` (dropped) and a reflection map (approximated)."""
    face = material_v4(solid("#ffd700"), bevel={"enabled": True, "size": 4},
                       reflection={"enabled": True, "map": "city", "amount": 0.5, "blur": 60, "rotation": 0, "colorize": 0})
    rims = [{"id": "a1b2c301", "enabled": True, "width": 4, "join": "round", "fillCounters": False, "offsetX": 0, "offsetY": 0,
             "material": material_v4(solid("#000000"))},
            {"id": "a1b2c302", "enabled": True, "width": 3, "join": "miter", "fillCounters": True, "offsetX": 1, "offsetY": -1,
             "material": material_v4(solid("#ffffff"))}]
    title = layer_base("0000aa01", "Title", kind="title2d", solo=True, source=text_source_v4("BIG WIN"),
                       style={"face": face, "rims": rims},
                       effects=[effect("0000ef01", "dropShadow", angle=60, distance=10, size=14, color="#000000", opacity=0.6)])
    return file_of(4, "Big Win", [title], comp(4))


def v6_groups_masks_clip():
    """v6: a group of a text and a clipped shape, shape / gradient / layer masks, a background and
    a Hue/Saturation adjustment. Seeded: a layer mask naming a layer the file lacks."""
    text = layer_base("0000bb01", "Word", kind="title2d", parentId="0000bb00", source=text_source_v11("WIN", tateChuYoko=False),
                      style={"face": material_v4(solid("#ff3333")), "rims": []},
                      masks=[{"id": "0000ba01", "enabled": True, "invert": False, "feather": 4, "density": 1, "linked": True, "kind": "shape",
                              "source": {"kind": "shape", "shape": "ellipse", "size": 300, "aspect": 1.5, "corner": 0, "slant": 0, "points": 5,
                                         "inner": 0.5, "jitter": 0, "seed": 1, "tail": {"x": 0, "y": -60, "width": 20}},
                              "x": 10, "y": -5, "rotation": 15},
                             {"id": "0000ba02", "enabled": True, "invert": True, "feather": 0, "density": 0.8, "linked": False, "kind": "gradient",
                              "shape": "linear", "x": 0, "y": 0, "angle": 90, "length": 200, "inner": 0, "mid": 0.5}])
    plate = layer_base("0000bb02", "Plate", kind="title2d", parentId="0000bb00", clip=True,
                       source={"kind": "shape", "shape": "rect", "size": 240, "aspect": 3, "corner": 20, "slant": 0, "points": 5, "inner": 0.5,
                               "jitter": 0, "seed": 1, "tail": {"x": 0, "y": -60, "width": 20}},
                       style={"face": material_v4(solid("#2244aa")), "rims": []},
                       masks=[{"id": "0000ba03", "enabled": True, "invert": False, "feather": 0, "density": 1, "linked": True, "kind": "layer",
                               "layerId": "0000bb01", "channel": "alpha"},
                              {"id": "0000ba04", "enabled": True, "invert": False, "feather": 0, "density": 1, "linked": True, "kind": "layer",
                               "layerId": "missing0", "channel": "luminance"}])
    group = layer_base("0000bb00", "Badge", kind="group", collapsed=True, companionGroupOf="0000bb01")
    background = layer_base("0000bb03", "Background", kind="background", guide=True,
                            fill={"kind": "radial", "color": "#10081c", "gradient": gradient((0, "#3a0a4a"), (1, "#000000")), "angle": 90, "scale": 1,
                                  "offset": 0, "span": "layer"})
    adjust = layer_base("0000bb04", "Hue/Saturation", kind="adjustment", transform=transform(opacity=0.5),
                        adjustment={"type": "hueSaturation", "hue": 30, "saturation": -20, "lightness": 5, "colorize": False})
    return file_of(6, "Groups", [background, text, plate, group, adjust], comp(6))


def v7_content_effects():
    """v7: content effects (a Sparkles between two effects) become a group with a Glints companion;
    a ベタフラ speed-lines generator moves its burst fill to a Sunburst below; a particles glow
    becomes an Outer Glow. Seeded: an effect type Pachislop never had (kept unknown)."""
    title = layer_base("0000cc01", "Jackpot", kind="title2d", source=text_source_v11("JACKPOT"),
                       style={"face": material_v11(solid("#ffffff")), "rims": [], "knockout": False,
                              "erode": {"enabled": False, "amount": 0.5, "edge": 6, "scale": 12, "angle": 0, "seed": 1}},
                       partEffects={},
                       effects=[effect("0000ce01", "outerGlow", size=20), effect("0000ce02", "sparkles", placement="hero", size=52),
                                effect("0000ce03", "dropShadow", angle=45, useGlobalLight=False), effect("0000ce04", "noSuchEffect", amount=3)])
    lines = layer_base("0000cc02", "Beta", kind="generator", transform=transform(opacity=0.8),
                       generator={"type": "speedLines", "params": {"type": "beta", "opacity": 50, "fillBurst": True, "burstColor": "#ff0000",
                                                                   "halftone": True, "dotSize": 6}})
    dust = layer_base("0000cc03", "Dust", kind="generator", generator={"type": "particles", "params": {"glow": 24, "count": 80}})
    return file_of(7, "Content", [lines, title, dust], comp(7))


def v8_lighting():
    """v8: a 3D text title in the v4-v10 shape (rings and backplates) with its own lighting and a
    tilted glyph (v8 tiltY turns the other way). Seeded: the comp's 3D scene (approximated)."""
    style = legacy_style3d(lighting={"environment": {"preset": "studio", "intensity": 1.5, "blur": 0.2}})
    title = layer_base("0000dd01", "Gold", kind="title3d",
                       source=text_source_v11("777", chars={"0": {"dx": 0, "dy": 0, "scale": 1, "rotate": 0, "tiltX": 5, "tiltY": 20}}),
                       style3d=style, partEffects={"face": [effect("0000de01", "outerGlow", size=8)]})
    return file_of(8, "Lighting", [title], comp(8))


def v10_rings():
    """v10: the bands conversion: the first ring folds into the letter, the second becomes a band,
    the backplate a band, part effects follow (band:<ring>, plate:<id>, side)."""
    style = legacy_style3d(mode="extrude", bevelPosition="outside", envLock=False, faceImage=1, edgeImage=0,
                           inflate={"height": 12, "profile": curve((0, 0), (0.35, 0.8), (1, 1)), "smoothness": 0, "detail": 1},
                           lighting={"environment": {}})
    title = layer_base("0000ee01", "Rings", kind="title3d", source=text_source_v11("BONUS"), style3d=style,
                       partEffects={"face": [effect("0000ea01", "outerGlow", size=1)], "band:ring0001": [effect("0000ea02", "outerGlow", size=2)],
                                    "band:ring0002": [effect("0000ea03", "outerGlow", size=3)], "side": [effect("0000ea04", "outerGlow", size=4)],
                                    "plate:plate001": [effect("0000ea05", "outerGlow", size=5)]},
                       warp={"style": "arcLower", "bend": 20, "horizontal": 0, "vertical": 0}, weight=2)
    return file_of(10, "Rings", [title], comp(10))


def v11_all_kinds():
    """v11: every layer kind and source with embedded assets: an embedded font, an SVG, a PNG, a
    GIF (first frame as PNG), a WebP (refused), a builtin font, materialLinks, dpi 300.
    Seeded: the WebP image (image_format_unsupported) and a font id neither embedded nor builtin
    (font_unavailable)."""
    font = FONT.read_bytes()
    fonts = [{"id": "font0001", "name": "Oswald", "data": b64(font)}]
    assets = [{"id": "svg00001", "name": "badge.svg", "mime": "image/svg+xml", "data": b64(SVG)},
              {"id": "png00001", "name": "grad", "mime": "image/png", "data": b64(png(16, 8))},
              {"id": "gif00001", "name": "flag.gif", "mime": "image/gif", "data": b64(gif(8, 4))},
              {"id": "webp0001", "name": "photo.webp", "mime": "image/webp", "data": b64(webp())}]
    style = {"face": material_v11(solid("#ffffff")), "rims": [], "knockout": False,
             "erode": {"enabled": False, "amount": 0.5, "edge": 6, "scale": 12, "angle": 0, "seed": 1}}
    linked = material_v11(solid("#c0a040"))
    linked["link"] = "gold0001"
    ops = {"warp": {"style": "none", "bend": 0, "horizontal": 0, "vertical": 0},
           "distort": {"mode": "none", "corners": [{"x": 0, "y": 0}] * 4, "mesh": [{"x": 0, "y": 0}] * 16}, "weight": 0}
    layers = [
        layer_base("0000ff00", "Background", kind="background", fill=solid("#202020")),
        layer_base("0000ff01", "Embedded", kind="title2d", clip=False, partEffects={}, source=text_source_v11("Oswald", font="font0001"),
                   style={**style, "face": linked}, **ops),
        layer_base("0000ff02", "Missing Font", kind="title2d", clip=False, partEffects={}, source=text_source_v11("Gone", font="nosuch-font"),
                   style=style, **ops),
        layer_base("0000ff03", "Star", kind="title2d", clip=False, partEffects={},
                   source={"kind": "shape", "shape": "star", "size": 200, "aspect": 1, "corner": 0, "slant": 0, "points": 6, "inner": 0.45,
                           "jitter": 0, "seed": 3, "tail": {"x": 0, "y": -60, "width": 20}}, style=style, **ops),
        layer_base("0000ff04", "Badge", kind="title2d", clip=False, partEffects={}, source={"kind": "svg", "assetId": "svg00001", "size": 160},
                   style=style, **ops),
        layer_base("0000ff05", "Picture", kind="title2d", clip=False, partEffects={},
                   source={"kind": "image", "assetId": "png00001", "size": 128, "trace": {"threshold": 128, "smooth": 0.5, "despeckle": 0.05}},
                   style={**style, "face": material_v11({**solid("#ffffff"), "kind": "image"})}, **ops),
        layer_base("0000ff06", "Flag", kind="title2d", clip=False, partEffects={},
                   source={"kind": "image", "assetId": "gif00001", "size": 64, "trace": {"threshold": 128, "smooth": 0.5, "despeckle": 0.05}},
                   style=style, **ops),
        layer_base("0000ff07", "Photo", kind="title2d", clip=False, partEffects={},
                   source={"kind": "image", "assetId": "webp0001", "size": 64, "trace": {"threshold": 128, "smooth": 0.5, "despeckle": 0.05}},
                   style=style, **ops),
        layer_base("0000ff08", "Group", kind="group", clip=False, collapsed=False),
        layer_base("0000ff09", "Glints", kind="generator", clip=False, parentId="0000ff08",
                   generator={"type": "glints", "params": {"follow": "0000ff01", "count": 12}}),
        layer_base("0000ff0a", "Levels", kind="adjustment", clip=False,
                   adjustment={"type": "levels", "channels": {k: {"inBlack": 10, "inWhite": 240, "gamma": 1.2, "outBlack": 0, "outWhite": 255}
                                                              for k in ("rgb", "r", "g", "b")}}),
    ]
    return file_of(11, "All Kinds", layers, comp(11, dpi=300, globalLight={"angle": 135, "altitude": 40}), fonts=fonts, assets=assets,
                   materialLinks={"gold0001": {"name": "Gold"}})


def v4_text_rims_v11(v4):
    """The v11 twin of v4_text_rims: what Pachislop's migration writes for it (fields it adds
    filled with the old behaviour), so both import identically."""
    twin = json.loads(json.dumps(v4))
    twin["version"] = 11
    p = twin["project"]
    p["version"] = 11
    p["comp"]["globalLight"] = {"angle": 120, "altitude": 30}
    p["comp"]["dpi"] = 72
    p["comp"]["environment"].update({"blur": 0, "repeatX": 1, "repeatY": 1})
    for layer in p["layers"]:
        layer["clip"] = False
        layer["transform"].update({"flipX": False, "flipY": False})
        layer["warp"] = {"style": "none", "bend": 0, "horizontal": 0, "vertical": 0}
        layer["distort"] = {"mode": "none", "corners": [{"x": 0, "y": 0}] * 4, "mesh": [{"x": 0, "y": 0}] * 16}
        layer["weight"] = 0
        layer["partEffects"] = {}
        s = layer["source"]
        s.update({"hScale": 1, "vScale": 1, "tsume": 0, "tateChuYoko": False, "lines": {}})
        for fx in layer["effects"]:
            if fx["type"] == "dropShadow":
                fx["params"]["useGlobalLight"] = False

        def fill_material(m):
            m.setdefault("opacity", 1)
            m.setdefault("refraction", 0)
            m["bevel"].update({k: v for k, v in {"useGlobalLight": False, "sizeMode": "fixed", "fit": 70}.items() if k not in m["bevel"]})
            m.setdefault("reflection", {"enabled": False, "map": "highway", "amount": 1, "blur": 75, "rotation": 0, "colorize": 0})
            m["satin"].setdefault("useGlobalLight", False)
            m["texture"].update({k: v for k, v in {"target": "color", "angle": 0}.items() if k not in m["texture"]})
            m["gloss"].setdefault("angle", 0)
            m.setdefault("glass", {"enabled": False, "color": "#3cc8ff", "tint": 1, "opacity": 0.7, "edge": 0.7, "caustic": 0.7})
            m.setdefault("sheen", {"enabled": False, "color": "#ffffff", "amount": 0.7, "position": -0.25, "width": 0.35})

        st = layer["style"]
        fill_material(st["face"])
        for r in st["rims"]:
            r["plate"] = {"kind": "contour", "close": 0, "corner": 4, "slant": 0}
            fill_material(r["material"])
        st["knockout"] = False
        st["erode"] = {"enabled": False, "amount": 0.5, "edge": 6, "scale": 12, "angle": 0, "seed": 1}
    return twin


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=pathlib.Path, default=OUT)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    v4 = v4_text_rims()
    fixtures = {
        "v4_text_rims": v4,
        "v4_text_rims_v11": v4_text_rims_v11(v4),
        "v6_groups_masks_clip": v6_groups_masks_clip(),
        "v7_content_effects": v7_content_effects(),
        "v8_lighting": v8_lighting(),
        "v10_rings": v10_rings(),
        "v11_all_kinds": v11_all_kinds(),
    }
    for name, data in fixtures.items():
        (args.out / f"{name}.pslot.json").write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {name}.pslot.json")


if __name__ == "__main__":
    main()
