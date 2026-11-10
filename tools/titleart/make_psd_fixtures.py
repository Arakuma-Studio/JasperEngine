#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes the Photoshop fixtures of S87's tests (docs/specs/S87-title-localisation-interchange.md
AC16, AC17, AC24): an independent Python writer of descriptors, `lfx2` / `lmfx` layer effects, ASL
style libraries and layered PSD / PSB files, written from the Adobe Photoshop File Formats
Specification (not from the engine's C++), so the C++ reader and style converter are tested against
another implementation of the format.

    python3 tools/titleart/make_psd_fixtures.py [--out editor/tests/titleart/fixtures/psd]

Outputs (deterministic bytes):
- styles.asl: one style per row of §6.9 (strokes and lmfx strokes, gradient overlay with opacity
  stops and midpoints, colour overlay, procedural and bitmap pattern overlays, bevels with each
  technique, direction Down, Emboss, Pillow Emboss, Stroke Emboss, Outer Bevel, Satin, Inner Glow
  from the centre, Outer Glow in Screen, drop and inner shadows, Scale Effects, point units, Lab
  and HSB colours, a Shape Burst stroke gradient) and one bitmap pattern;
- layers8.psd: an RGB 8-bit document with a pass-through group, a masked pixel layer, a clipped
  layer, a text layer with EngineData, the 13 adjustment types, solid and gradient fill layers, a
  hidden Multiply layer with a style, fill opacity and knockout, a vector mask and a layer of a
  blend mode Title Art lacks (Dissolve);
- layers16.psd: RGB 16-bit, layers in `Lr16`, ZIP with prediction;
- layers.psb: a PSB (version 2) with one layer;
- grey8.psd: Grayscale 8-bit;
- cmyk.psd: CMYK 8-bit with a styled layer (layer import refused, styles read);
- adobergb.psd: an RGB document with an embedded profile described "Adobe RGB (1998)" (our own
  minimal profile bytes, only its description matters).
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------- bytes

def u8(v): return struct.pack(">B", v)
def u16(v): return struct.pack(">H", v)
def i16(v): return struct.pack(">h", v)
def u32(v): return struct.pack(">I", v)
def i32(v): return struct.pack(">i", v)
def u64(v): return struct.pack(">Q", v)
def f64(v): return struct.pack(">d", v)
def f32(v): return struct.pack(">f", v)


def unicode_string(text: str, nul: bool = True) -> bytes:
    data = text.encode("utf-16-be")
    count = len(data) // 2 + (1 if nul else 0)
    return u32(count) + data + (b"\0\0" if nul else b"")


def pascal(text: str, pad: int) -> bytes:
    raw = text.encode("ascii", "replace")[:255]
    out = u8(len(raw)) + raw
    while len(out) % pad:
        out += b"\0"
    return out


def class_id(key: str) -> bytes:
    raw = key.encode("ascii")
    return (u32(0) + raw) if len(raw) == 4 else (u32(len(raw)) + raw)


# ---------------------------------------------------------------- descriptors

class Unit:
    def __init__(self, unit: str, value: float):
        self.unit, self.value = unit, value


class Enum:
    def __init__(self, type_: str, value: str):
        self.type, self.value = type_, value


class Obj:
    """A descriptor: class id and ordered items (key -> value)."""

    def __init__(self, cls: str, items: list[tuple[str, object]]):
        self.cls, self.items = cls, items


class Raw:
    def __init__(self, data: bytes):
        self.data = data


def value(v) -> bytes:
    if isinstance(v, bool):
        return b"bool" + u8(1 if v else 0)
    if isinstance(v, int):
        return b"long" + i32(v)
    if isinstance(v, float):
        return b"doub" + f64(v)
    if isinstance(v, str):
        return b"TEXT" + unicode_string(v)
    if isinstance(v, Unit):
        return b"UntF" + v.unit.encode("ascii") + f64(v.value)
    if isinstance(v, Enum):
        return b"enum" + class_id(v.type) + class_id(v.value)
    if isinstance(v, Obj):
        return b"Objc" + descriptor(v)
    if isinstance(v, list):
        return b"VlLs" + u32(len(v)) + b"".join(value(x) for x in v)
    if isinstance(v, Raw):
        return b"tdta" + u32(len(v.data)) + v.data
    raise TypeError(type(v))


def descriptor(d: Obj) -> bytes:
    out = unicode_string("") + class_id(d.cls) + u32(len(d.items))
    for key, v in d.items:
        out += class_id(key) + value(v)
    return out


def versioned(d: Obj) -> bytes:
    return u32(16) + descriptor(d)


# ---------------------------------------------------------------- style vocabulary

def rgb(r, g, b):
    return Obj("RGBC", [("Rd  ", float(r)), ("Grn ", float(g)), ("Bl  ", float(b))])


def px(v): return Unit("#Pxl", float(v))
def pct(v): return Unit("#Prc", float(v))
def ang(v): return Unit("#Ang", float(v))
def blend(m): return Enum("BlnM", m)


def contour(name, points):
    return Obj("ShpC", [("Nm  ", name), ("Crv ", [Obj("CrPt", [("Hrzn", float(x)), ("Vrtc", float(y))]) for x, y in points])])


def gradient(name, colors, opacities):
    """colors: (location 0..4096, midpoint 0..100, Obj colour); opacities: (location, midpoint, %)"""
    return Obj("Grdn", [
        ("Nm  ", name), ("GrdF", Enum("GrdF", "CstS")), ("Intr", 4096.0),
        ("Clrs", [Obj("Clrt", [("Clr ", c), ("Type", Enum("Clry", "UsrS")), ("Lctn", loc), ("Mdpn", mid)]) for loc, mid, c in colors]),
        ("Trns", [Obj("TrnS", [("Opct", pct(o)), ("Lctn", loc), ("Mdpn", mid)]) for loc, mid, o in opacities]),
    ])


def stroke(size, color, position="OutF", opacity=100.0, fill="SClr", extra=None):
    items = [("enab", True), ("Styl", Enum("FStl", position)), ("PntT", Enum("FrFl", fill)), ("Md  ", blend("Nrml")),
             ("Opct", pct(opacity)), ("Sz  ", px(size)), ("Clr ", color)]
    return Obj("FrFX", items + (extra or []))


def shadow(cls, angle, distance, size, color, opacity=75.0, global_light=False, mode="Mltp", choke=0.0):
    return Obj(cls, [("enab", True), ("Md  ", blend(mode)), ("Clr ", color), ("Opct", pct(opacity)), ("uglg", global_light),
                     ("lagl", ang(angle)), ("Dstn", px(distance)), ("Ckmt", pct(choke)), ("blur", px(size))])


def bevel(style="InrB", technique="SfBL", direction="In  ", size=10, depth=100.0, soften=0, gloss=None, shape=None):
    items = [("enab", True), ("hglM", blend("Scrn")), ("hglC", rgb(255, 255, 255)), ("hglO", pct(80.0)),
             ("sdwM", blend("Mltp")), ("sdwC", rgb(0, 0, 0)), ("sdwO", pct(60.0)),
             ("bvlT", Enum("bvlT", technique)), ("bvlS", Enum("BESl", style)), ("uglg", False), ("lagl", ang(135.0)),
             ("Lald", ang(40.0)), ("srgR", pct(depth)), ("blur", px(size)), ("bvlD", Enum("BESs", direction)), ("Sftn", px(soften))]
    if gloss is not None:
        items.append(("TrnS", gloss))
    if shape is not None:
        items += [("useShape", True), ("MpgS", shape)]
    return Obj("ebbl", items)


def effects(items, scale=100.0):
    return Obj("null", [("Scl ", pct(scale)), ("masterFXSwitch", True)] + items)


GOLD_GRADIENT = gradient("Gold", [(0, 50, rgb(255, 230, 120)), (2048, 25, rgb(200, 120, 20)), (4096, 50, rgb(255, 250, 200))],
                         [(0, 50, 100.0), (4096, 50, 20.0)])


def asl_styles():
    lab = Obj("LbCl", [("Lmnc", 50.0), ("A   ", 20.0), ("B   ", -30.0)])
    hsb = Obj("HSBC", [("H   ", ang(120.0)), ("Strt", 100.0), ("Brgh", 50.0)])
    burst = gradient("Burst", [(0, 50, rgb(255, 0, 0)), (4096, 50, rgb(0, 0, 255))], [(0, 50, 100.0), (4096, 50, 100.0)])
    return [
        ("Two Strokes", effects([("frameFXMulti", [stroke(4, rgb(255, 255, 255), opacity=60.0), stroke(10, rgb(0, 0, 0), "CtrF"),
                                                   stroke(3, rgb(255, 0, 0), "InsF")])])),
        ("Gradient Face", effects([("GrFl", Obj("GrFl", [("enab", True), ("Md  ", blend("Nrml")), ("Opct", pct(100.0)), ("Grad", GOLD_GRADIENT),
                                                         ("Angl", ang(90.0)), ("Type", Enum("GrdT", "Lnr ")), ("Rvrs", False),
                                                         ("Scl ", pct(100.0))]))])),
        ("Color Face", effects([("SoFi", Obj("SoFi", [("enab", True), ("Md  ", blend("Nrml")), ("Clr ", lab), ("Opct", pct(50.0))]))])),
        ("Zebra Pattern", effects([("patternFill", Obj("patternFill", [("enab", True), ("Md  ", blend("Nrml")), ("Opct", pct(80.0)),
                                                                      ("Ptrn", Obj("Ptrn", [("Nm  ", "Zebra Stripes"), ("Idnt", "zebra-0001")])),
                                                                      ("Scl ", pct(200.0))]))])),
        ("Bitmap Pattern", effects([("patternFill", Obj("patternFill", [("enab", True), ("Md  ", blend("Nrml")), ("Opct", pct(100.0)),
                                                                       ("Ptrn", Obj("Ptrn", [("Nm  ", "Swirl"), ("Idnt", "swirl-0001")])),
                                                                       ("Scl ", pct(100.0))]))])),
        ("Bevel Down", effects([("ebbl", bevel(technique="Slmt", direction="Out ", gloss=contour("Ring", [(0, 0), (64, 255), (128, 0), (255, 255)])))])),
        ("Custom Bevel", effects([("ebbl", bevel(technique="PrBL", shape=contour("Bumps", [(0, 0), (90, 200), (180, 80), (255, 255)])))])),
        ("Emboss", effects([("ebbl", bevel(style="Embs"))])),
        ("Pillow", effects([("ebbl", bevel(style="PlEb"))])),
        ("Stroke Emboss", effects([("FrFX", stroke(6, rgb(200, 200, 200))), ("ebbl", bevel(style="strokeEmboss"))])),
        ("Outer Bevel", effects([("FrFX", stroke(6, rgb(40, 40, 40))), ("ebbl", bevel(style="OtrB"))])),
        ("Satin", effects([("ChFX", Obj("ChFX", [("enab", True), ("Md  ", blend("Mltp")), ("Clr ", rgb(20, 0, 60)), ("Opct", pct(50.0)),
                                                ("lagl", ang(19.0)), ("Dstn", px(11)), ("blur", px(14)), ("Invr", True)]))])),
        ("Center Glow", effects([("IrGl", Obj("IrGl", [("enab", True), ("Md  ", blend("Scrn")), ("Clr ", hsb), ("Opct", pct(75.0)),
                                                      ("Ckmt", pct(10.0)), ("blur", px(8)), ("glwS", Enum("IGSr", "SrcC"))]))])),
        ("Screen Glow", effects([("OrGl", Obj("OrGl", [("enab", True), ("Md  ", blend("Scrn")), ("Clr ", rgb(255, 190, 0)), ("Opct", pct(90.0)),
                                                      ("Ckmt", pct(20.0)), ("blur", px(30))]))])),
        ("Shadows", effects([("dropShadowMulti", [shadow("DrSh", 120.0, 8, 6, rgb(0, 0, 0)), shadow("DrSh", 90.0, 2, 2, rgb(60, 0, 0), 50.0)]),
                             ("IrSh", shadow("IrSh", 60.0, 4, 5, rgb(0, 0, 40), global_light=True))])),
        ("Scaled Points", effects([("FrFX", Obj("FrFX", [("enab", True), ("Styl", Enum("FStl", "OutF")), ("PntT", Enum("FrFl", "SClr")),
                                                        ("Md  ", blend("Nrml")), ("Opct", pct(100.0)), ("Sz  ", Unit("#Pnt", 6.0)),
                                                        ("Clr ", rgb(0, 0, 255))]))], scale=200.0)),
        ("Shape Burst", effects([("FrFX", stroke(8, rgb(0, 0, 0), fill="GrFl", extra=[("Grad", burst), ("Type", Enum("GrdT", "shapeburst")),
                                                                                     ("Angl", ang(90.0)), ("Scl ", pct(100.0))]))])),
        ("Blending Only", Obj("null", [("masterFXSwitch", True)])),
    ]


# ---------------------------------------------------------------- patterns

def packbits(row: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(row):
        run = 1
        while i + run < len(row) and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 2:
            out += bytes([(257 - run) & 0xFF, row[i]])
            i += run
            continue
        start = i
        while i < len(row) and i - start < 128 and not (i + 1 < len(row) and row[i + 1] == row[i]):
            i += 1
        if i == start:
            i += 1
        out += bytes([i - start - 1]) + row[start:i]
    return bytes(out)


def vm_array(plane: bytes, w: int, h: int) -> bytes:
    body = u32(8) + i32(0) + i32(0) + i32(h) + i32(w) + u16(8) + u8(0) + plane
    return u32(1) + u32(len(body)) + body


def pattern_record(name: str, ident: str, w: int, h: int, rgb_planes: list[bytes]) -> bytes:
    arrays = b"".join(vm_array(p, w, h) for p in rgb_planes) + u32(0) + u32(0)
    vm = u32(3) + u32(16 + 4 + len(arrays)) + i32(0) + i32(0) + i32(h) + i32(w) + u32(3) + arrays
    body = u32(1) + u32(3) + u16(h) + u16(w) + unicode_string(name) + pascal(ident, 1) + vm
    rec = u32(len(body)) + body
    while len(rec) % 4:
        rec += b"\0"
    return rec


def swirl_pattern() -> bytes:
    w = h = 8
    r = bytes((x * 32) & 0xFF for y in range(h) for x in range(w))
    g = bytes((y * 32) & 0xFF for y in range(h) for x in range(w))
    b = bytes(((x + y) * 16) & 0xFF for y in range(h) for x in range(w))
    return pattern_record("Swirl", "swirl-0001", w, h, [r, g, b])


def write_asl(path: Path):
    patterns = swirl_pattern()
    out = u16(2) + b"8BSL" + u16(3) + u32(len(patterns)) + patterns
    styles = asl_styles()
    out += u32(len(styles))
    for name, fx in styles:
        ident = Obj("null", [("Nm  ", name), ("Idnt", name.lower().replace(" ", "-"))])
        style = Obj("null", [("Lefx", fx), ("blendOptions", Obj("blendOptions", [("Opct", pct(100.0))]))])
        block = versioned(ident) + versioned(style)
        rec = u32(len(block)) + block
        while (len(rec) - 4) % 4:
            rec += b"\0"
        out += rec
    path.write_bytes(out)


# ---------------------------------------------------------------- PSD

def engine_data(text: str, font: str, size: float, color, tracking: int, leading: float, justification: int) -> bytes:
    def s(v: str) -> bytes:
        raw = b"\xfe\xff" + v.encode("utf-16-be")
        return b"(" + raw.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)") + b")"
    a, r, g, b = color
    return (b"\n\n<<\n\t/EngineDict\n\t<<\n\t\t/Editor\n\t\t<<\n\t\t\t/Text " + s(text + "\r") +
            b"\n\t\t>>\n\t\t/ParagraphRun\n\t\t<<\n\t\t\t/RunArray [\n\t\t\t<<\n\t\t\t\t/ParagraphSheet\n\t\t\t\t<<\n\t\t\t\t\t/Properties\n\t\t\t\t\t<<\n"
            b"\t\t\t\t\t\t/Justification " + str(justification).encode() +
            b"\n\t\t\t\t\t>>\n\t\t\t\t>>\n\t\t\t>>\n\t\t\t]\n\t\t>>\n\t\t/StyleRun\n\t\t<<\n\t\t\t/RunArray [\n\t\t\t<<\n\t\t\t\t/StyleSheet\n\t\t\t\t<<\n"
            b"\t\t\t\t\t/StyleSheetData\n\t\t\t\t\t<<\n\t\t\t\t\t\t/Font 0\n\t\t\t\t\t\t/FontSize " + f"{size:.1f}".encode() +
            b"\n\t\t\t\t\t\t/AutoLeading false\n\t\t\t\t\t\t/Leading " + f"{leading:.1f}".encode() +
            b"\n\t\t\t\t\t\t/Tracking " + str(tracking).encode() +
            b"\n\t\t\t\t\t\t/FillColor << /Type 1 /Values [ " + f"{a:.5f} {r:.5f} {g:.5f} {b:.5f}".encode() +
            b" ] >>\n\t\t\t\t\t>>\n\t\t\t\t>>\n\t\t\t>>\n\t\t\t<<\n\t\t\t\t/StyleSheet << /StyleSheetData << /FontSize 12.0 >> >>\n\t\t\t>>\n\t\t\t]\n\t\t>>\n\t>>\n"
            b"\t/ResourceDict\n\t<<\n\t\t/FontSet [ << /Name " + s(font) + b" /Script 0 >> ]\n\t>>\n>>")


