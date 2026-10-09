"""The engine's Lua state is only used on the editor's main thread.

Lua states are not thread-safe. Play's startup thread used to read the scripts' properties by
running them in the game's state (Project::runPlayStartup -> ProjectUtils::loadLuaScriptProperties),
while the main thread ran Lua too, and a script crash stopped play from a helper thread (the
scripts' onShutdown ran there). The harness starts the editor with JASPER_LUA_THREAD_CHECK=abort:
any use of the state from another thread aborts the editor (LuaBinding::ThreadCheck), so these
tests fail on the first one.

Scenes: ThreadMain (16 entities, a Lua script each whose top level allocates a lot, uses the
engine API and counts its runs in a global), ThreadCrash (a script whose onUpdate errors and
whose onShutdown allocates).
"""

import asyncio

from harness import Editor

TIMEOUT = 900

WORKERS = 16

WORKER = """---@class {cls} : ScriptInstance
local {cls} = {{
    properties = {{
        {{ name = "speed", type = "float", default = {k}.5 }},
        {{ name = "label", type = "string", default = "worker{k}" }},
        {{ name = "offset", type = "vector3", default = {{ {k}, 2, 3 }} }},
    }}
}}

-- the top level runs each time the script is loaded: it allocates (garbage for the collector),
-- calls the engine API, and counts its runs in the state it runs in
local junk = {{}}
for i = 1, 20000 do junk[i] = {{ i, tostring(i) .. "x" }} end
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
    if self.frames > 5 then error("thread crash probe") end
end

-- runs when the crash stops play: allocates in the Lua state
function ThreadCrasher:onShutdown()
    local junk = {}
    for i = 1, 20000 do junk[i] = { i, tostring(i) } end
    crashShutdownRan = #junk
end

return ThreadCrasher
"""

# check_script runs a script's top level; this one allocates a lot
HEAVY = """local T = { properties = { { name = "n", type = "int", default = 1 } } }
local junk = {}
for i = 1, 200000 do junk[i] = { i, tostring(i) } end
return T
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    """The scenes, once per editor (a test that killed the editor gets a fresh one)."""
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="ThreadMain", type="3D", path="scenes/ThreadMain")
    S["main"] = made["scene"]["id"]
    for k in range(WORKERS):
        ent = await ed.call("create_entity", scene=S["main"], type="empty", name=f"Worker{k}")
        made = await ed.call("create_script", kind="lua", name=f"ThreadWorker{k}",
                             content=WORKER.format(cls=f"ThreadWorker{k}", k=k), scene=S["main"], entity=ent["entity_id"])
        S[f"worker{k}"] = made["path"]
    await ed.call("save_scene", scene=S["main"])

    made = await ed.call("create_scene", name="ThreadCrash", type="3D", path="scenes/ThreadCrash")
    S["crash"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["crash"], type="empty", name="Crasher")
    await ed.call("create_script", kind="lua", name="ThreadCrasher", content=CRASHER, scene=S["crash"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["crash"])
    await ed.call("save_project")
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def no_errors(ed: Editor) -> None:
    await ed.check_alive()
    errors = [e.get("message", "") for e in await ed.console_since(types=["error"], limit=1000)]
    ed.check(not errors, f"errors logged: {errors[:10]}")


async def test_play_reads_properties_outside_the_game_state(ed: Editor):
    """Play starts with every script's properties, read without running the scripts in the
    game's Lua state: the game ran each script's top level once, for its instance."""
    s = await setup(ed)
    await ed.console_since(None, limit=1)   # errors are checked from here
    await lua(ed, "threadLoads = nil; return true", allow_edit_mode=True)
    started = await ed.call("play", action="start", scene=s["main"])
    ed.check_eq(started["state"], "playing", "play starts")
    await ed.call("wait", frames=3)
    for k in (0, 7, WORKERS - 1):
        seen = await lua(ed, f"return script('Worker{k}', 'ThreadWorker{k}', 'ThreadMain').seen")
        ed.check_eq(seen, [[k + 0.5, f"worker{k}", k]], f"Worker{k} started with its property defaults")
    ed.check_eq(await lua(ed, "return threadLoads"), [WORKERS],
                "the game's state ran each script once (reading properties ran none there)")
    stopped = await ed.call("play", action="stop")
    ed.check_eq(stopped["state"], "stopped", "stop")
    await no_errors(ed)


async def test_properties_of_scripts_using_the_engine_at_top_level(ed: Editor):
    """The editor still reads the properties of a script whose top level calls the engine API
    (inert there), and that does not run in the game's state either."""
    await setup(ed)
    await ed.console_since(None, limit=1)
    await lua(ed, "threadLoads = nil; return true", allow_edit_mode=True)
    r = await ed.call("check_script", path=S["worker3"])
    ed.check_eq(r["ok"], True, f"check_script: {r}")
    ed.check_eq([p["name"] for p in r["properties"]], ["speed", "label", "offset"], "properties")
    ent = await ed.call("create_entity", scene=S["main"], type="empty", name="Reader")
    attached = await ed.call("attach_script", entity=ent["entity_id"], scene=S["main"], path=S["worker3"])
    ed.check_eq([p["name"] for p in attached["script"]["properties"]], ["speed", "label", "offset"],
                f"the Inspector's properties: {attached['script']}")
    ed.check_eq(await lua(ed, "return threadLoads", allow_edit_mode=True), [None],
                "reading properties ran the script in the game's state")
    await ed.call("undo")
    await ed.call("undo")
    await no_errors(ed)


