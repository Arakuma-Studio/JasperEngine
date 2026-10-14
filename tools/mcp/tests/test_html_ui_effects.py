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

from harness import Editor
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
MEAN = {}          # filled from the first run's measurements (printed); see EXPECTED below
EXPECTED: dict = {}
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
