"""Debug overlay (spec docs/superpowers/specs/2026-10-31-debug-overlay-design.md) end to end in editor
Play: Project Settings > Debug over MCP (validated, undoable, saved in project.yaml), the console
opened from Lua and seen in the Game view's screenshot, get_play_state's debug_overlay block, the
backquote key through send_input, DebugOverlay.getStats(), Log.getEntries with traces, script
commands, and Log.onMessageReceived in a script.
"""

import yaml

from harness import Editor
from htmlui_pixels import count_near, image_difference

EDITOR = {"create": True, "name": "Debug Overlay"}
TIMEOUT = 900

PROBE = """---@class LogProbe : ScriptInstance
local LogProbe = { properties = {} }

function LogProbe:init()
    _G.logProbe = {count = 0, last = nil}
    RegisterEvent(self, Log.onMessageReceived, "onLog")
    DebugOverlay.addCommand("probe", "probe <word>", function(args) _G.logProbe.command = args[1] end)
end

function LogProbe:onLog(message, stackTrace, level)
    local p = _G.logProbe
    p.count = p.count + 1
    p.last = {message = message, stackTrace = stackTrace, level = level}
end

return LogProbe
"""

# the console's panel colour (#0b0d12ee over the game)
CONSOLE = (12, 14, 19)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def start(ed: Editor) -> None:
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    await ed.call("wait", frames=3)


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_settings(ed: Editor):
    await ed.call("create_entity", name="LogProbe", type="empty")
    await ed.call("create_script", kind="lua", name="LogProbe", entity="LogProbe", content=PROBE)
    await ed.call("save_project", untitled_scene_dir="scenes")

    settings = (await ed.call("get_project_settings", category="debug"))["settings"]
    ed.check_eq(settings["debug.overlay"], "development", "default mode")
    for key in ("debug.overlay_toggle_key", "debug.overlay_stats_key", "debug.overlay_gesture", "debug.overlay_popup",
                "debug.overlay_open_on_error", "debug.overlay_stats", "debug.overlay_stats_corner", "debug.overlay_allow_lua",
                "debug.log_buffer_size", "debug.log_stack_traces"):
        ed.check_in(key, settings, "debug key listed")
    project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
    ed.check("debug" not in project_yaml, f"no debug node for the defaults: {project_yaml.get('debug')}")

    # validated: one bad value changes nothing
    await ed.check_error("set_project_settings", values={"debug.overlay": "sometimes", "debug.log_buffer_size": 50},
                         contains="must be one of: never development always")
    await ed.check_error("set_project_settings", values={"debug.overlay_toggle_key": "notakey"}, contains="not a key name")
    await ed.check_error("set_project_settings", values={"debug.log_buffer_size": -1}, contains="between 0 and")

    await ed.call("set_project_settings", values={"debug.overlay": "always", "debug.log_buffer_size": 200,
                                                  "debug.overlay_allow_lua": "always"})
    project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
    ed.check_eq(project_yaml.get("debug"), {"overlay": "always", "logBufferSize": 200, "allowLua": "always"}, "saved")

    # undoable, one step per key
    await ed.call("undo", scope="project")
    after = (await ed.call("get_project_settings", keys=["debug.overlay_allow_lua", "debug.overlay"]))["settings"]
    ed.check_eq(after["debug.overlay_allow_lua"], "development", f"undone: {after}")
    ed.check_eq(after["debug.overlay"], "always", f"the others stay: {after}")
    await ed.call("redo", scope="project")


