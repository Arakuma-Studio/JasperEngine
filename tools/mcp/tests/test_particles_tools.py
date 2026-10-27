"""S20 particles through MCP (docs/specs/S20-particles.md R30, AC15): every tool of the particles group, their
error paths (an invalid patch comes back with its diagnostics, an unknown module, a playing scene),
create_particles on the new component, a capture at a seeked time against its golden
(golden/particles/capture_t2.png; JASPER_PARTICLES_UPDATE_GOLDEN=1 writes it) and the tool group.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import os

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Particles CI"}
TIMEOUT = 1200

TOOLS = ("create_particle_effect", "get_particle_effect", "edit_particle_effect", "add_particle_module",
         "remove_particle_module", "set_particle_effect", "control_particles", "get_particle_stats", "capture_particles",
         "get_particle_migration_report")

GOLDEN_DIR = harness.REPO / "tools" / "mcp" / "tests" / "golden" / "particles"
UPDATE_GOLDEN = os.environ.get("JASPER_PARTICLES_UPDATE_GOLDEN") == "1"
GOLDEN_SIZE = (320, 180)

S: dict = {}


async def setup(ed: Editor) -> dict:
    """A 3D scene with a camera looking at a fire Particle System, the particles group enabled."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["particles"])
    made = await ed.call("create_scene", name="ParticleLab", type="3D", path="scenes/ParticleLab")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    fire = await ed.call("create_particles", scene=S["scene"], name="Fire", preset="fire", position=[0, 0, 0])
    S["fire"] = fire["particle_system"]["id"]
    return S


async def test_group(ed: Editor):
    """The particles tools are the particles group's: hidden by default, listed once enabled."""
    ed.require_tools("tool_groups", *TOOLS)
    await ed.call("tool_groups", disable=["particles"])
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"hidden by default: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["particles"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(TOOLS) <= listed, f"listed once enabled: {sorted(set(TOOLS) - listed)}")


async def test_create_particles(ed: Editor):
    """create_particles makes one GameObject with the component and an inline effect (the presets kept)."""
    s = await setup(ed)
    got = await ed.call("get_particle_effect", scene=s["scene"], entity=s["fire"])
    ed.check_eq(got["source"], "inline")
    modules = got["effect"]["modules"]
    ed.check("emission" in modules and "colorOverLifetime" in modules, f"fire's modules: {list(modules)}")
    ed.check_eq(modules["emission"]["rateOverTime"], 60, "the fire preset's rate")
    ed.check_eq(got["diagnostics"], [], "a preset has no diagnostics")


async def test_edit_and_undo(ed: Editor):
    """A merge patch is one undo step; undo gives the JSON back exactly; an invalid one is refused."""
    s = await setup(ed)
    before = await ed.call("get_particle_effect", scene=s["scene"], entity=s["fire"])
    edited = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"],
                           patch={"duration": 3, "modules": {"emission": {"rateOverTime": {"mode": "twoConstants", "constant": 5, "constantMax": 9}}}})
    ed.check_eq(edited["changed"], True)
    after = await ed.call("get_particle_effect", scene=s["scene"], entity=s["fire"])
    ed.check_eq(after["effect"]["duration"], 3)
    ed.check_eq(after["effect"]["modules"]["emission"]["rateOverTime"]["constantMax"], 9)
    await ed.call("undo", scene=s["scene"])
    undone = await ed.call("get_particle_effect", scene=s["scene"], entity=s["fire"])
    ed.check_eq(undone["effect"], before["effect"], "undo restores the effect exactly")
    await ed.call("redo", scene=s["scene"])
    redone = await ed.call("get_particle_effect", scene=s["scene"], entity=s["fire"])
    ed.check_eq(redone["effect"], after["effect"], "redo restores the edit exactly")
    await ed.call("undo", scene=s["scene"])

    err = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"],
                        patch={"modules": {"emission": {"rateOverTime": "lots"}}}, expect_error=True)
    ed.check_in("rateOverTime", err)
    err = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"], patch={"modules": {"sparkles": {}}}, expect_error=True)
    ed.check_in("sparkles", err)
    err = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"], patch="nope", expect_error=True)
    ed.check_in("merge patch", err)


