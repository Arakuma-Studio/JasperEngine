"""UI effects in the editor through MCP (phase 5, p5-tooling, spec §5.5).

A document using the catalogue's effects in every place effect() goes, as §5.5 lists them: a paint
effect as a background (`aurora`, the spec's hero), a backdrop filter with its blur prepass
(`frosted-glass`), a filter (`pixelate`), gradient text (`holographic` with background-clip: text),
a hover transition of a paint effect (`sheen`) and a project effect declared with @effect
(`tint-wave`, its .frag including a .glsl next to it) in a project whose assets directory is not
the project root.

- set_ui_effect_time pins the clock: a second screenshot is identical, and the live clock
  moves the aurora.
- inspect_ui lists each element's effects with their parameters (fx:<name> in the outline);
  list_ui_effects discovers built-in and project effects, and
  filters by name, category (glass: the four glass effects) and kind.
- In play, the pointer over the button transitions its sheen to the hover intensity.
- inspect_ui {file} (check --ui) reports effect-unknown, effect-unknown-param and
  effect-wrong-context with their line:col and suggestions.
- Hot reload: the .frag's colour changes the render; a GLSL error is reported by write_file
  (effect-shader-compile at the .frag line) and by check, the render recovers when fixed; a manifest
  error is effect-manifest at its line; a renamed .frag (also during play) is effect-file-missing
  until the CSS follows; a document using an effect only in Lua style or a @media block draws it.
- Stateful effects (phase 6, spec 4.5): a second document with `fog`, `ink-flow` and `time-trail`:
  pinned screenshots are identical, the live clock moves the fog, inspect_ui reports each
  simulation's state, and in play `eval_lua` splats into `ink-flow` (`el:effectSplat{...}`) and its
  region changes, deterministically under the pinned clock.

Runs on a scratch copy of the fixture project (canvas 1280 x 720, assets in assets/).
"""

from __future__ import annotations

import asyncio
import math
import os
import time

import harness
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
        background: #0b0d14; background-image: effect(aurora, speed 2, color-b #44ccff); }
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
         background-image: effect(holographic, speed 0.6); background-clip: text; }
.button { left: 440px; top: 300px; width: 360px; height: 220px; border: 0; border-radius: 20px; color: #ffffff;
          background-color: #202840;
          background-image: effect(sheen, intensity 0.3); transition: background-image 300ms; }
.button:hover { background-image: effect(sheen, intensity 0.9); }
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

# Effect regions (canvas px); non-flat pixels prove the configured effect drew content.
REGIONS = {
    "hero": {"rect": (44, 44, 396, 256), "sd": 30},
    "glass": {"rect": (490, 90, 750, 210), "sd": 8},
    "pix": {"rect": (840, 40, 1200, 260), "sd": 30},
    "title": {"rect": (40, 300, 400, 400), "sd": 20},
    "button": {"rect": (444, 304, 796, 516), "sd": 12},
    "tint": {"rect": (840, 300, 1200, 520), "sd": 15},
}


def write(ed: Editor, rel: str, text: str) -> None:
    path = ed.project / rel
    before = path.stat().st_mtime_ns if path.exists() else None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if before is not None and path.stat().st_mtime_ns == before:
        # The dependency watcher observes timestamps, so distinguish a same-tick edit directly.
        os.utime(path, ns=(path.stat().st_atime_ns, before + 1_000_000_000))


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


async def setup(ed: Editor):
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


