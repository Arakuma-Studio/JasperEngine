#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Write a full Color Scheme (.titlecolors) from its anchors (docs/specs/S84-title-library.md §6.9.1).

    python3 -I tools/titleart/expand_scheme.py <anchors.json> [--out <file>]
    python3 -I tools/titleart/expand_scheme.py <anchors.json> --out-dir editor/resources/titleart/color-schemes

The anchors file holds one scheme's anchors (an object) or several (a list; then --out-dir):

    {"id": "neon-pink", "name": {"en": "Neon Pink", "ja": "ネオンピンク"},
     "description": {"en": "...", "ja": "..."},
     "face": ["#ffe6f6", "#ff4ab8", "#c0007a"],          # light -> dark; one colour = a solid face
     "dark": "#1a0010", "accents": ["#ffffff", "#ff2a9a"],  # inner -> outer; "gold metal" / "silver metal"
     "base": "#2a0018", "glow": "#ff2a9a", "shadow": "#3a0020",
     "finish": "gloss",                                   # metal | gloss | matte
     "family_hint": ["neon", "retro"], "tags": ["pink", "neon", "glow"],
     "synonyms": {"en": ["hot pink"], "ja": ["ピンク", "pinku"]}}

The output is the canonical sparse JSON engine/core/titleart/ColorScheme.cpp's writeColorScheme
writes (§6.2 member order, members at their defaults omitted, S80's number and colour formatting,
two-space indent): ColorSchemeTest.CommittedFilesAreCanonical reads every committed file back and
compares the bytes. The schema defaults below are S80's and S86's (TitleArtSchema.cpp,
three_d/Title3DSchema.cpp); a change there changes this table.