async def test_modules(ed: Editor):
    s = await setup(ed)
    added = await ed.call("add_particle_module", scene=s["scene"], entity=s["fire"], module="noise", fields={"strength": 0.5})
    ed.check_in("noise", added["modules"])
    got = await ed.call("get_particle_effect", scene=s["scene"], entity=s["fire"])
    ed.check_eq(got["effect"]["modules"]["noise"]["strength"], 0.5)
    removed = await ed.call("remove_particle_module", scene=s["scene"], entity=s["fire"], module="noise")
    ed.check("noise" not in removed["modules"], f"removed: {removed['modules']}")
    err = await ed.call("add_particle_module", scene=s["scene"], entity=s["fire"], module="glitter", expect_error=True)
    ed.check_in("Unknown module", err)
    err = await ed.call("remove_particle_module", scene=s["scene"], entity=s["fire"], module="renderer", expect_error=True)
    ed.check_in("cannot be removed", err)


async def test_assets_and_assign(ed: Editor):
    """An asset effect: created from a preset, assigned, edited through the GameObject (the asset changes)."""
    s = await setup(ed)
    made = await ed.call("create_particle_effect", path="assets/fx/smoke.particles", preset="smoke")
    ed.check_eq(made["path"], "assets/fx/smoke.particles", "paths are the project's asset paths")
    ed.check(len(made["guid"]) == 32, f"guid: {made['guid']}")
    err = await ed.call("create_particle_effect", path="assets/fx/smoke.particles", preset="smoke", expect_error=True)
    ed.check_in("exists", err)
    err = await ed.call("create_particle_effect", path="assets/fx/bad.particles", effect={"duration": -1}, expect_error=True)
    ed.check_in("duration", err)

    smoke = await ed.call("create_particles", scene=s["scene"], name="Smoke", preset="none", position=[3, 0, 0])
    entity = smoke["particle_system"]["id"]
    assigned = await ed.call("set_particle_effect", scene=s["scene"], entity=entity, effect="assets/fx/smoke.particles")
    ed.check_eq(assigned["source"], "asset")
    ed.check_eq(assigned["path"], "assets/fx/smoke.particles")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=entity, patch={"modules": {"main": {"startSpeed": 2}}})
    by_path = await ed.call("get_particle_effect", path="assets/fx/smoke.particles")
    ed.check_eq(by_path["effect"]["modules"]["main"]["startSpeed"], 2, "the GameObject's edit changed the asset")
    text = (ed.project / "assets" / "fx" / "smoke.particles").read_text()
    ed.check_in('"startSpeed": 2', text)
    inline = await ed.call("set_particle_effect", scene=s["scene"], entity=entity, effect="inline")
    ed.check_eq(inline["source"], "inline")
    copied = await ed.call("get_particle_effect", scene=s["scene"], entity=entity)
    ed.check_eq(copied["effect"]["modules"]["main"]["startSpeed"], 2, "copy_to_inline kept the asset's effect")


