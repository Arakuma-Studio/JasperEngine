"""The S20 migration of old particle systems (docs/specs/S20-particles.md R32, D9, AC13) in the legacy
fixture project (tools/fixtures/particles-legacy: a format 2 scene whose Particle System actions draw
into Points): loading converts each system into an inline effect on its target without writing the
file, particles_migration_report lists them, the action stays the controller, the empty Points go;
a save writes format 3, and reopening the saved scene gives the same effects back (no diffs).

The trajectories themselves are AC13's ParticleMigration.* unit tests (recorded CSVs).
"""

from __future__ import annotations

import harness
from harness import Editor

CI = True
EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "particles-legacy"}
TIMEOUT = 600

SCENE = "Legacy Particles.scene"


async def entity_named(ed: Editor, scene, name: str) -> int:
    got = await ed.call("get_entity", scene=scene, entity=name, detail="summary")
    return got["id"] if "id" in got else got["entity"]["id"]


async def components(ed: Editor, scene, entity) -> set:
    return set((await ed.call("get_entity", scene=scene, entity=entity, detail="summary"))["components"])


async def effects(ed: Editor, scene) -> dict:
    out = {}
    for name in ("Fire", "Explosion"):
        out[name] = (await ed.call("particles_get_effect", scene=scene, entity=name))["effect"]
    return out


async def test_migrate_save_reload(ed: Editor):
    ed.require_tools("open_scene", "save_scene", "close_scene", "get_entity", "tool_groups")
    await ed.call("tool_groups", enable=["particles"])
    original = (ed.project / SCENE).read_bytes()
    opened = await ed.call("open_scene", scene=SCENE)
    sid = opened["scene"]["id"] if isinstance(opened.get("scene"), dict) else opened["id"]
    ed.check_eq((ed.project / SCENE).read_bytes(), original, "loading never writes the file")

    report = await ed.call("particles_migration_report")
    ed.check_eq(report["count"], 2, f"two systems converted: {report}")
    lines = [line for doc in report["documents"] for line in doc["conversions"]]
    ed.check(any("Fire Particles" in line and "Fire" in line for line in lines), f"the fire's line: {lines}")

    fire = await components(ed, sid, "Fire")
    ed.check("particlesystem" in fire, f"the effect moved to its target: {fire}")
    ed.check("points" not in fire, f"the empty Points went: {fire}")
    controller = await components(ed, sid, "Fire Particles")
    ed.check("action" in controller and "particlesystem" not in controller, f"the action stays the controller: {controller}")

    got = await ed.call("particles_get_effect", scene=sid, entity="Fire")
    ed.check_eq(got["source"], "inline")
    ed.check_eq(got["effect"]["modules"]["emission"]["rateOverTime"], 60, "rate 60")
    explosion = await ed.call("particles_get_effect", scene=sid, entity="Explosion")
    bursts = explosion["effect"]["modules"]["emission"]["bursts"]
    ed.check_eq(len(bursts), 1, f"the burst: {bursts}")
    ed.check_eq(explosion["effect"].get("looping"), False, "loop false")
    before = await effects(ed, sid)

    await ed.call("save_scene", scene=sid)
    saved = (ed.project / SCENE).read_text()
    ed.check("formatVersion: 3" in saved, "saved as format 3")
    ed.check("inlineEffect" in saved, "the inline effects are saved")
    await ed.call("close_scene", scene=sid, unsaved="discard")

    reopened = await ed.call("open_scene", scene=SCENE)
    rid = reopened["scene"]["id"] if isinstance(reopened.get("scene"), dict) else reopened["id"]
    after = await effects(ed, rid)
    ed.check_eq(after, before, "the saved effects read back the same")
    again = await ed.call("particles_migration_report")
    ed.check_eq(again["count"], 2, "a format 3 scene converts nothing more")