`gold metal` takes the gold scheme's rim paint (gold.titlecolors accents[0], Pachislop's GOLD_RIM)
and `silver metal` Pachislop's SILVER_RIM (blue.titlecolors accents[1]: the silver scheme's own
accents are white and a blue metal); until those files exist, the paints transcribed below from
Pachislop's src/presets/colourways.ts and src/model/defaults.ts (by SafEight, used with the
author's permission).
"""

import argparse
import json
import math
import os
import sys

FORMAT = "jasperengine.titlecolors"
VERSION = 1

# ---- the schema's kinds (member order and defaults) ----

STOP = {"pos": 0, "color": "#000000", "alpha": 1, "mid": 0.5}
GRADIENT = {"stops": [dict(STOP), {"pos": 1, "color": "#ffffff", "alpha": 1, "mid": 0.5}], "interp": "rgb"}
FILL_IMAGE = {"image": None, "scale": 1, "offset": [0, 0], "rotation": 0, "tile": False}
FILL = {"kind": "solid", "color": "#ffffff", "gradient": GRADIENT, "angle": 90, "scale": 1, "offset": 0, "span": "layer", "image": FILL_IMAGE}
PATTERN = {"kind": "none", "color": "#000000", "color2": "#ffffff", "opacity": 1, "scale": 12, "angle": 0, "seed": 1, "image": None,
           "shade": 0.6, "relief": 0}
SURFACE = {"kind": "none", "amount": 0.5, "scale": 20, "seed": 1}
BASE_TEXTURE = {"texture": None, "tiling": [1, 1], "offset": [0, 0], "projection": "front"}
MATERIAL_3D = {"preset": "custom", "color": "#ffffff", "gradient": None, "gradient_kind": "linear", "gradient_angle": 90,
               "gradient_span": "layer", "gradient_scale": 1, "gradient_offset": 0, "gradient_axis": "xy", "gradient_bend": 0,
               "metallic": 0, "roughness": 0.3, "env_multiplier": 1, "specular_multiplier": 1, "clearcoat": 0,
               "clearcoat_roughness": 0.04, "iridescence": 0, "iridescence_ior": 1.6, "iridescence_thickness_nm": [180, 520],
               "transmission": 0, "ior": 1.5, "dispersion": 0, "glass_depth": 0, "flakes": 0, "flake_scale": 0.6,
               "pattern": PATTERN, "surface": SURFACE, "base_texture": BASE_TEXTURE, "emission": "#000000",
               "emission_intensity": 0, "opacity": 1, "environment": {}, "link": ""}

# kind -> {object member: its kind}, {list member: its element kind}
KINDS = {
    "fill": (FILL, {"gradient": "gradient", "image": "fill_image"}, {}),
    "fill_image": (FILL_IMAGE, {}, {}),
    "gradient": (GRADIENT, {}, {"stops": "gradient_stop"}),
    "gradient_stop": (STOP, {}, {}),
    "pattern": (PATTERN, {}, {}),
    "material_3d": (MATERIAL_3D, {"gradient": "gradient", "pattern": "pattern", "surface": "surface_3d",
                                  "base_texture": "base_texture_3d"}, {}),
    "surface_3d": (SURFACE, {}, {}),
    "base_texture_3d": (BASE_TEXTURE, {}, {}),
}


def complete(value, kind):
    """`value` with every member of `kind` present, in the kind's order (S80's normalize, for the
    members this script writes)."""
    defaults, objects, lists = KINDS[kind]
    out = {}
    for key, d in defaults.items():
        if key not in value:
            out[key] = json.loads(json.dumps(d))
            continue
        v = value[key]
        if key in objects and isinstance(v, dict):
            out[key] = complete(v, objects[key])
        elif key in lists and isinstance(v, list):
            out[key] = [complete(e, lists[key]) if isinstance(e, dict) else e for e in v]
        else:
            out[key] = v
    for key, v in value.items():
        if key not in out:
            out[key] = v
    return out


def same(a, b):
    """JSON equality (1 == 1.0, as nlohmann's)."""
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return float(a) == float(b)
    if isinstance(a, dict) and isinstance(b, dict):
        return list(a.keys()) == list(b.keys()) and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


def sparse(value, kind, defaults=None):
    """sparseSchemaObject: members equal to the defaults omitted, objects recursively, lists whole
    with each element sparse against its kind's default."""
    kdefaults, objects, lists = KINDS[kind]
    defaults = kdefaults if defaults is None else defaults
    out = {}
    for key, v in value.items():
        if key not in kdefaults:
            out[key] = v
            continue
        if key in defaults and same(defaults[key], v):
            continue
        if key in objects and isinstance(v, dict):
            base = defaults.get(key) if isinstance(defaults.get(key), dict) else KINDS[objects[key]][0]
            out[key] = sparse(v, objects[key], base)
        elif key in lists and isinstance(v, list):
            out[key] = [sparse(e, lists[key], KINDS[lists[key]][0]) if isinstance(e, dict) else e for e in v]
        else:
            out[key] = v
    return out


# ---- canonical text (TitleArtIO.cpp writeValue) ----

def number_text(v):
    if isinstance(v, bool):
        raise TypeError("not a number")
    if isinstance(v, int):
        return str(v)
    if not math.isfinite(v) or v == 0:
        v = 0.0
    if v == math.floor(v) and abs(v) < 1e15:
        return "%.0f" % v
    return "%.7g" % v


def string_text(s):
    out = ['"']
    for ch in s:
        c = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif c < 0x20:
            out.append("\\u%04x" % c)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def write_value(v, indent=0):
    pad = " " * (indent * 2)
    inner = " " * (indent * 2 + 2)
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return number_text(v)
    if isinstance(v, str):
        return string_text(v)
    if isinstance(v, list):
        if not v:
            return "[]"
        if all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v):
            return "[" + ", ".join(number_text(x) for x in v) + "]"
        return "[\n" + ",\n".join(inner + write_value(x, indent + 1) for x in v) + "\n" + pad + "]"
    if isinstance(v, dict):
        if not v:
            return "{}"
        return "{\n" + ",\n".join(inner + string_text(k) + ": " + write_value(x, indent + 1) for k, x in v.items()) + "\n" + pad + "}"
    raise TypeError("not JSON: %r" % (v,))


# ---- colours ----

def rgb(hexc):
    h = hexc.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]


def hexof(c):
    return "#" + "".join("%02x" % max(0, min(255, int(math.floor(x * 255.0 + 0.5)))) for x in c)


