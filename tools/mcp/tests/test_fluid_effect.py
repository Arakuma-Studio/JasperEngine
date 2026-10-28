"""The HTML UI `fluid` effect (Pavel Dobryakov's WebGL fluid as `background-image: effect(fluid)`)
and the agents' determinism tools for stateful UI effects, through MCP.

- A UI document with two fluids: `.burst` (Pavel's start burst) and `.canvas` (no burst, bloom
  off: only what the test paints). list_ui_effects lists `fluid` with cost stateful, its presets
  and its @frame passes; the whole catalogue has 19 stateful effects.
- set_ui_effect_time pins the clock: the burst draws, a second screenshot is identical.
- set_ui_effect_time pointer_path on `.canvas` (a scripted drag, replayed at the pinned clock):
  the canvas is painted; the same path again gives the identical screenshot, another path a
  different one; [] clears it.
- set_ui_effect_time splats: scripted splats paint (and replace the earlier ones); inspect_ui
  reports the simulation (images, replays, splats, path samples).
- The mistakes: pointer_path without a pinned time or a selector, an element without a stateful effect.
- A CLI export with the effect's variants ships Pavel Dobryakov's notice, one with another effect does not.

Runs in a new empty project (EDITOR create).
"""

import asyncio
import math
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor
from htmlui_pixels import Shot

EDITOR = {"create": True, "name": "Fluid Effect"}
CI = True
TIMEOUT = 900

S: dict = {}

HTML = """<html>
<head>
    <title>fluid</title>
    <link rel="stylesheet" href="fluid.css"/>
</head>
<body>
    <div id="burst" class="burst"></div>
    <div id="canvas" class="canvas"></div>
    <div id="plain" class="plain"></div>
</body>
</html>
"""

CSS = """body { margin: 0; width: 1280px; height: 720px; background-color: #000000; }
.burst, .canvas { position: absolute; top: 60px; width: 520px; height: 360px; background-color: #000000; }
.burst { left: 40px; background-image: effect(fluid); }
.canvas { left: 680px; background-image: effect(fluid, splats-on-start off, bloom off); }
.plain { position: absolute; left: 40px; top: 500px; width: 200px; height: 100px; background-image: effect(stripes); }
"""

REGIONS = {"burst": (44, 64, 556, 416), "canvas": (684, 64, 1196, 416)}


def copy_project(source: Path) -> Path:
    tmp = Path(tempfile.mkdtemp(prefix="jasper-fluid-effect-export-"))

    def ignore(directory: str, names: list[str]) -> set[str]:
        out = {".git"} & set(names)
        if Path(directory).name == ".jasperengine":
            out |= {"build", "export"} & set(names)
        return out

    shutil.copytree(source, tmp / "project", ignore=ignore, symlinks=True)
    return tmp / "project"


def cli_export(ed: Editor, project: Path, out: Path, *args: str) -> None:
    cmd = harness.editor_command() + ["export", "--project", str(project), "--out", str(out), "--backend", "opengl", *args]
    r = subprocess.run(cmd, cwd=str(project), capture_output=True, text=True, timeout=1800)
    ed.check(r.returncode == 0, f"export failed ({r.returncode}):\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}")


def write(ed: Editor, rel: str, text: str) -> None:
    time.sleep(0.02)
    path = ed.project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


async def shot(ed: Editor, label: str) -> Shot:
    # a simulation step on a software rasteriser of a loaded builder takes its time: the capture
    # waits for three settled frames up to 300 s (its 30 s default is a desktop GPU's)
    raw = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280, "timeout_seconds": 300},
                            timeout=330)
    ed.check(not raw.is_error and raw.images, f"{label}: no screenshot: {raw.text[:300]}")
    png = raw.images[0][0]
    (ed.scratch.root / f"fluid-effect-{label}.png").write_bytes(png)
    return Shot(png, raw.value)


def stats(image: Shot, rect) -> tuple[float, float, int]:
    """Mean and standard deviation of the brightest channel, and the peak."""
    values = [max(p[0], p[1], p[2]) for p in image.region(*rect)]
    n = max(1, len(values))
    mean = sum(values) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in values) / n)
    return mean, sd, max(values) if values else 0