def text_block(text, font, size, color, tracking=50, leading=60.0, justification=2, scale=1.0, warp=False):
    ed = engine_data(text, font, size, color, tracking, leading, justification)
    desc = Obj("TxLr", [("Txt ", text), ("textGridding", Enum("textGridding", "None")), ("Ornt", Enum("Ornt", "Hrzn")),
                        ("AntA", Enum("Annt", "antiAliasSharp")), ("EngineData", Raw(ed))])
    warp_desc = Obj("warp", [("warpStyle", Enum("warpStyle", "warpArc" if warp else "warpNone")), ("warpValue", 0.0)])
    return (u16(1) + f64(scale) + f64(0.0) + f64(0.0) + f64(scale) + f64(100.0) + f64(80.0) + u16(50) + versioned(desc) +
            u16(1) + versioned(warp_desc) + f64(0.0) + f64(0.0) + f64(0.0) + f64(0.0))


def block(key: str, data: bytes, sig=b"8BIM", wide=False) -> bytes:
    while len(data) % 4:
        data += b"\0"
    return sig + key.encode("ascii") + (u64(len(data)) if wide else u32(len(data))) + data


class Layer:
    def __init__(self, name, top=0, left=0, rgba=None, w=0, h=0, blend_key="norm", opacity=255, clipping=False, visible=True,
                 extra: list[bytes] | None = None, section=0, mask=None):
        self.name, self.top, self.left, self.w, self.h = name, top, left, w, h
        self.rgba = rgba or b""
        self.blend_key, self.opacity, self.clipping, self.visible = blend_key, opacity, clipping, visible
        self.extra = extra or []
        self.section = section
        self.mask = mask    # (top, left, w, h, plane bytes, default)


