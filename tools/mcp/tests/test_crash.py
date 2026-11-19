"""S28 crash and hang diagnostics (docs/crash-diagnostics.md): the diagnostics group's tools.

get_editor_health answers off the main thread while debug_crash hangs it; the hang becomes a
report of this session; a runaway Lua function in Play is interrupted and Play goes on; a
session killed without a clean exit is reported at the next start (the notice, Lua
Crash.setReportHook, dismiss_crash_notice). Editors that need flags or a shared report root
start their own.
"""

import asyncio
import os
import shutil
import signal
import tempfile
import time
from pathlib import Path

import harness
import pngdiff
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
        # The hang starts on the main thread's next turn and shows once the main thread has been
        # quiet past the threshold; on a loaded machine either can take longer than a fixed sleep
        # allows for. So ask until it shows. An answer that says "not responsive, in the hang's
        # phase" was given while the main thread hung: a main-thread answer comes after the hang.
        latencies = []
        deadline = time.monotonic() + 60
        while True:
            start = time.monotonic()
            health = await ed.call("get_editor_health", timeout=10)
            latencies.append(time.monotonic() - start)
            in_hang = not health["responsive"] and health["phase"] == "debug_crash:hang"
            if in_hang or time.monotonic() > deadline:
                break
            await asyncio.sleep(0.1)
        ed.check(not health["responsive"], f"responsive is false during the hang: {health}")
        ed.check(health["stalled_ms"] >= 1000, f"stalled_ms: {health}")
        ed.check_eq(health["phase"], "debug_crash:hang", "the hang's phase")
        # how fast the off-main answers come is a budget (the benchmark run enforces it)
        slow = harness.over_budget("get_editor_health while the main thread hangs", {"slowest answer": max(latencies)}, 0.5)
        ed.check(not slow, f"get_editor_health answered in {max(latencies):.2f} s while the main thread hangs")

        # after the hang a normal (main-thread) call answers again, and the hang was reported
        reports = await ed.call("get_crash_reports", include_text=True, timeout=60)
        mine = [r for r in reports["reports"] if r["this_session"]]
        ed.check(mine and mine[0]["kind"] == "hang", f"this session's hang is listed: {reports}")
        # a loaded machine's main thread also misses the 1 s threshold on its own, before or after the
        # debug hang (each a hang-NNN.txt of its own; the text is the newest's): the debug hang's file
        # is the one that names its phase
        hangs = sorted(f for f in mine[0]["files"] if f.startswith("hang-") and f.endswith(".txt"))
        ed.check(hangs and hangs[0] == "hang-001.txt", f"hang-001.txt: {mine[0]['files']}")
        named = [f for f in hangs if "phase: debug_crash:hang" in (Path(mine[0]["folder"]) / f).read_text(errors="replace")]
        ed.check(named, f"a report names the phase: {hangs}, {mine[0].get('text', '')[:600]}")
        # (a loaded machine's slow frame can pass the 1 s threshold again: responsive is a state)
        deadline = time.monotonic() + 30
        health = await ed.call("get_editor_health")
        while not health["responsive"] and time.monotonic() < deadline:
            await asyncio.sleep(0.2)
            health = await ed.call("get_editor_health")
        ed.check(health["responsive"], f"responsive again: {health}")
        ed.check(health["hang_reports"] >= 1, f"hang_reports: {health}")
        ed.check("JASPERENGINE HANG:" in ed.stderr_text() and "phase: debug_crash:hang" in ed.stderr_text(), "the stderr line")


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
        if not found:
            health = await ed.call("get_editor_health")
            frames = (await ed.call("eval_lua", code="return runawayFrames"))["values"]
            recent = [e.get("message", "")[:300] for e in await ed.console_since(None, limit=40)]
            ed.fail(f"no script interrupted error; health {health}, runawayFrames {frames}, console {recent}, "
                    f"stderr {ed.stderr_text()[-3000:]}")
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

    # the dismissed report is not offered again. (The harness kills an editor that takes longer
    # than 2 s to exit; such a session is rightly reported as abnormal, so only this one is checked.)
    async with Editor(create=True, name="Crash Notice CI 3", env=env) as ed:
        reports = await ed.call("get_crash_reports")
        notice = reports.get("notice") or {}
        ed.check(notice.get("id") != session, f"the dismissed report is not noticed again: {reports}")
        killed = [r for r in reports["reports"] if r["id"] == session]
        ed.check(killed and killed[0]["handled"], f"it stays handled: {reports['reports']}")


