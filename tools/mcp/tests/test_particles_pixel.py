"""S49 pixel particles through MCP (docs/specs/S49-pixel-particles.md R20, R21, R36-R39, AC11, AC12): every pixel
preset made with create_particle_effect, played with set_particle_effect and previewed on the base-pixel grid
(S91's preview_particle_effect view pixel, 8 frames at scale 4: one filmstrip of the expected size, off_grid PASS on
drawn pixels, the S48 lints SKIP until S48 lands); the red control of off_grid (Snap to Pixel Grid off); S91's
bake_particle_sheet on the pixel view with the Aseprite JSON, gone again after one undo; the R37 example patches
through edit_particle_effect and back through get_particle_effect; an Analytic patch with Noise refused with its
diagnostic; the stats' S49 fields; a pixel preset one undo step; the group hidden until tool_groups enables it.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import json

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Pixel Particles CI"}
TIMEOUT = 1500

PRESETS = ("pixel_spark", "pixel_smoke", "pixel_fire", "pixel_ember", "pixel_grit", "pixel_debris", "pixel_light_flash",
           "pixel_smoke_loop", "pixel_ember_loop")
S48_CHECKS = {"off_palette", "partial_alpha", "spill", "one_shot_end", "loop_seam", "determinism"}

S: dict = {}


async def setup(ed: Editor) -> dict:
    """A 2D scene with one Particle System, the particles group enabled."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["particles"])
    made = await ed.call("create_scene", name="PixelLab", type="2D", path="scenes/PixelLab")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    system = await ed.call("create_particles", scene=S["scene"], name="Pixel", preset="none", position=[0, 0, 0])
    S["system"] = system["particle_system"]["id"]
    return S


def checks_of(value: dict) -> dict:
    return {r["id"]: r for r in value["checks"]}


async def pixel_preview(ed: Editor, s: dict, **args) -> dict:
    r = await ed.call_raw("preview_particle_effect", {"scene": s["scene"], "entity": s["system"], "view": "pixel", **args})
    ed.check(not r.is_error, f"preview: {r.text[:400]}")
    return r


async def test_group_hidden_until_enabled(ed: Editor):
    """The pixel presets come with create_particle_effect, the pixel view with preview_particle_effect: the particles
    group's, hidden by default."""
    ed.require_tools("tool_groups", "create_particle_effect", "preview_particle_effect")
    await ed.call("tool_groups", disable=["particles"])
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("preview_particle_effect" not in default and "create_particle_effect" not in default, "hidden by default")
    await ed.call("tool_groups", enable=["particles"])
    tools = {t.name: t for t in await ed.tools(groups=None)}
    ed.check({"preview_particle_effect", "create_particle_effect", "bake_particle_sheet"} <= set(tools), "listed once enabled")
    ed.check(set(PRESETS) <= set(tools["create_particle_effect"].input_schema["properties"]["preset"]["enum"]), "the pixel presets")
    for name in ("preview_particle_effect", "bake_particle_sheet"):
        ed.check_in("pixel", tools[name].input_schema["properties"]["view"]["enum"])


async def test_presets_sheets_and_checks(ed: Editor):
    """Each preset: created, played, previewed on the pixel grid as 8 frames at 4x with the checks (AC11)."""
    s = await setup(ed)
    for name in PRESETS:
        made = await ed.call("create_particle_effect", path=f"assets/fx/pixel/{name}.particles", preset=name)
        ed.check_eq(made["diagnostics"], [], f"{name}: no diagnostics")
        ed.check_in("set_particle_effect", made["next"])
        if name == "pixel_fire":
            children = made.get("children") or []
            ed.check(len(children) == 1 and children[0].endswith("fx/pixel/pixel_smoke.particles"), f"the fire's smoke beside it: {children}")
        await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect=f"assets/fx/pixel/{name}.particles")
        r = await pixel_preview(ed, s, frames=8, scale=4)
        value = r.value
        ed.check(len(r.images) == 1, f"{name}: one filmstrip")
        width, height, _ = pngdiff.decode(r.images[0][0])
        cell = value["frame"]["base_pixels"] * 4
        ed.check_eq(value["frame"]["view"], "pixel")
        ed.check_eq(width, 8 * cell + 7, f"{name}: eight cells of {cell} px (base pixels x 4) and their separators")
        ed.check(height >= cell, f"{name}: height {height}")
        checks = checks_of(value)
        grid = checks["off_grid"]
        ed.check_eq(grid["status"], "PASS", f"{name}: off_grid: {grid}")
        # measured on what the GPU drew (its sub-emitters' particles too: Debris draws nothing of its own)
        ed.check(grid["measured"]["drawn"] > 0, f"{name}: the cells show the effect: {grid}")
        # S48's lints run on the pixel cells (S49 R38): measured or a SKIP that says why
        for check in S48_CHECKS:
            ed.check_in(checks[check]["status"], ("PASS", "WARN", "FAIL", "SKIP"), f"{name}: {check}")
            ed.check("needs S48" not in (checks[check].get("limited_by") or ""), f"{name}: {check} runs S48's lint: {checks[check]}")
        ed.check_in(checks["partial_alpha"]["status"], ("PASS", "SKIP"), f"{name}: the dithered pixels are whole (additive: SKIP): {checks['partial_alpha']}")
        ed.check_eq(checks["determinism"]["status"], "SKIP", f"{name}: a preview is one run")