def same(a: Shot, b: Shot, rect) -> bool:
    return list(a.region(*rect)) == list(b.region(*rect))


def drag(y: float, x0: float = 60.0, x1: float = 460.0, t0: float = 0.2, t1: float = 0.6, n: int = 24) -> list:
    """A drag across the canvas (element px), from t0 to t1 s."""
    return [[x0 + (x1 - x0) * k / n, y + 40.0 * math.sin(k * 0.5), t0 + (t1 - t0) * k / n] for k in range(n + 1)]


async def canvas_state(ed: Editor) -> dict:
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=".canvas", depth=0)
    effect = r["elements"][0]["effects"][0]
    ed.check_eq(effect["name"], "fluid", "the canvas's effect")
    return effect["state"]


async def test_setup(ed: Editor):
    write(ed, "assets/ui/fluid.html", HTML)
    write(ed, "assets/ui/fluid.css", CSS)
    made = await ed.call("create_scene", name="FluidEffect", type="UI")
    S["scene"] = made["scene"]["id"]
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/fluid.html", name="Fluid")
    S["entity"] = r["entity"]["id"]
    ed.check_eq(r["diagnostics"], [], "the document loads clean")
    await ed.call("save_scene", scene=S["scene"], path="scenes/FluidEffect")


async def test_list_ui_effects(ed: Editor):
    raw = await ed.call_raw("list_ui_effects", {"name": "fluid"})
    ed.check(not raw.is_error, raw.text)
    fluid = raw.value["effects"][0]
    ed.check_eq((fluid["name"], fluid["kind"], fluid["cost"]), ("fluid", "paint", "stateful"), f"fluid: {fluid}")
    ed.check(fluid["source"].startswith("port WebGL-Fluid-Simulation"), fluid["source"])
    ed.check_eq(sorted(fluid["presets"]), ["calm-water", "ink", "mobile", "psychedelic", "smoke"], "its presets")
    ed.check_eq(fluid["presets"]["ink"]["curl"], "8", "ink's curl")
    names = [p["name"] for p in fluid["params"]]
    for name in ("preset", "density-dissipation", "velocity-dissipation", "pressure", "pressure-iterations", "curl", "splat-radius",
                 "splat-force", "shading", "colorful", "color-speed", "bloom", "bloom-intensity", "bloom-threshold",
                 "bloom-soft-knee", "sunrays", "sunrays-weight", "seed", "splats-on-start", "gradient-scale"):
        ed.check(name in names, f"{name} is a parameter: {names}")
    passes = fluid["state"]["passes"]
    frames = [p["name"] for p in passes if p.get("frame")]
    ed.check_eq(frames, ["bloom-prefilter", "bloom-down", "bloom-up", "bloom-final", "sunrays", "sunrays-blur"], "the @frame passes")
    ed.check(any(p.get("repeat") == "pressure-iterations" for p in passes), f"Jacobi repeats by pressure-iterations: {passes}")
    sizes = {st["name"]: (st["format"], st["size"]) for st in fluid["state"]["states"]}
    ed.check_eq(sizes["vel"], ("rg16f", "aspect 128 max 512"), "the velocity grid")
    ed.check_eq(sizes["pressure"][0], "r16f", "the pressure's format")
    everything = await ed.call("list_ui_effects")
    stateful = [e["name"] for e in everything["effects"] if e.get("builtin") and e.get("cost") == "stateful"]
    ed.check("fluid" in stateful and len(stateful) == 19, f"19 stateful effects with fluid: {stateful}")


async def test_pinned_burst(ed: Editor):
    r = await ed.call("set_ui_effect_time", time=1.0)
    ed.check_eq(r["time"], 1.0, f"pinned: {r}")
    # the variants compile on first use (async in the editor): wait for the burst
    deadline = time.monotonic() + 120
    while True:
        first = await shot(ed, "burst")
        mean, sd, peak = stats(first, REGIONS["burst"])
        if sd >= 8 or time.monotonic() > deadline:
            break
        await asyncio.sleep(1.0)
    ed.check(sd >= 8 and peak >= 150, f"the start burst draws (mean {mean:.1f}, sd {sd:.1f}, peak {peak})")
    _, _, canvas_peak = stats(first, REGIONS["canvas"])
    ed.check(canvas_peak <= 4, f"the canvas is empty before the test paints it (peak {canvas_peak})")
    again = await shot(ed, "burst-again")
    for name, rect in REGIONS.items():
        ed.check(same(first, again, rect), f"{name}: a second render at the pinned time differs")
    S["empty"] = first


