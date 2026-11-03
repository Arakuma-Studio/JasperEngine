"""MCP gamepad dispatch through a running document's Lua callbacks.

Engine tests own focus, slider and repeat semantics. This scenario protects conversion of
MCP button/axis names and values, cancellation by document handlers, and routing to game scripts.
"""

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

CI = True
EDITOR = {"create": True, "name": "Html Ui Gamepad CI"}

TIMEOUT = 900

DOC = "assets/ui/pad.html"

HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Pad</title>
  <style>
    html, body { margin: 0; background-color: #1e1e1e; font-family: sans-serif; color: #ffffff; }
    .menu { display: flex; flex-direction: column; gap: 10px; width: 300px; padding: 20px; }
    button { height: 40px; font: inherit; color: #ffffff; background-color: #404040; border: 0; }
    button:focus-visible { background-color: #ff00ff; outline: none; }
  </style>
</head>
<body>
  <div class="menu">
    <button id="b1">One</button>
    <button id="b2">Two</button>
    <button id="b3">Three</button>
  </div>
</body>
</html>
"""

PROBE = """---@class PadProbe : ScriptInstance
local PadProbe = {
    properties = {
        { name = "gameDowns", type = "int", default = 0 },
        { name = "gameAxes", type = "int", default = 0 },
        { name = "lastGameButton", type = "int", default = -1 },
        { name = "closes", type = "int", default = 0 },
    }
}

function PadProbe:init()
    _G.padLog = {}
    RegisterEngineEvent(self, "onGamepadButtonDown")
    RegisterEngineEvent(self, "onGamepadAxisMove")
    self.doc = UIDocument(self.scene, self.entity)
    ---@param event UIEvent
    self.onKey = function(event)
        local log = _G.padLog
        log[#log + 1] = event.code .. "|" .. event.key .. "|" .. event.gamepad .. "|" .. tostring(event["repeat"])
        if event.key == "Escape" then
            self.closes = self.closes + 1
            event:preventDefault()
        end
    end
    RegisterEvent(self, self.doc.onLoad, "bind")
    self:bind()
end

function PadProbe:bind()
    self.doc:addEventListener("keydown", self.onKey)
    self.doc:getElementById("b1"):focus()
end

function PadProbe:onGamepadButtonDown(gamepad, button)
    self.gameDowns = self.gameDowns + 1
    self.lastGameButton = button
end

function PadProbe:onGamepadAxisMove(gamepad, axis, value)
    self.gameAxes = self.gameAxes + 1
end

return PadProbe
"""


async def lua(ed: Editor, code: str, **args) -> list:
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def probe(ed: Editor, field: str):
    return (await lua(ed, f"return script('Pad', 'PadProbe').{field}"))[0]


async def focused(ed: Editor) -> str:
    return (await lua(ed, """local f = object('Pad', 'UIDocument'):querySelector(':focus')
        if f == nil or f.tagName == 'HTML' or f.tagName == 'BODY' then return '' end
        return f.id"""))[0]


async def pad_log(ed: Editor) -> list[str]:
    return (await lua(ed, "return table.concat(_G.padLog, '\\n')"))[0].splitlines()


async def send(ed: Editor, *events, frames_after=3):
    r = await ed.call_raw("send_input", {"events": list(events), "frames_after": frames_after})
    ed.check(not r.is_error, f"send_input {events}: {r.text[:300]}")


def press(button: str) -> dict:
    return {"type": "gamepad", "button": button, "action": "press"}


async def test_gamepad_input_routing(ed: Editor):
    """MCP gamepad names reach document Lua listeners and unconsumed events reach the game."""
    try:
        await ed.console_since(None, limit=1)
        made = await ed.call("create_scene", name="HtmlUiGamepad", type="UI")
        scene = made["scene"]["id"]
        await ed.call("write_file", path=DOC, content=HTML)
        ent = await ed.call("create_entity", scene=scene, type="ui_document", name="Pad")
        await ed.call("set_properties", scene=scene, entity=ent["entity_id"], values={"uidocument": {"filename": DOC}})
        await ed.call("create_script", kind="lua", name="PadProbe", content=PROBE, scene=scene, entity=ent["entity_id"])
        await ed.call("save_scene", scene=scene, path="scenes/HtmlUiGamepad")
        await ed.call("wait", frames=10)

        started = await ed.call("play", action="start", scene=scene, fixed_fps=60)
        ed.check(started, "play started")
        await ed.call("wait", frames=10)
        ed.check_eq(await focused(ed), "b1", "the script focused the first button")

        downs = await probe(ed, "gameDowns")
        await send(ed, press("dpad_down"), press("dpad_down"))
        ed.check_eq(await focused(ed), "b3", "the d-pad moved the focus down twice")
        await send(ed, press("dpad_up"))
        ed.check_eq(await focused(ed), "b2", "and up")
        await lua(ed, """_G.padClicks = 0
            object('Pad', 'UIDocument'):getElementById('b2'):addEventListener('click', function() _G.padClicks = _G.padClicks + 1 end)""")
        await send(ed, press("a"))
        ed.check_eq(await lua(ed, "return _G.padClicks"), [1], "A clicked the focused button")
        ed.check_eq(await probe(ed, "gameDowns"), downs, "the focused menu took the d-pad and A")
        log = await pad_log(ed)
        ed.check(log[:4] == ["GamepadDPadDown|ArrowDown|0|false", "GamepadDPadDown|ArrowDown|0|false",
                             "GamepadDPadUp|ArrowUp|0|false", "GamepadA|Enter|0|false"], f"the keydowns Lua saw: {log}")

        downs = await probe(ed, "gameDowns")
        await send(ed, press("b"))
        ed.check_eq(await probe(ed, "closes"), 1, "the menu's Escape handler ran for B")
        ed.check_eq(await probe(ed, "gameDowns"), downs, "B, handled by the menu, did not reach the game")
        ed.check_eq((await pad_log(ed))[-1], "GamepadB|Escape|0|false", "B's keydown")

        downs = await probe(ed, "gameDowns")
        await send(ed, press("right_bumper"), {"type": "gamepad", "axis": "left_trigger", "value": 1.0},
                   {"type": "gamepad", "axis": "left_trigger", "value": -1.0})
        log = await pad_log(ed)
        ed.check(log[-2:] == ["GamepadRightShoulder|GamepadRightShoulder|0|false", "GamepadLeftTrigger|GamepadLeftTrigger|0|false"],
                 f"the shoulder and the trigger reached the document: {log[-3:]}")
        ed.check_eq(await probe(ed, "gameDowns"), downs + 1, "nobody used the shoulder: the game got it")
        ed.check_eq(await probe(ed, "lastGameButton"), 5, "GAMEPAD_BUTTON_RIGHT_BUMPER")

        await lua(ed, "object('Pad', 'UIDocument'):querySelector(':focus'):blur()")
        ed.check_eq(await focused(ed), "", "no focus")
        downs = await probe(ed, "gameDowns")
        axes = await probe(ed, "gameAxes")
        await send(ed, press("dpad_down"), press("a"), {"type": "gamepad", "axis": "left_x", "value": 0.7},
                   {"type": "gamepad", "axis": "left_x", "value": 0.0})
        ed.check_eq(await probe(ed, "gameDowns"), downs + 2, "the game got the d-pad and A")
        ed.check_eq(await probe(ed, "gameAxes"), axes + 2, "and the stick")
        ed.check_eq(await focused(ed), "", "nothing was navigated")

        lines = [e.get("message", "") for e in await ed.console_since(types=["error", "warning"], limit=1000)]
        ed.check(not lines, f"errors or warnings: {lines[:10]}")

    finally:
        await ed.call("play", action="stop")
