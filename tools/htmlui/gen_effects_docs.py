#!/usr/bin/env python3
# Writes the effect catalogue of the HTML UI (phase-5 spec 3.13, 3.16):
#
#   docs/mcp/guide/html-ui-effects.md            every built-in effect by category: kind, summary,
#                                                source, cost, backdrop flag, a params table, a CSS
#                                                example and its thumbnail (served as
#                                                jasper://docs/html-ui-effects)
#   docs/mcp/guide/img/effects/<name>.png        the thumbnails: copies of the goldens
#                                                engine/tests/htmlui/golden/effects/<name>.t1.png
#   engine/shaderlib/effects/LICENSE.shaders-com.txt
#                                                the shaders.com MIT notice and the effects ported
#                                                from it (their @source lines)
#
#   python3 tools/htmlui/gen_effects_docs.py [--check]
#
# The manifests are read from engine/shaderlib/effects/*.frag with the grammar of the registry
# (what `jasperengine-editor ui-effects --json` prints for the built-in effects; reading the
# files keeps the generator usable without a build). --check exits 1 when an output is stale
# (nothing is written).

import os
import shlex
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EFFECTS = os.path.join(ROOT, "engine", "shaderlib", "effects")
GOLDENS = os.path.join(ROOT, "engine", "tests", "htmlui", "golden", "effects")
GUIDE = os.path.join(ROOT, "docs", "mcp", "guide")
CATALOGUE = os.path.join(GUIDE, "html-ui-effects.md")
THUMBNAILS = os.path.join(GUIDE, "img", "effects")
LICENSE = os.path.join(EFFECTS, "LICENSE.shaders-com.txt")

# the categories of the manifest grammar, in the catalogue's order
CATEGORIES = ["gradient", "noise", "pattern", "light", "energy", "space", "interactive", "glass", "blur", "stylize",
              "color", "distort", "transition"]

CATEGORY_TITLES = {
    "gradient": "Gradients", "noise": "Noise", "pattern": "Patterns", "light": "Light", "energy": "Energy",
    "space": "Space", "interactive": "Interactive (pointer)", "glass": "Glass", "blur": "Blur", "stylize": "Stylize",
    "color": "Colour", "distort": "Distortion", "transition": "Transitions",
}

TYPE_NAMES = {"number": "number", "integer": "integer", "percentage": "percentage", "length": "length", "angle": "angle",
              "time": "time", "color": "color", "position": "position", "enum": "enum", "boolean": "boolean",
              "palette": "palette"}

SHADERS_COM_LICENSE = """MIT License

Copyright (c) 2026 Shader Effects Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


# the effects/lib includes that are not shaders.com ports (Pavel Dobryakov's WebGL-Fluid-Simulation)
NOT_SHADERS_COM = {"fam_fluid_solver.glsl", "fam_fluid_look.glsl"}


def fail(message):
    raise SystemExit("gen_effects_docs.py: " + message)


def parse_manifest(path):
    """The manifest block of an effect file: a dict of its directives and its params."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().replace("\r", "").split("\n")
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    if i >= len(lines) or not lines[i].strip().startswith("#version"):
        fail("%s: an effect file starts with #version 450" % path)
    i += 1
    m = {"backdrop": False, "pointer": "none", "time": [], "suggest": [], "params": [], "source": "", "prepass": "",
         "margin": "", "geometry": "", "states": [], "passes": [], "warmup": "", "rate": "", "replay": "", "reseed": "",
         "presets": [],
         # an original effect whose file carries the kit's MIT notice follows parts of the kit
         "kit_notice": any("Shader Effects Inc." in line for line in lines)}
    while i < len(lines):
        stripped = lines[i].strip()
        if not stripped.startswith("//") or not stripped[2:].strip().startswith("@"):
            break
        body = stripped[2:].strip()[1:]
        directive, _, rest = body.partition(" ")
        rest = rest.strip()
        if directive == "param":
            m["params"].append(parse_param(path, rest))
        elif directive in ("effect", "kind", "category", "summary", "source", "cost", "prepass", "margin", "geometry"):
            m[directive] = rest
        elif directive == "backdrop":
            m["backdrop"] = rest == "yes"
        elif directive == "pointer":
            m["pointer"] = rest
        elif directive == "time":
            m["time"] = [p.strip() for p in rest.split(",") if p.strip()]
        elif directive == "suggest-for":
            m["suggest"] = rest.split()
        elif directive == "state":
            # phase 6: <name> <format> <size> [options]
            words = rest.split()
            size = words[2] if len(words) > 2 else ""
            if size == "aspect" and len(words) > 3:
                size += " " + words[3]
            m["states"].append({"name": words[0], "format": words[1], "size": size})
        elif directive in ("init", "step", "frame"):
            words = rest.split()
            m["passes"].append({"name": words[0], "init": directive == "init", "frame": directive == "frame",
                                "state": words[1] if len(words) > 1 else "", "options": words[2:]})
        elif directive == "preset":
            option, _, values = rest.partition(" ")
            m["presets"].append((option, values.strip()))
        elif directive in ("warmup", "rate", "replay", "reseed"):
            m[directive] = rest
        else:
            fail("%s: unknown directive @%s" % (path, directive))
        i += 1
    for key in ("effect", "kind", "category", "summary", "cost"):
        if not m.get(key):
            fail("%s: no @%s" % (path, key))
    return m