async def test_pointer_path(ed: Editor):
    path = drag(180.0)
    r = await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", pointer_path=path)
    ed.check_eq((r["path"], r["simulations"]), (len(path), 1), f"the path was taken: {r}")
    painted = await shot(ed, "path")
    mean, sd, peak = stats(painted, REGIONS["canvas"])
    ed.check(peak >= 40 and sd >= 3, f"the drag painted the canvas (mean {mean:.1f}, sd {sd:.1f}, peak {peak})")
    ed.check(same(painted, S["empty"], REGIONS["burst"]), "the burst's element is untouched by the canvas's path")
    # the same path again: the same bytes
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", pointer_path=path)
    again = await shot(ed, "path-again")
    ed.check(same(painted, again, REGIONS["canvas"]), "the same pointer path gives another image")
    # the same path, then another pinned time and back: the same bytes again (replayed, not accumulated)
    await ed.call("set_ui_effect_time", time=2.0)
    await shot(ed, "path-later")
    await ed.call("set_ui_effect_time", time=1.0)
    back = await shot(ed, "path-back")
    ed.check(same(painted, back, REGIONS["canvas"]), "the path replays the same at 1 s after 2 s")
    # another path: another image
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", pointer_path=drag(80.0))
    other = await shot(ed, "path-other")
    ed.check(not same(painted, other, REGIONS["canvas"]), "a different pointer path gives the same image")
    state = await canvas_state(ed)
    ed.check(state["path"] == 25 and state["images"] == 16 and state["replays"] >= 1 and not state["replaying"], f"inspect_ui: {state}")
    # [] clears it
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", pointer_path=[])
    cleared = await shot(ed, "path-cleared")
    _, _, peak = stats(cleared, REGIONS["canvas"])
    ed.check(peak <= 4, f"the cleared path leaves the canvas empty (peak {peak})")


async def test_splats(ed: Editor):
    splats = [{"x": 260, "y": 180, "dx": 600, "dy": 0, "radius": 40, "color": "#ff4020", "strength": 1.5},
              {"x": 120, "y": 90, "radius": 30, "color": "#20a0ff", "strength": 1.5, "t": 0.5}]
    r = await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", splats=splats)
    ed.check_eq((r["splats"], r["simulations"]), (2, 1), f"the splats were taken: {r}")
    first = await shot(ed, "splats")
    mean, sd, peak = stats(first, REGIONS["canvas"])
    ed.check(peak >= 150, f"the splats painted (mean {mean:.1f}, sd {sd:.1f}, peak {peak})")
    state = await canvas_state(ed)
    ed.check_eq(state["splats"], 2, f"inspect_ui counts the logged splats: {state}")
    # the same splats again replace the earlier ones: the same image
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", splats=splats)
    again = await shot(ed, "splats-again")
    ed.check(same(first, again, REGIONS["canvas"]), "the same splats again give another image")
    ed.check_eq((await canvas_state(ed))["splats"], 2, "replaced, not added")
    # one splat fewer: another image; none: empty again
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", splats=splats[:1])
    fewer = await shot(ed, "splats-fewer")
    ed.check(not same(first, fewer, REGIONS["canvas"]), "a different set of splats gives the same image")
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", splats=[])
    _, _, peak = stats(await shot(ed, "splats-none"), REGIONS["canvas"])
    ed.check(peak <= 4, f"no splats: the canvas is empty (peak {peak})")


