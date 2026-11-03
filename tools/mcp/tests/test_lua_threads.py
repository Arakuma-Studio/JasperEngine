"""Play startup inspects scripts without using the game's Lua state; crash stop stays on its owner thread.

The real editor runs with JASPER_LUA_THREAD_CHECK=abort. An engine allocation by an
incorrect startup/shutdown thread therefore fails instead of depending on a race.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Lua State Isolation"}

WORKER = """---@class {cls} : ScriptInstance
local {cls} = {{ properties = {{
    {{ name = "speed", type = "float", default = {k}.5 }},
    {{ name = "label", type = "string", default = "worker{k}" }},
    {{ name = "offset", type = "vector3", default = {{ {k}, 2, 3 }} }},
}} }}
local up = Vector3(0, 1, 0)
threadLoads = (threadLoads or 0) + 1
function {cls}:init()
    self.seen = {{ self.speed, self.label, self.offset.x }}
end
return {cls}
"""

CRASHER = """---@class ThreadCrasher : ScriptInstance
local ThreadCrasher = { properties = {} }
function ThreadCrasher:init()
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onShutdown")
end
function ThreadCrasher:onUpdate()
    self.frames = self.frames + 1
    if self.frames > 1 then error("thread crash probe") end
end
function ThreadCrasher:onShutdown()
    local v = Vector3(1, 2, 3)
    crashShutdownRan = v.x
end
return ThreadCrasher
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="ThreadMain", type="3D", path="scenes/ThreadMain")
    S["main"] = made["scene"]["id"]
    for k in range(2):
        ent = await ed.call("create_entity", scene=S["main"], type="empty", name=f"Worker{k}")
        made = await ed.call("create_script", kind="lua", name=f"ThreadWorker{k}",
                             content=WORKER.format(cls=f"ThreadWorker{k}", k=k),
                             scene=S["main"], entity=ent["entity_id"])
        S[f"worker{k}"] = made["path"]
    await ed.call("save_scene", scene=S["main"])
    made = await ed.call("create_scene", name="ThreadCrash", type="3D", path="scenes/ThreadCrash")
    S["crash"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["crash"], type="empty", name="Crasher")
    await ed.call("create_script", kind="lua", name="ThreadCrasher", content=CRASHER,
                  scene=S["crash"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["crash"])
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def test_play_reads_properties_outside_the_game_state(ed: Editor):
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await lua(ed, "threadLoads = nil", allow_edit_mode=True)
    checked = await ed.call("check_script", path=s["worker0"])
    ed.check(checked["ok"], f"script whose top level uses the engine could not be inspected: {checked}")
    ed.check_eq({p["name"] for p in checked["properties"]}, {"speed", "label", "offset"})
    ed.check_eq(await lua(ed, "return threadLoads", allow_edit_mode=True), [None],
                "property inspection must not execute in the game's state")
    started = await ed.call("play", action="start", scene=s["main"])
    ed.check_eq(started["state"], "playing")
    try:
        await ed.call("wait", frames=1)
        for k in range(2):
            ed.check_eq(await lua(ed, f"return script('Worker{k}', 'ThreadWorker{k}', 'ThreadMain').seen"),
                        [[k + 0.5, f"worker{k}", k]], "runtime script receives its authored defaults")
        ed.check_eq(await lua(ed, "return threadLoads"), [2],
                    "each runtime script executes once; startup property inspection executes elsewhere")
    finally:
        await ed.call("play", action="stop")
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"unexpected script errors: {errors}")


async def test_a_crash_stops_play_on_the_main_thread(ed: Editor):
    s = await setup(ed)
    await lua(ed, "crashShutdownRan = nil", allow_edit_mode=True)
    await ed.call("play", action="start", scene=s["crash"], fixed_fps=60, auto_pause=True)
    result = await ed.call_raw("wait", {"frames": 10})
    ed.check(result.is_error and result.structured["reason"] == "play_ended",
             f"the script fault must end play: {result.text}")
    ed.check("thread crash probe" in result.text, "the reported failure identifies the script fault")
    ed.check_eq((await ed.call("get_play_state"))["state"], "stopped")
    ed.check_eq(await lua(ed, "return crashShutdownRan", allow_edit_mode=True), [1],
                "shutdown executes safely in the game's state on the main thread")
    await ed.call("play", action="start", scene=s["main"], fixed_fps=60, auto_pause=True)
    ed.check_eq(await lua(ed, "return script('Worker1', 'ThreadWorker1', 'ThreadMain').seen"),
                [[1.5, "worker1", 1]], "the editor can run a healthy scene after the fault")
    await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