def solid_rgba(w, h, color, alpha_fn=None):
    out = bytearray()
    for y in range(h):
        for x in range(w):
            a = alpha_fn(x, y) if alpha_fn else 255
            out += bytes(color) + bytes([a])
    return bytes(out)


def encode_plane(plane: bytes, w: int, h: int, depth: int, compression: int, psb: bool) -> bytes:
    bps = depth // 8
    if w == 0 or h == 0:
        return u16(0)
    if compression == 0:
        return u16(0) + plane
    if compression == 1:
        rows = [packbits(plane[y * w:(y + 1) * w]) for y in range(h)]
        counts = b"".join((u32 if psb else u16)(len(r)) for r in rows)
        return u16(1) + counts + b"".join(rows)
    # ZIP with prediction: per-row deltas
    out = bytearray()
    for y in range(h):
        prev = 0
        for x in range(w):
            if bps == 1:
                v = plane[y * w + x]
                out.append((v - prev) & 0xFF)
            else:
                v = struct.unpack_from(">H", plane, (y * w + x) * 2)[0]
                out += u16((v - prev) & 0xFFFF)
            prev = v
    return u16(3) + zlib.compress(bytes(out), 9)


def layer_info(layers: list[Layer], depth: int, compression: int, psb: bool, mode: int = 3) -> bytes:
    bps = depth // 8
    records, data = b"", b""
    for layer in layers:
        w, h = layer.w, layer.h
        planes = []
        for c in (3, 0, 1, 2):
            if bps == 1:
                planes.append(bytes(layer.rgba[i * 4 + c] for i in range(w * h)) if w * h else b"")
            else:
                planes.append(b"".join(u16(layer.rgba[i * 4 + c] * 257) for i in range(w * h)) if w * h else b"")
        colors = 1 if mode == 1 else 3
        chans = [(-1, encode_plane(planes[0], w, h, depth, compression, psb))] + \
                [(i, encode_plane(planes[i + 1], w, h, depth, compression, psb)) for i in range(colors)]
        if layer.mask:
            mt, ml, mw, mh, mplane, _ = layer.mask
            mp = mplane if bps == 1 else b"".join(u16(v * 257) for v in mplane)
            chans.append((-2, encode_plane(mp, mw, mh, depth, compression, psb)))
        rec = i32(layer.top) + i32(layer.left) + i32(layer.top + h) + i32(layer.left + w) + u16(len(chans))
        for cid, cdata in chans:
            rec += i16(cid) + (u64(len(cdata)) if psb else u32(len(cdata)))
        rec += b"8BIM" + layer.blend_key.encode("ascii") + u8(layer.opacity) + u8(1 if layer.clipping else 0)
        rec += u8((0 if layer.visible else 2) | 8 | (0x10 if layer.section else 0)) + u8(0)
        extra = b""
        if layer.mask:
            mt, ml, mw, mh, _, default = layer.mask
            extra += u32(20) + i32(mt) + i32(ml) + i32(mt + mh) + i32(ml + mw) + u8(default) + u8(0) + u16(0)
        else:
            extra += u32(0)
        extra += u32(0) + pascal(layer.name, 4)
        extra += block("luni", unicode_string(layer.name, nul=False))
        if layer.section:
            extra += block("lsct", u32(layer.section) + (b"" if layer.section == 3 else b"8BIM" + (b"pass" if layer.blend_key == "pass" else b"norm")))
        for e in layer.extra:
            extra += e
        rec += u32(len(extra)) + extra
        records += rec
        data += b"".join(c for _, c in chans)
    body = i16(-len(layers)) + records + data
    while len(body) % 4:
        body += b"\0"
    return body


