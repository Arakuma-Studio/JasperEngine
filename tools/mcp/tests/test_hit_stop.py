"""Hit-Stop in Play over MCP (docs/specs/S48-pixel-fx.md R20-R21, AC16-AC17): HitStop.play from eval_lua
freezes Time.deltaTime, a falling Rigidbody 2D and game timers while unscaled timers run, resumes after its
unscaled seconds, keeps the longer of overlapping calls, slows with timeScale; a Hit Stop Exempt GameObject
(added with add_component hitstopexempt) and its child keep their scripts' Time.deltaTime, tweens and
particles running while another GameObject freezes; `only` freezes just the listed GameObjects."""

from harness import Editor

EDITOR = {"create": True, "name": "Hit Stop"}
CI = True
TIMEOUT = 900

DT = 1.0 / 60.0

# accumulates the deltas its GameObject's scripts see (Time.deltaTime and Engine.deltatime)
PROBE = """local HitProbe = {}
function HitProbe:init()
    RegisterEngineEvent(self, 'onUpdate')
end
function HitProbe:onUpdate()
    HSP = HSP or {}
    HSE = HSE or {}
    local name = GameObject(self.scene, self.entity).name
    HSP[name] = (HSP[name] or 0) + Time.deltaTime
    HSE[name] = (HSE[name] or 0) + Engine.deltatime
end
return HitProbe"""

