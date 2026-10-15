"""UI effects in the editor through MCP (phase 5, p5-tooling, spec §5.5).

A document using the reference effects of p5-infra in every place effect() goes: a paint effect
as a background (`stripes`, the spec's `aurora`), a backdrop filter with its blur prepass
(`frosted-glass`), a filter (`pixelate`), gradient text (`stripes` with background-clip: text, the
spec's `holographic`), a hover transition of a pointer effect (`spotlight`, the spec's `sheen`) and
a project effect declared with @effect (`tint-wave`, its .frag including a .glsl next to it) in a
project whose assets directory is not the project root.

- set_ui_effect_time pins the clock: every effect region is drawn (its pixels vary) with the
  colours recorded here, a second screenshot is identical, the live clock moves the stripes.
- inspect_ui lists each element's effects with their parameters (fx:<name> in the outline);
  list_ui_effects filters by name, category and kind and includes the project effect.
- inspect_ui {file} (check --ui) reports effect-unknown, effect-unknown-param and
  effect-wrong-context with their line:col and suggestions.
- Hot reload: the .frag's colour changes the render; a GLSL error is reported by write_file
  (effect-shader-compile at the .frag line) and by check, the render recovers when fixed; a manifest
  error is effect-manifest at its line; a renamed .frag (also during play) is effect-file-missing
  until the CSS follows; a document using an effect only in Lua style or a @media block draws it.

Runs on a scratch copy of the fixture project (canvas 1280 x 720, assets in assets/).
"""

from __future__ import annotations

import asyncio
import math
import time

from harness import Editor, TestFailure
from htmlui_pixels import Shot

S: dict = {}

HTML = """<html>
<head>
    <title>effects</title>
    <link rel="stylesheet" href="effects.css"/>
</head>
<body>
    <div id="hero" class="hero"></div>
    <div id="behind" class="behind">
        <div class="b1"></div><div class="b2"></div><div class="b3"></div><div class="b4"></div>
    </div>
    <div id="glass" class="glass"></div>
    <div id="pix" class="pix">
        <div class="b1"></div><div class="b2"></div><div class="b3"></div><div class="b4"></div>
    </div>
    <h1 id="title" class="title">JASPER</h1>
    <button id="button" class="button">Play</button>
    <div id="tint" class="tint"></div>
</body>
</html>
"""

CSS = """@effect tint-wave { src: url(fx/tint-wave.frag); }

body { margin: 0; width: 1280px; height: 720px; background-color: #101218; font-family: sans-serif; }
.hero, .behind, .glass, .pix, .title, .button, .tint { position: absolute; display: block; margin: 0; }
.hero { left: 40px; top: 40px; width: 360px; height: 220px; border-radius: 12px;
        background: #0b0d14; background-image: effect(stripes, color-a #ff3355, color-b #2255ff, density 6, speed 0.5); }
.behind { left: 440px; top: 40px; width: 360px; height: 220px; }
.behind div, .pix div { position: absolute; width: 50%; height: 50%; }
.b1 { left: 0; top: 0; background-color: #e74c3c; }
.b2 { left: 50%; top: 0; background-color: #f1c40f; }
.b3 { left: 0; top: 50%; background-color: #2ecc71; }
.b4 { left: 50%; top: 50%; background-color: #3498db; }
.glass { left: 480px; top: 80px; width: 280px; height: 140px; border-radius: 16px;
         backdrop-filter: effect(frosted-glass, blur 14px, tint #ffffff22); }
.pix { left: 840px; top: 40px; width: 360px; height: 220px; filter: effect(pixelate, size 24px, gap 0.2); }
.title { left: 40px; top: 300px; width: 360px; height: 100px; font-size: 80px; font-weight: bold; color: transparent;
         background-image: effect(stripes, color-a #ffcc00, color-b #00ccff, density 4, speed 0.3); background-clip: text; }
.button { left: 440px; top: 300px; width: 360px; height: 220px; border: 0; border-radius: 20px; color: #ffffff;
          background-color: #202840;
          background-image: effect(spotlight, color #ffee88, radius 140px, intensity 0.3, idle-intensity 0.9);
          transition: background-image 300ms; }
.button:hover { background-image: effect(spotlight, color #ffee88, radius 140px, intensity 1, idle-intensity 0.9); }
.tint { left: 840px; top: 300px; width: 360px; height: 220px; background-color: #000000;
        background-image: effect(tint-wave, color #33ff88, speed 0.5); }
"""