def resources(dpi=72.0, icc: bytes | None = None, angle=120, altitude=30) -> bytes:
    def res(rid, data):
        r = b"8BIM" + u16(rid) + u16(0) + u32(len(data)) + data
        return r + (b"\0" if len(data) % 2 else b"")
    out = res(0x03ED, u32(int(dpi * 65536)) + u16(1) + u16(1) + u32(int(dpi * 65536)) + u16(1) + u16(1))
    out += res(0x040D, i32(angle)) + res(0x0419, i32(altitude))
    if icc:
        out += res(0x040F, icc)
    return out


def write_psd(path: Path, w: int, h: int, layers: list[Layer], depth=8, mode=3, psb=False, compression=1, icc=None, dpi=72.0,
              patterns: bytes | None = None):
    channels = 4 if mode in (3, 4) else 2
    if mode == 4:
        channels = 5
    out = b"8BPS" + u16(2 if psb else 1) + b"\0" * 6 + u16(channels) + u32(h) + u32(w) + u16(depth) + u16(mode)
    out += u32(0)
    res = resources(dpi=dpi, icc=icc)
    out += u32(len(res)) + res
    info = layer_info(layers, depth, compression, psb, mode)
    tail = b""
    if patterns:
        tail += block("Patt", patterns)
    if depth == 16:
        section = (u64(0) if psb else u32(0)) + u32(0) + block("Lr16", info, wide=psb) + tail
    else:
        section = (u64(len(info)) if psb else u32(len(info))) + info + u32(0) + tail
    out += (u64(len(section)) if psb else u32(len(section))) + section
    # merged image: raw, white
    out += u16(0) + b"\xff" * (w * h * channels * (depth // 8))
    path.write_bytes(out)


def adjustment(key: str, data: bytes) -> bytes:
    return block(key, data)


def adjustments() -> list[tuple[str, bytes]]:
    levels = u16(2) + (u16(10) + u16(240) + u16(5) + u16(250) + u16(120)) + b"".join(u16(0) + u16(255) + u16(0) + u16(255) + u16(100) for _ in range(28))
    curves = u8(0) + u16(1) + u32(0b1) + u16(3) + u16(0) + u16(0) + u16(160) + u16(128) + u16(255) + u16(255)
    hue = u16(2) + u8(0) + u8(0) + i16(0) + i16(25) + i16(0) + i16(30) + i16(-20) + i16(10) + b"\0" * (6 * 14)
    brit = i16(20) + i16(-10) + i16(127) + u8(0) + u8(0)
    expa = u16(1) + f32(0.5) + f32(0.01) + f32(1.2)
    vib = versioned(Obj("null", [("vibrance", 30), ("Strt", -10)]))
    blnc = i16(10) + i16(0) + i16(-5) + i16(0) + i16(20) + i16(0) + i16(0) + i16(0) + i16(15) + u8(1) + u8(0)
    blwh = versioned(Obj("null", [("Rd  ", 40), ("Yllw", 60), ("Grn ", 40), ("Cyn ", 60), ("Bl  ", 20), ("Mgnt", 80),
                                  ("useTint", True), ("tintColor", rgb(225, 211, 179))]))
    phfl = u16(2) + u16(0) + u16(65535) + u16(32768) + u16(0) + u16(0) + u32(40) + u8(1) + b"\0\0\0"
    grdm = (u16(1) + u8(1) + u8(0) + unicode_string("Map", nul=False) + u16(2) +
            u32(0) + u32(50) + u16(0) + u16(0) + u16(0) + u16(0) + u16(0) + u16(0) +
            u32(4096) + u32(50) + u16(0) + u16(65535) + u16(65535) + u16(65535) + u16(0) + u16(0) +
            u16(2) + u32(0) + u32(50) + u16(100) + u32(4096) + u32(50) + u16(100))
    return [("levl", levels), ("curv", curves), ("hue2", hue), ("brit", brit), ("expA", expa), ("vibA", vib), ("blnc", blnc),
            ("blwh", blwh), ("phfl", phfl), ("grdm", grdm), ("nvrt", b""), ("post", u16(5) + u16(0)), ("thrs", u16(100) + u16(0)),
            ("selc", u16(1) + b"\0" * 80)]


def vector_mask(w, h) -> bytes:
    """A closed square path from (0.25, 0.25) to (0.75, 0.75) of the document."""
    def fixed(v):
        return i32(int(round(v * 16777216)))
    recs = u16(8) + u16(0) + b"\0" * 22                      # initial fill: off
    recs += u16(0) + u16(4) + b"\0" * 22                     # closed subpath of 4 knots
    for x, y in [(0.25, 0.25), (0.75, 0.25), (0.75, 0.75), (0.25, 0.75)]:
        pt = fixed(y) + fixed(x)
        recs += u16(1) + pt * 3
    return u32(3) + u32(0) + recs


def styled_effects():
    return block("lfx2", u32(0) + versioned(effects([("FrFX", stroke(4, rgb(0, 0, 0))),
                                                     ("DrSh", shadow("DrSh", 120.0, 6, 4, rgb(0, 0, 0)))])))


def layers8() -> list[Layer]:
    W = H = 64
    half = lambda x, y: 255 if x < 16 else 0
    mask_plane = bytes(255 if x < 8 else 0 for y in range(16) for x in range(16))
    text = Layer("Title", 10, 10, solid_rgba(40, 20, (255, 255, 255)), 40, 20,
                 extra=[block("TySh", text_block("BIG WIN", "JasperFixtureSans-Bold", 24.0, (1.0, 1.0, 0.8, 0.0), scale=2.0)), styled_effects()])
    layers = [
        Layer("Background", 0, 0, solid_rgba(W, H, (30, 60, 90)), W, H),
        Layer("</Layer group>", section=3),
        Layer("Masked", 8, 8, solid_rgba(16, 16, (255, 0, 0)), 16, 16, mask=(8, 8, 16, 16, mask_plane, 0)),
        Layer("Clipped", 8, 8, solid_rgba(32, 16, (0, 255, 0), half), 32, 16, clipping=True, blend_key="scrn", opacity=128),
        Layer("Group", blend_key="pass", section=1),
        text,
    ]
    for key, data in adjustments():
        layers.append(Layer(f"Adjust {key}", extra=[adjustment(key, data)]))
    layers.append(Layer("Solid Fill", extra=[block("SoCo", versioned(Obj("null", [("Clr ", rgb(10, 200, 30))])))]))
    layers.append(Layer("Gradient Fill", extra=[block("GdFl", versioned(Obj("null", [("Grad", GOLD_GRADIENT), ("Angl", ang(45.0)),
                                                                                     ("Type", Enum("GrdT", "Rdl ")), ("Rvrs", False),
                                                                                     ("Scl ", pct(100.0))])))]))
    layers.append(Layer("Hidden Styled", 40, 40, solid_rgba(10, 10, (0, 0, 255)), 10, 10, blend_key="mul ", visible=False,
                        extra=[styled_effects(), block("iOpa", u8(128)), block("knko", u8(1))]))
    layers.append(Layer("Vector Masked", 0, 0, solid_rgba(W, H, (200, 200, 0)), W, H, extra=[block("vmsk", vector_mask(W, H))]))
    layers.append(Layer("Dissolve", 50, 0, solid_rgba(8, 8, (255, 255, 255)), 8, 8, blend_key="diss"))
    return layers


def adobe_rgb_profile() -> bytes:
    """A minimal ICC header and a 'desc' tag naming Adobe RGB (only the description is read)."""
    text = b"Adobe RGB (1998)\0"
    desc = b"desc" + u32(0) + u32(len(text)) + text + u32(0) + u32(0) + u16(0) + u8(0) + b"\0" * 67
    header = bytearray(128)
    header[36:40] = b"acsp"
    table = u32(1) + b"desc" + u32(128 + 4 + 12) + u32(len(desc))
    body = bytes(header) + table + desc
    return u32(len(body)) + body[4:]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=str(ROOT / "editor/tests/titleart/fixtures/psd"))
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    write_asl(out / "styles.asl")
    write_psd(out / "layers8.psd", 64, 64, layers8(), patterns=swirl_pattern())
    two = [Layer("Base", 0, 0, solid_rgba(32, 32, (10, 20, 30)), 32, 32), Layer("Top", 4, 4, solid_rgba(8, 8, (250, 128, 3)), 8, 8)]
    write_psd(out / "layers16.psd", 32, 32, two, depth=16, compression=3)
    write_psd(out / "layers.psb", 32, 32, [Layer("Only", 2, 2, solid_rgba(10, 10, (1, 2, 3)), 10, 10)], psb=True)
    write_psd(out / "grey8.psd", 16, 16, [Layer("Grey", 0, 0, solid_rgba(16, 16, (90, 90, 90)), 16, 16)], mode=1)
    cmyk = [Layer("Styled", 0, 0, solid_rgba(8, 8, (0, 0, 0)), 8, 8, extra=[styled_effects()])]
    write_psd(out / "cmyk.psd", 8, 8, cmyk, mode=4)
    write_psd(out / "adobergb.psd", 8, 8, [Layer("Pixel", 0, 0, solid_rgba(8, 8, (255, 0, 0)), 8, 8)], icc=adobe_rgb_profile())
    print("wrote", ", ".join(sorted(p.name for p in out.iterdir())))


if __name__ == "__main__":
    main()