def parse_param(path, text):
    spec, _, description = text.partition(" -- ")
    try:
        tokens = shlex.split(spec)
    except ValueError as error:
        fail("%s: @param %s: %s" % (path, text, error))
    if len(tokens) < 3:
        fail("%s: @param %s: name, type and default" % (path, text))
    name, kind, default = tokens[0], tokens[1], tokens[2]
    rest = tokens[3:]
    param = {"name": name, "type": kind, "default": default, "range": "", "options": [], "description": description.strip()}
    if kind == "enum":
        if rest:
            param["options"] = rest[0].split("|")
    elif len(rest) >= 2:
        param["range"] = "%s to %s" % (rest[0], rest[1])
    return param


def effects():
    out = []
    for name in sorted(os.listdir(EFFECTS)):
        if not name.endswith(".frag") or name.startswith("_"):
            continue
        out.append(parse_manifest(os.path.join(EFFECTS, name)))
    return out


def example(m):
    """One CSS rule using the effect, with its first simple parameter at its default."""
    args = ""
    for p in m["params"]:
        if p["type"] in ("palette", "position", "color"):
            continue
        args = ", %s %s" % (p["name"], p["default"])
        break
    call = "effect(%s%s)" % (m["effect"], args)
    if m["kind"] == "paint":
        return ".card { background: #101218; background-image: %s; border-radius: 12px; }" % call
    if m["backdrop"]:
        return ".panel { backdrop-filter: %s; border-radius: 16px; }" % call
    return ".card { filter: %s; }" % call


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


FORMAT_BYTES = {"rgba8": 4, "rgba16f": 8, "rg16f": 4, "r16f": 2, "rgba32f": 16, "r32f": 4}


def stateful_text(m):
    """Phase 6: a stateful effect's state images, their memory for a fixed size, and its passes."""
    single = set()
    for st in m["states"]:
        writers = [p for p in m["passes"] if p["state"] == st["name"]]
        if writers and all("add" in p["options"] or "scatter" in p["options"] for p in writers):
            single.add(st["name"])
    images = []
    fixed_bytes = 0
    box = False
    for st in m["states"]:
        count = 1 if st["name"] in single else 2
        images.append("`%s` %s %s%s" % (st["name"], st["format"], st["size"], "" if count == 1 else " x2"))
        if "x" in st["size"] and not st["size"].startswith("box"):
            w, h = st["size"].split("x")
            fixed_bytes += int(w) * int(h) * FORMAT_BYTES.get(st["format"], 8) * count
        else:
            box = True
    steps = [p for p in m["passes"] if not p["init"] and not p["frame"]]
    frames = [p for p in m["passes"] if p["frame"]]
    memory = "%.1f MB" % (fixed_bytes / (1024.0 * 1024.0)) + (" plus the area-sized images" if box else "")
    text = "Stateful (a simulation per element, phase 6): state %s (%s); %d pass%s a step" % (
        ", ".join(images), memory, len(steps), "" if len(steps) == 1 else "es")
    if frames:
        text += " and %d a drawn frame" % len(frames)
    if m["rate"]:
        text += " at %s Hz" % m["rate"].split()[0]
    else:
        text += " at 60 Hz"
    if m["warmup"]:
        text += "; a warm-up of %s steps" % m["warmup"].split()[0]
    if m["reseed"]:
        text += "; `%s` restarts it" % m["reseed"]
    return text + "."