FRAG = """#version 450
// @effect     tint-wave
// @kind       paint
// @category   gradient
// @summary    A tinted wave (a project effect of the MCP tests)
// @source     original
// @cost       low
// @time       speed
// @param      color color #3388ff -- Tint of the wave
// @param      speed number 1 -5 5 -- Waves per second

#include "effects/lib/jfx_common.glsl"
#include "local-wave.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    vec3 base = vec3(1.0, 1.0, 1.0);
    return vec4(base * params.color.rgb * local_wave(c.uv.x, c.anim.x), params.color.a);
}

#include "effects/lib/main_paint.glsl"
"""

GLSL = """// a project include of tint-wave.frag, next to it
float local_wave(float x, float t){
    return 0.55 + 0.45 * sin((x * 2.0 + t) * 6.2831853);
}
"""

# the regions of the effects (canvas px) and their mean colour at t = 1.5 s, measured on
# glcore/llvmpipe (tolerance below); `sd` is the minimum standard deviation of the region
REGIONS = {
    "hero": {"rect": (44, 44, 396, 256), "sd": 30},
    "glass": {"rect": (490, 90, 750, 210), "sd": 8},
    "pix": {"rect": (840, 40, 1200, 260), "sd": 30},
    "title": {"rect": (40, 300, 400, 400), "sd": 20},
    "button": {"rect": (444, 304, 796, 516), "sd": 6},
    "tint": {"rect": (840, 300, 1200, 520), "sd": 15},
}
EXPECTED = {"hero": (146, 68, 171), "glass": (168, 176, 130), "pix": (96, 98, 65), "title": (41, 61, 50),
            "button": (114, 113, 90), "tint": (34, 185, 97)}
MEAN_TOL = 18


def write(ed: Editor, rel: str, text: str) -> None:
    time.sleep(0.02)
    path = ed.project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


async def shot(ed: Editor, label: str) -> Shot:
    raw = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280})
    ed.check(not raw.is_error and raw.images, f"{label}: no screenshot: {raw.text[:300]}")
    png = raw.images[0][0]
    (ed.scratch.root / f"html-ui-effects-{label}.png").write_bytes(png)
    return Shot(png, raw.value)


def stats(image: Shot, rect) -> tuple[tuple[float, float, float], float]:
    pixels = list(image.region(*rect))
    n = max(1, len(pixels))
    mean = tuple(sum(p[i] for p in pixels) / n for i in range(3))
    var = sum(sum((p[i] - mean[i]) ** 2 for i in range(3)) / 3 for p in pixels) / n
    return mean, math.sqrt(var)


def same(a: Shot, b: Shot, rect) -> bool:
    return list(a.region(*rect)) == list(b.region(*rect))


async def console_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(None, types=["error"], limit=1000)
    return [e.get("message", "") for e in entries]


async def test_setup(ed: Editor):
    write(ed, "assets/ui/fx/tint-wave.frag", FRAG)
    write(ed, "assets/ui/fx/local-wave.glsl", GLSL)
    write(ed, "assets/ui/effects.html", HTML)
    write(ed, "assets/ui/effects.css", CSS)
    made = await ed.call("create_scene", name="HtmlUiEffects", type="UI")
    S["scene"] = made["scene"]["id"]
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/effects.html", name="Effects")
    S["entity"] = r["entity"]["id"]
    ed.check_eq(r["diagnostics"], [], "the document loads clean")
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiEffects")


