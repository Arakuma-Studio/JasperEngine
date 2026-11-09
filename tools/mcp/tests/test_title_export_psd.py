"""S87 layered PSD export (docs/specs/S87-title-localisation-interchange.md R39, R40, R44, AC20;
jasper://docs/title-art "Import and export"): export_title format psd, read back by
tools/mcp/psdread.py (an independent standard-library reader written from the Adobe document, not
from the engine's C++): the header, section lengths, the layer records with their `luni` names,
blend keys, clipping and visibility, the group nesting from the section dividers, PackBits (8-bit)
and ZIP with prediction (16-bit) channel data, the merged image equal to the PNG export of the same
frame, the embedded ICC profile laid out as §6.10, per-language files and byte determinism. Red
control: one flipped length byte makes psdread fail naming the offset.
"""

import json

import psdread
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title PSD Export"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = "assets/titles/layered.titleart"
    await ed.call("edit_title", path=path, create={"text": "WIN", "canvas": [512, 256]}, bake="never")
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_rim", "layer": "Title", "values": {"width": 6}},
        {"op": "add_effect", "layer": "Title", "type": "drop_shadow"},
        {"op": "add_layer", "from": {"type": "shape", "name": "Shine", "size": 90, "clip": True, "blend": "screen"}},
        {"op": "add_layer", "from": {"type": "shape", "name": "Plate", "size": 60, "visible": False, "blend": "multiply"}},
        {"op": "group", "layers": ["Plate"], "name": "Back"},
        {"op": "add_layer", "from": {"type": "adjustment", "name": "Invert", "opacity": 0.25, "adjustment": {"type": "invert"}}},
        {"op": "add_language", "code": "de"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"},
    ])
    S["path"] = path


def names_of(psd: dict, nodes: list) -> list:
    out = []
    for n in nodes:
        layer = psd["layers"][n["layer"]]
        out.append((layer["name"], names_of(psd, n["children"])) if layer["section"] in (1, 2) else layer["name"])
    return out


def layer_named(psd: dict, name: str) -> dict:
    for layer in psd["layers"]:
        if layer["name"] == name:
            return layer
    raise AssertionError(f"no layer {name!r} in {[l['name'] for l in psd['layers']]}")


