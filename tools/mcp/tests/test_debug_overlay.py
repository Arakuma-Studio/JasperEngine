"""Debug overlay (spec docs/superpowers/specs/2026-10-31-debug-overlay-design.md) end to end in editor
Play: Project Settings > Debug over MCP (validated, undoable, saved in project.yaml), the console
opened from Lua and seen in the Game view's screenshot, get_play_state's debug_overlay block, the
backquote key through send_input, DebugOverlay.getStats(), Log.getEntries with traces, script
commands, and Log.onMessageReceived in a script.
"""

import asyncio
import math
import os
import struct
import time

import yaml

from harness import Editor, skip
from htmlui_pixels import count_near, image_difference

EDITOR = {"create": True, "name": "Debug Overlay"}
CI_EXCLUDED = ("long-running (about 75 minutes on a loaded server: test_release_export builds and runs a real Desktop player); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

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


def write_tone(path, seconds=2.0, frequency=440.0, amplitude=0.3, rate=44100):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = int(seconds * rate)
    data = b"".join(struct.pack("<h", int(round(amplitude * 32767 * math.sin(2 * math.pi * frequency * i / rate))))
                    for i in range(samples))
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    path.write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


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

    # undoable: one step per key, folded into one for the call (an MCP call is one undo step,
    # McpUndo::foldCall); DebugReflectTest.EachRowIsOneUndoStep checks the steps per key
    await ed.call("undo", scope="project")
    keys = ["debug.overlay_allow_lua", "debug.overlay", "debug.log_buffer_size"]
    after = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq({k: after.get(k) for k in keys}, {"debug.overlay_allow_lua": "development", "debug.overlay": "development", "debug.log_buffer_size": 500},
                "the call undone as one step")
    await ed.call("redo", scope="project")
    after = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq({k: after.get(k) for k in keys}, {"debug.overlay_allow_lua": "always", "debug.overlay": "always", "debug.log_buffer_size": 200}, "redone")


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
        shown = await ed.screenshot("game", path=(ed.scratch.root / "console.png") if ed.scratch else None)
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
        console = await ed.call("read_console", contains="probe failure")
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


async def test_checked_values_in_play(ed: Editor):
    """A stats mode that is none is refused (get_play_state keeps working), the buffer size is capped,
    the stats' audio voices are get_play_state's with the audio mixer's group buses playing (SoLoud counts
    those as voices). (Project Settings changed during Play apply at once: the dialog's; MCP's
    set_project_settings waits for Play to stop. DebugSettingsTest covers it.)"""
    write_tone(ed.project / "tone.wav")
    r = await ed.call_raw("audio_mixer", {"action": "edit", "ops": [{"op": "add_group", "name": "Music"},
                                                                    {"op": "add_group", "name": "SFX"}]})
    ed.check(not r.is_error, f"mixer: {r.text[:300]}")
    await ed.call("create_entity", name="Tone", type="audio_source")
    await ed.call("set_properties", entity="Tone", values={"audiosource": {"filename": "tone.wav", "looping": True,
                                                                           "state": "playing", "outputGroup": "Music"}})
    await ed.call("save_project", untitled_scene_dir="scenes")
    await start(ed)
    try:
        got = await lua(ed, "local ok, err = pcall(function() DebugOverlay.statsMode = 3 end) return ok, tostring(err), DebugOverlay.statsMode")
        ed.check(got[0] is False and "STATS_OFF" in got[1], f"statsMode = 3 refused: {got}")
        ed.check_eq(got[2], 0, "unchanged")
        ed.check_eq((await ed.call("get_play_state"))["debug_overlay"]["stats"], "off", "get_play_state")
        ed.check_eq(await lua(ed, "Log.bufferSize = 5000000 return Log.bufferSize"), [100000], "the buffer size is capped")

        # the stats' voices are the game's (the tone; not the audio mixer's buses), as get_play_state's
        await ed.call("wait", frames=10)
        voices = await lua(ed, "return DebugOverlay.getStats().audioVoices")
        state = await ed.call("get_play_state")
        ed.check_in("Music", state["audio"]["mixer_meters"], f"the mixer runs: {state['audio']}")
        ed.check_eq(state["audio"]["voices"], 1, f"the tone plays: {state['audio']}")
        ed.check_eq(voices[0], state["audio"]["voices"], f"audio voices: {voices} vs {state['audio']}")
    finally:
        await stop(ed)
        await ed.call("delete_entities", entities=["Tone"])
        await ed.call("save_project")


EXPORT_PROBE = """local OverlayExport = { properties = {} }

function OverlayExport:init()
    self.frames = 0
    print("overlay-export enabled=" .. tostring(DebugOverlay.enabled) .. " development=" .. tostring(Engine.isDevelopmentBuild)
          .. " buffer=" .. Log.bufferSize)
    RegisterEngineEvent(self, "onUpdate")
end

function OverlayExport:onUpdate()
    self.frames = self.frames + 1
    if self.frames == 5 then
        DebugOverlay.showConsole()
        Log.warn("overlay-export warning")
    elseif self.frames == 30 then
        local s = DebugOverlay.getStats()
        local w = Log.getEntries(1, {LogLevel.WARN})[1]
        print("overlay-export visible=" .. tostring(DebugOverlay.consoleVisible) .. " fps=" .. tostring(s.fps > 0)
              .. " trace=" .. tostring(w ~= nil and w.stackTrace ~= ""))
    end
end

return OverlayExport
"""


async def test_release_export(ed: Editor):
    """A Release desktop export: not a development build; the overlay of mode "always" works there, the
    project's log buffer size applies. Builds the engine from source: JASPER_SLOW_BUILD=1."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    await ed.call("create_entity", name="OverlayExport", type="empty")
    await ed.call("create_script", kind="lua", name="OverlayExport", entity="OverlayExport", content=EXPORT_PROBE)
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
    await ed.call("set_project_settings", values={
        "debug.overlay": "always", "debug.log_buffer_size": 321,
        "graphics.runtime_shaders": False, "preferences.build_jobs": 3,
        "player.window_width": 640, "player.window_height": 480,
    })
    out = ed.scratch.root / "export-desktop"
    t0 = time.monotonic()
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=3, timeout_seconds=30)
    first = job
    while job["status"] == "running":
        await asyncio.sleep(15)
        job = await ed.call("get_job", job_id=first["job_id"], log_lines=10)
        if time.monotonic() - t0 > TIMEOUT:
            ed.fail(f"build still running: {job}")
    ed.check_eq(job["status"], "succeeded", f"{job}")
    raw = await ed.call_raw("run_build", {"seconds": 8, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    log = raw.value["log"]
    lines = [line for line in (log if isinstance(log, list) else str(log).splitlines()) if "overlay-export" in line]
    print("[test] exported player:", lines)
    ed.check(any("overlay-export enabled=true development=false buffer=321" in line for line in lines), f"init: {lines}")
    ed.check(any("overlay-export visible=true fps=true trace=true" in line for line in lines), f"console: {lines}")
