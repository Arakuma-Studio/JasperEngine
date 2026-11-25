"""S22 input as an agent sees it (docs/specs/S22-gameplay.md AC14): a Lua script counts the frame
edges it reads (Input.getKeyDown/getKeyUp in onUpdate, the fixed window in onFixedUpdate), and
send_input drives it; hold_frames 0 presses and releases in one frame, which both edges see
exactly once. get_input_state shows the frame's edges. Mouse, gamepad and touch variants, and
the mouse delta of a captured pointer. The input scheme (docs/specs/input-device-awareness.md
AC15): send_input switches it, configure_game_view forces it, Lua reads it, and a HUD document's
touch-scheme stick follows it.

Scene InputCI (3D) with one GameObject running InputCounter. Play runs with auto_pause and a
fixed 1/60 s step (one fixed step per frame), so frames are exact.
"""

from harness import Editor, tool_meta

CI = True
EDITOR = {"create": True, "name": "Input CI"}

TIMEOUT = 900

COUNTER = """---@class InputCounter : ScriptInstance
local InputCounter = { properties = {} }

local function zero()
    counts = { down = 0, up = 0, held = 0, fixedDown = 0, fixedUp = 0, mouseDown = 0, mouseUp = 0,
               padDown = 0, padUp = 0, touchDown = 0, touchUp = 0, touchEnded = 0, touchStart = 0, any = 0,
               deltaX = 0, deltaY = 0, downFrame = -1, upFrame = -1 }
end

function InputCounter:init()
    zero()
    resetCounts = zero
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onTouchStart")
end

function InputCounter:onTouchStart(pointer)
    if pointer == 0 then counts.touchStart = counts.touchStart + 1 end
end

function InputCounter:onUpdate()
    if Input.getKeyDown(Input.KEY_SPACE) then counts.down = counts.down + 1; counts.downFrame = Engine.frameCount end
    if Input.getKeyUp(Input.KEY_SPACE) then counts.up = counts.up + 1; counts.upFrame = Engine.frameCount end
    if Input.getKey(Input.KEY_SPACE) then counts.held = counts.held + 1 end
    if Input.getMouseButtonDown(Input.MOUSE_BUTTON_LEFT) then counts.mouseDown = counts.mouseDown + 1 end
    if Input.getMouseButtonUp(Input.MOUSE_BUTTON_LEFT) then counts.mouseUp = counts.mouseUp + 1 end
    if Input.getGamepadButtonDown(0, Input.GAMEPAD_BUTTON_A) then counts.padDown = counts.padDown + 1 end
    if Input.getGamepadButtonUp(0, Input.GAMEPAD_BUTTON_A) then counts.padUp = counts.padUp + 1 end
    if Input.getTouchDown(3) then counts.touchDown = counts.touchDown + 1 end
    if Input.getTouchUp(3) then counts.touchUp = counts.touchUp + 1 end
    for i = 0, Input.getTouchCount() - 1 do
        local touch = Input.getTouch(i)
        if touch.pointer == 3 and touch.phase == Input.TOUCH_ENDED and touch.began then
            counts.touchEnded = counts.touchEnded + 1
        end
    end
    if Input.anyKeyDown() then counts.any = counts.any + 1 end
    local delta = Input.getMouseDelta()
    counts.deltaX = counts.deltaX + delta.x
    counts.deltaY = counts.deltaY + delta.y
end

function InputCounter:onFixedUpdate()
    if Input.getKeyDown(Input.KEY_SPACE) then counts.fixedDown = counts.fixedDown + 1 end
    if Input.getKeyUp(Input.KEY_SPACE) then counts.fixedUp = counts.fixedUp + 1 end
end

return InputCounter
"""

# the HUD: an on-screen stick shown only while the player uses touch
HUD = """<!DOCTYPE html>
<html>
<head><style>
    html, body { margin: 0; width: 100%; height: 100%; }
    #stick { position: absolute; left: 0; bottom: 0; width: 200px; height: 200px; }
</style></head>
<body><onscreen-stick id="stick" visibility="touch-scheme"></onscreen-stick></body>
</html>
"""

