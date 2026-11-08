"""S49 pixel particles through MCP (docs/specs/S49-pixel-particles.md R20, R21, R36-R39, AC11, AC12): every pixel
preset made with create_particle_effect, played with set_particle_effect and captured as a contact sheet
(capture_particles frames 8 scale 4 checks true: one sheet of the expected size, every check S49 computes PASS, the
S48 ones SKIP until S48 lands); the red control of off_grid (Snap to Pixel Grid off); Bake to Sprite Sheet's PNG strip
and Aseprite JSON, gone again after one undo; the R37 example patches through edit_particle_effect and back through
get_particle_effect; an Analytic patch with Noise refused with its diagnostic; the stats' S49 fields; the group
hidden until tool_groups enables it.

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
    return {r["id"]: r for r in value["checks"]["results"]}


async def test_group_hidden_until_enabled(ed: Editor):
    """capture_particles and create_particle_effect are the particles group's: hidden by default."""
    ed.require_tools("tool_groups", "create_particle_effect", "capture_particles")
    await ed.call("tool_groups", disable=["particles"])
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_particles" not in default and "create_particle_effect" not in default, "hidden by default")
    await ed.call("tool_groups", enable=["particles"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check({"capture_particles", "create_particle_effect"} <= listed, "listed once enabled")
    schema = next(t for t in await ed.tools(groups=None) if t.name == "create_particle_effect").input_schema
    ed.check(set(PRESETS) <= set(schema["properties"]["preset"]["enum"]), "the preset enum has the pixel presets")


async def test_presets_sheets_and_checks(ed: Editor):
    """Each preset: created, played, captured as 8 frames at 4x with the checks (AC11)."""
    s = await setup(ed)
    for name in PRESETS:
        made = await ed.call("create_particle_effect", path=f"assets/fx/pixel/{name}.particles", preset=name)
        ed.check_eq(made["diagnostics"], [], f"{name}: no diagnostics")
        ed.check_in("set_particle_effect", made["next"])
        if name == "pixel_fire":
            children = made.get("children") or []
            ed.check(len(children) == 1 and children[0].endswith("fx/pixel/pixel_smoke.particles"), f"the fire's smoke beside it: {children}")
        await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect=f"assets/fx/pixel/{name}.particles")
        r = await ed.call_raw("capture_particles", {"scene": s["scene"], "entity": s["system"], "time": 0.1, "frames": 8, "scale": 4,
                                                    "checks": True})
        ed.check(not r.is_error and len(r.images) == 1, f"{name}: {r.text[:300]}")
        value = r.value
        width, height, _ = pngdiff.decode(r.images[0][0])
        ed.check_eq((width, height), (value["width"], value["height"]), f"{name}: the sheet's size")
        cell_w, cell_h = value["cell"]
        # 8 frames in 3 columns and 3 rows, 1 px between cells, a label band under each
        ed.check(value["width"] == 3 * cell_w * 4 + 2, f"{name}: width {value['width']} for cells of {cell_w}")
        ed.check(value["height"] >= 3 * cell_h * 4 + 2, f"{name}: height {value['height']} for cells of {cell_h}")
        ed.check_eq(len(value["frames"]), 8)
        ed.check(any(f["particle_count"] > 0 for f in value["frames"]), f"{name}: particles drawn")
        checks = checks_of(value)
        for check, result in checks.items():
            if check in S48_CHECKS:
                ed.check_eq(result["status"], "SKIP", f"{name}: {check} is S48's")
                ed.check_in("needs S48", result["message"])
            else:
                ed.check_eq(result["status"], "PASS", f"{name}: {check}: {result}")
        ed.check({"off_grid", "over_budget"} <= set(checks), f"{name}: S49's checks: {sorted(checks)}")


async def test_off_grid_red_control(ed: Editor):
    """Spark with Snap to Pixel Grid off fails off_grid."""
    s = await setup(ed)
    await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect="assets/fx/pixel/pixel_spark.particles")
    await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect="inline")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"modules": {"pixelArt": {"snap": False}}})
    r = await ed.call("capture_particles", scene=s["scene"], entity=s["system"], time=0.05, frames=4, checks=True)
    grid = checks_of(r)["off_grid"]
    ed.check_eq(grid["status"], "FAIL", f"off_grid: {grid}")
    ed.check(grid["count"] > 0 and grid["fix"], f"off_grid names the particles and the fix: {grid}")
    await ed.call("undo", scene=s["scene"])
    r = await ed.call("capture_particles", scene=s["scene"], entity=s["system"], time=0.05, frames=4, checks=True)
    ed.check_eq(checks_of(r)["off_grid"]["status"], "PASS", "snapped again")