# a coroutine and a self:invoke timer of its GameObject, started once HS_GO is set: Coroutine.wait
# and the timer pass on its clock, and what they read of Time.deltaTime goes to HS_WAITED / HS_TIMED
WAIT_PROBE = """local WaitProbe = {}
function WaitProbe:start()
    local name = GameObject(self.scene, self.entity).name
    while not HS_GO do Coroutine.nextFrame() end
    Coroutine.wait(0.05)
    HS_WAITED = HS_WAITED or {}
    HS_WAITED[name] = Time.deltaTime
    self:invoke('timed', 0.05)
end
function WaitProbe:timed()
    HS_TIMED = HS_TIMED or {}
    HS_TIMED[GameObject(self.scene, self.entity).name] = Time.deltaTime
end
return WaitProbe"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    scene = (await ed.call("create_scene", name="HitStop", type="2D"))["scene"]["id"]
    S["scene"] = scene
    hero = (await ed.call("create_entity", scene=scene, name="Hero", position=[0, 0, 0]))["entity_id"]
    cape = (await ed.call("create_entity", scene=scene, name="Cape", parent=hero, position=[0, 0, 0]))["entity_id"]
    enemy = (await ed.call("create_entity", scene=scene, name="Enemy", position=[100, 0, 0]))["entity_id"]
    ball = (await ed.call("create_entity", scene=scene, name="Ball", type="rigidbody2d", position=[0, 200, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=scene, entity=ball, body_type="dynamic", add=[{"shape": "circle", "radius": 8}])
    await ed.call("add_component", scene=scene, entity=hero, component="hitstopexempt")
    for name, entity in (("Hero", hero), ("Cape", cape), ("Enemy", enemy)):
        await ed.call("create_script", kind="lua", name="HitProbe" + name, content=PROBE, entity=entity)
    S.update(hero=hero, cape=cape, enemy=enemy, ball=ball)
    for name, entity in (("Hero", hero), ("Enemy", enemy)):
        await ed.call("create_script", kind="lua", name="WaitProbe" + name, content=WAIT_PROBE, entity=entity)
    await ed.call("create_entity", scene=scene, type="text", name="Label", position=[0, 0, 0], values={"text": {"text": "HIT", "fontSize": 48}})
    await ed.call("add_component", scene=scene, entity="Label", component="texteffect")
    await ed.call("save_scene", scene=scene, path="scenes/HitStop")
    return S


async def lua(ed: Editor, code: str) -> list:
    return (await ed.call("eval_lua", code=code))["values"]


async def test_component_and_its_fields(ed: Editor):
    s = await setup(ed)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["hero"], components=["hitstopexempt"])
    exempt = got["components"]["hitstopexempt"]
    ed.check_eq((exempt["enabled"], exempt["includeChildren"]), (True, True), f"defaults: {exempt}")
    await ed.call("set_properties", scene=s["scene"], entity=s["hero"], values={"hitstopexempt": {"includeChildren": False}})
    got = await ed.call("get_entity", scene=s["scene"], entity=s["hero"], components=["hitstopexempt"])
    ed.check_eq(got["components"]["hitstopexempt"]["includeChildren"], False)
    await ed.call("undo")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["hero"], components=["hitstopexempt"])
    ed.check_eq(got["components"]["hitstopexempt"]["includeChildren"], True, "one undo step")


async def test_freeze(ed: Editor):
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=10)
        before = await lua(ed, "local b = GameObject(Engine.scene, Engine.scene:findEntity('Ball')) "
                               "HS_game, HS_real = false, false "
                               "Timer.after(0.05, function() HS_game = true end) "
                               "Timer.after(0.05, function() HS_real = true end, {unscaled = true}) "
                               "HitStop.play(0.1) "
                               "return b.position.y, HitStop.active, HitStop.remaining")
        ed.check(before[1] is True and abs(before[2] - 0.1) < 1e-4, f"active with 0.1 s left: {before}")
        await ed.call("wait", frames=4)
        frozen = await lua(ed, "local b = GameObject(Engine.scene, Engine.scene:findEntity('Ball')) "
                               "return b.position.y, Time.deltaTime, HS_game, HS_real, HitStop.active, HitStop.remaining, "
                               "HitStop.unfrozenDeltaTime")
        ed.check_eq(frozen[1], 0.0, f"Time.deltaTime is 0 during the freeze: {frozen}")
        ed.check(abs(frozen[0] - before[0]) < 1e-6, f"the body does not move: {before[0]} -> {frozen[0]}")
        ed.check(frozen[2] is False, "a game-time timer waits")
        ed.check(frozen[3] is True, "an unscaled timer ran")
        ed.check(frozen[4] is True and abs(frozen[5] - (0.1 - 4 * DT)) < 1e-3, f"counting down on unscaled time: {frozen}")
        ed.check(abs(frozen[6] - DT) < 1e-5, f"the unfrozen delta: {frozen[6]}")

        await ed.call("wait", frames=4)
        after = await lua(ed, "local b = GameObject(Engine.scene, Engine.scene:findEntity('Ball')) "
                              "return b.position.y, Time.deltaTime, HitStop.active")
        ed.check(after[2] is False, f"over after 0.1 s of unscaled time: {after}")
        ed.check(abs(after[1] - DT) < 1e-5, f"Time.deltaTime runs again: {after}")
        ed.check(after[0] < frozen[0] - 1e-4, f"the body falls again: {frozen[0]} -> {after[0]}")
        await ed.call("wait", frames=4)
        ed.check((await lua(ed, "return HS_game"))[0] is True, "the game timer fires once the game runs")

        # overlapping calls keep the longer time; stop() ends it; timeScale slows
        values = await lua(ed, "HitStop.play(0.1) HitStop.play(0.05) local r = HitStop.remaining "
                               "HitStop.stop() return r, HitStop.active")
        ed.check(abs(values[0] - 0.1) < 1e-5 and values[1] is False, f"longer kept, stop ends: {values}")
        await lua(ed, "HitStop.play(0.1, {timeScale = 0.25})")
        await ed.call("wait", frames=2)
        slowed = (await lua(ed, "return Time.deltaTime"))[0]
        ed.check(abs(slowed - 0.25 * DT) < 1e-6, f"timeScale 0.25 slows to a quarter: {slowed}")
        await lua(ed, "HitStop.stop()")
        error = await ed.call_raw("eval_lua", {"code": "HitStop.play(0.1, {only = {42}})"})
        ed.check(error.is_error or "GameObject" in error.text, f"a bad list is refused with its fix: {error.text}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    errors = [e for e in (await ed.call("read_console", since=cursor, types=["error"]))["entries"] if "only[1]" not in e.get("message", "")]
    ed.check(not errors, f"unexpected errors: {errors}")


async def test_exempt(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await lua(ed, "local scene = Engine.scene "
                      "local hero = GameObject(scene, scene:findEntity('Hero')) "
                      "local enemy = GameObject(scene, scene:findEntity('Enemy')) "
                      "for _, pair in ipairs({{'h', hero}, {'e', enemy}}) do "
                      "  local tw = PositionTween(scene) tw:setTarget(pair[2]) "
                      "  tw:setAction(Vector3(0, 0, 0), Vector3(600, 0, 0), 10.0, false) tw:start() end "
                      "for _, pair in ipairs({{'HeroSparks', hero}, {'EnemySparks', enemy}}) do "
                      "  local ps = ParticleSystem(scene) ps.name = pair[1] ps:setParent(pair[2]) ps:play() end")
        await ed.call("wait", frames=3)
        # a hit-stop asked during a frame freezes from the next one
        await lua(ed, "HitStop.play(0.2)")
        await ed.call("wait", frames=1)
        start = await lua(ed, "HSP, HSE = {}, {} local scene = Engine.scene "
                              "local function x(n) return GameObject(scene, scene:findEntity(n)).position.x end "
                              "local function t(n) return ParticleSystem(scene, scene:findEntity(n)).time end "
                              "return x('Hero'), x('Enemy'), t('HeroSparks'), t('EnemySparks')")
        await ed.call("wait", frames=5)
        during = await lua(ed, "local scene = Engine.scene "
                               "local function x(n) return GameObject(scene, scene:findEntity(n)).position.x end "
                               "local function t(n) return ParticleSystem(scene, scene:findEntity(n)).time end "
                               "return HSP.Hero or 0, HSP.Cape or 0, HSP.Enemy or 0, HSE.Hero or 0, HSE.Enemy or 0, "
                               "x('Hero'), x('Enemy'), t('HeroSparks'), t('EnemySparks')")
        hero, cape, enemy, heroEngine, enemyEngine = during[:5]
        ed.check(abs(hero - 5 * DT) < 1e-4, f"the exempt GameObject's Time.deltaTime runs: {during}")
        ed.check(abs(cape - 5 * DT) < 1e-4, f"its child's too (includeChildren): {during}")
        ed.check_eq(enemy, 0.0, f"another GameObject's script sees 0: {during}")
        ed.check(abs(heroEngine - 5 * DT) < 1e-4 and enemyEngine == 0.0, f"Engine.deltatime the same: {during}")
        ed.check(during[5] > start[0] + 1e-3, f"the exempt tween moves: {start[0]} -> {during[5]}")
        ed.check(abs(during[6] - start[1]) < 1e-6, f"the other tween holds: {start[1]} -> {during[6]}")
        ed.check(during[7] > start[2] + 1e-3, f"the exempt particles run: {start[2]} -> {during[7]}")
        ed.check(abs(during[8] - start[3]) < 1e-6, f"the other particles hold: {start[3]} -> {during[8]}")

        # includeChildren off: the child freezes with the world (red control of the child case)
        await lua(ed, "HitStop.stop() local scene = Engine.scene "
                      "HitStopExempt(scene, scene:findEntity('Hero')).includeChildren = false HitStop.play(0.2)")
        await ed.call("wait", frames=1)
        await lua(ed, "HSP = {}")
        await ed.call("wait", frames=4)
        values = await lua(ed, "return HSP.Hero or 0, HSP.Cape or 0")
        ed.check(values[0] > 0 and values[1] == 0.0, f"without includeChildren the child freezes: {values}")
        await lua(ed, "HitStop.stop()")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_only(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await lua(ed, "local scene = Engine.scene "
                      "HitStop.play(0.2, {only = {GameObject(scene, scene:findEntity('Enemy'))}})")
        await ed.call("wait", frames=1)
        await lua(ed, "HSP = {}")
        await ed.call("wait", frames=5)
        values = await lua(ed, "return HSP.Enemy or 0, HSP.Hero or 0, Time.deltaTime, HitStop.active")
        ed.check_eq(values[0], 0.0, f"the listed GameObject freezes: {values}")
        ed.check(abs(values[2] - DT) < 1e-5, f"the world runs: {values}")
        ed.check(values[3] is True, "a hit-stop is running")
        # the world's scripts, exempt or not, see the frame delta
        ed.check(abs(values[1] - 5 * DT) < 1e-4, f"the others run: {values}")
        await lua(ed, "HitStop.stop()")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def probes_done(ed: Editor, frames: int, name: str) -> tuple:
    await ed.call("wait", frames=frames)
    return tuple(await lua(ed, f"return HS_WAITED and HS_WAITED['{name}'], HS_TIMED and HS_TIMED['{name}']"))


async def test_exempt_coroutines_and_timers_run_through_the_freeze(ed: Editor):
    """A Hit Stop Exempt GameObject's Coroutine.wait and self:invoke timer keep running at the unfrozen delta and read
    Time.deltaTime from it; a frozen GameObject's wait; a hit's `only` list freezes the listed GameObject's clock while the
    world runs."""
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await lua(ed, "HS_WAITED, HS_TIMED = nil, nil HitStop.play(1.0)")
        await ed.call("wait", frames=1)
        await lua(ed, "HS_GO = true")
        hero = await probes_done(ed, 10, "Hero")
        ed.check(hero[0] is not None, f"the exempt coroutine's wait passed during the freeze: {hero}")
        ed.check(hero[1] is not None, f"the exempt timer fired during the freeze: {hero}")
        ed.check(abs(hero[0] - DT) < 1e-4, f"the exempt coroutine reads the unfrozen delta: {hero}")
        ed.check(abs(hero[1] - DT) < 1e-4, f"the exempt timer's callback reads the unfrozen delta: {hero}")
        enemy = await probes_done(ed, 1, "Enemy")
        ed.check(enemy[0] is None, f"the frozen GameObject's wait did not pass during the freeze: {enemy}")
        await lua(ed, "HitStop.stop()")
        enemy = await probes_done(ed, 10, "Enemy")
        ed.check(enemy[0] is not None and enemy[1] is not None, f"it resumes with the game: {enemy}")
        await lua(ed, "HS_GO = nil")   # the next Play's probes wait again
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await lua(ed, "HS_WAITED, HS_TIMED = nil, nil "
                      "local scene = Engine.scene "
                      "HitStop.play(1.0, {only = {GameObject(scene, scene:findEntity('Enemy'))}})")
        await ed.call("wait", frames=1)
        await lua(ed, "HS_GO = true")
        enemy = await probes_done(ed, 10, "Enemy")
        ed.check(enemy[0] is None, f"the listed GameObject's wait is frozen: {enemy}")
        hero = await probes_done(ed, 1, "Hero")
        ed.check(hero[0] is not None and hero[1] is not None, f"the rest of the world runs: {hero}")
        await lua(ed, "HitStop.stop()")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"no script errors: {errors}")


async def test_unscaled_text_effect_runs_during_the_freeze(ed: Editor):
    """A Text Effect on unscaled time keeps moving while a hit-stop freezes the game delta (the unscaled frame, not
    dt / timeScale which is 0 in a freeze)."""
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        await lua(ed, "local fx = TextEffect(Engine.scene, Engine.scene:findEntity('Label')) "
                      "fx.useUnscaledTime = true fx.loop = true "
                      "fx:setLayer(1, {type = TextEffectType.Wave, amplitude = 0.8, duration = 1}) fx:play() "
                      "HitStop.play(5.0)")
        await ed.call("wait", frames=4)
        first = await ed.screenshot(view="game", max_width=0)
        await ed.call("wait", frames=15)
        second = await ed.screenshot(view="game", max_width=0)
        ed.check(first != second, "the unscaled Wave moves the glyphs during the freeze")
        ed.check((await lua(ed, "return HitStop.active"))[0] is True, "the freeze still held")
        await lua(ed, "HitStop.stop()")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
