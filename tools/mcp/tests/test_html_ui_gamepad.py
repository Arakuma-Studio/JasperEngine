"""Gamepads in HTML UI documents (phase 4, p4-input) through MCP send_input in play mode, plus the
two data-binding fixes of the package seen from an agent: no Console line for a data-style
expression of a model no script created yet (edit mode), and data-for rows counted without their
hidden template.

A UI scene with one document: a column of buttons (a :focus-visible colour for the screenshot),
a range, and a HUD part bound to the model `hud` (data-style-width, a data-for list). A Lua
script records the document's keydown events (code, key, gamepad, repeat), closes on Escape
(B), and counts what reaches the game (onGamepadButtonDown, onGamepadAxisMove).
"""

import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_html_ui_input import Shot, near  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Html Ui Gamepad CI"}

TIMEOUT = 900

S: dict = {}

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
    .bar { width: 300px; height: 10px; background-color: #000000; }
    .fill { height: 100%; background-color: #00ff00; }
  </style>
</head>
<body>
  <div class="menu">
    <button id="b1">One</button>
    <button id="b2">Two</button>
    <button id="b3">Three</button>
    <button id="b4">Four</button>
    <button id="b5">Five</button>
    <input id="volume" type="range" min="0" max="10" step="1" value="5">
  </div>
  <div data-model="hud">
    <div class="bar"><div id="fill" class="fill" data-style-width="health + '%'"></div></div>
    <ul id="log"><li data-for="entry : entries">{{ entry }}</li></ul>
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
    self.model = self.doc:createDataModel("hud", { health = 50, entries = { "a", "b" } })
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


async def test_setup_edit_mode_and_play(ed: Editor):
    """The document loads in edit mode with no Console line (its model does not exist yet), then plays."""
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiGamepad", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Pad")
    await ed.call("set_properties", scene=S["scene"], entity=ent["entity_id"], values={"uidocument": {"filename": DOC}})
    await ed.call("create_script", kind="lua", name="PadProbe", content=PROBE, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiGamepad")
    await ed.call("wait", frames=10)

    loaded = await lua(ed, "return object('Pad', 'UIDocument').loaded, #object('Pad', 'UIDocument'):querySelectorAll('#log li')",
                       allow_edit_mode=True)
    ed.check_eq(loaded, [True, 0], "edit mode: loaded; no model, no rows (the data-for template is not counted)")
    lines = [e.get("message", "") for e in await ed.console_since(types=["error", "warning"], limit=1000)]
    ed.check(not lines, f"edit mode: no Console warning or error for the unbound data-style-width: {lines[:5]}")

    started = await ed.call("play", action="start", scene=S["scene"], fixed_fps=60)
    ed.check(started, "play started")
    await ed.call("wait", frames=10)
    ed.check_eq(await focused(ed), "b1", "the script focused the first button")
    values = await lua(ed, """local doc = object('Pad', 'UIDocument')
        local rows = doc:querySelectorAll('#log li')
        return #rows, rows[1].textContent, rows[2].textContent, doc:getElementById('log').childElementCount,
               math.floor(doc:getElementById('fill'):getBoundingClientRect().width + 0.5)""")
    ed.check_eq(values, [2, "a", "b", 2, 150], "the HUD part: two rows (no template), width 50%")


async def test_dpad_and_a(ed: Editor):
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


async def test_focus_visible_shows(ed: Editor):
    """The button the gamepad moved to shows its :focus-visible colour."""
    # (left of the label: a button centers its label, as Chromium does since p4-gaps)
    values = await lua(ed, """local r = object('Pad', 'UIDocument'):getElementById('b2'):getBoundingClientRect()
        return r.left + 10, r.top + r.height / 2""")
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / "html-ui-gamepad-focus.png").write_bytes(png)
    image = Shot(png, shot.value)
    ed.check(near(image.px(values[0], values[1]), (255, 0, 255)), f"#b2 shows :focus-visible: {image.px(values[0], values[1])}")
    ed.check(near(image.px(values[0], values[1] + 50), (64, 64, 64)), f"#b3 does not: {image.px(values[0], values[1] + 50)}")


async def test_b_cancels(ed: Editor):
    downs = await probe(ed, "gameDowns")
    await send(ed, press("b"))
    ed.check_eq(await probe(ed, "closes"), 1, "the menu's Escape handler ran for B")
    ed.check_eq(await probe(ed, "gameDowns"), downs, "B, handled by the menu, did not reach the game")
    ed.check_eq((await pad_log(ed))[-1], "GamepadB|Escape|0|false", "B's keydown")


async def test_other_buttons_reach_lua_and_the_game(ed: Editor):
    downs = await probe(ed, "gameDowns")
    await send(ed, press("right_bumper"), {"type": "gamepad", "axis": "left_trigger", "value": 1.0},
               {"type": "gamepad", "axis": "left_trigger", "value": -1.0})
    log = await pad_log(ed)
    ed.check(log[-2:] == ["GamepadRightShoulder|GamepadRightShoulder|0|false", "GamepadLeftTrigger|GamepadLeftTrigger|0|false"],
             f"the shoulder and the trigger reached the document: {log[-3:]}")
    ed.check_eq(await probe(ed, "gameDowns"), downs + 1, "nobody used the shoulder: the game got it")
    ed.check_eq(await probe(ed, "lastGameButton"), 5, "GAMEPAD_BUTTON_RIGHT_BUMPER")


async def test_slider_and_stick_repeat(ed: Editor):
    await lua(ed, "object('Pad', 'UIDocument'):getElementById('volume'):focus()")
    await send(ed, press("dpad_right"), press("dpad_right"))
    ed.check_eq(await lua(ed, "return tonumber(object('Pad', 'UIDocument'):getElementById('volume').value)"), [7],
                "the d-pad adjusted the focused range")

    # the left stick held up for 40 frames at 60 fps: one step, then repeats after 0.4 s every 0.1 s
    axes = await probe(ed, "gameAxes")
    count = len(await pad_log(ed))
    await send(ed, {"type": "gamepad", "axis": "left_y", "value": -1.0}, {"type": "wait", "frames": 40},
               {"type": "gamepad", "axis": "left_y", "value": 0.0})
    log = (await pad_log(ed))[count:]
    ups = [line for line in log if line.startswith("GamepadLeftThumbstickUp|ArrowUp|0|")]
    ed.check(len(ups) >= 3 and ups[0].endswith("|false") and all(u.endswith("|true") for u in ups[1:]),
             f"the stick's first keydown and its repeats: {log}")
    ed.check_eq(await focused(ed), "b" + str(max(1, 6 - len(ups))), "the focus went up once per keydown")
    ed.check_eq(await probe(ed, "gameAxes"), axes, "the focused menu took the stick")


async def test_unfocused_gamepad_is_the_games(ed: Editor):
    await lua(ed, "object('Pad', 'UIDocument'):querySelector(':focus'):blur()")
    ed.check_eq(await focused(ed), "", "no focus")
    downs = await probe(ed, "gameDowns")
    axes = await probe(ed, "gameAxes")
    await send(ed, press("dpad_down"), press("a"), {"type": "gamepad", "axis": "left_x", "value": 0.7},
               {"type": "gamepad", "axis": "left_x", "value": 0.0})
    ed.check_eq(await probe(ed, "gameDowns"), downs + 2, "the game got the d-pad and A")
    ed.check_eq(await probe(ed, "gameAxes"), axes + 2, "and the stick")
    ed.check_eq(await focused(ed), "", "nothing was navigated")


async def test_stop_and_errors(ed: Editor):
    await ed.call("play", action="stop")
    lines = [e.get("message", "") for e in await ed.console_since(types=["error", "warning"], limit=1000)]
    ed.check(not lines, f"errors or warnings: {lines[:10]}")