async def test_off_grid_red_control(ed: Editor):
    """Spark with Snap to Pixel Grid off fails off_grid (its base pixels drawn between grid lines)."""
    s = await setup(ed)
    await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect="assets/fx/pixel/pixel_spark.particles")
    await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect="inline")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"modules": {"pixelArt": {"snap": False}}})
    grid = checks_of((await pixel_preview(ed, s, frames=4)).value)["off_grid"]
    ed.check_eq(grid["status"], "FAIL", f"off_grid: {grid}")
    ed.check(grid["measured"]["off_grid"] > 0 and "Snap to Pixel Grid" in grid["fix_hint"], f"off_grid counts them and names the fix: {grid}")
    await ed.call("undo", scene=s["scene"])
    ed.check_eq(checks_of((await pixel_preview(ed, s, frames=4)).value)["off_grid"]["status"], "PASS", "snapped again")


async def test_bake_and_undo(ed: Editor):
    """Bake to Sprite Sheet on the pixel view: the sheet, its effect and the Aseprite json-hash; one undo removes them."""
    s = await setup(ed)
    await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect="assets/fx/pixel/pixel_smoke_loop.particles")
    r = await ed.call("bake_particle_sheet", scene=s["scene"], entity=s["system"], view="pixel", scale=2, columns=3, rows=2,
                      aseprite=True, output="assets/sheets/smoke")
    folder = ed.project / "assets" / "sheets"
    png, effect, data = folder / "smoke.png", folder / "smoke.particles", folder / "smoke.json"
    ed.check(png.is_file() and effect.is_file() and data.is_file(), f"baked: {r.get('written')}")
    cell = r["cell_size"]
    ed.check_eq(cell, r["frame"]["base_pixels"] * 2, "a cell is the base pixels at scale 2")
    sheet = json.loads(data.read_text())
    ed.check_eq(len(sheet["frames"]), 6)
    first = next(iter(sheet["frames"].values()))
    ed.check_eq((first["frame"]["w"], first["frame"]["h"]), (cell, cell))
    ed.check(first["duration"] > 0, "a frame's duration")
    sheet_w, sheet_h, _ = pngdiff.decode(png.read_bytes())
    ed.check_eq((sheet_w, sheet_h), (3 * cell, 2 * cell))
    ed.check_eq(sheet["meta"]["size"], {"w": sheet_w, "h": sheet_h})
    await ed.call("undo", scope="project")
    ed.check(not png.exists() and not effect.exists() and not data.exists(), "one undo removes the sheet, its effect and its JSON")


