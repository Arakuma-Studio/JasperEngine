"""S22 input as an agent sees it (docs/specs/S22-gameplay.md AC14): a Lua script counts the frame
edges it reads (Input.getKeyDown/getKeyUp in onUpdate, the fixed window in onFixedUpdate), and
send_input drives it; hold_frames 0 presses and releases in one frame, which both edges see
exactly once. get_input_state shows the frame's edges. Mouse, gamepad and touch variants, and
the mouse delta of a captured pointer.

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
               padDown = 0, padUp = 0, touchDown = 0, touchUp = 0, touchEnded = 0, any = 0,
               deltaX = 0, deltaY = 0, downFrame = -1, upFrame = -1 }
end

function InputCounter:init()
    zero()
    resetCounts = zero
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onFixedUpdate")
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