def luma(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def lab(c):
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]
    x = (0.4124 * lin[0] + 0.3576 * lin[1] + 0.1805 * lin[2]) / 0.95047
    y = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
    z = (0.0193 * lin[0] + 0.1192 * lin[1] + 0.9505 * lin[2]) / 1.08883
    f = lambda t: t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116
    fx, fy, fz = f(x), f(y), f(z)
    return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)]


def delta_e(a, b):
    """CIE76 distance of two colours."""
    la, lb = lab(rgb(a)), lab(rgb(b))
    return math.sqrt(sum((p - q) ** 2 for p, q in zip(la, lb)))


def stops_of(colours):
    n = len(colours)
    return [{"pos": round(i / (n - 1), 4) if n > 1 else 0, "color": c} for i, c in enumerate(colours)]


def sample(colours, pos):
    """The linear gradient of evenly spaced `colours` at `pos` (sRGB blend, as rgb gradients)."""
    if len(colours) == 1:
        return colours[0]
    stops = stops_of(colours)
    for a, b in zip(stops, stops[1:]):
        if pos <= b["pos"]:
            t = (pos - a["pos"]) / (b["pos"] - a["pos"])
            ca, cb = rgb(a["color"]), rgb(b["color"])
            return hexof([x + (y - x) * t for x, y in zip(ca, cb)])
    return colours[-1]


def gradient(colours):
    return {"stops": stops_of(colours)}


# ---- paints ----

def material_for(finish, color):
    if finish == "metal":
        return {"color": color, "metallic": 1, "roughness": 0.25}
    if finish == "gloss":
        return {"color": color, "metallic": 0, "roughness": 0.15, "clearcoat": 1}
    if finish == "matte":
        return {"color": color, "roughness": 0.7}
    raise SystemExit("finish must be metal, gloss or matte, not %r" % finish)


def solid_paint(color, shade, material):
    return {"fill": {"kind": "solid", "color": color}, "material_3d": material, "shade": shade}


def dark_paint(color):
    # keylines, dark rings and dropped bases: satin dark metal in 3D (colourways.ts darkPaint)
    return solid_paint(color, "#000000", {"color": color, "metallic": 1, "roughness": 0.4, "env_multiplier": 0.6})


def g(*stops):
    return {"stops": [{"pos": p, "color": c} for p, c in stops]}


# Pachislop's GOLD_RIM and SILVER_RIM (colourways.ts; the gradients and presets of defaults.ts)
GOLD_RAMP = g((0, "#3a0c00"), (0.25, "#a83800"), (0.5, "#f08c00"), (0.75, "#ffd23a"), (1, "#fff8d6"))
SILVER_RAMP = g((0, "#14161c"), (0.35, "#5d6474"), (0.7, "#c9cfdb"), (1, "#ffffff"))
POLISHED_GOLD = g((0, "#fff8d8"), (0.06, "#ffe890"), (0.16, "#ffc838"), (0.3, "#f0a018"), (0.4, "#c06a06"), (0.47, "#5a2602"),
                  (0.51, "#1a0800"), (0.55, "#120500"), (0.6, "#5a2804"), (0.72, "#c87810"), (0.84, "#f8b830"), (0.94, "#ffe080"),
                  (1, "#fff4c8"))
CHROME = g((0, "#ffffff"), (0.22, "#e8eef5"), (0.45, "#8a94a3"), (0.52, "#2e343d"), (0.6, "#7d8794"), (0.8, "#dde4ee"), (1, "#ffffff"))
TRANSCRIBED = {
    "gold metal": {
        "fill": {"kind": "linear", "color": "#8a2c00",
                 "gradient": g((0, "#8a2c00"), (0.17, "#ffe470"), (0.33, "#c05a00"), (0.5, "#ffbe30"), (0.67, "#fff0a0"), (0.83, "#f09a10"),
                               (1, "#ffe060"))},
        "ramp": GOLD_RAMP,
        "material_3d": {"preset": "gold", "color": "#ffc23a", "gradient": POLISHED_GOLD, "gradient_span": "line", "gradient_bend": 0.6,
                        "metallic": 1, "roughness": 0.06},
        "shade": "#4a2200",
    },
    "silver metal": {
        "fill": {"kind": "linear", "color": "#f4f6fa",
                 "gradient": g((0, "#f4f6fa"), (0.28, "#9aa3b2"), (0.5, "#283040"), (0.72, "#c3cad6"), (1, "#ffffff"))},
        "ramp": SILVER_RAMP,
        "material_3d": {"preset": "chrome", "color": "#ffffff", "gradient": CHROME, "metallic": 1, "roughness": 0.04},
        "shade": "#1a2234",
    },
}
# where the committed schemes hold them: (file, accent index)
METAL_SOURCES = {"gold metal": ("gold.titlecolors", 0), "silver metal": ("blue.titlecolors", 1)}