def catalogue_text(all_effects):
    by_category = {}
    for m in all_effects:
        by_category.setdefault(m["category"], []).append(m)
    unknown = sorted(set(by_category) - set(CATEGORIES))
    if unknown:
        fail("unknown categories: %s" % ", ".join(unknown))
    paint = sum(1 for m in all_effects if m["kind"] == "paint")
    lines = [
        "# HTML UI effect catalogue",
        "",
        "Generated by `tools/htmlui/gen_effects_docs.py` from the effects' manifests "
        "(`engine/shaderlib/effects/*.frag`); do not edit. How to use effects (where `effect()` goes, layering, "
        "parameters, animation, gradient text, backdrop effects, writing your own): the \"Effects\" section of "
        "`jasper://docs/html-ui`. `jasperengine-editor ui-effects --json` prints the same catalogue as JSON.",
        "",
        "%d effects: %d paint effects (images for `background-image`, `background` and `mask-image`; in `filter` and "
        "`backdrop-filter` they recolour the content) and %d filter effects (for `filter` and `backdrop-filter`)."
        % (len(all_effects), paint, len(all_effects) - paint),
        "",
        "Every effect also takes the universal parameters: `opacity` (percentage, default `100%`), `time-scale` "
        "(number in -10 to 10, default `1`), `time-offset` (time in -3600s to 3600s, default `0s`) and, for paint "
        "effects, `blend` (`normal`, `add`, `screen` or `multiply`). The thumbnails are the effects at t = 1.5 s in "
        "the test fixtures (pointer effects with the pointer at 70 %, 40 %; stateful effects replay their simulation "
        "to that time, the pointer moving along the harness's arc).",
        "",
        "| Effect | Kind | Category | Cost | Summary |",
        "|---|---|---|---|---|",
    ]
    for category in CATEGORIES:
        for m in sorted(by_category.get(category, []), key=lambda e: e["effect"]):
            kind = m["kind"] + (", backdrop" if m["backdrop"] else "")
            lines.append("| [`%s`](#%s) | %s | %s | %s | %s |" % (m["effect"], m["effect"], kind, category, m["cost"], cell(m["summary"])))
    for category in CATEGORIES:
        group = sorted(by_category.get(category, []), key=lambda e: e["effect"])
        if not group:
            continue
        lines += ["", "## %s" % CATEGORY_TITLES[category]]
        for m in group:
            lines += ["", "### `%s`" % m["effect"], ""]
            lines.append("![%s](img/effects/%s.png)" % (m["effect"], m["effect"]))
            lines.append("")
            lines.append(m["summary"] + ".")
            lines.append("")
            facts = ["Kind: %s" % m["kind"], "cost: %s" % m["cost"], "backdrop: %s" % ("yes" if m["backdrop"] else "no")]
            if m["pointer"] != "none":
                facts.append("pointer: %s" % m["pointer"])
            if m["time"]:
                facts.append("animated by: %s" % ", ".join("`%s`" % t for t in m["time"]))
            facts.append("source: %s" % m["source"])
            lines.append("; ".join(facts) + ".")
            if m["states"]:
                lines += ["", stateful_text(m)]
            if m["presets"]:
                lines += ["", "Presets (`preset <name>`: the parameters left at their defaults take its values):", ""]
                for option, values in m["presets"]:
                    lines.append("- `%s`: %s" % (option, ", ".join("`%s`" % v.strip() for v in values.split(","))))
            if m["suggest"]:
                lines += ["", "Also found as: %s." % ", ".join("`%s`" % s for s in m["suggest"])]
            lines += ["", "| Param | Type | Default | Range or options | Description |", "|---|---|---|---|---|"]
            for p in m["params"]:
                extent = " \\| ".join("`%s`" % o for o in p["options"]) if p["options"] else p["range"]
                lines.append("| `%s` | %s | `%s` | %s | %s |" % (p["name"], TYPE_NAMES.get(p["type"], p["type"]), cell(p["default"]),
                                                            extent or "", cell(p["description"])))
            if not m["params"]:
                lines.append("| (none) | | | | |")
            lines += ["", "```css", example(m), "```"]
    return "\n".join(lines) + "\n"