async def test_notice_after_crash():
    with tempfile.TemporaryDirectory(prefix="jasper-segv-root-") as root_text:
        root = Path(root_text)
        env = {"JASPERENGINE_CRASH_REPORT_DIR": str(root)}
        first = Editor(create=True, name="Native Crash CI", env=env, extra_args=["--allow-debug-crash"])
        await first.__aenter__()
        try:
            session = (await first.call("get_crash_reports"))["session"]
            await first.call("debug_crash", mode="segv")
            report = root / "reports" / session / "crash.txt"
            for _ in range(100):
                if report.exists() and "backtrace:" in report.read_text():
                    break
                await asyncio.sleep(0.1)
            first.check(report.is_file(), "SIGSEGV wrote the native crash report")
            first.check("reason: SIGSEGV" in report.read_text(), "the report records the original signal")
        finally:
            await first.close()
        async with Editor(create=True, name="Native Crash Restart CI", env=env) as ed:
            reports = await ed.call("get_crash_reports", include_text=True)
            ed.check(reports["notice_open"], "a native crash opens the startup notice")
            crashed = [r for r in reports["reports"] if r["id"] == session]
            ed.check(crashed and crashed[0]["kind"] == "crash", f"native report listed: {reports}")
            ed.check(not crashed[0]["handled"], "report starts unhandled")
            await ed.call("dismiss_crash_notice", id=session)
            ed.check((report.parent / "handled").is_file(), "dismissal saved")
        # (as test_notice_after_abnormal_exit: a slow exit the harness kills is its own abnormal session)
        async with Editor(create=True, name="Native Crash Dismissed CI", env=env) as ed:
            reports = await ed.call("get_crash_reports")
            notice = reports.get("notice") or {}
            ed.check(notice.get("id") != session, f"dismissed crash stays dismissed after restart: {reports}")
            crashed = [r for r in reports["reports"] if r["id"] == session]
            ed.check(crashed and crashed[0]["handled"], f"it stays handled: {reports['reports']}")


# -- the Gameplay Journal and user reports (docs/specs/shipped-game-diagnostics.md) ----------------


def jpeg_size(data: bytes):
    """A baseline or progressive JPEG's (width, height), from its SOF marker."""
    at = 2
    while at + 9 < len(data):
        if data[at] != 0xFF:
            return 0, 0
        marker = data[at + 1]
        length = int.from_bytes(data[at + 2:at + 4], "big")
        if marker in (0xC0, 0xC1, 0xC2):
            return int.from_bytes(data[at + 7:at + 9], "big"), int.from_bytes(data[at + 5:at + 7], "big")
        at += 2 + length
    return 0, 0


async def journal_scene(ed: Editor, name: str):
    """A 2D scene with a red background, saved: Play shows a red frame."""
    made = await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}")
    scene = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": [1.0, 0.0, 0.0, 1.0]})
    await ed.call("save_scene", scene=scene)
    return scene


async def journal_entries(ed: Editor, since: int):
    return [e["message"] for e in (await ed.call("read_console", since=since, categories=["journal"], limit=200))["entries"]]


async def test_journal_engine_events(ed: Editor):
    scene = await journal_scene(ed, "JournalEvents")
    since = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=3)
    await ed.call("play", action="pause")
    await ed.call("play", action="resume")
    await ed.call("wait", frames=2)
    await ed.call("play", action="stop")
    messages = await journal_entries(ed, since)
    ed.check(any(m.startswith("engine.play state=start") for m in messages), f"engine.play start: {messages}")
    ed.check(any(m.startswith("engine.play state=pause") for m in messages), f"engine.play pause: {messages}")
    ed.check(any(m.startswith("engine.scene_loaded ") and "name=JournalEvents" in m for m in messages),
             f"engine.scene_loaded: {messages}")
    ed.check(all(not m.startswith("+") for m in messages), f"the Console shows the entry without its time: {messages}")

    await ed.call("set_project_settings", values={"player.crash.journal_engine_events": False})
    since = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=3)
    await ed.call("play", action="stop")
    messages = await journal_entries(ed, since)
    ed.check_eq([m for m in messages if m.startswith("engine.")], [], "no engine entries with Journal Engine Events off")
    await ed.call("set_project_settings", values={"player.crash.journal_engine_events": True})


