"""Platform services (docs/superpowers/specs/2026-10-31-platform-services-design.md): the app
lifecycle an agent simulates with send_input {type: "app"} (focus, pause, low memory) and its
order in Lua events and script messages; System.openURL recorded and not launched in a headless
run; Engine.targetFrameRate pacing editor Play; the Game view's simulated language and device
(model, dpi, rotation by Screen.orientation, onOrientationChanged, a landscape screenshot); the
Player settings round trip, their rename hints and what Play start applies from them.
"""

from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Platform Services"}
CI = True

RECORDER = """---@class Recorder : ScriptInstance
local Recorder = { properties = {} }
function Recorder:init()
    platformLog = {}
    RegisterEngineEvent(self, "onFocus")
    RegisterEngineEvent(self, "onPause")
    RegisterEngineEvent(self, "onResume")
    RegisterEngineEvent(self, "onLowMemory")
    RegisterEngineEvent(self, "onOrientationChanged")
end
function Recorder:onFocus(hasFocus) table.insert(platformLog, "onFocus " .. tostring(hasFocus)) end
function Recorder:onPause() table.insert(platformLog, "onPause") end
function Recorder:onResume() table.insert(platformLog, "onResume") end
function Recorder:onLowMemory() table.insert(platformLog, "onLowMemory") end
function Recorder:onOrientationChanged(o) table.insert(platformLog, "onOrientationChanged " .. tostring(o)) end
function Recorder:onApplicationFocus(focused) table.insert(platformLog, "onApplicationFocus " .. tostring(focused)) end
function Recorder:onApplicationPause(paused) table.insert(platformLog, "onApplicationPause " .. tostring(paused)) end
return Recorder
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="PlatformMain", type="2D", path="scenes/PlatformMain")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Recorder")
    await ed.call("create_script", kind="lua", name="Recorder", content=RECORDER, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Probe")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def log_since_start(ed: Editor) -> list:
    return (await lua(ed, "return platformLog"))[0] or []


async def test_app_events_keep_the_documented_order(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        state = (await ed.call("get_play_state"))["platform"]
        ed.check(state["focused"] and not state["paused"], f"Play starts focused and running: {state}")
        await lua(ed, "platformLog = {}")
        await ed.call("send_input", events=[
            {"type": "app", "action": "focus_lost"}, {"type": "app", "action": "pause"},
            {"type": "app", "action": "pause"},   # a duplicate from the platform: nothing
            {"type": "wait", "ms": 200},
            {"type": "app", "action": "resume"}, {"type": "app", "action": "focus_gained"}])
        ed.check_eq(await log_since_start(ed), [
            "onApplicationFocus false", "onFocus false", "onApplicationPause true", "onPause",
            "onApplicationPause false", "onResume", "onApplicationFocus true", "onFocus true"])

        # pausing a focused app loses the focus first; gaining it while paused resumes first
        await lua(ed, "platformLog = {}")
        await ed.call("send_input", events=[{"type": "app", "action": "pause"}])
        state = (await ed.call("get_play_state"))["platform"]
        ed.check(state["paused"] and not state["focused"], f"paused and unfocused: {state}")
        await ed.call("send_input", events=[{"type": "app", "action": "focus_gained"}, {"type": "app", "action": "low_memory"}])
        ed.check_eq(await log_since_start(ed), [
            "onApplicationFocus false", "onFocus false", "onApplicationPause true", "onPause",
            "onApplicationPause false", "onResume", "onApplicationFocus true", "onFocus true", "onLowMemory"])
        ed.check_eq(await lua(ed, "return Engine.isFocused"), [True])

        bad = await ed.call_raw("send_input", {"events": [{"type": "app", "action": "sleep"}]})
        ed.check(bad.is_error and "pause, resume" in bad.text, bad.text)
    finally:
        await ed.call("play", action="stop")


async def test_a_simulated_pause_ends_with_the_play(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("send_input", events=[{"type": "app", "action": "pause"}])
        ed.check((await ed.call("get_play_state"))["platform"]["paused"], "paused")
    finally:
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        state = (await ed.call("get_play_state"))["platform"]
        ed.check(state["focused"] and not state["paused"], f"the next Play starts focused and running: {state}")
    finally:
        await ed.call("play", action="stop")


async def test_open_url_is_recorded_not_launched(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        cursor = (await ed.call("read_console", limit=1))["next_cursor"]
        ok = await lua(ed, "return System.openURL('https://play.google.com/store/apps/details?id=com.example.game'), "
                           "System.openURL('javascript:alert(1)'), System.openURL('example.com')")
        ed.check_eq(ok, [True, False, False])
        urls = (await ed.call("get_play_state"))["platform"]["opened_urls"]
        ed.check_eq([u["url"] for u in urls], ["https://play.google.com/store/apps/details?id=com.example.game"])
        ed.check_eq(urls[0]["launched"], False, "a headless run opens no browser")
        console = (await ed.call("read_console", since=cursor))["entries"]
        text = " ".join(e["message"] for e in console)
        ed.check_in("Open URL: https://play.google.com", text)
        ed.check_in("javascript: URLs are not opened", text)
        ed.check_in("no scheme", text)
    finally:
        await ed.call("play", action="stop")


async def test_target_frame_rate_paces_play(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        async def measure(target: int) -> float:
            values = await lua(ed, f"Engine.vSyncCount = 0; Engine.targetFrameRate = {target}; "
                                   "return Engine.targetFrameRate, Engine.vSyncCount")
            ed.check_eq(values, [target, 0])
            await ed.call("wait", seconds=0.5)
            before = await lua(ed, "return Engine.frameCount, Engine.systemTime")
            await ed.call("wait", seconds=2)
            after = await lua(ed, "return Engine.frameCount, Engine.systemTime")
            return (after[0] - before[0]) / (after[1] - before[1])

        # capped: never faster than the target. A shared, saturated test box was measured running
        # the 15 fps target at 9.6-12.9 (alone it passes 13..17), so the lower bound only checks that
        # frames keep coming; the limiter's timing is unit-tested (FrameLimiter)
        fps = await measure(15)
        ed.check(5 <= fps <= 17, f"15 fps target, measured {fps:.1f}")
        fps = await measure(30)
        ed.check(fps <= 35, f"30 fps target, measured {fps:.1f}")
        state = (await ed.call("get_play_state"))["platform"]
        ed.check_eq((state["target_frame_rate"], state["vsync_count"]), (30, 0))
        # Unity's rules: 0 is the platform default, past 1000 is 1000
        ed.check_eq(await lua(ed, "Engine.targetFrameRate = 0; local a = Engine.targetFrameRate; "
                                  "Engine.targetFrameRate = 5000; return a, Engine.targetFrameRate"), [-1, 1000])
    finally:
        await ed.call("play", action="stop")
    # the next Play starts from the project's values again
    await ed.call("play", action="start", scene=s["scene"])
    try:
        ed.check_eq(await lua(ed, "return Engine.targetFrameRate, Engine.vSyncCount"), [-1, 1])
    finally:
        await ed.call("play", action="stop")


async def test_simulated_language(ed: Editor):
    s = await setup(ed)
    r = await ed.call("configure_game_view", language="es-MX")
    ed.check_eq(r["language"], "es-MX")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        values = await lua(ed, "return System.getSystemLanguage() == SystemLanguage.Spanish, System.getLanguageCode(), "
                               "System.getRegionCode(), System.getLocale(), System.getPreferredLanguages()")
        ed.check_eq(values, [True, "es", "MX", "es-MX", ["es-MX"]])
        ed.check_eq((await ed.call("get_play_state"))["platform"]["system_language"], "Spanish")
    finally:
        await ed.call("play", action="stop")
    bad = await ed.call_raw("configure_game_view", {"language": "x"})
    ed.check(bad.is_error and "BCP 47" in bad.text, bad.text)
    r = await ed.call("configure_game_view", language="system")
    ed.check_eq(r["language"], "system")


async def test_simulated_device_rotates_for_screen_orientation(ed: Editor):
    s = await setup(ed)
    r = await ed.call("configure_game_view", device="iphone_15_pro", orientation="portrait")
    ed.check_eq((r["device"], r["orientation"]), ("iphone_15_pro", "portrait"))
    ed.check_in("pixel_8", r["devices"])
    bad = await ed.call_raw("configure_game_view", {"orientation": "portrait_upside_down"})
    ed.check(bad.is_error and "cannot show" in bad.text, f"iPhones do not turn upside down: {bad.text}")
    try:
        await ed.call("play", action="start", scene=s["scene"])
        # the project default (auto-rotation with portrait allowed) keeps the portrait device
        values = await lua(ed, "return Screen.orientation, Input.deviceOrientation, System.getDeviceModel(), "
                               "System.getDeviceType() == DeviceType.Handheld, Screen.dpi, System.isEditor(), "
                               "Screen.sleepTimeout")
        # a simulated phone keeps the screen on as the device would (player.keep_screen_on)
        ed.check_eq(values, [1, 1, "iPhone16,1", True, 460, True, -1])
        await lua(ed, "platformLog = {}")

        # AutoRotate.cs: lock landscape, then auto-rotate within landscape
        await lua(ed, "Screen.autorotateToPortrait = false; Screen.autorotateToPortraitUpsideDown = false; "
                      "Screen.orientation = ScreenOrientation.LandscapeLeft")
        await ed.call("wait", frames=5)
        values = await lua(ed, "return Screen.orientation, Input.deviceOrientation, Engine.canvasWidth > Engine.canvasHeight")
        ed.check_eq(values, [3, 3, True])
        ed.check_eq(await log_since_start(ed), ["onOrientationChanged 3"])
        ed.check_eq((await ed.call("get_play_state"))["platform"]["orientation"], "landscape_left")

        shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 400})
        ed.check(not shot.is_error and shot.images, shot.text)
        width, height, _, _ = decode_png(shot.images[0][0])
        ed.check(width > height, f"the Game view shows the device in landscape: {width}x{height}")

        await lua(ed, "Screen.orientation = ScreenOrientation.AutoRotation")
        await ed.call("send_input", events=[{"type": "app", "action": "orientation", "orientation": "landscape_right"},
                                            {"type": "wait", "frames": 5}])
        ed.check_eq(await lua(ed, "return Screen.orientation"), [4])
        ed.check_eq(await log_since_start(ed), ["onOrientationChanged 3", "onOrientationChanged 4"])
        refused = await ed.call_raw("send_input", {"events": [{"type": "app", "action": "orientation", "orientation": "portrait"}]})
        ed.check(refused.is_error and "cannot show portrait" in refused.text, f"portrait is not allowed now: {refused.text}")
    finally:
        await ed.call("play", action="stop")
        # the game's rotation ends with the Play: the toolbar's orientation again
        r = await ed.call("configure_game_view")
        ed.check_eq(r["orientation"], "portrait")
        await ed.call("configure_game_view", device="none")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        values = await lua(ed, "return Input.deviceOrientation, System.getDeviceType() == DeviceType.Desktop")
        ed.check_eq(values, [0, True])
        missing = await ed.call_raw("send_input", {"events": [{"type": "app", "action": "orientation", "orientation": "portrait"}]})
        ed.check(missing.is_error and "configure_game_view" in missing.text, missing.text)
    finally:
        await ed.call("play", action="stop")


async def test_device_and_app_information(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        values = await lua(ed, "return System.getAppName(), System.getAppVersion(), System.getAppBuild(), "
                               "System.getAppIdentifier(), System.getOperatingSystem():sub(1, 6), System.getAndroidApiLevel(), "
                               "System.getProcessorCount() >= 1, System.getSystemMemorySize() > 0, System.vibrate(), "
                               "System.hapticFeedback(HapticFeedback.Success), Screen.sleepTimeout, Screen.refreshRate > 0")
        # Keep Screen On is a phone setting: a desktop (no simulated device) starts at the system's
        ed.check_eq(values, ["Platform Services", "1.0", 1, "com.yourcompany.project", "Linux ", 0, True, True, False,
                             False, -2, True])
        state = (await ed.call("get_play_state"))["platform"]
        ed.check_eq((state["vibrations"], state["haptics"]), (1, 1))
        error = await ed.call_raw("eval_lua", {"code": "return System.vibrate('long')"})
        ed.check(error.is_error, "a wrong argument type raises")
    finally:
        await ed.call("play", action="stop")


async def test_player_settings_round_trip_and_play_start(ed: Editor):
    s = await setup(ed)
    values = {"player.default_orientation": "landscape_left",
              "player.allowed_orientations": ["landscape_left", "landscape_right"],
              "player.keep_screen_on": False, "player.target_frame_rate": 45}
    await ed.call("set_project_settings", values=values)
    back = (await ed.call("get_project_settings", keys=list(values)))["settings"]
    ed.check_eq(back, values)
    text = (ed.project / "project.yaml").read_text()
    ed.check_in("presentation:", text)
    ed.check_in("defaultOrientation: landscape_left", text)
    for key, hint in (("android.orientation", "player.default_orientation"), ("android.keep_screen_on", "player.keep_screen_on")):
        msg = await ed.call("set_project_settings", values={key: True}, expect_error=True)
        ed.check_in("renamed", msg)
        ed.check_in(hint, msg)
    bad = await ed.call("set_project_settings", values={"player.allowed_orientations": []}, expect_error=True)
    ed.check_in("non-empty", bad)

    await ed.call("configure_game_view", device="pixel_8", orientation="portrait")
    try:
        await ed.call("play", action="start", scene=s["scene"])
        await ed.call("wait", frames=5)
        values_in_play = await lua(ed, "return Engine.targetFrameRate, Screen.orientation, Screen.sleepTimeout, "
                                       "Screen.autorotateToPortrait, Screen.autorotateToLandscapeRight")
        ed.check_eq(values_in_play, [45, 3, -2, False, True])
        ed.check_eq((await ed.call("get_play_state"))["platform"]["requested_orientation"], "landscape_left")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", device="none")
        await ed.call("set_project_settings", values={
            "player.default_orientation": "auto_rotation",
            "player.allowed_orientations": ["portrait", "landscape_left", "landscape_right"],
            "player.keep_screen_on": True, "player.target_frame_rate": -1})
    ed.check("presentation:" not in (ed.project / "project.yaml").read_text(), "defaults are not written")
