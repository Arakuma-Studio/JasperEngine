"""Coroutines and timers in editor Play (spec docs/superpowers/specs/2026-10-31-coroutines-design.md 10):
a coroutine moves a GameObject in game time, get_runtime_entity lists what each script's coroutines
wait for and its timers, get_play_state counts them, a coroutine error reads with its origin and
traceback and ends Play, and Play stop leaves nothing behind."""

from harness import Editor

EDITOR = {"create": True, "name": "Coroutines"}
CI = True

MOVER = """---@class CoMover : ScriptInstance
local CoMover = { properties = {} }

function CoMover:init()
    self.go = GameObject(self.scene, self.entity)
    self.steps = 0
    self.mover = self:startCoroutine(function()
        for i = 1, 3 do
            Coroutine.wait(0.1)
            local p = self.go.position
            self.go.position = Vector3(p.x + 1, p.y, p.z)
            self.steps = i
        end
        Coroutine.waitUntil(function() return self.released end)
    end)
    self:startCoroutine("idle")
    self:invokeRepeating("tick", 10, 5)
end

function CoMover:idle()
    while true do Coroutine.nextFrame() end
end

function CoMover:tick() end

return CoMover
"""

CRASHER = """---@class CoCrasher : ScriptInstance
local CoCrasher = { properties = {} }

local function explode()
    error("coroutine probe failure")
end

function CoCrasher:init()
    self:startCoroutine(function()
        Coroutine.wait(0.05)
        explode()
    end)
end

return CoCrasher
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="CoMain", type="3D", path="scenes/CoMain")
    S["main"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["main"], type="empty", name="Mover")
    S["mover"] = ent["entity_id"]
    await ed.call("create_script", kind="lua", name="CoMover", content=MOVER, scene=S["main"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["main"])
    made = await ed.call("create_scene", name="CoCrash", type="3D", path="scenes/CoCrash")
    S["crash"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["crash"], type="empty", name="Crasher")
    await ed.call("create_script", kind="lua", name="CoCrasher", content=CRASHER, scene=S["crash"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["crash"])
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


def mover_script(entity: dict) -> dict:
    return next(s for s in entity["scripts"] if s["class"] == "CoMover")


async def test_a_coroutine_moves_in_game_time_and_is_listed(ed: Editor):
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    started = await ed.call("play", action="start", scene=s["main"], fixed_fps=60, auto_pause=True)
    ed.check_eq(started["state"], "playing")
    try:
        await ed.call("wait", game_seconds=0.35)
        values = await lua(ed, "local m = script('Mover', 'CoMover') return m.steps, m.go.position.x, m.mover.status")
        ed.check_eq(values, [3, 3.0, "suspended"], "three moves of Coroutine.wait(0.1) in 0.35 game seconds")

        entity = await ed.call("get_runtime_entity", entity="Mover")
        script = mover_script(entity)
        coroutines = script.get("coroutines", [])
        ed.check_eq(len(coroutines), 2, f"both coroutines listed: {script}")
        waiting = sorted(c.get("waiting", "") for c in coroutines)
        ed.check_eq(waiting, ["frames 1", "until"], f"what each coroutine waits for: {coroutines}")
        ed.check(any(c.get("method") == "idle" for c in coroutines), f"the one started by name names its method: {coroutines}")
        ed.check(all("CoMover.lua:" in c["origin"] for c in coroutines), f"origins: {coroutines}")
        timers = script.get("timers", [])
        ed.check_eq([(t["callback"], t["interval"], t["unscaled"]) for t in timers], [("tick", 5.0, False)],
                    f"the invokeRepeating timer: {timers}")
        ed.check(0 < timers[0]["remaining"] <= 10, f"remaining seconds: {timers}")

        state = await ed.call("get_play_state")
        ed.check_eq((state["coroutines"], state["timers"]), (2, 1), f"get_play_state totals: {state}")

        # a function eval_lua made runs on after the call; its print reaches the Console (no dead context)
        await lua(ed, "Coroutine.start(function() Coroutine.wait(0.05) print('late coroutine print') end) "
                      "Timer.after(0.05, function() print('late timer print') end)")
        await ed.call("wait", game_seconds=0.1)
        console = await ed.call("read_console", since=cursor)
        texts = " ".join(e["message"] for e in console["entries"])
        ed.check("late coroutine print" in texts and "late timer print" in texts, f"late prints: {texts[-800:]}")
        ed.check_eq((await ed.call("get_play_state"))["coroutines"], 2, "the eval coroutine ended")

        # deactivation stops the script's coroutines, its timer goes on
        await lua(ed, "script('Mover', 'CoMover').go:setActive(false)")
        script = mover_script(await ed.call("get_runtime_entity", entity="Mover"))
        ed.check_eq((len(script.get("coroutines", [])), len(script.get("timers", []))), (0, 1),
                    f"deactivated: coroutines stop, timers continue: {script}")
        # an unowned coroutine, to show nothing survives Play stop
        await lua(ed, "Coroutine.start(function() while true do Coroutine.nextFrame() end end)")
        ed.check_eq((await ed.call("get_play_state"))["coroutines"], 1)
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    state = await ed.call("get_play_state")
    ed.check_eq((state["coroutines"], state["timers"]), (0, 0), f"Play stop ends every coroutine and timer: {state}")

    await ed.call("play", action="start", scene=s["main"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        state = await ed.call("get_play_state")
        ed.check_eq((state["coroutines"], state["timers"]), (2, 1), f"a new Play has only its own: {state}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"unexpected script errors: {errors}")


async def test_a_coroutine_error_names_its_origin_and_ends_play(ed: Editor):
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=s["crash"], fixed_fps=60, auto_pause=True)
    result = await ed.call_raw("wait", {"game_seconds": 0.5})
    ed.check(result.is_error and result.structured["reason"] == "play_ended", f"the coroutine error ends Play: {result.text}")
    ed.check_eq((await ed.call("get_play_state"))["state"], "stopped")
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    text = "\n".join(e["message"] for e in errors)
    ed.check("Lua coroutine error in CoCrasher (GameObject 'Crasher', scene 'CoCrash'), started at " in text
             and "CoCrasher.lua:9: " in text, f"the error names the script, GameObject, scene and start: {text[:1500]}")
    ed.check("coroutine probe failure" in text and "stack traceback:" in text and "explode" in text,
             f"the coroutine's own traceback: {text[:1500]}")
    ed.check("Location: coroutine:" in text, f"the crash report locates the coroutine: {text[:1500]}")