async def test_pinned_clock_renders(ed: Editor):
    r = await ed.call("set_ui_effect_time", time=1.5)
    ed.check_eq(r["time"], 1.5, f"pinned: {r}")
    # the project effect compiles on first use (async in the editor): wait for its region
    deadline = time.monotonic() + 60
    while True:
        image = await shot(ed, "t1")
        _, sd = stats(image, REGIONS["tint"]["rect"])
        if sd >= REGIONS["tint"]["sd"] or time.monotonic() > deadline:
            break
        await asyncio.sleep(1.0)
    S["t1"] = image
    report = []
    for name, region in REGIONS.items():
        mean, sd = stats(image, region["rect"])
        report.append(f"{name}: mean ({mean[0]:.0f}, {mean[1]:.0f}, {mean[2]:.0f}) sd {sd:.1f}")
        ed.check(sd >= region["sd"], f"{name}: the region is flat (sd {sd:.1f} < {region['sd']}): the effect did not draw")
        if name in EXPECTED:
            want = EXPECTED[name]
            ed.check(all(abs(mean[i] - want[i]) <= MEAN_TOL for i in range(3)),
                     f"{name}: mean colour {tuple(round(v) for v in mean)}, expected {want} +-{MEAN_TOL}")
    print("     [effects] " + "; ".join(report))
    errors = [e for e in await console_errors(ed) if "[HTML UI]" in e or "hader" in e]
    ed.check(not errors, f"errors: {errors}")


async def test_determinism_and_live_clock(ed: Editor):
    again = await shot(ed, "t1-again")
    for name, region in REGIONS.items():
        ed.check(same(S["t1"], again, region["rect"]), f"{name}: a second render at the pinned time differs")
    r = await ed.call("set_ui_effect_time", time=None)
    ed.check(r["time"] is None, f"live: {r}")
    await asyncio.sleep(0.5)
    live = await shot(ed, "live")
    ed.check(not same(S["t1"], live, REGIONS["hero"]["rect"]), "the live clock moves the stripes")
    await ed.call("set_ui_effect_time", time=1.5)


def line_col(text: str, needle: str, occurrence: int = 1) -> tuple[int, int]:
    seen = 0
    for i, line in enumerate(text.split("\n")):
        if needle in line:
            seen += 1
            if seen == occurrence:
                return i + 1, line.index(needle) + 1
    raise AssertionError(f"no line with {needle!r}")


async def inspect(ed: Editor, selector: str) -> tuple[dict, str]:
    raw = await ed.call_raw("inspect_ui", {"scene": S["scene"], "entity": S["entity"], "selector": selector, "depth": 0})
    ed.check(not raw.is_error, raw.text)
    elements = raw.value["elements"]
    ed.check_eq(len(elements), 1, f"{selector} matches one element")
    return elements[0], raw.text


async def test_inspect_ui_effects(ed: Editor):
    hero, text = await inspect(ed, ".hero")
    ed.check("fx:stripes" in text, f"the outline names the effect: {text}")
    ed.check_eq([(e["property"], e["index"], e["name"], e["form"]) for e in hero["effects"]],
                [("background-image", 1, "stripes", "paint")], "the hero's effects")
    params = hero["effects"][0]["params"]
    ed.check_eq((params.get("color-a"), params.get("color-b"), params.get("density"), params.get("speed")),
                ("#ff3355ff", "#2255ffff", "6", "0.5"), f"canonical parameter values: {params}")
    for universal in ("opacity", "time-scale", "time-offset", "blend"):
        ed.check(universal in params, f"the universal parameter {universal} is listed: {params}")
    cases = {".glass": ("backdrop-filter", "frosted-glass", "filter"), ".pix": ("filter", "pixelate", "filter"),
             ".title": ("background-image", "stripes", "filter"), ".button": ("background-image", "spotlight", "paint"),
             ".tint": ("background-image", "tint-wave", "paint")}
    for selector, (prop, name, form) in cases.items():
        element, text = await inspect(ed, selector)
        ed.check_eq([(e["property"], e["name"], e["form"]) for e in element.get("effects", [])], [(prop, name, form)], selector)
        ed.check(f"fx:{name}" in text, f"{selector}: fx:{name} in {text}")
    behind, text = await inspect(ed, ".behind")
    ed.check("effects" not in behind and "fx:" not in text, f"no effects key without effects: {behind}")