async def test_refused_calls_change_nothing(ed: Editor):
    """A refused call leaves the clock where it was; more than 8 splats in one step are refused, not
    dropped; a splat after the pinned time is reported as future input."""
    await ed.call("set_ui_effect_time", time=1.0)
    await shot(ed, "refused-before")
    nine = [{"x": 20 + 40 * k, "y": 100, "radius": 20, "strength": 1.5} for k in range(9)]
    r = await ed.call_raw("set_ui_effect_time", {"time": 2.5, "entity": S["entity"], "selector": ".canvas", "splats": nine})
    ed.check(r.is_error and "more than 8 splats" in r.text, f"9 splats in one step: {r.text}")
    r = await ed.call_raw("set_ui_effect_time", {"time": 2.5, "entity": S["entity"], "selector": ".plain", "splats": nine[:1]})
    ed.check(r.is_error, f"an element without a simulation: {r.text}")
    state = await canvas_state(ed)
    ed.check(abs(state["time"] - 1.0) < 1e-6, f"refused calls left the clock pinned at 1 s: {state}")
    # spread over steps (t 1/60 s apart) they are taken; one later than the pinned time is future input
    spread = [dict(item, t=0.5 + k / 60.0) for k, item in enumerate(nine)] + [{"x": 100, "y": 100, "t": 1.5}]
    r = await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", splats=spread)
    ed.check_eq((r["splats"], r["splats_applied"], r["splats_future"]), (10, 9, 1), f"the splats' account: {r}")
    first = await shot(ed, "spread")
    _, _, peak = stats(first, REGIONS["canvas"])
    ed.check(peak >= 150, f"nine spread splats painted (peak {peak})")
    await ed.call("set_ui_effect_time", time=1.0, entity=S["entity"], selector=".canvas", splats=[])


async def test_mistakes(ed: Editor):
    r = await ed.call_raw("set_ui_effect_time", {"time": None, "selector": ".canvas", "pointer_path": drag(100.0)})
    ed.check(r.is_error and "pinned clock" in r.text, f"a path on the live clock: {r.text}")
    r = await ed.call_raw("set_ui_effect_time", {"time": 1.0, "pointer_path": drag(100.0)})
    ed.check(r.is_error and "selector is required" in r.text, f"a path without a selector: {r.text}")
    r = await ed.call_raw("set_ui_effect_time", {"time": 1.0, "selector": ".canvas", "pointer_path": [[1, 2]]})
    ed.check(r.is_error and "[x, y, t]" in r.text, f"a bad point: {r.text}")
    r = await ed.call_raw("set_ui_effect_time", {"time": 1.0, "selector": ".canvas", "splats": [{"y": 3}]})
    ed.check(r.is_error and "x and y" in r.text, f"a splat without x: {r.text}")
    r = await ed.call_raw("set_ui_effect_time", {"time": 1.0, "entity": S["entity"], "selector": ".plain", "splats": [{"x": 1, "y": 2}]})
    ed.check(r.is_error and "no stateful effect" in r.text, f"an element without a simulation: {r.text}")
    r = await ed.call_raw("set_ui_effect_time", {"time": 1.0, "entity": S["entity"], "selector": ".nothing", "splats": []})
    ed.check(r.is_error, f"a selector matching nothing: {r.text}")
    await ed.call("set_ui_effect_time", time=None)


async def test_export_ships_the_notice(ed: Editor):
    """A CLI export with the effect's variants ships Pavel Dobryakov's notice (and the shaders.com
    one: every UI effect uses the include library); one with another UI effect only, just the latter."""
    notice = Path("licenses") / "webgl-fluid-simulation-LICENSE.txt"
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    project = copy_project(ed.project)
    try:
        with_fluid = project.parent / "export-fluid-effect"
        cli_export(ed, project, with_fluid, "--shader", "uieffect/fluid")
        ed.check((with_fluid / notice).is_file(), f"the fluid effect ships the notice: {sorted(p.name for p in with_fluid.iterdir())}")
        ed.check("Copyright (c) 2017 Pavel Dobryakov" in (with_fluid / notice).read_text(), "Pavel Dobryakov's MIT notice")
        ed.check((with_fluid / "licenses" / "shaders.com-LICENSE.txt").is_file(), "and the UI effects' notice")
        other = project.parent / "export-stripes"
        cli_export(ed, project, other, "--shader", "uieffect/stripes")
        ed.check(not (other / notice).exists(), "another UI effect alone: no fluid notice")
        ed.check((other / "licenses" / "shaders.com-LICENSE.txt").is_file(), "the UI effects' notice")
    finally:
        shutil.rmtree(project.parent, ignore_errors=True)
