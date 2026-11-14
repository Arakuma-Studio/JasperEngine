"""Old particle scripts on a migrated scene (docs/specs/S94-rendering-anim-gaps.md F2, AC2).

`Legacy K.scene` (tools/fixtures/particles-legacy) has the layout of JasperArcade's scene K as the port
wrote it: an unversioned scene whose FxConfetti GameObject holds an empty Points and whose
FxConfettiSystem GameObject holds the old Particle System action targeting it. Loading migrates the system
onto FxConfetti and removes the empty Points. KFx.lua then wraps both GameObjects the old way:
`Points(scene, FxConfetti)` for the sprite frames and `ParticleSystem(scene, FxConfettiSystem)` for the
sprite initializer and the restart. Both drive the migrated system, with no Console error.
"""

from __future__ import annotations

import harness
from harness import Editor

CI = True
EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "particles-legacy"}
TIMEOUT = 600

SCENE = "Legacy K.scene"

# ARCADE scripts/k/KFx.lua, system() and the confetti frames of KFx.new
KFX = """
local e, pe = scene:findEntity('FxConfetti'), scene:findEntity('FxConfettiSystem')
local s = { points = Points(scene, e), ps = ParticleSystem(scene, pe) }
s.points.entityOwned, s.ps.entityOwned = false, false
for i = 0, 7 do
    s.points:addSpriteFrame(i, 'c' .. i, Rect((i % 4) * 0.25, (i // 4) * 0.5, 0.25, 0.5))
end
s.ps:setSpriteIntializer(0, 7)
s.ps:stop()
s.ps:reset()
s.ps:start()
return s.ps.entity == e
"""


async def test_k_pattern_script(ed: Editor):
    ed.require_tools("open_scene", "play", "eval_lua", "read_console", "control_particles", "get_particle_stats",
                     "get_particle_effect", "get_entity", "tool_groups")
    await ed.call("tool_groups", enable=["particles"])
    opened = await ed.call("open_scene", scene=SCENE)
    sid = opened["scene"]["id"] if isinstance(opened.get("scene"), dict) else opened["id"]
    target = await ed.call("get_entity", scene=sid, entity="FxConfetti", detail="summary")
    components = set(target["components"] if "components" in target else target["entity"]["components"])
    ed.check("particlesystem" in components and "points" not in components, f"migrated onto its target: {components}")

    start = (await ed.call("read_console", limit=1)).get("next_cursor")
    # the project's start scene is the other one: Play runs this one
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        r = await ed.call("eval_lua", code=KFX)
        ed.check_eq(r["values"][0], True, "ParticleSystem(scene, FxConfettiSystem) is the migrated system")
        await ed.call("wait", frames=2)
        stats = await ed.call("get_particle_stats", scene=sid, entity="FxConfetti")
        ed.check_eq(stats["systems"][0]["particle_count"], 40, f"the restart fired the burst of 40: {stats['systems'][0]}")

        effect = (await ed.call("get_particle_effect", scene=sid, entity="FxConfetti"))["effect"]
        sheet = effect["modules"]["textureSheetAnimation"]
        ed.check_eq(len(sheet["sprites"]), 8, f"the 8 frames are the effect's sprites: {sheet}")
        five = sheet["sprites"][5]
        ed.check_eq([five.get(k) for k in ("x", "y", "width", "height")], [0.25, 0.5, 0.25, 0.5], f"frame 5's rect: {five}")

        cleared = await ed.call("control_particles", scene=sid, entity="FxConfetti", action="clear")
        ed.check_eq(cleared["stats"]["particle_count"], 0)
        await ed.call("control_particles", scene=sid, entity="FxConfetti", action="emit", count=5)
        await ed.call("wait", frames=2)
        emitted = await ed.call("get_particle_stats", scene=sid, entity="FxConfetti")
        ed.check_eq(emitted["systems"][0]["particle_count"], 5, "control_particles emits on the migrated system")

        errors = await ed.call("read_console", since=start, levels=["error", "fatal"], limit=50)
        ed.check_eq([e["message"] for e in errors["entries"]], [], "no Console error")
        notes = await ed.call("read_console", since=start, contains="ps.textureSheetAnimation", limit=50)
        ed.check_eq(len(notes["entries"]), 1, f"one deprecation note for the forwarded frames: {notes['entries']}")
    finally:
        await ed.call("play", action="stop")