async def test_journal_in_console(ed: Editor):
    scene = await journal_scene(ed, "JournalConsole")
    await ed.call("play", action="start", scene=scene)
    since = (await ed.call("read_console", limit=1))["next_cursor"]
    values = (await ed.call("eval_lua", code=(
        'Journal.log("test.kind", {a = 1}) '
        'local e = Journal.getEntries(1)[1] '
        'return e.kind, e.frame >= 0, Journal.enabled, Journal.getText():sub(1, 23)')))["values"]
    await ed.call("play", action="stop")
    ed.check_eq(values, ["test.kind", True, True, "JasperEngine journal v1"], "Journal.getEntries and getText")
    messages = await journal_entries(ed, since)
    ed.check("test.kind a=1" in messages, f"read_console categories journal: {messages}")


async def test_user_report_screenshot(ed: Editor):
    scene = await journal_scene(ed, "UserReport")
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=5)
    await ed.call("eval_lua", code="_G.shot = nil; Crash.beginReport(function(ok, err) _G.shot = ok; _G.shotError = err end)")
    waited = await ed.call_raw("wait", {"until": "_G.shot ~= nil", "timeout_seconds": 60})
    ed.check(not waited.is_error, f"the capture's callback ran: {waited.text[:500]}")
    values = (await ed.call("eval_lua", code=(
        'local r, err = Crash.createReport{note = "x", category = "gameplay", fields = {level = "forest-2"}} '
        'return _G.shot, tostring(_G.shotError), r and r.id or err, r and r.kind, r and r.category')))["values"]
    await ed.call("play", action="stop")
    ed.check_eq(values[0], True, f"a screenshot is in the draft: {values}")
    report_id = values[2]
    ed.check_eq(values[3:], ["user", "gameplay"], f"the report table: {values}")

    listed = await ed.call("get_crash_reports", kinds=["user"], files=["user.txt"])
    users = [r for r in listed["reports"] if r["id"] == report_id]
    ed.check(users and users[0]["kind"] == "user", f"get_crash_reports kinds user lists it: {listed}")
    ed.check(all(r["kind"] == "user" for r in listed["reports"]), "only user reports")
    folder = Path(users[0]["folder"])
    jpeg = (folder / "screenshot.jpg").read_bytes()
    ed.check(jpeg[:2] == b"\xff\xd8", "screenshot.jpg is a JPEG")
    ed.check(len(jpeg) <= 600 * 1024, f"at most 600 KiB: {len(jpeg)}")
    width, height = jpeg_size(jpeg)
    ed.check(0 < width <= 1280, f"at most 1280 px wide: {width}x{height}")
    # the centre against a red image of that size (diff_images decodes the JPEG)
    project = ed.project
    shutil.copy(folder / "screenshot.jpg", project / "user-report-shot.jpg")
    (project / "user-report-red.png").write_bytes(pngdiff.encode(width, height, bytes([255, 0, 0, 255]) * (width * height)))
    diff = await ed.call("diff_images", a="user-report-shot.jpg", b="user-report-red.png", tolerance=24, max_fraction=0.0,
                         max_diff=255, max_mean=255, crop=[width // 2 - 8, height // 2 - 8, 16, 16], image="none")
    ed.check(diff["ok"], f"the centre is the red frame: {diff}")
    ed.check((folder / "journal.txt").is_file() and (folder / "device.json").is_file(), "journal.txt and device.json")
    await ed.call("eval_lua", allow_edit_mode=True, code=f'return Crash.markHandled("{report_id}")')


async def test_read_report_file(ed: Editor):
    scene = await journal_scene(ed, "ReadReportFile")
    await ed.call("play", action="start", scene=scene)
    values = (await ed.call("eval_lua", code=(
        'local r, err = Crash.createReport{note = "read me"} '
        'if not r then return err end '
        'local bytes = Crash.readReportFile(r.id, "user.txt") '
        'local missing, why = Crash.readReportFile(r.id, "nope.txt") '
        'local escape = Crash.readReportFile(r.id, "../" .. r.id .. "/user.txt") '
        'Crash.markHandled(r.id) '
        'return r.id, bytes, missing == nil, why, escape == nil')))["values"]
    await ed.call("play", action="stop")
    ed.check(len(values) == 5, f"a report was created: {values}")
    report_id, text = values[0], values[1]
    listed = await ed.call("get_crash_reports")
    folder = Path([r for r in listed["reports"] if r["id"] == report_id][0]["folder"])
    ed.check_eq(text, (folder / "user.txt").read_text(), "readReportFile returns the file's bytes")
    ed.check_eq(values[2:], [True, "not_found", True], "an unknown or escaping name: nil, not_found")
