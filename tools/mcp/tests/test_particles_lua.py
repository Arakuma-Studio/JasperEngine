"""The S20 Lua API in Play (docs/specs/S20-particles.md R31, AC13, AC15): ParticleSystem play / stop /
emit / seek and its properties, exposed parameters (setFloat / getFloat), onCollision batches (at most
maxCollisionEvents per tick, delivered on the main thread after the simulation), sendTrigger raising a
Trigger sub-emitter (onSubEmit), and an old script's deprecated setters still running.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Particles Lua CI"}
TIMEOUT = 900

S: dict = {}
SCENES: list = []   # the scene setup made: Play starts it, and stop() forgets S

CAP = 8


async def lua(ed: Editor, code: str):
    r = await ed.call("eval_lua", code=code)
    values = r.get("values") if isinstance(r, dict) else r
    return values[0] if isinstance(values, list) and len(values) == 1 else values


async def setup(ed: Editor) -> dict:
    """A scene with a falling effect that hits a plane at y = 0 and a Trigger sub-emitter, playing."""
    if S.get("ed") is ed:
        return S
    S.clear()
    ed.require_tools("create_particles", "eval_lua", "play", "tool_groups")
    await ed.call("tool_groups", enable=["particles"])
    made = await ed.call("create_scene", name="ParticleLua", type="3D", path="scenes/ParticleLua")
    S["scene"] = made["scene"]["id"]
    SCENES.append(S["scene"])
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("create_particle_effect", path="assets/fx/burst.particles", preset="sparks")
    rain = await ed.call("create_particles", scene=S["scene"], name="Rain", preset="none", position=[0, 3, 0])
    S["rain"] = rain["particle_system"]["id"]
    await ed.call("edit_particle_effect", scene=S["scene"], entity=S["rain"], patch={
        "seed": 5,
        "simulationSpace": "world",
        "maxParticles": 500,
        "exposed": [{"name": "Rate", "path": "emission.rateOverTime", "type": "float"}],
        "subEmitters": [{"trigger": "trigger", "triggerName": "boom", "effect": "assets/fx/burst.particles", "emitCount": 4}],
        "modules": {
            "main": {"startSpeed": 0, "startLifetime": 4, "gravityModifier": 1},
            "emission": {"rateOverTime": 200},
            "collision": {"collisionType": "planes", "planes": [{"point": [0, -3, 0], "normal": [0, 1, 0]}], "sendEvents": True,
                          "maxCollisionEvents": CAP, "bounce": 0.2},
            "subEmitters": {},
        },
    })
    await ed.call("play", action="start")
    await ed.call("wait", frames=5)
    S["ed"] = ed
    return S


async def stop(ed: Editor):
    if S.get("ed") is ed:
        await ed.call("play", action="stop")
        S.clear()


async def test_playback(ed: Editor):
    await setup(ed)
    ed.check_eq(await lua(ed, "return object('Rain', 'ParticleSystem').isPlaying"), True, "Play On Awake")
    await lua(ed, "local ps = object('Rain', 'ParticleSystem'); ps:stop(true); ps:emit(5)")
    # emitted particles are born at the system's next tick
    await ed.call("wait", frames=2)
    count = await lua(ed, "return object('Rain', 'ParticleSystem').particleCount")
    ed.check_eq(count, 5, "stop(true) cleared, emit(5) made five")
    ed.check_eq(await lua(ed, "return object('Rain', 'ParticleSystem').isEmitting"), False, "stopped systems do not emit")
    t = await lua(ed, "local ps = object('Rain', 'ParticleSystem'); ps:seek(1.5); return ps.time")
    ed.check(abs(t - 1.5) < 0.05, f"seek: {t}")
    await lua(ed, "local ps = object('Rain', 'ParticleSystem'); ps.playbackSpeed = 2; ps:play()")
    ed.check_eq(await lua(ed, "return object('Rain', 'ParticleSystem').playbackSpeed"), 2)
    seed = await lua(ed, "local ps = object('Rain', 'ParticleSystem'); return ps.seed")
    ed.check(isinstance(seed, int), f"seed: {seed}")


async def test_exposed(ed: Editor):
    await setup(ed)
    got = await lua(ed, "local ps = object('Rain', 'ParticleSystem'); local ok = ps:setFloat('Rate', 99); return {ok, ps:getFloat('Rate')}")
    ed.check_eq(got, [True, 99], "setFloat / getFloat")
    missing = await lua(ed, "return object('Rain', 'ParticleSystem'):setFloat('Nope', 1)")
    ed.check_eq(missing, False, "an unknown parameter")


async def test_collision_events(ed: Editor):
    await setup(ed)
    await lua(ed, """
        local ps = object('Rain', 'ParticleSystem')
        ps.playbackSpeed = 1
        ps:restart()
        hits, batches, biggest, killed = 0, 0, 0, 0
        ps:onCollision(function(events)
            batches = batches + 1
            hits = hits + #events
            if #events > biggest then biggest = #events end
            local e = events[1]
            if e and e.normal.y < 0.99 then killed = -1000 end
        end)
    """)
    await ed.call("wait", frames=120)
    got = await lua(ed, "return {hits, batches, biggest, killed}")
    hits, batches, biggest, bad = got
    ed.check(hits > 0 and batches > 0, f"collisions reported: {got}")
    ed.check(biggest <= CAP, f"at most maxCollisionEvents ({CAP}) per tick: {biggest}")
    ed.check(bad == 0, "the normal is the plane's")


async def test_send_trigger(ed: Editor):
    await setup(ed)
    await lua(ed, """
        local ps = object('Rain', 'ParticleSystem')
        subEmits = 0
        trigger = ''
        ps:onSubEmit(function(events)
            subEmits = subEmits + #events
            trigger = events[1].trigger
        end)
        ps:sendTrigger('boom')
    """)
    await ed.call("wait", frames=10)
    got = await lua(ed, "return {subEmits, trigger}")
    ed.check(got[0] > 0 and got[1] == "trigger", f"onSubEmit after sendTrigger: {got}")


async def test_deprecated_setters(ed: Editor):
    """An old script's setters still run: they edit the inline effect (with a deprecation warning)."""
    await setup(ed)
    got = await lua(ed, """
        local ps = object('Rain', 'ParticleSystem')
        ps.rate = 30
        ps.maxParticles = 300
        ps:setLifeInitializer(1, 2)
        ps:setSizeInitializer(0.1, 0.2)
        ps:addBurst(0, 10)
        ps:start()
        return {ps.rate, ps.maxParticles, ps.running}
    """)
    ed.check_eq(got[0], 30, "rate")
    ed.check_eq(got[1], 300, "maxParticles")
    ed.check_eq(got[2], True, "start() plays it")
    await stop(ed)