async def test_patch_paths(ed: Editor):
    """R37's example patches apply and round-trip (AC12); an Analytic patch with Noise is refused."""
    s = await setup(ed)
    # a system of its own on the ember preset (Simulated; every module Analytic can run), inline
    made = await ed.call("create_particle_effect", path="assets/fx/pixel/ramp_ref.particles", preset="pixel_ember")
    system = (await ed.call("create_particles", scene=s["scene"], name="Patched", preset="none", position=[0, 0, 0]))["particle_system"]["id"]
    s = dict(s, system=system)
    await ed.call("set_particle_effect", scene=s["scene"], entity=system, effect=made["path"])
    await ed.call("set_particle_effect", scene=s["scene"], entity=system, effect="inline")
    patches = [
        ({"modules": {"pixelArt": {"fade": "dither", "ditherSize": 4}}}, lambda e: e["modules"]["pixelArt"].get("ditherSize", 4) == 4),
        ({"modules": {"renderer": {"renderMode": "pixel", "pixelShape": "streak"}}},
         lambda e: e["modules"]["renderer"]["renderMode"] == "pixel" and e["modules"]["renderer"]["pixelShape"] == "streak"),
        ({"modules": {"colorOverLifetime": {"color": {"mode": "ramp", "ramp": None, "rampColors": ["#fff3a6", "#ffe26b", "#ff8a2a"], "rampBias": 0.62}}}},
         lambda e: e["modules"]["colorOverLifetime"]["color"]["mode"] == "ramp" and e["modules"]["colorOverLifetime"]["color"]["rampBias"] == 0.62),
        ({"modules": {"collision": {"settleSpeed": 70}}}, lambda e: e["modules"]["collision"]["settleSpeed"] == 70),
        ({"simulationMode": "analytic", "modules": {"collision": None}}, lambda e: e["simulationMode"] == "analytic"),
    ]
    for patch, check in patches:
        edited = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch=patch)
        ed.check_eq(edited["changed"], True, f"applied: {patch}")
        effect = (await ed.call("get_particle_effect", scene=s["scene"], entity=s["system"]))["effect"]
        ed.check(check(effect), f"round trip of {patch}: {json.dumps(effect)[:400]}")
    # the palette reference form too
    ref = (await ed.call("get_particle_effect", path=made["path"]))["effect"]["modules"]["colorOverLifetime"]["color"]["ramp"]
    ed.check(ref.endswith(".palette#ramp:hot"), f"a ramp reference into the project's palette copy: {ref}")
    await ed.call("edit_particle_effect", path=made["path"], patch={"modules": {"colorOverLifetime": {"color": {"ramp": ref.replace("#ramp:hot", "#ramp:fire")}}}})
    again = (await ed.call("get_particle_effect", path=made["path"]))["effect"]["modules"]["colorOverLifetime"]["color"]["ramp"]
    ed.check(again.endswith("#ramp:fire"), f"the ramp edited: {again}")
    # Analytic refuses Noise with its diagnostic and changes nothing
    err = await ed.call_raw("edit_particle_effect", {"scene": s["scene"], "entity": s["system"], "patch": {"modules": {"noise": {"enabled": True}}}})
    ed.check(err.is_error and "analytic_unsupported" in json.dumps(err.value) + err.text, f"refused: {err.text[:400]}")
    effect = (await ed.call("get_particle_effect", scene=s["scene"], entity=s["system"]))["effect"]
    ed.check("noise" not in effect["modules"], "nothing changed")
    stats = (await ed.call("get_particle_stats", scene=s["scene"], entity=s["system"]))["systems"][0]
    ed.check_eq(stats["simulation_mode"], "analytic")
    ed.check(stats["analytic_slots"] > 0 and stats["pixel"] is True, f"stats: {stats}")
    sought = await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="seek", time=3600.0)
    ed.check(abs(sought["stats"]["time"] - 3600.0) < 1e-3, f"an Analytic seek lands at once: {sought['stats']}")


async def test_preset_is_one_undo_step(ed: Editor):
    """A pixel preset with a sub-emitter writes the effect and its child as ONE project undo step."""
    await setup(ed)
    made = await ed.call("create_particle_effect", path="assets/fx/undo/fire.particles", preset="pixel_fire")
    fire = ed.project / "assets" / "fx" / "undo" / "fire.particles"
    smoke = ed.project / "assets" / "fx" / "undo" / "pixel_smoke.particles"
    ed.check(fire.is_file() and smoke.is_file(), f"the fire and its smoke: {made.get('children')}")
    await ed.call("undo", scope="project")
    ed.check(not fire.exists() and not smoke.exists(), "one undo removes the effect and its child")
    await ed.call("redo", scope="project")
    ed.check(fire.is_file() and smoke.is_file(), "one redo writes both again")