async def test_list_ui_effects(ed: Editor):
    write(ed, "assets/ui/fx/spare-glow.frag", FRAG.replace("tint-wave", "spare-glow").replace('#include "local-wave.glsl"\n', "")
          .replace("local_wave(c.uv.x, c.anim.x)", "1.0"))
    everything = await ed.call("list_ui_effects")
    names = [e["name"] for e in everything["effects"]]
    ed.check_eq(everything["count"], len(names), "count")
    for name in ("cursor-trail", "frosted-glass", "pixelate", "spotlight", "stripes", "tint-wave", "spare-glow"):
        ed.check(name in names, f"{name} listed: {names}")
    ed.check_eq(names, sorted(names), "sorted by name")
    builtin = [e for e in everything["effects"] if e["builtin"]]
    ed.check(len(builtin) >= 5 and all(e["thumbnail"].startswith("docs/mcp/guide/img/effects/") for e in builtin), "thumbnails")
    if len(names) > 12:
        ed.check(all("params" not in e and "param_names" in e for e in everything["effects"]), "a long list leaves the params out")

    glass = await ed.call("list_ui_effects", category="glass")
    ed.check("frosted-glass" in [e["name"] for e in glass["effects"]], f"glass: {glass}")
    ed.check(all(e["category"] == "glass" for e in glass["effects"]), "only glass")
    filters = await ed.call("list_ui_effects", kind="filter")
    ed.check(filters["count"] >= 2 and all(e["kind"] == "filter" for e in filters["effects"]), f"filters: {filters}")

    raw = await ed.call_raw("list_ui_effects", {"name": "tint-wave"})
    one = raw.value["effects"]
    ed.check_eq(len(one), 1, "one effect by name")
    tint = one[0]
    ed.check_eq((tint["builtin"], tint["declared"], tint["path"], tint["kind"]),
                (False, True, "assets/ui/fx/tint-wave.frag", "paint"), f"the project effect: {tint}")
    ed.check_eq([(p["name"], p["type"], p["default"]) for p in tint["params"]], [("color", "color", "#3388ff"), ("speed", "number", "1")],
                "its parameters")
    ed.check("- speed number 1 [-5..5]: Waves per second" in raw.text, raw.text)
    spare = (await ed.call("list_ui_effects", name="spare-glow"))["effects"][0]
    ed.check_eq((spare["builtin"], spare["declared"]), (False, False), f"a .frag no document declares: {spare}")

    error = await ed.call("list_ui_effects", name="stripez", expect_error=True)
    ed.check("stripes" in error, f"the nearest name: {error}")
    error = await ed.call("list_ui_effects", category="nope", expect_error=True)
    ed.check("glass" in error and "pattern" in error, f"the categories: {error}")


BAD_CSS = """.a { background-image: effect(glassmorphism); }
.b { background-image: effect(stripes, speeed 2); }
.c { color: effect(pixelate); }
"""


async def test_check_ui_effect_diagnostics(ed: Editor):
    write(ed, "assets/ui/bad.css", BAD_CSS)
    r = await ed.call("inspect_ui", file="assets/ui/bad.css")
    found = {d["code"]: d for d in r["diagnostics"]}
    cases = [("HTMLUI-FX-0001", "glassmorphism", "frosted-glass"), ("HTMLUI-FX-0004", "speeed", "speed"),
             ("HTMLUI-FX-0002", "effect(pixelate)", "filter")]
    for code, needle, suggestion in cases:
        ed.check(code in found, f"{code} reported: {r['diagnostics']}")
        d = found[code]
        line, col = line_col(BAD_CSS, needle)
        ed.check_eq((d["path"], d["line"], d["col"]), ("assets/ui/bad.css", line, col), f"{code} at the {needle}")
        ed.check(suggestion in d.get("suggestion", ""), f"{code} suggests {suggestion}: {d}")
    ed.check_eq(found["HTMLUI-FX-0001"]["severity"], "error", "effect-unknown is an error")
    ed.check_eq(found["HTMLUI-FX-0004"]["severity"], "warning", "effect-unknown-param is a warning")
    (ed.project / "assets/ui/bad.css").unlink()


