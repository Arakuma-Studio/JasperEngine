"""S49 pixel particles in the editor, driven like a person (docs/specs/S49-pixel-particles.md R30-R34,
AC15): xdotool in the harness's Xvfb clicks the Inspector, the Scene view's Particle Effect panel and
the Palette window.

- The Renderer's Pixel Shape menu shows each shape's icon (drawn by the shape rule) and picks Streak.
- Simulation Mode switched to Analytic in the Inspector with Noise on: the effect keeps it (the banner
  lists what Analytic cannot run), "Disable unsupported modules" unchecks Noise as one undo step.
- The panel's scrub lands at once on Analytic times; a capture at a time is the same image however the
  scrub got there.
- Pixel Grid draws the base-pixel grid over a zoomed-in Scene view.
- A ramp strip dragged from the Palette window onto a Palette Ramp colour sets the reference.

Runs in a new empty project; the test's steps share the editor.
"""

from __future__ import annotations

import json

import pngdiff
import ui_input
from harness import Editor

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel Particles UI"}
TIMEOUT = 900

PALETTE = "assets/palettes/fx.palette"
S: dict = {}


async def effect(ed: Editor) -> dict:
    return (await ed.call("get_particle_effect", scene=S["scene"], entity=S["system"]))["effect"]


async def until(ed: Editor, probe, what: str, frames: int = 600):
    value = None
    for _ in range(frames // 4):
        value = await probe()
        if value:
            return value
        await ui_input.frames(ed, 4)
    ed.fail(f"{what} (last: {value!r})")


def pixels_in(png: bytes, rect: dict) -> list[tuple[int, int, int]]:
    width, height, rgba = pngdiff.decode(png)
    out = []
    for y in range(int(rect["y"]), min(height, int(rect["y"] + rect["height"]))):
        for x in range(int(rect["x"]), min(width, int(rect["x"] + rect["width"]))):
            i = (y * width + x) * 4
            out.append((rgba[i], rgba[i + 1], rgba[i + 2]))
    return out


async def editor_shot(ed: Editor) -> bytes:
    shot = await ed.call_raw("screenshot", {"view": "editor", "max_width": 8192})
    ed.check(bool(shot.images), "an editor screenshot")
    return shot.images[0][0]


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S["ed"] = ed
    await ed.call("tool_groups", enable=["particles", "editor_ux"])
    scene = (await ed.call("create_scene", name="PixelUi", type="2D", path="scenes/PixelUi"))["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    made = await ed.call("create_particle_effect", path="assets/fx/ember.particles", preset="pixel_ember")
    system = (await ed.call("create_particles", scene=scene, name="Embers", preset="none", position=[0, 0, 0]))["particle_system"]["id"]
    S.update(scene=scene, system=system)
    await ed.call("set_particle_effect", scene=scene, entity=system, effect=made["path"])
    await ed.call("set_particle_effect", scene=scene, entity=system, effect="inline")
    await ed.call("select_entities", scene=scene, entity=system)
    await ed.call("focus_window", window="inspector")
    return S


async def test_pixel_shape_icons(ed: Editor):
    """The Pixel Shape menu: an icon per shape in the shape rule's colours; Streak picked."""
    await setup(ed)
    ui = ui_input.for_editor(ed)
    await ui.click_revealed("##renderer.pixelShape", "Inspector")
    rows = []
    for i in range(9):
        row = await ui.find("##shape", index=i)
        ed.check(row is not None, f"shape row {i} is drawn")
        rows.append(row)
    shot = await editor_shot(ed)
    for i, row in enumerate(rows):
        rect = row["rect"]
        icon = {"x": rect["x"], "y": rect["y"], "width": rect["height"], "height": rect["height"]}
        lit = [p for p in pixels_in(shot, icon) if p in ((255, 243, 166), (255, 138, 42)) or (p[0] > 200 and p[1] > 120 and p[2] < 200)]
        ed.check(len(lit) > 0, f"shape {i}'s icon is drawn")
    await ui.click(ui.centre(rows[4]["rect"]))   # Dot, Cross, Disc, Ring, Streak, ...
    await until(ed, lambda: _renderer_shape(ed, "streak"), "Streak picked in the menu")


async def _renderer_shape(ed: Editor, shape: str):
    return (await effect(ed))["modules"]["renderer"].get("pixelShape") == shape


async def test_analytic_switch_and_disable(ed: Editor):
    """Analytic chosen in the Inspector with Noise on; the banner's button disables Noise in one undo step."""
    await setup(ed)
    ui = ui_input.for_editor(ed)
    await ed.call("add_particle_module", scene=S["scene"], entity=S["system"], module="noise")
    await ui.click_revealed("##effect.simulationMode", "Inspector")
    await ui.click_item("Analytic")

    async def analytic():
        e = await effect(ed)
        return e if e.get("simulationMode") == "analytic" else None
    switched = await until(ed, analytic, "the Inspector switches to Analytic (with Noise still on)")
    ed.check(switched["modules"]["noise"].get("enabled", True), "Noise stays on until the banner's button")
    got = await ed.call("get_particle_effect", scene=S["scene"], entity=S["system"])
    ed.check("analytic_unsupported" in json.dumps(got["diagnostics"]), f"the diagnostics name it: {got['diagnostics']}")

    await ui.click_revealed("Disable unsupported modules", "Inspector")

    async def noise_off():
        e = await effect(ed)
        return e if e["modules"]["noise"].get("enabled", True) is False else None
    await until(ed, noise_off, "Disable unsupported modules unchecks Noise")
    stats = (await ed.call("get_particle_stats", scene=S["scene"], entity=S["system"]))["systems"][0]
    ed.check_eq(stats["simulation_mode"], "analytic")
    await ed.call("undo", scene=S["scene"])
    after = await effect(ed)
    ed.check(after["modules"]["noise"].get("enabled", True) is not False and after["simulationMode"] == "analytic",
             f"one undo step brings Noise back, Analytic stays: {json.dumps(after)[:300]}")
    await ed.call("redo", scene=S["scene"])
    ed.check((await effect(ed))["modules"]["noise"].get("enabled", True) is False, "redo")


async def test_overlay_scrub_is_exact(ed: Editor):
    """The Particle Effect panel's scrub on an Analytic system: each click lands at once; the capture at a
    landed time is the same after scrubbing elsewhere and back."""
    await setup(ed)
    ui = ui_input.for_editor(ed)
    await ed.call("focus_window", window="scene")
    await ed.call("select_entities", scene=S["scene"], entity=S["system"])
    slider = await ui.find("##time")
    ed.check(slider is not None, "the panel's scrub is drawn")
    rect = slider["rect"]
    seen = []
    for fraction in (0.2, 0.55, 0.85, 0.35):
        await ui.click(ui.at(rect, (rect["width"] * fraction, rect["height"] / 2)))
        await ui_input.frames(ed, 4)
        stats = (await ed.call("get_particle_stats", scene=S["scene"], entity=S["system"]))["systems"][0]
        seen.append(stats["time"])
    ed.check(len({round(t, 3) for t in seen}) == 4 and all(t > 0 for t in seen), f"each click lands somewhere else: {seen}")
    first = await ed.call_raw("capture_particles", {"scene": S["scene"], "entity": S["system"], "time": seen[0], "frames": 1})
    await ui.click(ui.at(rect, (rect["width"] * 0.7, rect["height"] / 2)))
    await ui_input.frames(ed, 4)
    again = await ed.call_raw("capture_particles", {"scene": S["scene"], "entity": S["system"], "time": seen[0], "frames": 1})
    ed.check(first.images and again.images and first.images[0][0] == again.images[0][0], "the capture at a time does not depend on the scrub")
    ed.check(first.value["frames"][0]["particle_count"] > 0, f"particles at {seen[0]}: {first.value['frames']}")


async def test_pixel_grid_toggle(ed: Editor):
    """Pixel Grid draws the base-pixel grid on a Scene view zoomed in past 4x."""
    await setup(ed)
    ui = ui_input.for_editor(ed)
    await ed.call("set_scene_camera", scene=S["scene"], center=[0, 0], ortho_size=12)
    await ui_input.frames(ed, 6)
    view = await ed.call_raw("screenshot", {"view": "scene", "max_width": 8192})
    await ui.click_item("Pixel Grid")
    await ui_input.frames(ed, 6)
    grid = await ed.call_raw("screenshot", {"view": "scene", "max_width": 8192})
    ed.check(view.images and grid.images and view.images[0][0] != grid.images[0][0], "Pixel Grid changes the Scene view")
    await ui.click_item("Pixel Grid")


async def test_palette_ramp_drag(ed: Editor):
    """A ramp strip of the Palette window dropped on Color over Lifetime's Palette Ramp bar sets the reference."""
    await setup(ed)
    ui = ui_input.for_editor(ed)
    (ed.project / PALETTE).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / PALETTE).write_text(json.dumps({"format": "jasperengine.palette", "version": 1, "name": "FX", "columns": 4,
                                                  "colors": ["#1a1c2c", "#5d275d", "#b13e53", "#ef7d57"],
                                                  "ramps": [{"name": "embers", "colors": [0, 1, 2, 3]}]}, indent=2))
    await ed.call("sync_files", paths=[PALETTE], full=False)
    await ed.call("edit_particle_effect", scene=S["scene"], entity=S["system"],
                  patch={"modules": {"colorOverLifetime": {"color": {"mode": "ramp", "ramp": None, "rampColors": ["#ffffff"]}}}})
    await ed.call("focus_window", window="inspector")
    bar = await ui.reveal("##rampBar", "Inspector")
    await ed.call("run_command", id="window.palette", payload=PALETTE)
    strip = None
    for _ in range(60):
        strip = await ui.find("###palette_ramp_0", "Palette", frames=4)
        if strip:
            break
    ed.check(strip is not None, "the Palette window draws the ramp's strip")
    bar = await ui.find("##rampBar", "Inspector")
    ed.check(bar is not None, "the ramp bar is still drawn")
    await ui.drag(ui.centre(strip["rect"]), ui.centre(bar["rect"]), steps=16)

    async def dropped():
        color = (await effect(ed))["modules"]["colorOverLifetime"]["color"]
        return color if color.get("ramp", "").endswith("fx.palette#ramp:embers") else None
    await until(ed, dropped, "the dropped ramp sets the colour's reference")