def metal_paint(name, schemes_dir):
    file, i = METAL_SOURCES[name]
    path = os.path.join(schemes_dir, file)
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            accents = json.load(f).get("paints", {}).get("accents", [])
        if len(accents) > i:
            return json.loads(json.dumps(accents[i]))
    return json.loads(json.dumps(TRANSCRIBED[name]))


def paint_color(p):
    """A paint's swatch colour (colourways.ts paintColor): the solid colour, else the middle stop."""
    fill = p.get("fill", {})
    if fill.get("kind", "solid") == "solid":
        return fill.get("color", "#ffffff")
    stops = fill["gradient"]["stops"]
    return stops[len(stops) // 2]["color"]


def sparse_paint(p):
    out = {}
    fill = sparse(complete(p.get("fill", {}), "fill"), "fill")
    if fill:
        out["fill"] = fill
    if p.get("ramp") is not None:
        out["ramp"] = sparse(complete(p["ramp"], "gradient"), "gradient")
    material = sparse(complete(p.get("material_3d", {}), "material_3d"), "material_3d")
    if material:
        out["material_3d"] = material
    if p.get("shade", "#000000") != "#000000":
        out["shade"] = p["shade"]
    if p.get("pattern") is not None:
        out["pattern"] = sparse(complete(p["pattern"], "pattern"), "pattern")
    for k, v in p.items():
        if k not in ("fill", "ramp", "material_3d", "shade", "pattern"):
            out[k] = v
    return out


# ---- the light ramp ----

def light_ramp(face, glow, shadow):
    """The face stops, glow and shadow by Rec.601 luma, near-duplicates (ΔE < 3, the ramp's black
    and white ends included) removed; padded with greys of the largest gap or thinned at the
    smallest one to 4..7 entries."""
    ramp = []
    for c in sorted(set(face + [glow, shadow]), key=lambda c: (luma(rgb(c)), c)):
        if delta_e(c, "#000000") < 3 or delta_e(c, "#ffffff") < 3:
            continue
        if any(delta_e(c, k) < 3 for k in ramp):
            continue
        ramp.append(c)
    while len(ramp) > 7:
        # the entry whose neighbours are closest in luma adds the least
        lumas = [0.0] + [luma(rgb(c)) for c in ramp] + [1.0]
        gaps = [lumas[i + 2] - lumas[i] for i in range(len(ramp))]
        del ramp[gaps.index(min(gaps))]
    while len(ramp) < 4:
        ends = ["#000000"] + ramp + ["#ffffff"]
        lumas = [luma(rgb(c)) for c in ends]
        gaps = [lumas[i + 1] - lumas[i] for i in range(len(ends) - 1)]
        i = gaps.index(max(gaps))
        mid = hexof([(x + y) / 2 for x, y in zip(rgb(ends[i]), rgb(ends[i + 1]))])
        ramp.insert(i, mid)
    return ramp


# ---- the scheme ----

def check_tags(scheme_id, tags):
    if len(tags) < 3:
        raise SystemExit("%s: at least 3 tags" % scheme_id)
    for t in tags:
        if not t or any(not (c.isascii() and (c.islower() or c.isdigit() or c == "-")) for c in t):
            raise SystemExit("%s: tag %r is not lowercase [a-z0-9-]" % (scheme_id, t))


def lower(c):
    return c.lower()


def expand(a, schemes_dir):
    sid = a["id"]
    face = [lower(c) for c in a["face"]]
    dark, base, glow, shadow = (lower(a[k]) for k in ("dark", "base", "glow", "shadow"))
    finish = a["finish"]
    check_tags(sid, a["tags"])
    lightest, darkest = face[0], face[-1]
    mid = sample(face, 0.5)

    if len(face) == 1:
        face_fill = {"kind": "solid", "color": face[0]}
    else:
        face_fill = {"kind": "linear", "color": face[0], "gradient": gradient(face), "angle": 90}
    face_paint = {"fill": face_fill,
                  "ramp": gradient([dark, darkest, mid, lightest, "#ffffff"]) if finish == "metal" else None,
                  "material_3d": material_for(finish, sample(face, 0.4)),
                  "shade": shadow}

    def accent(name):
        if name in METAL_SOURCES:
            return metal_paint(name, schemes_dir)
        c = lower(name)
        return solid_paint(c, shadow, material_for(finish, c))

    accents = [accent(x) for x in a["accents"]]
    side = material_for(finish, hexof([x * 0.7 for x in rgb(darkest)]))

    out = {"format": FORMAT, "version": VERSION, "id": sid, "revision": a.get("revision", 1), "name": a["name"]}
    if "description" in a:
        out["description"] = a["description"]
    out["family_hint"] = a["family_hint"]
    out["tags"] = a["tags"]
    out["synonyms"] = a["synonyms"]
    paints = {}
    for key, p in (("face", face_paint), ("dark", dark_paint(dark))):
        s = sparse_paint(p)
        if s:
            paints[key] = s
    paints["accents"] = [sparse_paint(p) for p in accents]
    s = sparse_paint(dark_paint(base))
    if s:
        paints["base"] = s
    side_sparse = sparse(complete(side, "material_3d"), "material_3d")
    if side_sparse:
        paints["side_3d"] = side_sparse
    out["paints"] = paints
    if lightest != "#ffffff":
        out["face_glow"] = lightest
    if glow != "#ffffff":
        out["glow"] = glow
    glow_gradient = sparse(complete(gradient([glow, mid]), "gradient"), "gradient")
    if glow_gradient:
        out["glow_gradient"] = glow_gradient
    if shadow != "#000000":
        out["shadow"] = shadow
    if lightest != "#ffffff":
        out["glint"] = lightest
    out["light_ramp"] = light_ramp(face, glow, shadow)
    orb_ramp = {"stops": [{"pos": 0, "color": sample(face, 0.1)}, {"pos": 0.55, "color": sample(face, 0.55)},
                          {"pos": 1, "color": sample(face, 0.95)}]}
    orb = {}
    s = sparse(complete(orb_ramp, "gradient"), "gradient")
    if s:
        orb["ramp"] = s
    if orb:
        out["orb"] = orb
    out["swatch"] = face + [dark, paint_color(accents[-1])]
    out["provenance"] = {"source": "jasperengine"}
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("anchors", help="a JSON file holding one scheme's anchors, or a list of them")
    parser.add_argument("--out", help="the .titlecolors file to write (one scheme; stdout without it)")
    parser.add_argument("--out-dir", help="the directory every scheme of a list is written to as <id>.titlecolors")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--schemes-dir", default=os.path.join(here, "..", "..", "editor", "resources", "titleart", "color-schemes"),
                        help="where gold.titlecolors and blue.titlecolors are read from (the metal accents)")
    args = parser.parse_args()
    with open(args.anchors, encoding="utf-8") as f:
        anchors = json.load(f)
    items = anchors if isinstance(anchors, list) else [anchors]
    if len(items) > 1 and not args.out_dir:
        raise SystemExit("a list of anchors needs --out-dir")
    for a in items:
        text = write_value(expand(a, args.schemes_dir)) + "\n"
        if args.out_dir:
            os.makedirs(args.out_dir, exist_ok=True)
            with open(os.path.join(args.out_dir, a["id"] + ".titlecolors"), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        elif args.out:
            with open(args.out, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        else:
            sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