async def wait_tint(ed: Editor, label: str, predicate, seconds: float = 60.0) -> Shot:
    """Screenshots until the tint region satisfies predicate(mean, sd)."""
    deadline = time.monotonic() + seconds
    while True:
        image = await shot(ed, label)
        mean, sd = stats(image, REGIONS["tint"]["rect"])
        if predicate(mean, sd):
            return image
        if time.monotonic() > deadline:
            ed.fail(f"{label}: the tint region stays at mean {tuple(round(v) for v in mean)} sd {sd:.1f}")
        await asyncio.sleep(0.5)


async def test_hot_reload_project_effect(ed: Editor):
    before, _ = stats(S["t1"], REGIONS["tint"]["rect"])
    # an external edit of the .frag: the documents reload and the variants compile again
    write(ed, "assets/ui/fx/tint-wave.frag", FRAG.replace("vec3(1.0, 1.0, 1.0)", "vec3(1.0, 0.1, 1.0)"))
    changed = await wait_tint(ed, "frag-edited", lambda mean, sd: sd > 5 and mean[1] < before[1] - 60)
    S["edited"] = stats(changed, REGIONS["tint"]["rect"])[0]

    # a GLSL error: write_file compiles it (each form) and says where
    broken = FRAG.replace("vec3(1.0, 1.0, 1.0)", "vec3(1.0, 0.1)")
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/tint-wave.frag", "content": broken})
    ed.check(not raw.is_error, raw.text)
    results = raw.value.get("shader_check", [])
    failed = [r for r in results if not r.get("ok")]
    ed.check(failed, f"the shader check fails: {raw.text}")
    line, _ = line_col(broken, "vec3(1.0, 0.1)")
    for r in failed:
        ed.check_eq((r.get("effect"), r.get("code")), ("tint-wave", "HTMLUI-FX-0015"), f"effect-shader-compile: {r}")
        ed.check(any(e["line"] == line for e in r.get("errors", [])), f"the error at the .frag line {line}: {r}")
    ed.check("[HTMLUI-FX-0015]" in raw.text and f"line {line}:" in raw.text, raw.text)
    # the effect does not draw while it does not compile (the black background shows)
    await wait_tint(ed, "frag-broken", lambda mean, sd: sd < 3)

    # fixed: the render recovers
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/tint-wave.frag", "content": FRAG})
    ed.check(not raw.is_error and all(r.get("ok") for r in raw.value.get("shader_check", [])) and "Shader compiles." in raw.text, raw.text)
    recovered = await wait_tint(ed, "frag-fixed", lambda mean, sd: sd >= REGIONS["tint"]["sd"] and abs(mean[1] - before[1]) <= MEAN_TOL)
    ed.check(same(recovered, S["t1"], REGIONS["tint"]["rect"]), "the fixed effect renders as before")


async def test_include_edit_reloads(ed: Editor):
    """An include of the project effect (fx/local-wave.glsl) is watched like its .frag."""
    before, _ = stats(S["t1"], REGIONS["tint"]["rect"])
    write(ed, "assets/ui/fx/local-wave.glsl", GLSL.replace("0.55 + 0.45 *", "0.1 + 0.1 *"))
    darker = await wait_tint(ed, "include-edited", lambda mean, sd: sd > 2 and mean[1] < before[1] - 60)
    S["include-edited"] = darker
    write(ed, "assets/ui/fx/local-wave.glsl", GLSL)
    restored = await wait_tint(ed, "include-restored", lambda mean, sd: abs(mean[1] - before[1]) <= MEAN_TOL and sd >= REGIONS["tint"]["sd"])
    ed.check(same(restored, S["t1"], REGIONS["tint"]["rect"]), "the restored include renders as before")