async def test_start_stop_while_the_main_thread_runs_lua(ed: Editor):
    """Play start-stop cycles while the main thread runs Lua that allocates heavily: in edit
    mode before each start, during the startup (check_script runs a script's top level), and in
    the game. The startup thread must not touch the state meanwhile."""
    s = await setup(ed)
    await ed.console_since(None, limit=1)
    for cycle in range(5):
        await lua(ed, "local t = {} for i = 1, 100000 do t[i] = {i} end collectgarbage('step', 200) return #t",
                  allow_edit_mode=True)
        results = await asyncio.gather(
            ed.call("play", action="start", scene=s["main"]),
            *[ed.call("check_script", code=HEAVY, language="lua") for _ in range(4)])
        ed.check_eq(results[0]["state"], "playing", f"cycle {cycle}: play starts")
        for r in results[1:]:
            ed.check_eq(r["ok"], True, f"cycle {cycle}: check_script: {r}")
        got = await lua(ed, "local t = {} for i = 1, 100000 do t[i] = tostring(i) end collectgarbage() "
                            "return script('Worker1', 'ThreadWorker1', 'ThreadMain').seen[2]")
        ed.check_eq(got, ["worker1"], f"cycle {cycle}: the game runs")
        await ed.call("play", action="stop")
    await no_errors(ed)


async def test_a_crash_stops_play_on_the_main_thread(ed: Editor):
    """A script error in play (the editor's crash guard) stops play; the stop runs the
    scripts' onShutdown, which uses the Lua state: on the main thread."""
    s = await setup(ed)
    await lua(ed, "crashShutdownRan = nil; return true", allow_edit_mode=True)
    started = await ed.call("play", action="start", scene=s["crash"])
    ed.check_eq(started["state"], "playing", "play starts")
    r = await ed.call_raw("wait", {"seconds": 10})
    ed.check(r.is_error and r.structured["reason"] == "play_ended", f"the crash ends play: {r.text[:500]}")
    ed.check("thread crash probe" in r.text, f"the script error is reported: {r.text[:800]}")
    st = await ed.call("get_play_state")
    ed.check_eq(st["state"], "stopped", "the crash stopped play")
    ed.check_eq(await lua(ed, "return crashShutdownRan", allow_edit_mode=True), [20000], "onShutdown ran at the stop")
    await ed.check_alive()
    # and play works after it
    started = await ed.call("play", action="start", scene=s["main"])
    ed.check_eq(started["state"], "playing", "a session after the crash")
    await ed.call("play", action="stop")
    await ed.check_alive()