async def test_effect_clock_controls_rendering(ed: Editor):
    await setup(ed)
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
    ed.check(sd >= REGIONS["tint"]["sd"], "the project effect compiled and rendered")
    errors = [e for e in await console_errors(ed) if "[HTML UI]" in e or "hader" in e]
    ed.check(not errors, f"errors: {errors}")

    again = await shot(ed, "t1-again")
    for name, region in REGIONS.items():
        ed.check(same(S["t1"], again, region["rect"]), f"{name}: a second render at the pinned time differs")
    r = await ed.call("set_ui_effect_time", time=None)
    ed.check(r["time"] is None, f"live: {r}")
    await ed.call("wait", frames=2)
    live = await shot(ed, "live")
    ed.check(not same(S["t1"], live, REGIONS["hero"]["rect"]), "the live clock moves the aurora")
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
    ed.check("fx:aurora" in text, f"the outline names the effect: {text}")
    ed.check_eq([(e["property"], e["index"], e["name"], e["form"]) for e in hero["effects"]],
                [("background-image", 1, "aurora", "paint")], "the hero's effects")
    params = hero["effects"][0]["params"]
    # the given ones and the manifest's defaults, in canonical form
    ed.check_eq((params.get("speed"), params.get("color-b"), params.get("color-a"), params.get("curtains"), params.get("center")),
                ("2", "#44ccffff", "#a533f8ff", "4", "50% 0%"), f"canonical parameter values: {params}")
    for universal in ("opacity", "time-scale", "time-offset", "blend"):
        ed.check(universal in params, f"the universal parameter {universal} is listed: {params}")
    cases = {".glass": ("backdrop-filter", "frosted-glass", "filter"), ".pix": ("filter", "pixelate", "filter"),
             ".title": ("background-image", "holographic", "filter"), ".button": ("background-image", "sheen", "paint"),
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
    for name in ("aurora", "cursor-trail", "frosted-glass", "holographic", "pixelate", "sheen", "spotlight", "stripes", "tint-wave",
                 "spare-glow"):
        ed.check(name in names, f"{name} listed: {names}")
    ed.check_eq(names, sorted(names), "sorted by name")
    builtin = [e for e in everything["effects"] if e["builtin"]]
    glass = await ed.call("list_ui_effects", category="glass")
    ed.check_eq(sorted(e["name"] for e in glass["effects"]), ["fluted-glass", "frosted-glass", "glass-tiles", "liquid-glass"],
                f"glass: {glass}")
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
    spare = (await ed.call("list_ui_effects", name="spare-glow"))["effects"][0]
    ed.check_eq((spare["builtin"], spare["declared"]), (False, False), f"a .frag no document declares: {spare}")

    error = await ed.call("list_ui_effects", name="stripez", expect_error=True)
    ed.check("stripes" in error, f"the nearest name: {error}")
    error = await ed.call("list_ui_effects", category="nope", expect_error=True)
    ed.check("glass" in error and "pattern" in error, f"the categories: {error}")


async def test_sheen_hover_transition(ed: Editor):
    """In play, the pointer over the button: `transition: background-image 300ms` takes its sheen from
    intensity 0.3 to the :hover rule's 0.9 (the effect clock stays pinned, the transition runs on the
    UI clock)."""
    rect = REGIONS["button"]["rect"]
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("set_ui_effect_time", time=1.5)
        await ed.call("send_input", events=[{"type": "mouse", "action": "move", "x": 20, "y": 700}], space="canvas", frames_after=3)
        idle = await shot(ed, "sheen-idle")
        await ed.call("send_input", events=[{"type": "mouse", "action": "move", "x": (rect[0] + rect[2]) // 2, "y": rect[3] - 10}],
                      space="canvas", frames_after=20)
        hover = await shot(ed, "sheen-hover")
        r = await ed.call("inspect_ui", entity=S["entity"], selector=".button", depth=0)
        params = r["elements"][0]["effects"][0]["params"]
        ed.check_eq(params.get("intensity"), "0.9", f"the hover rule's intensity after the transition: {params}")
        (a, sd_a), (b, sd_b) = stats(idle, rect), stats(hover, rect)
        ed.check(sum(b) > sum(a) + 30, f"the hovered sheen is brighter: mean {tuple(round(v) for v in b)}, was {tuple(round(v) for v in a)}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


STATE_HTML = """<html>
<head>
    <title>stateful effects</title>
    <link rel="stylesheet" href="stateful.css"/>
</head>
<body>
    <div id="fog" class="fog"></div>
    <div id="ink" class="ink"></div>
    <div id="trail" class="trail"><div class="b1"></div><div class="b4"></div></div>
</body>
</html>
"""

STATE_CSS = """body { margin: 0; width: 1280px; height: 720px; background-color: #101218; }
.fog, .ink, .trail { position: absolute; top: 40px; width: 360px; height: 220px; }
.fog { left: 40px; background: #0b0d14; background-image: effect(fog); }
.ink { left: 440px; background: #000000; background-image: effect(ink-flow); }
.trail { left: 840px; filter: effect(time-trail, trail-source alpha); }
.trail div { position: absolute; width: 50%; height: 50%; }
.b1 { left: 0; top: 0; background-color: #e74c3c; }
.b4 { left: 50%; top: 50%; background-color: #3498db; }
"""

STATE_REGIONS = {"fog": (44, 44, 396, 256), "ink": (444, 44, 796, 256), "trail": (840, 40, 1200, 260)}


async def state_shot(ed: Editor, label: str) -> Shot:
    raw = await ed.call_raw("screenshot", {"view": "game", "scene": S["state-scene"], "max_width": 1280})
    ed.check(not raw.is_error and raw.images, f"{label}: no screenshot: {raw.text[:300]}")
    png = raw.images[0][0]
    (ed.scratch.root / f"html-ui-effects-{label}.png").write_bytes(png)
    return Shot(png, raw.value)


async def test_stateful_effects(ed: Editor):
    write(ed, "assets/ui/stateful.html", STATE_HTML)
    write(ed, "assets/ui/stateful.css", STATE_CSS)
    made = await ed.call("create_scene", name="HtmlUiStateful", type="UI")
    S["state-scene"] = made["scene"]["id"]
    r = await ed.call("create_ui_document", scene=S["state-scene"], html="assets/ui/stateful.html", name="Stateful")
    S["state-entity"] = r["entity"]["id"]
    ed.check_eq(r["diagnostics"], [], "the stateful document loads clean")
    await ed.call("save_scene", scene=S["state-scene"], path="scenes/HtmlUiStateful")
    await ed.call("set_ui_effect_time", time=1.5)
    # the variants compile on first use (async in the editor): wait for the fog
    deadline = time.monotonic() + 90
    while True:
        first = await state_shot(ed, "state-t1")
        _, sd = stats(first, STATE_REGIONS["fog"])
        if sd >= 4 or time.monotonic() > deadline:
            break
        await asyncio.sleep(1.0)
    ed.check(sd >= 4, f"the fog draws (sd {sd:.1f})")
    _, trail_sd = stats(first, STATE_REGIONS["trail"])
    ed.check(trail_sd >= 20, f"the time-trail filter draws its content (sd {trail_sd:.1f})")
    again = await state_shot(ed, "state-t1-again")
    for name, rect in STATE_REGIONS.items():
        ed.check(same(first, again, rect), f"{name}: a second render at the pinned time differs")
    # inspect_ui: each simulation's state
    for selector, name in ((".fog", "fog"), (".ink", "ink-flow"), (".trail", "time-trail")):
        r = await ed.call("inspect_ui", scene=S["state-scene"], entity=S["state-entity"], selector=selector, depth=0)
        effect = r["elements"][0]["effects"][0]
        ed.check_eq(effect["name"], name, selector)
        state = effect.get("state")
        ed.check(state and state["bytes"] > 0 and state["steps"] > 0 and not state["paused"], f"{selector}: the simulation's state: {effect}")
        ed.check(abs(state["time"] - 1.5) < 1e-6, f"{selector}: the simulation time is the pinned time: {state}")
    plain = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=".hero", depth=0)
    ed.check("state" not in plain["elements"][0]["effects"][0], "a stateless effect has no state")
    # list_ui_effects names a simulation's images and passes
    raw = await ed.call_raw("list_ui_effects", {"name": "fog"})
    fog = raw.value["effects"][0]
    ed.check_eq((fog["cost"], [st["name"] for st in fog["state"]["states"]]), ("stateful", ["vel", "pd", "dye"]), f"fog's state: {fog}")
    ed.check(fog["state"]["passes"] and fog["state"]["rate"] == 60, f"fog's passes and rate: {fog['state']}")
    # the live clock moves the fog
    await ed.call("set_ui_effect_time", time=None)
    await ed.call("wait", frames=2)
    live = await state_shot(ed, "state-live")
    ed.check(not same(first, live, STATE_REGIONS["fog"]), "the live clock moves the fog")
    await ed.call("set_ui_effect_time", time=1.5)


async def test_stateful_splat_in_play(ed: Editor):
    rect = STATE_REGIONS["ink"]
    await ed.call("play", action="start", scene=S["state-scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("set_ui_effect_time", time=1.5)
        await ed.call("send_input", events=[{"type": "mouse", "action": "move", "x": 20, "y": 700}], space="canvas", frames_after=3)
        before = await state_shot(ed, "ink-before")
        _, sd_before = stats(before, rect)
        code = ("local ink = UIDocument(scene, entity('Stateful')):querySelector('.ink')\n"
                "return ink:effectSplat{ x = 180, y = 110, dx = 900, dy = -200, radius = 50, color = '#ff8a00', strength = 2 }")
        values = (await ed.call("eval_lua", code=code))["values"]
        ed.check_eq(values, [1], "one simulation took the splat")
        none = (await ed.call("eval_lua", code="return UIDocument(scene, entity('Stateful')):querySelector('.ink'):effectSplat{ x = 1, y = 1, effect = 'fog' }"))["values"]
        ed.check_eq(none, [0], "a splat for an effect the element does not use is not taken")
        await ed.call("wait", frames=18)
        after = await state_shot(ed, "ink-after")
        mean, sd_after = stats(after, rect)
        ed.check(not same(before, after, rect) and sd_after > sd_before + 3,
                 f"the splat shows in the ink (sd {sd_before:.1f} -> {sd_after:.1f}, mean {tuple(round(v) for v in mean)})")
        again = await state_shot(ed, "ink-again")
        ed.check(same(after, again, rect), "the pinned clock replays the logged splat identically")
        reset = (await ed.call("eval_lua", code="return UIDocument(scene, entity('Stateful')):querySelector('.ink'):effectReset()"))["values"]
        ed.check_eq(reset, [1], "effectReset restarts the simulation")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


PROBE_HTML = """<html>
<head>
    <title>a project simulation</title>
    <link rel="stylesheet" href="probe.css"/>
</head>
<body><div id="probe" class="probe"></div></body>
</html>
"""

PROBE_CSS = """@effect sim-probe { src: url(fx/sim-probe.frag); }

body { margin: 0; width: 1280px; height: 720px; background-color: #101218; }
.probe { position: absolute; left: 40px; top: 40px; width: 256px; height: 128px; background-image: effect(sim-probe); }
"""


async def probe_effect(ed: Editor) -> dict:
    r = await ed.call("inspect_ui", scene=S["probe-scene"], entity=S["probe-entity"], selector=".probe", depth=0)
    effects = r["elements"][0].get("effects", []) if r["elements"] else []
    return effects[0] if effects else {}


async def probe_steps_until(ed: Editor, predicate, seconds: float = 90.0) -> dict:
    deadline = time.monotonic() + seconds
    while True:
        effect = await probe_effect(ed)
        state = effect.get("state")
        if (state and predicate(state)) or time.monotonic() > deadline:
            return effect
        await asyncio.sleep(0.25)


async def test_stateful_project_effect(ed: Editor):
    """A project effect with state (phase-6 stateful spec 3.11 and 3.7, the §5 item "project effects with state:
    compile, check, hot reload restart"; p6-accept): EffectStatefulTest's sim-probe.frag in the project."""
    frag = (harness.REPO / "engine" / "tests" / "htmlui" / "fixtures" / "effects" / "fx" / "sim-probe.frag").read_text(encoding="utf-8")
    # write_file checks every form: the paint and filter forms and their step forms
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/sim-probe.frag", "content": frag})
    ed.check(not raw.is_error, raw.text)
    results = raw.value.get("shader_check", [])
    forms = sorted(r.get("form", "") for r in results)
    ed.check_eq(forms, ["filter", "filter-step", "paint", "paint-step"], f"the forms checked: {results}")
    ed.check(all(r.get("ok") and r.get("effect") == "sim-probe" for r in results), f"every form compiles: {results}")
    checked = await ed.call("check_shader", shader="assets/ui/fx/sim-probe.frag", timeout=300)
    ed.check_eq((checked["failed"], checked["findings"]), (0, []), f"check_shader: {str(checked)[:2000]}")

    write(ed, "assets/ui/probe.html", PROBE_HTML)
    write(ed, "assets/ui/probe.css", PROBE_CSS)
    made = await ed.call("create_scene", name="HtmlUiSimProbe", type="UI")
    S["probe-scene"] = made["scene"]["id"]
    r = await ed.call("create_ui_document", scene=S["probe-scene"], html="assets/ui/probe.html", name="SimProbe")
    S["probe-entity"] = r["entity"]["id"]
    ed.check_eq(r["diagnostics"], [], "the project simulation's document loads clean")
    await ed.call("save_scene", scene=S["probe-scene"], path="scenes/HtmlUiSimProbe")
    await ed.call("set_ui_effect_time", time=None)
    # the live clock steps it (its variants compile on first use)
    effect = await probe_steps_until(ed, lambda st: st["steps"] > 180)
    state = effect.get("state") or {}
    ed.check(effect.get("name") == "sim-probe" and state.get("steps", 0) > 180 and state.get("bytes", 0) > 0 and not state.get("paused"),
             f"the project simulation steps on the live clock: {effect}")
    before = state["steps"]

    # a GLSL error in a step pass: only the step forms fail, FX-0015 at the .frag line
    good_line = "return vec4(s.uv, fract(n.x / 60.0), 1.0);"
    broken = frag.replace(good_line, "return vec4(s.uv, fract(n.x / 60.0));")
    ed.check(broken != frag, "the step pass to break")
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/sim-probe.frag", "content": broken})
    results = raw.value.get("shader_check", [])
    failed = [r for r in results if not r.get("ok")]
    line, _ = line_col(broken, "return vec4(s.uv, fract(n.x / 60.0));")
    ed.check(sorted(r.get("form") for r in failed) == ["filter-step", "paint-step"] and
             all(r.get("code") == "HTMLUI-FX-0015" for r in failed), f"the step forms fail with effect-shader-compile: {results}")
    ed.check(all(any(e["line"] == line for e in r.get("errors", [])) for r in failed), f"at the .frag line {line}: {failed}")
    checked = await ed.call("check_shader", shader="assets/ui/fx/sim-probe.frag", timeout=300)
    ed.check(checked["failed"] == 2 and checked["findings"] and
             (checked["findings"][0]["path"], checked["findings"][0]["line"]) == ("assets/ui/fx/sim-probe.frag", line),
             f"check_shader finds the step forms' error at the .frag line {line}: {str(checked)[:2000]}")

    # a manifest error: a pass that writes a state the manifest does not declare (FX-0014 at its line)
    bad = frag.replace("// @step       grow field", "// @step       grow fields")
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/sim-probe.frag", "content": bad})
    results = raw.value.get("shader_check", [])
    line, _ = line_col(bad, "@step       grow fields")
    ed.check(results and all(r.get("code") == "HTMLUI-FX-0014" and not r["ok"] for r in results), f"effect-manifest: {results}")
    ed.check(any(e["line"] == line for e in results[0].get("errors", [])), f"at the manifest line {line}: {results}")

    # fixed with another state layout (the field's size cap): the hot reload restarts the simulation
    changed = frag.replace("box*0.25 max 64 linear resample", "box*0.25 max 32 linear resample")
    ed.check(changed != frag, "the state layout to change")
    raw = await ed.call_raw("write_file", {"path": "assets/ui/fx/sim-probe.frag", "content": changed})
    ed.check(all(r.get("ok") for r in raw.value.get("shader_check", [])), raw.text)
    effect = await probe_steps_until(ed, lambda st: 0 < st["steps"] < before)
    state = effect.get("state") or {}
    ed.check(0 < state.get("steps", 0) < before, f"the changed manifest restarted the simulation ({before} steps before): {effect}")
    effect = await probe_steps_until(ed, lambda st: st["steps"] > 60)
    ed.check((effect.get("state") or {}).get("steps", 0) > 60, f"and it steps again: {effect}")
    await ed.call("write_file", path="assets/ui/fx/sim-probe.frag", content=frag)
    await ed.call("set_ui_effect_time", time=1.5)


BAD_CSS = """.a { background-image: effect(glassmorphism); }
.b { background-image: effect(aurora, speeed 2); }
.c { color: effect(plasma); }
"""


async def test_check_ui_effect_diagnostics(ed: Editor):
    write(ed, "assets/ui/bad.css", BAD_CSS)
    r = await ed.call("inspect_ui", file="assets/ui/bad.css")
    found = {d["code"]: d for d in r["diagnostics"]}
    cases = [("HTMLUI-FX-0001", "glassmorphism", "frosted-glass"), ("HTMLUI-FX-0004", "speeed", "speed"),
             ("HTMLUI-FX-0002", "effect(plasma)", "background-image")]
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
    await wait_tint(ed, "frag-edited", lambda mean, sd: sd > 5 and mean[1] < before[1] - 60)

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
    recovered = await wait_tint(ed, "frag-fixed", lambda mean, sd: sd >= REGIONS["tint"]["sd"] and abs(mean[1] - before[1]) <= 18)
    ed.check(same(recovered, S["t1"], REGIONS["tint"]["rect"]), "the fixed effect renders as before")


async def test_include_edit_reloads(ed: Editor):
    """An include of the project effect (fx/local-wave.glsl) is watched like its .frag."""
    before, _ = stats(S["t1"], REGIONS["tint"]["rect"])
    write(ed, "assets/ui/fx/local-wave.glsl", GLSL.replace("0.55 + 0.45 *", "0.1 + 0.1 *"))
    await wait_tint(ed, "include-edited", lambda mean, sd: sd > 2 and mean[1] < before[1] - 60)
    write(ed, "assets/ui/fx/local-wave.glsl", GLSL)
    restored = await wait_tint(ed, "include-restored", lambda mean, sd: abs(mean[1] - before[1]) <= 18 and sd >= REGIONS["tint"]["sd"])
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

    await ed.call("set_ui_effect_time", time=None)
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=".tint", depth=0)
    ed.check(r["document"]["diagnostics_count"]["error"] == 0, f"the document is clean again: {r['document']}")