# a marker in the console on every scheme change
SCHEME_LOG = """---@class SchemeLog : ScriptInstance
local SchemeLog = { properties = {} }

function SchemeLog:init()
    InputActions.onSchemeChanged = function(scheme, previous)
        print("[scheme] " .. previous .. " -> " .. scheme)
    end
end

return SchemeLog
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="InputCI", type="3D", path="scenes/InputCI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Counter")
    await ed.call("create_script", kind="lua", name="InputCounter", content=COUNTER, scene=S["scene"],
                  entity=ent["entity_id"])
    log = await ed.call("create_entity", scene=S["scene"], type="empty", name="SchemeLog")
    await ed.call("create_script", kind="lua", name="SchemeLog", content=SCHEME_LOG, scene=S["scene"],
                  entity=log["entity_id"])
    await ed.call("write_file", path="assets/ui/hud.html", content=HUD)
    hud = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Hud")
    S["hud"] = hud["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["hud"], values={"uidocument": {"filename": "assets/ui/hud.html"}})
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def start(ed: Editor) -> None:
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    await ed.call("wait", frames=2)
    await ed.call("eval_lua", code="resetCounts()")


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def counts(ed: Editor) -> dict:
    return (await ed.call("eval_lua", code="return counts"))["values"][0]


async def send(ed: Editor, *events, frames_after: int = 0) -> dict:
    return await ed.call("send_input", events=list(events), frames_after=frames_after)


async def test_input_tools_group(ed: Editor):
    """get_input_state is the gameplay group's: hidden until a session enables the group."""
    ed.require_tools("tool_groups", "get_input_state")
    ed.check_eq(tool_meta(ed.tool("get_input_state")).get("jasper/group"), "gameplay", "group")
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("get_input_state" not in default, "hidden by default")
    await ed.call("tool_groups", enable=["gameplay"])
    try:
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check("get_input_state" in listed, "listed once gameplay is enabled")
    finally:
        await ed.call("tool_groups", disable=["gameplay"])


async def test_key_tap_in_one_frame(ed: Editor):
    """hold_frames 0: down and up in the same window, seen once by the update and once by a fixed step."""
    await start(ed)
    try:
        await send(ed, {"type": "key", "key": "space", "hold_frames": 0})
        state = await ed.call("get_input_state", fields=["raw"])
        keys = state["raw"]["keys"]
        ed.check("space" in keys["down"] and "space" in keys["up"], f"both edges in the frame: {keys}")
        ed.check("space" not in keys["held"], f"not held after a tap: {keys}")
        ed.check(state["raw"]["any_key_down"], "anyKeyDown")
        c = await counts(ed)
        ed.check_eq((c["down"], c["up"], c["held"]), (1, 1, 0), f"one Down and one Up, never held: {c}")
        ed.check_eq(c["downFrame"], c["upFrame"], "the same frame")
        # the first fixed step at or after that frame sees it (a frame can run no step)
        await ed.call("wait", frames=5)
        c = await counts(ed)
        ed.check_eq((c["down"], c["up"], c["fixedDown"], c["fixedUp"]), (1, 1, 1, 1), f"exactly once: {c}")
        state = await ed.call("get_input_state", fields=["raw"])
        ed.check(not state["raw"]["keys"]["down"], f"the next frames have no edge: {state['raw']['keys']}")
    finally:
        await stop(ed)


async def test_key_held_two_frames(ed: Editor):
    """The default hold: the press and the release in different frames."""
    await start(ed)
    try:
        await send(ed, {"type": "key", "key": "space"})
        await ed.call("wait", frames=2)
        c = await counts(ed)
        ed.check_eq((c["down"], c["up"]), (1, 1), f"{c}")
        ed.check_eq(c["upFrame"] - c["downFrame"], 2, f"held 2 frames: {c}")
        ed.check(c["held"] >= 2, f"held in between: {c}")
    finally:
        await stop(ed)


async def test_mouse_and_gamepad_taps(ed: Editor):
    await start(ed)
    try:
        await send(ed, {"type": "mouse", "action": "click", "x": 100, "y": 100, "hold_frames": 0},
                   {"type": "gamepad", "gamepad": 0, "button": "a", "hold_frames": 0})
        await ed.call("wait", frames=2)
        c = await counts(ed)
        ed.check_eq((c["mouseDown"], c["mouseUp"]), (1, 1), f"mouse: {c}")
        ed.check_eq((c["padDown"], c["padUp"]), (1, 1), f"gamepad: {c}")
        await send(ed, {"type": "gamepad", "gamepad": 0, "button": "b", "hold_frames": 0})
        state = await ed.call("get_input_state", fields=["gamepads"])
        pads = state["gamepads"]
        ed.check(pads and pads[0]["gamepad"] == 0, f"the virtual pad: {pads}")
        ed.check("b" in pads[0]["down"] and "b" in pads[0]["up"], f"the pad's edges: {pads}")
        ed.check("raw" not in state and "touches" not in state, "only the fields asked for")
        await send(ed, {"type": "release_all"})
    finally:
        await stop(ed)