async def test_pixel_particles_lua(ed: Editor):
    """S49 R40 (AC12): a Play script makes a system pixel art (Render Mode Pixel, Dither, a Palette Ramp through
    the live colour's setRamp), switches it to Analytic, and reads the CPU twin of a slot."""
    ed.require_tools("create_particle_effect", "create_particles", "eval_lua", "play", "tool_groups")
    await ed.call("tool_groups", enable=["particles"])
    # a pixel preset brings the project's copy of builtin:pixelfx-effects
    made = await ed.call("create_particle_effect", path="assets/fx/ember.particles", preset="pixel_ember")
    ramp = (await ed.call("get_particle_effect", path=made["path"]))["effect"]["modules"]["colorOverLifetime"]["color"]["ramp"]
    palette = ramp.split("#ramp:")[0]
    if not SCENES:
        await setup(ed)
        await stop(ed)
    scene = SCENES[-1]   # the scene Play starts
    await ed.call("select_scene", scene=scene)
    embers = (await ed.call("create_particles", scene=scene, name="Embers", preset="none", position=[0, 0, 0]))["particle_system"]["id"]
    # the ember preset as the system's inline effect: Cutout dots on the hot ramp, Simulated
    await ed.call("set_particle_effect", scene=scene, entity=embers, effect=made["path"])
    await ed.call("set_particle_effect", scene=scene, entity=embers, effect="inline")
    await ed.call("play", action="start")
    await ed.call("wait", frames=5)
    got = await lua(ed, f"""
        local ps = object('Embers', 'ParticleSystem')
        ps.renderer.renderMode = ParticleSystemRenderMode.Pixel
        ps.renderer.pixelShape = ParticlePixelShape.Disc
        ps.pixelArt.fade = ParticleFade.Dither
        ps.colorOverLifetime.enabled = true
        ps.colorOverLifetime.color:setRamp('{palette}', 'fire', 4)
        local color = ps.colorOverLifetime.color
        return {{ps.renderer.renderMode, ps.pixelArt.fade, color.ramp, color.rampSteps, ps.simulationMode, ps.renderer.pixelShape}}
    """)
    ed.check_eq(got[0], "pixel")
    ed.check_eq(got[5], "disc")
    ed.check_eq(got[1], "dither")
    ed.check_eq(got[2], palette + "#ramp:fire", "setRamp wrote the system's colour")
    ed.check_eq(got[3], 4)
    ed.check_eq(got[4], "simulated")
    ed.check_eq(await lua(ed, "return object('Embers', 'ParticleSystem'):getAnalyticParticle(0, 1.0)"), None,
                "a Simulated system has no Analytic twin")
    got = await lua(ed, """
        local ps = object('Embers', 'ParticleSystem')
        ps.simulationMode = ParticleSimulationMode.Analytic
        local p = ps:getAnalyticParticle(0, 1.0)
        local alive = 0
        for s = 0, 199 do local q = ps:getAnalyticParticle(s, 1.0); if q and q.alive then alive = alive + 1 end end
        return {ps.simulationMode, p ~= nil, p and p.life or -1, alive}
    """)
    ed.check_eq(got[0], "analytic", f"Analytic: {got}")
    ed.check_eq(got[1], True, "the twin answers")
    ed.check(got[2] > 0 and got[3] > 0, f"live slots at 1 s: {got}")
    # listening to an Analytic system's events warns once (they never come)
    await lua(ed, """
        local ps = object('Embers', 'ParticleSystem')
        ps:onParticleDeath(function() end)
        ps:onParticleDeath(function() end)
        ps:onCollision(function() end)
        return true
    """)
    console = await ed.call_raw("read_console", {"contains": "analytic_no_events"})
    ed.check_eq(console.text.count("analytic_no_events: Embers"), 1, f"one warning: {console.text[:600]}")
    await ed.call("play", action="stop")