async def test_psd_layers_and_merged(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="psd", options={"overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check_eq(r["files"], ["Recordings/Title Art/layered/layered.psd"], "the default folder and name")
    ed.check(any("psd_blending" in w for w in r["warnings"]), "the sRGB / linear blending warning, once")
    data = (ed.project / r["files"][0]).read_bytes()
    psd = psdread.read(data)
    ed.check_eq((psd["version"], psd["depth"], psd["mode"]), (1, 8, 3), "PSD v1, 8-bit RGB")
    ed.check_eq(psd["icc"]["description"], "sRGB (JasperEngine)", "our generated profile, §6.10's layout")
    ed.check(all(abs(a - b) < 1e-4 for a, b in zip(psd["icc"]["white"], [0.9642, 1.0, 0.8249])), "its matrix sums to D50")
    ed.check_eq(names_of(psd, psd["tree"]), ["Title", "Shine", ("Back", ["Plate"]), "Invert"], "the layer tree, bottom first")
    shine = layer_named(psd, "Shine")
    ed.check(shine["clipping"], "Shine clips to Title")
    ed.check_eq(shine["blend"], "scrn", "its blend key")
    plate = layer_named(psd, "Plate")
    ed.check(not plate["visible"], "a hidden layer stays a hidden record")
    ed.check_eq(plate["blend"], "mul ", "multiply")
    invert = layer_named(psd, "Invert")
    ed.check("nvrt" in invert["keys"], "a native invert adjustment layer")
    ed.check_eq(invert["opacity"], 64, "its opacity on the record")
    title = layer_named(psd, "Title")
    ed.check(title["right"] > title["left"], "the title has pixels")
    # the merged image is the true render: the PNG export of the same frame
    r2 = await ed.call("export_title", path=S["path"], format="png", options={"overwrite": True})
    width, height, rgba = pngdiff.decode((ed.project / r2["files"][0]).read_bytes())
    ed.check_eq((width, height), (psd["width"], psd["height"]), "the same frame as the PNG export")
    merged = psd["merged"]
    plane = width * height
    worst = 0
    for i in range(0, plane, 7):
        a = merged[3 * plane + i]
        if a == 0:
            continue
        for c in range(3):
            worst = max(worst, abs(merged[c * plane + i] - rgba[i * 4 + c]))
        worst = max(worst, abs(a - rgba[i * 4 + 3]))
    ed.check(worst <= 2, f"the merged image equals the PNG export (worst {worst})")
    # the channels decode to the layer's own render: the PNG part of the same frame, PackBits
    # here and ZIP with prediction in the 16-bit file
    r3 = await ed.call("export_title", path=S["path"], format="png", options={"parts": True, "overwrite": True})
    manifest = json.loads((ed.project / "Recordings/Title Art/layered/layered.parts.json").read_text())
    part = next(p for p in manifest["parts"] if p["name"] == "Title")
    pw, ph, part_rgba = pngdiff.decode((ed.project / "Recordings/Title Art/layered" / part["file"]).read_bytes())
    ed.check(r3.get("ok") and (pw, ph) == (width, height), "the parts share the PSD's frame")
    r4 = await ed.call("export_title", path=S["path"], format="psd", options={"bit_depth": 16, "overwrite": True})
    deep = psdread.read((ed.project / r4["files"][0]).read_bytes())
    deep_title = layer_named(deep, "Title")
    ed.check("Lr16" not in deep_title["keys"] and deep_title["planes"], "16-bit channels decoded")
    for layer, step, name in ((title, 1, "PackBits"), (deep_title, 2, "ZIP with prediction")):
        lw = layer["right"] - layer["left"]
        worst, seen = 0, 0
        for y in range(layer["top"], layer["bottom"], 3):
            for x in range(layer["left"], layer["right"], 3):
                i = (y - layer["top"]) * lw + (x - layer["left"])
                a = layer["planes"][-1][i * step]
                if a == 0:
                    continue
                seen += 1
                p = (y * width + x) * 4
                worst = max(worst, abs(a - part_rgba[p + 3]), *(abs(layer["planes"][c][i * step] - part_rgba[p + c]) for c in range(3)))
        ed.check(seen > 0 and worst <= 2, f"{name} channels equal the layer's render (worst {worst} over {seen} samples)")


async def test_psd_16bit_languages_determinism(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="psd", languages="all", options={"bit_depth": 16, "overwrite": True})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check_eq(sorted(r["files"]), ["Recordings/Title Art/layered/layered.de.psd", "Recordings/Title Art/layered/layered.en.psd"],
                "one PSD per language")
    first = (ed.project / "Recordings/Title Art/layered/layered.de.psd").read_bytes()
    psd = psdread.read(first)
    ed.check_eq(psd["depth"], 16, "16-bit")
    ed.check(psd["layers"], "16-bit layers read from Lr16 (ZIP with prediction)")
    r = await ed.call("export_title", path=S["path"], format="psd", languages=["de"], options={"bit_depth": 16, "overwrite": True})
    second = (ed.project / r["files"][0]).read_bytes()
    ed.check(first == second, "the same document gives the same bytes")


async def test_psd_red_control_and_refusals(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=S["path"], format="psd", options={"overwrite": True})
    data = bytearray((ed.project / r["files"][0]).read_bytes())
    # the layer and mask section's length: after the header, the colour mode data and the resources
    at = 26
    at += 4 + int.from_bytes(data[at:at + 4], "big")
    at += 4 + int.from_bytes(data[at:at + 4], "big")
    data[at + 1] ^= 0x40
    try:
        psdread.read(bytes(data))
        ed.check(False, "a corrupted length must fail")
    except psdread.PsdError as e:
        ed.check_in("offset", str(e), "psdread names the offset")
    msg = await ed.check_error("export_title", path=S["path"], format="psd", options={"alpha": "premultiplied"})
    ed.check_in("straight alpha", msg, "a PSD stores straight alpha")
    msg = await ed.check_error("export_title", path=S["path"], format="psd", options={"parts": True})
    ed.check_in("bad_option", msg, "parts is the png format's")