async def test_touch_tap_in_one_frame(ed: Editor):
    await start(ed)
    try:
        await send(ed, {"type": "touch", "action": "tap", "pointer": 3, "x": 200, "y": 150, "hold_frames": 0})
        state = await ed.call("get_input_state", fields=["touches"])
        touches = [t for t in state["touches"] if t["pointer"] == 3]
        ed.check(len(touches) == 1, f"one entry: {state['touches']}")
        ed.check_eq((touches[0]["phase"], touches[0]["began"], touches[0]["down"], touches[0]["up"]),
                    ("ended", True, True, True), f"began and ended in the frame: {touches}")
        await ed.call("wait", frames=2)
        c = await counts(ed)
        ed.check_eq((c["touchDown"], c["touchUp"], c["touchEnded"]), (1, 1, 1), f"{c}")
    finally:
        await stop(ed)


async def test_captured_mouse_delta(ed: Editor):
    """A relative move of 40 canvas pixels with the pointer captured sums to a delta of 40."""
    await start(ed)
    try:
        await ed.call("eval_lua", code="Engine.mouseMode = MouseMode.CAPTURED")
        await send(ed, {"type": "mouse", "action": "move", "x": 300, "y": 200})
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="resetCounts()")
        await send(ed, {"type": "mouse", "action": "move", "dx": 25, "dy": 0},
                   {"type": "mouse", "action": "move", "dx": 15, "dy": -6})
        await ed.call("wait", frames=2)
        c = await counts(ed)
        ed.check(abs(c["deltaX"] - 40) <= 1 and abs(c["deltaY"] + 6) <= 1, f"delta summed to (40, -6): {c}")
        state = await ed.call("get_input_state", fields=["raw"])
        ed.check(state["raw"]["pointer_lock"] in ("locked", "pending"), f"captured: {state['raw']}")
        lock = (await ed.call("eval_lua", code="return Input.getPointerLockState() ~= Input.POINTER_UNLOCKED"))["values"]
        ed.check_eq(lock, [True], "the game sees a requested lock")
        await ed.call("eval_lua", code="Engine.mouseMode = MouseMode.NORMAL")
        state = await ed.call("get_input_state", fields=["raw"])
        ed.check_eq(state["raw"]["pointer_lock"], "unlocked", "NORMAL unlocks")
    finally:
        await stop(ed)


async def test_play_start_resets_input(ed: Editor):
    """A key left down at Stop (send_input action down) is not held in the next Play."""
    await start(ed)
    await send(ed, {"type": "key", "key": "w", "action": "down"})
    await stop(ed)
    await start(ed)
    try:
        held = (await ed.call("eval_lua", code="return Input.getKey(Input.KEY_W)"))["values"]
        ed.check_eq(held, [False], "nothing held from the last Play")
        state = await ed.call("get_input_state", fields=["raw"])
        ed.check(not state["raw"]["keys"]["held"], f"{state['raw']['keys']}")
    finally:
        await stop(ed)


async def test_simulate_touch_from_mouse(ed: Editor):
    """S95 REQ-12 (AC-13): Project Settings > Input > Simulate Touch From Mouse makes a mouse click in Play
    also finger 0 (onTouchStart), as in desktop and web builds; off (the default) it does not."""
    await setup(ed)
    await ed.call("set_project_settings", values={"input.simulate_touch_from_mouse": True})
    try:
        await start(ed)
        try:
            await send(ed, {"type": "mouse", "action": "click", "x": 100, "y": 100, "hold_frames": 0})
            await ed.call("wait", frames=2)
            c = await counts(ed)
            ed.check_eq((c["mouseDown"], c["touchStart"]), (1, 1), f"the click is also finger 0: {c}")
            on = (await ed.call("eval_lua", code="return Engine.callTouchInMouseEvent"))["values"][0]
            ed.check(on is True, "Engine.callTouchInMouseEvent at Play start")
        finally:
            await stop(ed)
        off = (await ed.call("eval_lua", code="return Engine.callTouchInMouseEvent", allow_edit_mode=True))["values"][0]
        ed.check(off is False, "Play stop turns it off for the editor")
    finally:
        await ed.call("set_project_settings", values={"input.simulate_touch_from_mouse": False})

    await start(ed)
    try:
        await send(ed, {"type": "mouse", "action": "click", "x": 100, "y": 100, "hold_frames": 0})
        await ed.call("wait", frames=2)
        c = await counts(ed)
        ed.check_eq((c["mouseDown"], c["touchStart"]), (1, 0), f"off: a mouse click only: {c}")
    finally:
        await stop(ed)


