"""Cheaper frames (docs/superpowers/specs/2026-11-04-draw-perf-design.md), as agents and scripts see them:
get_material_variant reports the directional-only Lit variant ('Dlo') of a scene whose lights are all
directional, and the general one once a point light is in it; Lua reads the frame's SetPass calls
(pipelines applied, Unity's name) from Profiler.frameStats() and DebugOverlay.getStats()."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "DrawPerf"}
TIMEOUT = 900


async def variant_when_ready(ed: Editor, eid, want_directional: bool) -> dict:
    """the box's variant once the one asked for is built (a new variant compiles while the old one draws)"""
    v = {}
    for _ in range(120):
        v = await ed.call("get_material_variant", entity=eid)
        if v.get("directional_only") == want_directional and (v["ready"] or v["failed"]):
            return v
        await ed.call("wait", frames=10, timeout_seconds=900)
    return v


async def test_directional_only_variant(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "get_material_variant", "delete_entities", "wait")
    made = await ed.call("create_scene", name="Sun", type="3D", path="scenes/Sun.scene", close_previous=True, unsaved="discard")
    scene = made["scene"]["id"]
    box = (await ed.call("create_entity", scene=scene, type="box", name="Box"))["entity_id"]
    # a new 3D scene is lit by its sun alone
    v = await variant_when_ready(ed, box, True)
    ed.check(v.get("directional_only") is True, f"a sun-only scene draws the 'Dlo' Lit: {v}")
    ed.check(not v["failed"], f"it builds: {v}")

    lamp = (await ed.call("create_entity", scene=scene, type="point_light", name="Lamp", position=[0, 3, 0]))["entity_id"]
    v = await variant_when_ready(ed, box, False)
    ed.check(v.get("directional_only") is False, f"a point light: the general Lit: {v}")

    await ed.call("delete_entities", entities=[lamp])
    v = await variant_when_ready(ed, box, True)
    ed.check(v.get("directional_only") is True, f"the point light gone: 'Dlo' again: {v}")


async def test_setpass_calls_in_lua(ed: Editor):
    ed.require_tools("eval_lua", "wait")
    await ed.call("wait", frames=5)
    stats = (await ed.call("eval_lua", code="return Profiler.frameStats()", allow_edit_mode=True))["values"][0]
    ed.check("setPassCalls" in stats, f"Profiler.frameStats() has setPassCalls: {stats}")
    ed.check(0 < stats.get("setPassCalls", 0) <= stats["drawCalls"], f"SetPass calls within the draws: {stats}")
    overlay = (await ed.call("eval_lua", code="return DebugOverlay.getStats()", allow_edit_mode=True))["values"][0]
    ed.check("setPassCalls" in overlay, f"DebugOverlay.getStats() has setPassCalls: {overlay}")
