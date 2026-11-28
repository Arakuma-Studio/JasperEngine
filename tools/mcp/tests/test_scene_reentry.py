# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""S95 AC-6 (docs/specs/S95-lua-runtime-gaps.md; JasperArcade ledger S6, S18, S21, S31): a scene
entered again in Play is built from its file. In scene A a script moves a GameObject, hides one,
instantiates three copies and emits particles; after a load of B and a load of A again (and after
SceneManager.loadScene of the current scene) the transforms and active flags are the saved ones,
the copies are gone and the particle system holds none. A pinning test: main fixed it (03727c3f1);
test_lifecycle.py covers the scripts and is out of the CI suite.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Scene Reentry"}

DISTURB = """
local mover = object('Mover')
mover.position = Vector3(9, 8, 7)
mover:setRotation(0, 90, 0)
object('Lamp'):setActive(false)
for i = 1, 3 do GameObject.instantiate(mover) end
object('Sparks', 'ParticleSystem'):emit(20)
"""

STATE = """
local mover = GameObject.find('Mover')
local p = mover.position
return p.x, p.y, p.z, mover.rotation == Quaternion(), GameObject.find('Lamp') ~= nil,
    GameObject.find('Mover(Clone)') == nil,
    object('Sparks', 'ParticleSystem').particleCount
"""


async def test_reentry_rebuilds_scene(ed: Editor):
    a = (await ed.call("create_scene", name="ReentryA", type="3d"))["scene"]["id"]
    await ed.call("create_entity", scene=a, name="Mover", type="empty", position=[1, 2, 3])
    await ed.call("create_entity", scene=a, name="Lamp", type="empty")
    sparks = await ed.call("create_particles", scene=a, name="Sparks", preset="none")
    await ed.call("edit_particle_effect", scene=a, entity=sparks["particle_system"]["id"], patch={
        "maxParticles": 100,
        "modules": {"main": {"startSpeed": 0, "startLifetime": 60}, "emission": {"rateOverTime": 0}},
    })
    await ed.call("save_scene", scene=a, path="ReentryA.scene")
    b = (await ed.call("create_scene", name="ReentryB", type="3d"))["scene"]["id"]
    await ed.call("create_entity", scene=b, name="Elsewhere", type="empty")
    await ed.call("save_scene", scene=b, path="ReentryB.scene")

    saved = [1, 2, 3, True, True, True, 0]
    await ed.call("play", action="start", scene=a, auto_pause=True, fixed_fps=30)
    try:
        await ed.call("wait", frames=2)
        ed.check_eq((await ed.call("eval_lua", code=STATE))["values"], saved, "the saved state at Play start")

        for reload in ("SceneManager.loadScene('ReentryB')", "SceneManager.loadScene(SceneManager.currentSceneName)"):
            await ed.call("eval_lua", code=DISTURB)
            await ed.call("wait", frames=2)
            disturbed = (await ed.call("eval_lua", code=STATE))["values"]
            ed.check_eq(disturbed, [9, 8, 7, False, False, False, 20], f"the script changed scene A ({reload})")

            await ed.call("eval_lua", code=reload)
            await ed.call("wait", frames=3)
            if "ReentryB" in reload:
                state = await ed.call("get_play_state")
                ed.check_in("ReentryB", [s["name"] for s in state["running_scenes"]], "B was loaded")
                await ed.call("eval_lua", code="SceneManager.loadScene('ReentryA')")
                await ed.call("wait", frames=3)
            ed.check_eq((await ed.call("eval_lua", code=STATE))["values"], saved, f"A built anew after {reload}")
    finally:
        await ed.call("play", action="stop")
