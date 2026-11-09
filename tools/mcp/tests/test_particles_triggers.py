"""S91's trigger actions (docs/specs/S91-effect-authoring.md R9, AC8): an effect's triggers start, pause
and stop the system from control_particles (Edit mode, through the Scene view's preview) and from Lua
in Play, and a Trigger sub-emitter fires from a system with no live particle.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Particle Triggers CI"}
TIMEOUT = 900

S: dict = {}

TRIGGERS = [{"name": "start"}, {"name": "hold", "action": "pause"}, {"name": "halt", "action": "stop"},
            {"name": "wipe", "action": "stopAndClear"}]


async def lua(ed: Editor, code: str):
    r = await ed.call("eval_lua", code=code)
    values = r.get("values") if isinstance(r, dict) else r
    return values[0] if isinstance(values, list) and len(values) == 1 else values


async def setup(ed: Editor) -> dict:
    """A scene with a stopped looping stream whose effect declares the four triggers, and a parent with
    no emission of its own whose Trigger sub-emitter is named "boom"."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    ed.require_tools("create_particles", "control_particles", "eval_lua", "play", "tool_groups")
    await ed.call("tool_groups", enable=["particles"])
    made = await ed.call("create_scene", name="Triggers", type="3D", path="scenes/Triggers")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    stream = await ed.call("create_particles", scene=S["scene"], name="Stream", preset="none", position=[0, 1, 0])
    S["stream"] = stream["particle_system"]["id"]
    await ed.call("edit_particle_effect", scene=S["scene"], entity=S["stream"], patch={
        "seed": 3, "looping": True, "triggers": TRIGGERS,
        "modules": {"main": {"startLifetime": 10, "startSpeed": 0}, "emission": {"rateOverTime": 60}},
    })
    await ed.call("create_particle_effect", path="assets/fx/spark.particles", preset="sparks")
    fuse = await ed.call("create_particles", scene=S["scene"], name="Fuse", preset="none", position=[2, 1, 0])
    S["fuse"] = fuse["particle_system"]["id"]
    await ed.call("edit_particle_effect", scene=S["scene"], entity=S["fuse"], patch={
        "seed": 4,
        "subEmitters": [{"trigger": "trigger", "triggerName": "boom", "effect": "assets/fx/spark.particles", "emitCount": 6}],
        "modules": {"emission": {"rateOverTime": 0}, "subEmitters": {}},
    })
    return S


async def test_trigger_actions_from_mcp(ed: Editor):
    """control_particles trigger: play, pause and stop by the effect's triggers, in Edit mode."""
    s = await setup(ed)
    await ed.call("control_particles", scene=s["scene"], entity=s["stream"], action="clear")
    started = await ed.call("control_particles", scene=s["scene"], entity=s["stream"], action="trigger", name="start")
    ed.check_eq(started["stats"]["playing"], True, f"start plays it: {started['stats']}")
    await ed.call("wait", frames=20)
    held = await ed.call("control_particles", scene=s["scene"], entity=s["stream"], action="trigger", name="hold")
    ed.check_eq(held["stats"]["paused"], True, f"hold pauses it: {held['stats']}")
    ed.check(held["stats"]["particle_count"] > 0, f"it emitted while playing: {held['stats']}")
    halted = await ed.call("control_particles", scene=s["scene"], entity=s["stream"], action="trigger", name="halt")
    ed.check_eq(halted["stats"]["emitting"], False, f"halt stops the emission: {halted['stats']}")
    ed.check(halted["stats"]["particle_count"] > 0, "a stop keeps the particles")
    wiped = await ed.call("control_particles", scene=s["scene"], entity=s["stream"], action="trigger", name="wipe")
    ed.check_eq(wiped["stats"]["particle_count"], 0, f"wipe clears them: {wiped['stats']}")
    err = await ed.call("control_particles", scene=s["scene"], entity=s["stream"], action="trigger", expect_error=True)
    ed.check_in("name", err)


async def test_trigger_fires_with_no_particle(ed: Editor):
    """A Trigger sub-emitter fires from a stopped parent that has no particle: once, from the emitter."""
    s = await setup(ed)
    await ed.call("control_particles", scene=s["scene"], entity=s["fuse"], action="clear")
    await ed.call("control_particles", scene=s["scene"], entity=s["fuse"], action="trigger", name="boom")
    stepped = await ed.call("control_particles", scene=s["scene"], entity=s["fuse"], action="step", ticks=1)
    ed.check(stepped["stats"]["particle_count"] >= 6, f"its children were spawned: {stepped['stats']}")


async def test_trigger_actions_from_lua(ed: Editor):
    """ps:sendTrigger runs the same actions in Play."""
    s = await setup(ed)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", frames=5)
        got = await lua(ed, """
            local ps = object('Stream', 'ParticleSystem')
            ps:stop(true)
            local stopped = not ps.isPlaying
            ps:sendTrigger('start')
            local playing = ps.isPlaying
            ps:sendTrigger('hold')
            local paused = ps.isPaused
            ps:sendTrigger('start')
            ps:sendTrigger('halt')
            return {stopped, playing, paused, ps.isEmitting}
        """)
        ed.check(got == [True, True, True, False], f"stopped, played, paused, stopped emitting: {got}")
    finally:
        await ed.call("play", action="stop")