def license_text(all_effects):
    ported = [m for m in all_effects if m["source"].startswith("shaders.com")]
    adapted = [m for m in all_effects if not m["source"].startswith("shaders.com") and ("kit" in m["source"] or m["kit_notice"])]
    lines = [
        "Effects of the JasperEngine HTML UI ported from shaders.com",
        "",
        "These UI effects of engine/shaderlib/effects are ports of shaders.com effects",
        "(https://shaders.com/, https://github.com/shader-effects-inc/shaders, packages/core/src/shaders/<Name>)",
        "to GLSL, under the MIT license below:",
        "",
    ]
    width = max([len(m["effect"]) for m in ported] + [1])
    for m in sorted(ported, key=lambda e: e["effect"]):
        lines.append("  %s  %s" % (m["effect"].ljust(width), m["source"]))
    if adapted:
        lines += ["", "These effects are original but follow parts of the shaders.com kit:", ""]
        width = max(len(m["effect"]) for m in adapted)
        for m in sorted(adapted, key=lambda e: e["effect"]):
            lines.append("  %s  %s" % (m["effect"].ljust(width), m["source"]))
    others = [m for m in all_effects if not m["source"].startswith("shaders.com") and m["source"].startswith("port ")]
    families = sorted(name for name in os.listdir(os.path.join(EFFECTS, "lib"))
                      if name.startswith("fam_") and name.endswith(".glsl") and name not in NOT_SHADERS_COM)
    lines += [
        "",
        "The include library engine/shaderlib/effects/lib (jfx_color, jfx_hash, jfx_noise, jfx_sdf,",
        "jfx_blend, jfx_tone and the other jfx_*.glsl files) and these effect families' includes",
        "there port functions of the shaders.com kit (packages/core/src/gpu/kit) under the same",
        "license:",
        "",
    ]
    lines += ["  " + name for name in families]
    lines += [
        "",
        "Not from shaders.com: %s (Pavel Dobryakov's WebGL-Fluid-Simulation, MIT: see" % ", ".join(sorted(NOT_SHADERS_COM)),
        "LICENSE.webgl-fluid-simulation.txt)%s." % (
            "" if not others else ", and the effects ported from other projects (" +
            ", ".join("%s: %s" % (m["effect"], m["source"][len("port "):]) for m in sorted(others, key=lambda e: e["effect"])) + ")"),
        "",
        "Generated by tools/htmlui/gen_effects_docs.py from the effects' @source lines.",
        "",
        "-" * 78,
        "",
    ]
    return "\n".join(lines) + SHADERS_COM_LICENSE


def read_bytes(path):
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return f.read()


def main():
    check = "--check" in sys.argv[1:]
    all_effects = effects()
    outputs = {CATALOGUE: catalogue_text(all_effects).encode("utf-8"), LICENSE: license_text(all_effects).encode("utf-8")}
    for m in all_effects:
        golden = os.path.join(GOLDENS, m["effect"] + ".t1.png")
        data = read_bytes(golden)
        if data is None:
            fail("%s has no golden %s (run the golden driver first)" % (m["effect"], os.path.relpath(golden, ROOT)))
        outputs[os.path.join(THUMBNAILS, m["effect"] + ".png")] = data
    # thumbnails of effects that are gone
    stale = []
    if os.path.isdir(THUMBNAILS):
        for name in sorted(os.listdir(THUMBNAILS)):
            if name.endswith(".png") and os.path.join(THUMBNAILS, name) not in outputs:
                stale.append(os.path.join(THUMBNAILS, name))
    changed = [path for path, data in sorted(outputs.items()) if read_bytes(path) != data]
    if check:
        for path in changed + stale:
            print("%s is stale: run tools/htmlui/gen_effects_docs.py" % os.path.relpath(path, ROOT))
        if changed or stale:
            sys.exit(1)
        print("the effect catalogue is current (%d effects)" % len(all_effects))
        return
    for path in changed:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(outputs[path])
        print("wrote %s" % os.path.relpath(path, ROOT))
    for path in stale:
        os.remove(path)
        print("removed %s" % os.path.relpath(path, ROOT))
    if not changed and not stale:
        print("the effect catalogue is current (%d effects)" % len(all_effects))


if __name__ == "__main__":
    main()