async def test_control_and_stats(ed: Editor):
    """Edit-mode playback through the Scene view's preview: play advances time, seek lands on it, emit adds."""
    s = await setup(ed)
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"], patch={"seed": 7})
    played = await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="play")
    ed.check_eq(played["edit_mode"], True)
    await ed.call("wait", frames=30)
    stats = await ed.call("get_particle_stats", scene=s["scene"], entity=s["fire"])
    system = stats["systems"][0]
    ed.check(system["time"] > 0 and system["particle_count"] > 0, f"the preview played: {system}")
    sought = await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="seek", time=1.5)
    ed.check(abs(sought["stats"]["time"] - 1.5) < 0.05, f"seek: {sought['stats']}")
    await ed.call("wait", frames=10)
    held = await ed.call("get_particle_stats", scene=s["scene"], entity=s["fire"])
    ed.check(abs(held["systems"][0]["time"] - 1.5) < 0.05, f"a seek holds the preview: {held['systems'][0]}")
    cleared = await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="clear")
    ed.check_eq(cleared["stats"]["particle_count"], 0)
    # emitted particles are born at the system's next tick; a cleared (stopped) system emits only them
    await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="emit", count=10)
    one = await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="step", ticks=1)
    ed.check_eq(one["stats"]["particle_count"], 10, f"emit: {one['stats']}")
    stepped = await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="step", ticks=30)
    ed.check(stepped["stats"]["tick_count"] >= 30, f"step: {stepped['stats']}")
    everything = await ed.call("get_particle_stats", scene=s["scene"])
    ed.check(len(everything["systems"]) >= 1 and "world" in everything, f"stats: {everything}")
    err = await ed.call("control_particles", scene=s["scene"], entity=s["fire"], action="dance", expect_error=True)
    ed.check_in("action must be", err)


async def test_capture_golden(ed: Editor):
    """The fire at 2 s through a game camera, against its golden."""
    s = await setup(ed)
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"], patch={"seed": 11})
    camera = await ed.call("create_entity", scene=s["scene"], type="camera", name="ParticleCamera")
    await ed.call("set_transform", scene=s["scene"], entity=camera["entity_id"], position=[0, 1, 6])
    await ed.call("set_main_camera", scene=s["scene"], entity=camera["entity_id"])
    await ed.call("configure_game_view", aspect="16:9")
    r = await ed.call_raw("capture_particles", {"scene": s["scene"], "entity": s["fire"], "time": 2.0, "camera": "game", "max_width": 0})
    ed.check(not r.is_error and r.images, f"capture: {r.text[:300]}")
    ed.check(abs(r.value["time"] - 2.0) < 1e-6 and r.value["stats"]["particle_count"] > 0, f"capture: {r.value}")
    png = r.images[0][0]
    (ed.scratch.root / "capture_t2.png").write_bytes(png)
    image = pngdiff.decode(png)
    if "canvas_rect_in_image" in r.value:
        rect = r.value["canvas_rect_in_image"]
        image = pngdiff.crop(image, round(rect["x"]), round(rect["y"]), round(rect["width"]), round(rect["height"]))
    image = pngdiff.downscale(image, *GOLDEN_SIZE)
    path = GOLDEN_DIR / "capture_t2.png"
    if UPDATE_GOLDEN:
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        path.write_bytes(pngdiff.encode(image))
        copy = harness.REPO / "build" / "particles_golden" / path.name
        copy.parent.mkdir(parents=True, exist_ok=True)
        copy.write_bytes(path.read_bytes())
        print(f"     [golden] wrote {path.relative_to(harness.REPO)}: look at it")
        return
    ed.check(path.is_file(), f"no golden {path} (JASPER_PARTICLES_UPDATE_GOLDEN=1 writes it)")
    ok, fraction, diff = pngdiff.compare(image, pngdiff.load(path), 24, 0.01)
    if not ok:
        (ed.scratch.root / "capture_t2.actual.png").write_bytes(pngdiff.encode(image))
        (ed.scratch.root / "capture_t2.diff.png").write_bytes(diff or b"")
        ed.keep = True
    ed.check(ok, f"capture_t2: {fraction * 100:.3f}% of the pixels differ from the golden")


async def test_playing_scene_refuses_edits(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", frames=10)
        stats = await ed.call("get_particle_stats", scene=s["scene"])
        ed.check(any(item["playing"] for item in stats["systems"]), f"Play On Awake systems play: {stats['systems']}")
        err = await ed.call("edit_particle_effect", scene=s["scene"], entity=s["fire"], patch={"duration": 2}, expect_error=True)
        ed.check_in("playing", err)
    finally:
        await ed.call("play", action="stop")


async def test_migration_report(ed: Editor):
    report = await ed.call("get_particle_migration_report")
    ed.check_eq(report["count"], 0, "a new project converted nothing")