async def scheme(ed: Editor) -> dict:
    return (await ed.call("get_input_state", fields=["raw"]))["scheme"]


async def stick_hidden(ed: Editor) -> bool:
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["hud"], selector="#stick", depth=0)
    return "hidden" in r["elements"][0].get("flags", [])


async def test_scheme_follows_send_input(ed: Editor):
    """Each device kind send_input drives switches the scheme the next frame; the HUD's touch-scheme stick follows."""
    await start(ed)
    try:
        ed.check_eq((await scheme(ed))["current"], "Keyboard&Mouse", "a desktop starts on Keyboard&Mouse")
        ed.check(await stick_hidden(ed), "the touch-scheme stick is hidden off touch")
        await send(ed, {"type": "gamepad", "gamepad": 0, "button": "a", "hold_frames": 0})
        st = await scheme(ed)
        ed.check_eq((st["current"], st["gamepad"]), ("Gamepad", 0), f"a pad press: {st}")
        await send(ed, {"type": "touch", "action": "tap", "pointer": 3, "x": 600, "y": 100, "hold_frames": 0})
        await ed.call("wait", frames=1)
        st = await scheme(ed)
        ed.check_eq((st["current"], st["pointer"]), ("Touch", "Touch"), f"a tap: {st}")
        ed.check(not await stick_hidden(ed), "the stick shows while the player uses touch")
        await send(ed, {"type": "key", "key": "e", "hold_frames": 0})
        await ed.call("wait", frames=1)
        st = await scheme(ed)
        ed.check_eq(st["current"], "Keyboard&Mouse", f"a key: {st}")
        ed.check(st["override"] is None, f"not forced: {st}")
        ed.check(await stick_hidden(ed), "hidden again")
        state = await ed.call("get_input_state", fields=["raw"])
        ed.check_eq(state["changes"], 3, f"three switches: {state['scheme']}")
    finally:
        await stop(ed)


async def test_forced_scheme(ed: Editor):
    """configure_game_view input_scheme forces it from Play start; auto clears it."""
    await ed.call("tool_groups", enable=["editor_ux"])
    try:
        r = await ed.call("configure_game_view", input_scheme="gamepad")
        ed.check_eq(r["input_scheme"], "gamepad", f"{r}")
        await start(ed)
        try:
            st = await scheme(ed)
            ed.check_eq((st["current"], st["override"]), ("Gamepad", "Gamepad"), f"forced at Play start: {st}")
            await send(ed, {"type": "key", "key": "e", "hold_frames": 0})
            ed.check_eq((await scheme(ed))["current"], "Gamepad", "the override wins over a key")
            await ed.call("configure_game_view", input_scheme="auto")
            await ed.call("wait", frames=1)
            st = await scheme(ed)
            ed.check_eq((st["current"], st["override"]), ("Keyboard&Mouse", None), f"auto: the tracked one: {st}")
        finally:
            await stop(ed)
        bad = await ed.call_raw("configure_game_view", {"input_scheme": "joystick"})
        ed.check(bad.is_error, "an unknown scheme is refused")
    finally:
        await ed.call("configure_game_view", input_scheme="auto")
        await ed.call("tool_groups", disable=["editor_ux"])


async def test_lua_api(ed: Editor):
    """InputActions.currentScheme reads it; onSchemeChanged logs once per switch."""
    await start(ed)
    try:
        await ed.console_since(None)
        values = (await ed.call("eval_lua", code="return InputActions.currentScheme, InputScheme.Touch, InputActions.lastGamepad"))["values"]
        ed.check_eq(values, ["Keyboard&Mouse", "Touch", -1], "the scheme from Lua")
        await send(ed, {"type": "gamepad", "gamepad": 0, "button": "a", "hold_frames": 0})
        await send(ed, {"type": "gamepad", "gamepad": 0, "button": "b", "hold_frames": 0})
        await send(ed, {"type": "touch", "action": "tap", "pointer": 3, "x": 600, "y": 100, "hold_frames": 0})
        await ed.call("wait", frames=2)
        values = (await ed.call("eval_lua", code="return InputActions.currentScheme, InputActions.lastGamepad"))["values"]
        ed.check_eq(values, ["Touch", 0], "after a pad and a tap")
        lines = [str(entry.get("message", "")) for entry in await ed.console_since()]
        markers = [line for line in lines if "[scheme]" in line]
        ed.check_eq(len(markers), 2, f"one marker per switch: {markers}")
        ed.check("Keyboard&Mouse -> Gamepad" in markers[0] and "Gamepad -> Touch" in markers[-1], f"{markers}")
    finally:
        await stop(ed)