async def test_console_in_play(ed: Editor):
    await start(ed)
    try:
        state = await ed.call("get_play_state")
        ed.check_eq(state.get("debug_overlay"), {"enabled": True, "console_visible": False, "stats": "off"}, "overlay block")
        before = await ed.screenshot("game")

        await lua(ed, "DebugOverlay.showConsole()")
        await ed.call("wait", frames=3)
        state = await ed.call("get_play_state")
        ed.check(state["debug_overlay"]["console_visible"], f"console visible: {state['debug_overlay']}")
        shown = await ed.screenshot("game")
        ed.check(len(shown) > 1000, "screenshot not empty")
        ed.check(image_difference(before, shown) > 0.05, "the console changes the Game view")
        ed.check(count_near(shown, CONSOLE, tol=20) > 50, "the console's panel colour is in the Game view")

        # the backquote key, as a player presses it
        r = await ed.call_raw("send_input", {"events": [{"type": "key", "key": "backquote"}], "frames_after": 3})
        ed.check(not r.is_error, r.text[:300])
        state = await ed.call("get_play_state")
        ed.check(not state["debug_overlay"]["console_visible"], f"hidden by backquote: {state['debug_overlay']}")
        hidden = await ed.screenshot("game")
        ed.check(image_difference(before, hidden) < image_difference(before, shown), "hidden again")

        # F3: the stats panel
        await ed.call_raw("send_input", {"events": [{"type": "key", "key": "f3"}], "frames_after": 3})
        state = await ed.call("get_play_state")
        ed.check_eq(state["debug_overlay"]["stats"], "fps", "F3 shows the FPS panel")
    finally:
        await stop(ed)


async def test_stats_and_log_from_lua(ed: Editor):
    await start(ed)
    try:
        await ed.call("wait", frames=30)
        ok = await lua(ed, "local s = DebugOverlay.getStats() return s.fps > 0, s.frameMsAvg > 0, s.drawCalls >= 0, s.scenes >= 1, s.entities >= 1")
        ed.check_eq(ok, [True, True, True, True, True], "getStats")

        await lua(ed, "Log.error('probe failure')")
        got = await lua(ed, "local e = Log.getEntries(1)[1] return e.message, e.level == LogLevel.ERROR, e.stackTrace ~= ''")
        ed.check_eq(got, ["probe failure", True, True], "Log.getEntries with a traceback")
        errors = await lua(ed, "return #Log.getEntries(20, {LogLevel.ERROR}), Log.bufferSize")
        ed.check(errors[0] >= 1, f"errors: {errors}")
        ed.check_eq(errors[1], 200, "the project's buffer size")

        # the script's listener got it (main thread, same argument order as Unity)
        last = await lua(ed, "local p = _G.logProbe return p.count > 0, p.last.message, p.last.level")
        ed.check_eq(last, [True, "probe failure", 4], "Log.onMessageReceived in a script")

        # the script's console command, and lua commands (allowed in the editor's Play)
        await lua(ed, "DebugOverlay.executeCommand('probe hello')")
        ed.check_eq(await lua(ed, "return _G.logProbe.command"), ["hello"], "script command")
        await lua(ed, "DebugOverlay.executeCommand('lua 6*7')")
        ed.check_eq(await lua(ed, "return Log.getEntries(1)[1].message"), ["42"], "lua command")

        # every line reaches the editor's Console too
        console = await ed.call("read_console", max_entries=50)
        ed.check("probe failure" in str(console), "the Console has the line")
    finally:
        await stop(ed)
    # the script's command went with Play
    state = await ed.call("get_play_state")
    ed.check("debug_overlay" not in state, "no overlay block when stopped")


async def test_never_mode(ed: Editor):
    await ed.call("set_project_settings", values={"debug.overlay": "never"})
    await start(ed)
    try:
        ed.check_eq(await lua(ed, "return DebugOverlay.enabled, Engine.isDevelopmentBuild"), [False, True], "never, in a development build")
        await lua(ed, "DebugOverlay.showConsole()")
        ed.check_eq(await lua(ed, "return DebugOverlay.consoleVisible"), [False], "a no-op while off")
        # a remote switch turns it on
        await lua(ed, "DebugOverlay.enabled = true DebugOverlay.showConsole()")
        await ed.call("wait", frames=2)
        ed.check((await ed.call("get_play_state"))["debug_overlay"]["console_visible"], "turned on from Lua")
    finally:
        await stop(ed)
        await ed.call("set_project_settings", values={"debug.overlay": "development"})
