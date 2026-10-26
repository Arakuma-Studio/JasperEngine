"""S28 crash and hang diagnostics (docs/crash-diagnostics.md): the diagnostics group's tools.

get_editor_health answers off the main thread while debug_crash hangs it; the hang becomes a
report of this session; a runaway Lua function in Play is interrupted and Play goes on; a
session killed without a clean exit is reported at the next start (the notice, Lua
Crash.setReportHook, dismiss_crash_notice). Editors that need flags or a shared report root
start their own.
"""

import asyncio
import os
import signal
import tempfile
import time
from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Crash CI"}

TIMEOUT = 900

S28_TOOLS = ["get_editor_health", "get_crash_reports", "dismiss_crash_notice", "set_hang_watchdog", "debug_crash"]

RUNAWAY = """---@class Runaway : ScriptInstance
local Runaway = { properties = {} }

function Runaway:init()
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

-- the third frame never returns, until the hang watchdog interrupts it
function Runaway:onUpdate()
    self.frames = self.frames + 1
    runawayFrames = self.frames
    if self.frames == 3 then
        while true do end
    end
end

return Runaway
"""


async def test_tools_are_in_the_diagnostics_group(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    for name in S28_TOOLS:
        ed.check(name not in default, f"{name} is hidden until diagnostics is enabled")
        ed.check(ed.has_tool(name), f"{name} is registered")
    await ed.call("tool_groups", enable=["diagnostics"])
    listed = {t.name for t in await ed.tools(groups=None)}
    for name in S28_TOOLS:
        ed.check(name in listed, f"{name} is listed once diagnostics is enabled")


async def test_debug_crash_is_refused_without_the_flag(ed: Editor):
    text = await ed.call("debug_crash", mode="segv", expect_error=True)
    ed.check("--allow-debug-crash" in text, f"the refusal says how to allow it: {text}")
    health = await ed.call("get_editor_health")
    ed.check(health["responsive"], f"still responsive: {health}")
    ed.check(health["watchdog"]["enabled"], "the editor runs the hang watchdog by default")


async def test_health_off_main():
    async with Editor(create=True, name="Crash Health CI", extra_args=["--allow-debug-crash"]) as ed:
        await ed.call("set_hang_watchdog", threshold_ms=1000)
        before = await ed.call("get_crash_reports")
        ed.check(not [r for r in before["reports"] if r["this_session"]], "no report of this session yet")

        answer = await ed.call("debug_crash", mode="hang", hang_ms=6000)
        ed.check_eq(answer["mode"], "hang")
        await asyncio.sleep(2.5)
        start = time.monotonic()
        health = await ed.call("get_editor_health", timeout=10)
        elapsed = time.monotonic() - start
        ed.check(elapsed < 0.5, f"get_editor_health answered in {elapsed:.2f} s while the main thread hangs")
        ed.check(not health["responsive"], f"responsive is false during the hang: {health}")
        ed.check(health["stalled_ms"] >= 1000, f"stalled_ms: {health}")
        ed.check_eq(health["phase"], "debug_crash:hang", "the hang's phase")

        # after the hang a normal (main-thread) call answers again, and the hang was reported
        reports = await ed.call("get_crash_reports", include_text=True, timeout=60)
        mine = [r for r in reports["reports"] if r["this_session"]]
        ed.check(mine and mine[0]["kind"] == "hang", f"this session's hang is listed: {reports}")
        ed.check("hang-001.txt" in mine[0]["files"], f"hang-001.txt: {mine[0]['files']}")
        ed.check("phase: debug_crash:hang" in mine[0].get("text", ""), "the report names the phase")
        health = await ed.call("get_editor_health")
        ed.check(health["responsive"], f"responsive again: {health}")
        ed.check(health["hang_reports"] >= 1, f"hang_reports: {health}")
        ed.check("JASPERENGINE HANG:" in ed.stderr_text(), "the stderr line")


async def test_runaway_lua():
    async with Editor(create=True, name="Crash Lua CI") as ed:
        await ed.call("set_hang_watchdog", enabled=True, threshold_ms=3000, break_lua_after_s=2)
        made = await ed.call("create_scene", name="Runaway", type="3D", path="scenes/Runaway")
        scene = made["scene"]["id"]
        ent = await ed.call("create_entity", scene=scene, type="empty", name="Runner")
        await ed.call("create_script", kind="lua", name="Runaway", content=RUNAWAY, scene=scene, entity=ent["entity_id"])
        await ed.call("save_scene", scene=scene)
        await ed.console_since(None, limit=1)
        started = await ed.call("play", action="start", scene=scene)
        ed.check_eq(started["state"], "playing", "play starts")
        deadline = time.monotonic() + 30
        found = []
        while time.monotonic() < deadline and not found:
            found = await ed.console_since(contains="script interrupted", limit=10)
            if not found:
                await asyncio.sleep(0.5)
        ed.check(found, "the Console has the script interrupted error")
        message = found[0].get("message", "")
        ed.check("ran for" in message and "hang watchdog" in message, f"the message: {message}")
        health = await ed.call("get_editor_health")
        ed.check(health["responsive"], f"responsive after the interrupt: {health}")
        await ed.call("wait", frames=3)
        state = await ed.call("get_play_state")
        ed.check_eq(state["state"], "playing", "Play goes on after the interrupt")
        frames = (await ed.call("eval_lua", code="return runawayFrames"))["values"][0]
        ed.check(frames > 3, f"later frames ran: {frames}")
        await ed.call("play", action="stop")


async def test_notice_after_abnormal_exit():
    root = Path(tempfile.mkdtemp(prefix="jasper-crash-root-"))
    env = {"JASPERENGINE_CRASH_REPORT_DIR": str(root)}

    first = Editor(create=True, name="Crash Notice CI", env=env)
    await first.__aenter__()
    try:
        session = (await first.call("get_crash_reports"))["session"]
        pid = int(session.rsplit("-", 1)[1])
        first.check((root / "sessions" / f"{pid}.json").is_file(), "the session marker exists while it runs")
        os.kill(pid, signal.SIGKILL)
        for _ in range(100):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            await asyncio.sleep(0.1)
    finally:
        await first.close()

    async with Editor(create=True, name="Crash Notice CI 2", env=env) as ed:
        reports = await ed.call("get_crash_reports")
        ed.check(reports["notice_open"], f"the notice is open after a killed session: {reports}")
        ed.check_eq(reports["notice"]["id"], session, "the notice is about that session")
        killed = [r for r in reports["reports"] if r["id"] == session]
        ed.check(killed and killed[0]["kind"] == "abnormal_exit", f"listed as abnormal_exit: {reports['reports']}")
        ed.check(not killed[0]["handled"], "not handled yet")
        ed.check("closed unexpectedly last time" in reports["notice"]["text"], reports["notice"]["text"])
        warned = await ed.console_since(None, contains="closed unexpectedly last time", limit=5)
        ed.check(warned, "the Console has the notice too")

        # the Lua API sees it; a hook that declines leaves it pending
        hooked = (await ed.call("eval_lua", allow_edit_mode=True, code=(
            "local calls = {} "
            "Crash.setReportHook(function(r) calls[#calls + 1] = r.kind .. ' ' .. r.id; return false end) "
            "return calls, Crash.lastSessionEndedAbnormally(), #Crash.getPendingReports()")))["values"]
        ed.check_eq(hooked[0], [f"abnormal_exit {session}"], "the hook got the pending report once")
        ed.check_eq(hooked[1], True, "lastSessionEndedAbnormally")
        ed.check_eq(hooked[2], 1, "still pending after a declining hook")

        dismissed = await ed.call("dismiss_crash_notice", id=session)
        ed.check_eq(dismissed["notice_open"], False, "Dismiss closes the notice")
        ed.check((root / "reports" / session / "handled").is_file(), "Dismiss writes handled")
        again = (await ed.call("eval_lua", allow_edit_mode=True, code="return #Crash.getPendingReports()"))["values"]
        ed.check_eq(again, [0], "nothing pending once handled")

    async with Editor(create=True, name="Crash Notice CI 3", env=env) as ed:
        reports = await ed.call("get_crash_reports")
        ed.check(not reports["notice_open"], f"no notice after the clean exit and the dismissed report: {reports}")
        ed.check(not [r for r in reports["reports"] if not r["handled"]], "no unhandled report")
