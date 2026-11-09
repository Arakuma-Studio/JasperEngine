#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes the Title Library's red controls (docs/specs/S84-title-library.md §6.14.4) into
tools/mcp/tests/fixtures/title-library-red/: per check, a bad document that fails it and its
fixed twin that passes, plus index.json ({file: check, twin}). The gate (test_title_library.py)
asserts each bad fails exactly its check against its twin; calibration (title-library calibrate)
reads the bad ones as the red side of every threshold.

    python3 -I tools/titleart/make_red_controls.py
"""

import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "tools" / "mcp" / "tests" / "fixtures" / "title-library-red"


def solid(color, **extra):
    m = {"fill": {"kind": "solid", "color": color}}
    m.update(extra)
    return m


def rim(width, color, **extra):
    r = {"width": width, "material": solid(color)}
    r.update(extra)
    return r


def doc(face=None, rims=None, effects=None, text="HERO", position=(0, 0), extra=None, look=None):
    layer = {"id": "00000001", "type": "text", "name": "Title", "text": text, "size": 200,
             "transform": {"position": list(position)},
             "style": {"face": face or solid("#ffffff"), "rims": rims or []}}
    if effects:
        layer["effects"] = effects
    if look:
        layer["look"] = look
    if extra:
        layer["style"].update(extra.get("style", {}))
        for k, v in extra.items():
            if k != "style":
                layer[k] = v
    return {"format": "jasperengine.titleart", "version": 1, "guid": "84" + "0" * 30,
            "canvas": {"width": 1280, "height": 640},
            "layers": [layer],
            "exposed": [{"name": "text", "type": "text", "targets": [{"layer": "00000001", "field": "text"}]}]}


RAINBOW = {"stops": [{"pos": 0, "color": "#ff0000"}, {"pos": 0.33, "color": "#00ff00"}, {"pos": 0.66, "color": "#0000ff"}, {"pos": 1, "color": "#ff00ff"}]}
GOLD = {"stops": [{"pos": 0, "color": "#fff4c8"}, {"pos": 0.5, "color": "#ffc040"}, {"pos": 1, "color": "#7a3a00"}]}

PAIRS = {
    "style_lint.contrast": (doc(face=solid("#ffffff"), rims=[rim(3, "#808080"), rim(3, "#8a8a8a")]),
                            doc(face=solid("#ffffff"), rims=[rim(3, "#202020"), rim(3, "#e0e0e0")])),
    "style_lint.hue_count": (doc(face=solid("#e01010"), rims=[rim(3, "#10c010"), rim(3, "#1030e0")]),
                             doc(face=solid("#e01010"), rims=[rim(3, "#0a0604"), rim(3, "#ffffff")])),
    "style_lint.rims_thin": (doc(rims=[rim(1, "#000000"), rim(1, "#ffd040")]), doc(rims=[rim(3, "#000000"), rim(5, "#ffd040")])),
    "style_lint.rims_thick": (doc(rims=[rim(8, "#000000"), rim(12, "#ffd040")]), doc(rims=[rim(4, "#000000"), rim(8, "#ffd040")])),
    "style_lint.hard_shadow": (doc(rims=[rim(6, "#000000")], effects=[{"id": "00000021", "type": "drop_shadow", "params": {"opacity": 0.95, "size": 2, "distance": 10, "color": "#000000"}}]),
                               doc(rims=[rim(6, "#000000")], effects=[{"id": "00000021", "type": "drop_shadow", "params": {"opacity": 0.6, "size": 14, "distance": 10, "color": "#000000"}}])),
    "style_lint.spectrum_span": (doc(text="HE\nRO", face={"fill": {"kind": "linear", "gradient": RAINBOW, "span": "layer"}}, rims=[rim(6, "#000000")]),
                                 doc(text="HE\nRO", face={"fill": {"kind": "linear", "gradient": RAINBOW, "span": "line"}}, rims=[rim(6, "#000000")])),
    "style_lint.noop_knockout": (doc(extra={"style": {"knockout": True}}), doc(face={"fill": {"kind": "solid", "color": "#000000"}, "opacity": 0}, rims=[rim(6, "#ffffff")], extra={"style": {"knockout": True}})),
    "style_lint.noop_erode": (doc(rims=[rim(6, "#000000")], extra={"style": {"erode": {"enabled": True, "amount": 0}}}),
                              doc(rims=[rim(6, "#000000")], extra={"style": {"erode": {"enabled": True, "amount": 0.5, "edge": 6}}})),
    "color_scheme_match": (doc(face={"fill": {"kind": "linear", "gradient": {"stops": [{"pos": 0, "color": "#e8f0a0"}, {"pos": 1, "color": "#5a6a10"}]}}, "finish": {"enabled": True, "gradient": {"stops": [{"pos": 0, "color": "#2a3000"}, {"pos": 1, "color": "#f0ffc0"}]}}},
                               rims=[rim(6, "#0a0604")], look={"id": "", "color_scheme": "gold", "owner": ""}),
                           None),
    # a metal that reads flat: a real ramp, but only about 1.5 L* once its finish is flat (the library's real metals span 18 and up)
    "metal_range": (doc(face={"fill": {"kind": "linear", "gradient": {"stops": [{"pos": 0, "color": "#a89060"}, {"pos": 1, "color": "#c8b078"}]}},
                             "finish": {"enabled": True, "gradient": {"stops": [{"pos": 0, "color": "#a89060"}, {"pos": 1, "color": "#c8b078"}]}}}, rims=[rim(6, "#0a0604")]),
                    doc(face={"fill": {"kind": "linear", "gradient": GOLD}, "bevel": {"enabled": True, "depth": 4}, "finish": {"enabled": True, "gradient": GOLD}}, rims=[rim(6, "#0a0604")])),
    "not_clipped": (doc(rims=[rim(6, "#000000")], position=(560, 0)), doc(rims=[rim(6, "#000000")])),
    "glyphs_present": (doc(text="HERO大", rims=[rim(6, "#000000")]), doc(rims=[rim(6, "#000000")])),
    "noop_effects": (doc(rims=[rim(6, "#000000")], effects=[{"id": "00000021", "type": "outer_glow", "params": {"opacity": 0}}]),
                     doc(rims=[rim(6, "#000000")], effects=[{"id": "00000021", "type": "outer_glow", "params": {"opacity": 0.8}}])),
    "legibility_contrast": (doc(face=solid("#101010")), doc(face=solid("#ffffff"), rims=[rim(6, "#000000")])),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    index = {}
    for check, (bad, good) in PAIRS.items():
        name = check.replace(".", "-")
        (OUT / f"{name}-bad.titleart").write_text(json.dumps(bad, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        entry = {"check": check, "bad": f"{name}-bad.titleart"}
        if good is not None:
            (OUT / f"{name}-good.titleart").write_text(json.dumps(good, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            entry["good"] = f"{name}-good.titleart"
        else:
            entry["good"] = None   # the twin is the bad one with its check's fix ops applied
        index[name] = entry
    (OUT / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(PAIRS)} red controls to {OUT}")


if __name__ == "__main__":
    main()
