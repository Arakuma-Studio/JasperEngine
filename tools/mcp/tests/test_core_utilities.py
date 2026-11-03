"""S09 core utilities as an agent sees them: the logger (categories, levels, sources, the MCP
log stream), RAII event subscriptions released with their script, and the scheduler behind Lua's
Timer (game, unscaled and real clocks, pause, owners, coroutines).

Scene CoreUtil (3D): TagOwner (a Lua script registering Engine.onUpdate with an explicit tag),
TimerOwner (a Lua script with a timer owned by itself). Play runs with auto_pause and a fixed
1/60 s step, so frame counts are exact.
"""

import asyncio
import json

from harness import Editor, RawEditor

CI = True
EDITOR = {"create": True, "name": "Core Utilities CI"}

TIMEOUT = 900

TAG_OWNER = """---@class TagOwner : ScriptInstance
local TagOwner = { properties = {} }

function TagOwner:init()
    tagCalls = 0
    -- an explicit tag: before S09 it left the subscription behind when the script went away
    RegisterEvent(self, Engine.onUpdate, "tick", "myTag")
end

function TagOwner:tick()
    tagCalls = tagCalls + 1
end

return TagOwner
"""

TIMER_OWNER = """---@class TimerOwner : ScriptInstance
local TimerOwner = { properties = {} }

function TimerOwner:init()
    ownedFired = false
    Timer.after(0.5, function() ownedFired = true end, { owner = self })
end

return TimerOwner
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="CoreUtil", type="3D", path="scenes/CoreUtil")
    S["scene"] = made["scene"]["id"]
    for name, cls, code in (("TagOwner", "TagOwner", TAG_OWNER), ("TimerOwner", "TimerOwner", TIMER_OWNER)):
        ent = await ed.call("create_entity", scene=S["scene"], type="empty", name=name)
        await ed.call("create_script", kind="lua", name=cls, content=code, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def start(ed: Editor) -> None:
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def no_errors(ed: Editor, since) -> None:
    errors = [e.get("message", "") for e in await ed.console_since(since, types=["error"], limit=1000)]
    ed.check(not errors, f"errors logged: {errors[:10]}")


async def cursor(ed: Editor) -> int:
    return (await ed.call("read_console", limit=1))["next_cursor"]


# -- timers ------------------------------------------------------------------------


async def test_timer_after_game_time(ed: Editor):
    await start(ed)
    begin = await lua(ed, "afterFrame = nil; Timer.after(0.5, function() afterFrame = Engine.frameCount end) "
                          "return Engine.frameCount")
    await ed.call("wait", frames=29)
    ed.check_eq(await lua(ed, "return afterFrame"), [None], "not before 0.5 s of game time")
    await ed.call("wait", frames=1)
    ed.check_eq(await lua(ed, "return afterFrame"), [begin[0] + 30], "on frame 30 at 1/60 s")
    await stop(ed)


async def test_timer_respects_editor_pause(ed: Editor):
    await start(ed)
    await ed.call("play", action="set", auto_pause=False)
    await lua(ed, "pausedFired = false; Timer.after(0.25, function() pausedFired = true end)")
    paused = await ed.call("play", action="pause")
    ed.check_eq(paused["state"], "paused", f"pause: {paused}")
    await asyncio.sleep(1.0)
    ed.check_eq(await lua(ed, "return pausedFired"), [False], "a game timer does not run in the editor's Pause")
    await lua(ed, "stepFrames = 0; Timer.nextFrame(function() stepFrames = stepFrames + 1 end) "
                  "Timer.afterFrames(2, function() stepFrames = stepFrames + 10 end)")
    stepped = await ed.call("play", action="step", frames=1)
    ed.check_eq(stepped["stepped_frames"], 1, f"step: {stepped}")
    ed.check_eq(await lua(ed, "return stepFrames"), [1], "Step advances exactly one frame")
    await ed.call("play", action="resume")
    await ed.call("wait", frames=20)
    ed.check_eq(await lua(ed, "return pausedFired, stepFrames"), [True, 11], "runs once resumed")
    await stop(ed)


async def test_timer_timescale_zero(ed: Editor):
    await start(ed)
    await ed.call("play", action="set", time_scale=0)
    await lua(ed, "scaledFired = false; unscaledFired = false "
                  "Timer.after(0.1, function() scaledFired = true end) "
                  "Timer.after(0.1, function() unscaledFired = true end, { clock = 'unscaled' })")
    await ed.call("wait", frames=12)
    ed.check_eq(await lua(ed, "return scaledFired, unscaledFired"), [False, True],
                "time scale 0 freezes the game clock, not the unscaled one")
    await ed.call("play", action="set", time_scale=1)
    await stop(ed)


async def test_timer_owner_destroyed(ed: Editor):
    since = await cursor(ed)
    await start(ed)
    await ed.call("wait", frames=5)
    await lua(ed, "GameObject.destroy(script('TimerOwner', 'TimerOwner', 'CoreUtil'))")
    await ed.call("wait", frames=40)
    ed.check_eq(await lua(ed, "return ownedFired"), [False], "the owner's timer died with it")
    await stop(ed)
    await no_errors(ed, since)


async def test_lua_explicit_tag_released(ed: Editor):
    since = await cursor(ed)
    await start(ed)
    await ed.call("wait", frames=5)
    before = (await lua(ed, "return tagCalls"))[0]
    ed.check(before >= 4, f"the tagged subscription runs: {before}")
    await lua(ed, "GameObject.destroy(script('TagOwner', 'TagOwner', 'CoreUtil'))")
    await ed.call("wait", frames=2)
    gone = (await lua(ed, "return tagCalls"))[0]
    await ed.call("wait", frames=20)
    ed.check_eq((await lua(ed, "return tagCalls"))[0], gone, "an explicit tag is released with its script")
    await stop(ed)
    await no_errors(ed, since)


async def test_timer_play_stop_clears(ed: Editor):
    await start(ed)
    await lua(ed, "Timer.every(10, function() end) Timer.after(100, function() end, { clock = 'real' })")
    timers = await ed.call("list_timers")
    ed.check(timers["total_game_pending"] >= 2, f"pending while playing: {timers['game']['pending']}")
    names = [t["name"] for t in timers["game"]["nearest"]]
    ed.check("Timer.every" in names and "Timer.after" in names, f"named: {names}")
    await stop(ed)
    timers = await ed.call("list_timers")
    ed.check_eq(timers["total_game_pending"], 0, f"Stop clears the game's timers: {timers['game']}")


async def test_coroutine_wait(ed: Editor):
    await start(ed)
    await lua(ed, """
        coLog = {}
        coFlag = false
        coHandle = Timer.run(function()
            table.insert(coLog, "start")
            Timer.wait(0.1)
            table.insert(coLog, "waited")
            Timer.waitFrames(2)
            table.insert(coLog, "frames")
            Timer.waitUntil(function() return coFlag end)
            table.insert(coLog, "until")
        end)
    """)
    ed.check_eq(await lua(ed, "return table.concat(coLog, ',')"), ["start"], "runs to its first wait at once")
    await ed.call("wait", frames=10)
    ed.check_eq(await lua(ed, "return table.concat(coLog, ',')"), ["start,waited,frames"], "wait and waitFrames")
    await ed.call("wait", frames=3)
    ed.check_eq(await lua(ed, "return table.concat(coLog, ',')"), ["start,waited,frames"], "waitUntil polls")
    await lua(ed, "coFlag = true")
    await ed.call("wait", frames=2)
    ed.check_eq(await lua(ed, "return table.concat(coLog, ','), coHandle:isActive()"), ["start,waited,frames,until", False],
                "the coroutine finished")
    r = await ed.call_raw("eval_lua", {"code": "Timer.wait(1)"})
    ed.check("Timer.wait must be called inside Timer.run" in r.text, f"outside a coroutine: {r.text[:400]}")
    await stop(ed)


async def test_timer_error_pauses_play(ed: Editor):
    since = await cursor(ed)
    await start(ed)
    await ed.call("play", action="set", auto_pause=False)
    await lua(ed, "Timer.after(0.05, function() error('timer boom') end)")
    r = await ed.call_raw("wait", {"seconds": 10})
    st = await ed.call("get_play_state")
    ed.check(st["state"] != "playing", f"the erroring timer paused (then stopped) Play: {st['state']} / {r.text[:300]}")
    errors = [e.get("message", "") for e in await ed.console_since(since, types=["error"], limit=100)]
    ed.check(any("timer boom" in e and "traceback" in e for e in errors), f"reported with a traceback: {errors[:3]}")
    if st["state"] != "stopped":
        await stop(ed)


async def test_editor_tasks_fifo(ed: Editor):
    """Main-thread tasks go through the editor scheduler: many concurrent calls, all answered."""
    results = await asyncio.gather(*[ed.call("read_console", limit=1) for _ in range(30)])
    ed.check_eq(len(results), 30, "every call answered")
    timers = await ed.call("list_timers", domain="real")
    ed.check("editor" in timers and "pending" in timers["editor"], f"editor scheduler listed: {timers}")


# -- logging -------------------------------------------------------------------------


async def test_read_console_category_level_fields(ed: Editor):
    since = await cursor(ed)
    await lua(ed, "Log.category('coretest'):warn('category warning') Log.warn('lua warning') Log.info('lua info')",
              allow_edit_mode=True)
    r = await ed.call("read_console", since=since, categories=["coretest"])
    ed.check_eq(len(r["entries"]), 1, f"category filter: {r['entries']}")
    e = r["entries"][0]
    ed.check_eq((e["level"], e["category"], e["type"]), ("warn", "coretest", "warning"), f"fields: {e}")
    ed.check("source" in e and e["source"]["line"] >= 1, f"a Lua record has its chunk:line: {e}")
    ed.check(isinstance(e["frame"], int), f"frame: {e}")
    r = await ed.call("read_console", since=since, levels=["info"], categories=["lua"])
    ed.check_eq([x["message"] for x in r["entries"]], ["lua info"], f"level filter: {r['entries']}")
    r = await ed.call("read_console", since=since, has_source=False)
    ed.check(all("source" not in x for x in r["entries"]), "has_source false")


async def test_set_log_level_roundtrip(ed: Editor):
    await ed.call("set_log_level", level="debug", category="render")
    cfg = await ed.call("get_log_config")
    render = [c for c in cfg["categories"] if c["name"] == "render"]
    ed.check_eq(render[0]["level"], "debug", f"render: {render}")
    ed.check(any(s["name"] == "memory" for s in cfg["sinks"]), f"sinks: {cfg['sinks']}")
    ed.check("dropped" in cfg["counters"] and "suppressed" in cfg["counters"], f"counters: {cfg['counters']}")
    await ed.call("set_log_level", level="error", sink="file", category="lua")
    cfg = await ed.call("get_log_config")
    file_sink = [s for s in cfg["sinks"] if s["name"] == "file"]
    ed.check(file_sink and file_sink[0]["category_levels"].get("lua") == "error", f"file sink: {file_sink}")
    await ed.call("set_log_level", level="info", category="render")
    await ed.call("set_log_level", level="info", sink="file", category="lua")
    bad = await ed.call_raw("set_log_level", {"level": "loud"})
    ed.check(bad.is_error, "an unknown level is refused")


async def test_wait_for_log(ed: Editor):
    since = await cursor(ed)
    waiting = asyncio.create_task(ed.call("wait_for_log", since=since, contains="waited for this", timeout_ms=10000))
    await asyncio.sleep(0.3)
    await lua(ed, "Log.info('waited for this')", allow_edit_mode=True)
    r = await waiting
    ed.check_eq(r["timed_out"], False, f"wait_for_log: {r}")
    ed.check_eq(r["entries"][0]["message"], "waited for this", f"entries: {r['entries']}")
    t = await ed.call("wait_for_log", since=r["next_cursor"], contains="never logged", timeout_ms=300)
    ed.check_eq(t["timed_out"], True, f"timeout: {t}")


async def test_mcp_logging_notifications():
    async with RawEditor(create=True, name="Core Utilities Log Stream") as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "logging/setLevel", "params": {"level": "warning"}})
        r = await raw.response(2)
        if "error" in r:
            raw.fail(f"logging/setLevel: {r}")
        raw.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
            "name": "eval_lua", "arguments": {"code": "Log.info('not sent') Log.warn('streamed warning')",
                                              "allow_edit_mode": True}}})
        await raw.response(3)
        deadline = asyncio.get_running_loop().time() + 10
        found = None
        while asyncio.get_running_loop().time() < deadline and not found:
            for m in raw.messages:
                p = m.get("params") or {}
                if m.get("method") == "notifications/message" and "streamed warning" in json.dumps(p.get("data")):
                    found = p
            await asyncio.sleep(0.1)
        if not found:
            raw.fail("no notifications/message for the warning")
        if found["level"] != "warning" or found["logger"] != "lua":
            raw.fail(f"notification fields: {found}")
        infos = [m for m in raw.messages if m.get("method") == "notifications/message" and
                 "not sent" in json.dumps((m.get("params") or {}).get("data"))]
        if infos:
            raw.fail(f"below the session's level, yet sent: {infos}")