async def test_manifest_error(ed: Editor):
    bad = FRAG.replace("// @param      speed number 1 -5 5", "// @param      speed number 9 -5 5")
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/tint-wave.frag", "content": bad})
    results = raw.value.get("shader_check", [])
    line, _ = line_col(bad, "@param      speed")
    ed.check(results and all(r.get("code") == "HTMLUI-FX-0014" and not r["ok"] for r in results), f"effect-manifest: {raw.text}")
    ed.check(any(e["line"] == line for e in results[0].get("errors", [])), f"at the manifest line {line}: {results}")
    # the document reloads and reports it at the .frag line; the effect is not drawn
    await wait_tint(ed, "manifest-broken", lambda mean, sd: sd < 3)
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=".tint", depth=0)
    diags = r.get("diagnostics", [])
    ed.check(any(d.get("code") == "HTMLUI-FX-0014" and d.get("path") == "assets/ui/fx/tint-wave.frag" and d.get("line") == line
                 for d in diags), f"the live document reports effect-manifest at the .frag line: {diags}")
    await ed.call("write_file", path="assets/ui/fx/tint-wave.frag", content=FRAG)
    await wait_tint(ed, "manifest-fixed", lambda mean, sd: sd >= REGIONS["tint"]["sd"])


async def test_rename_frag_during_play(ed: Editor):
    await ed.call("play", action="start", scene=S["scene"])
    try:
        await ed.call("set_ui_effect_time", time=1.5)
        await wait_tint(ed, "play", lambda mean, sd: sd >= REGIONS["tint"]["sd"])
        # renamed: the document reports the missing file and draws without the effect
        (ed.project / "assets/ui/fx/tint-wave.frag").rename(ed.project / "assets/ui/fx/tint-wave2.frag")
        await wait_tint(ed, "play-renamed", lambda mean, sd: sd < 3)
        r = await ed.call("inspect_ui", entity=S["entity"], selector=".tint", depth=0)
        text = str(r)
        ed.check("HTMLUI-FX-0013" in text, f"effect-file-missing while the .frag is gone: {text[:3000]}")
        # the CSS follows the rename: drawn again, in play
        write(ed, "assets/ui/effects.css", CSS.replace("fx/tint-wave.frag", "fx/tint-wave2.frag"))
        await wait_tint(ed, "play-followed", lambda mean, sd: sd >= REGIONS["tint"]["sd"])
    finally:
        await ed.call("play", action="stop")
    # At once, while the edit-mode document is still reloading for the CSS edited during play: the
    # files written now are not taken as seen by that reload (UIDocumentSystem stamps a document's
    # files before reading them). The effect declared during play from the renamed file is gone
    # with it, and the document, whose CSS is back to the text of a cached compile, declares its own.
    (ed.project / "assets/ui/fx/tint-wave2.frag").rename(ed.project / "assets/ui/fx/tint-wave.frag")
    write(ed, "assets/ui/effects.css", CSS)
    try:
        await wait_tint(ed, "edit-restored", lambda mean, sd: sd >= REGIONS["tint"]["sd"])
    except TestFailure as failure:
        r = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=".tint", depth=0)
        effects = await ed.call("list_ui_effects", name="tint-wave")
        raise TestFailure(f"{failure}\nthe document: {str(r)[:3000]}\nthe effect: {str(effects)[:1500]}") from None


async def test_no_errors_left(ed: Editor):
    await ed.call("set_ui_effect_time", time=None)
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=".tint", depth=0)
    ed.check(r["document"]["diagnostics_count"]["error"] == 0, f"the document is clean again: {r['document']}")
    await ed.check_alive()