async def test_bake_and_undo(ed: Editor):
    """Bake to Sprite Sheet: the strip PNG and its Aseprite json-hash; one undo removes both."""
    s = await setup(ed)
    await ed.call("set_particle_effect", scene=s["scene"], entity=s["system"], effect="assets/fx/pixel/pixel_smoke_loop.particles")
    r = await ed.call("capture_particles", scene=s["scene"], entity=s["system"], time=0, frames=6, scale=2,
                      save_path="assets/sheets/smoke.png")
    png = ed.project / "assets" / "sheets" / "smoke.png"
    data = ed.project / "assets" / "sheets" / "smoke.json"
    ed.check(png.is_file() and data.is_file(), f"baked: {r.get('saved')}")
    sheet = json.loads(data.read_text())
    ed.check_eq(len(sheet["frames"]), 6)
    first = next(iter(sheet["frames"].values()))
    ed.check_eq(first["frame"]["w"], r["cell"][0] * 2)
    ed.check(first["duration"] > 0, "a frame's duration")
    strip_w, strip_h, _ = pngdiff.decode(png.read_bytes())
    ed.check_eq((strip_w, strip_h), (r["cell"][0] * 2 * 6, r["cell"][1] * 2))
    ed.check_eq(sheet["meta"]["size"], {"w": strip_w, "h": strip_h})
    await ed.call("undo", scope="project")
    ed.check(not png.exists() and not data.exists(), "one undo removes the strip and its JSON")


async def test_patch_paths(ed: Editor):
    """R37's example patches apply and round-trip (AC12); an Analytic patch with Noise is refused."""
    s = await setup(ed)
    # a system of its own, on the default (Simulated, Billboard) effect
    system = (await ed.call("create_particles", scene=s["scene"], name="Patched", preset="none", position=[0, 0, 0]))["particle_system"]["id"]
    s = dict(s, system=system)
    patches = [
        ({"modules": {"pixelArt": {"fade": "dither", "ditherSize": 4}}}, lambda e: e["modules"]["pixelArt"].get("ditherSize", 4) == 4),
        ({"modules": {"renderer": {"renderMode": "pixel", "pixelShape": "streak"}}},
         lambda e: e["modules"]["renderer"]["renderMode"] == "pixel" and e["modules"]["renderer"]["pixelShape"] == "streak"),
        ({"modules": {"colorOverLifetime": {"color": {"mode": "ramp", "rampColors": ["#fff3a6", "#ffe26b", "#ff8a2a"], "rampBias": 0.62}}}},
         lambda e: e["modules"]["colorOverLifetime"]["color"]["mode"] == "ramp" and e["modules"]["colorOverLifetime"]["color"]["rampBias"] == 0.62),
        ({"modules": {"collision": {"settleSpeed": 70}}}, lambda e: e["modules"]["collision"]["settleSpeed"] == 70),
        # (the default effect's Start Velocity is not Analytic's: removed in the same patch)
        ({"simulationMode": "analytic", "modules": {"collision": None, "startVelocity": None}}, lambda e: e["simulationMode"] == "analytic"),
    ]
    for patch, check in patches:
        edited = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch=patch)
        ed.check_eq(edited["changed"], True, f"applied: {patch}")
        effect = (await ed.call("get_particle_effect", scene=s["scene"], entity=s["system"]))["effect"]
        ed.check(check(effect), f"round trip of {patch}: {json.dumps(effect)[:400]}")
    # the palette reference form too
    made = await ed.call("create_particle_effect", path="assets/fx/pixel/ramp_ref.particles", preset="pixel_ember")
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
