"""HTML UI input in play mode through MCP send_input (phase 1, p1-input).
World-space routing and capture share the project workflow in test_html_ui_integration.

A UI scene with one document: a button, two text inputs, a scroll box with coloured stripes,
two touch targets. A Lua script counts what reaches the game (onMouseDown, onKeyDown,
onCharInput, onMouseScroll, onTouchStart). The value of a text field is read back by selecting
it and copying it (Ctrl+A, Ctrl+C) into the clipboard, then System.getClipboardText(): that
checks the exact string, astral characters included. A Lua click listener independently
checks that document input reaches runtime scripts.

Runs on a scratch copy of the fixture project (canvas 1280 x 720). Coordinates are canvas
pixels, which are document pixels here.
"""

from harness import Editor, REPO
from htmlui_pixels import Shot, check_color

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

TIMEOUT = 900

DOC = "assets/ui/input.html"

HTML = """<html>
<head>
    <title>input</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
               font-family: sans-serif; font-size: 20px; color: #ffffff; }
        div, input { display: block; position: absolute; }
        #button { left: 40px; top: 40px; width: 200px; height: 60px; background-color: #808080; cursor: pointer; }
        #button:hover { background-color: #00ff00; }
        #button:active { background-color: #0000ff; }
        input { width: 400px; height: 36px; background-color: #303030; color: #ffffff; font-size: 20px; }
        #first { left: 40px; top: 140px; }
        #second { left: 40px; top: 200px; }
        #scroller { left: 600px; top: 40px; width: 200px; height: 100px; overflow: auto; background-color: #000000; }
        .stripe { position: relative; width: 180px; height: 100px; }
        #s1 { background-color: #ff0000; }
        #s2 { background-color: #ffff00; }
        #s3 { background-color: #00ffff; }
        #touchA { left: 600px; top: 300px; width: 150px; height: 80px; background-color: #a0a0a0; }
        #touchB { left: 800px; top: 300px; width: 150px; height: 80px; background-color: #a0a0a0; }
        #touchA:active, #touchB:active { background-color: #ff00ff; }
    </style>
</head>
<body>
    <div id="button"></div>
    <input type="text" id="first" value=""/>
    <input type="text" id="second" value=""/>
    <div id="scroller"><div class="stripe" id="s1"></div><div class="stripe" id="s2"></div><div class="stripe" id="s3"></div></div>
    <div id="touchA"></div>
    <div id="touchB"></div>
</body>
</html>
"""

PROBE = """---@class InputProbe : ScriptInstance
local InputProbe = {
    properties = {
        { name = "downs", type = "int", default = 0 },
        { name = "keyDowns", type = "int", default = 0 },
        { name = "lastKey", type = "int", default = 0 },
        { name = "chars", type = "int", default = 0 },
        { name = "scrolls", type = "int", default = 0 },
        { name = "touches", type = "int", default = 0 },
        { name = "clicks", type = "int", default = 0 },
    }
}

function InputProbe:init()
    RegisterEngineEvent(self, "onMouseDown")
    RegisterEngineEvent(self, "onKeyDown")
    RegisterEngineEvent(self, "onCharInput")
    RegisterEngineEvent(self, "onMouseScroll")
    RegisterEngineEvent(self, "onTouchStart")
end

function InputProbe:attach()
    local doc = UIDocument(self.scene, self.entity)
    doc:getElementById("button"):addEventListener("click", function(event) self.clicks = self.clicks + 1 end)
end

function InputProbe:onMouseDown(button, x, y, mods) self.downs = self.downs + 1 end
function InputProbe:onKeyDown(key, rep, mods) self.keyDowns = self.keyDowns + 1; self.lastKey = key end
function InputProbe:onCharInput(c) self.chars = self.chars + 1 end
function InputProbe:onMouseScroll(x, y, mods) self.scrolls = self.scrolls + 1 end
function InputProbe:onTouchStart(pointer, x, y) self.touches = self.touches + 1 end

return InputProbe
"""

GRAY = (128, 128, 128)
GREEN = (0, 255, 0)
RED = (255, 0, 0)
YELLOW = (255, 255, 0)
MAGENTA = (255, 0, 255)
LIGHT = (160, 160, 160)

P_BUTTON = (140, 70)
P_FIRST = (60, 158)
P_SECOND = (60, 218)
P_SCROLL = (700, 90)
P_TOUCH_A = (675, 340)
P_TOUCH_B = (875, 340)
P_BODY = (1100, 600)

TEXT = "héllo \U0001F600"   # héllo 😀


async def shoot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-input-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def send(ed: Editor, *events, frames_after=2):
    r = await ed.call_raw("send_input", {"events": list(events), "space": "canvas", "frames_after": frames_after})
    ed.check(not r.is_error, f"send_input {events}: {r.text[:300]}")
    return r


async def probe(ed: Editor, field: str):
    return (await ed.call("eval_lua", code=f"return script('Menu', 'InputProbe').{field}"))["values"][0]


async def clipboard(ed: Editor) -> str:
    return (await ed.call("eval_lua", code="return System.getClipboardText()"))["values"][0]


async def set_clipboard(ed: Editor, text: str):
    await ed.call("eval_lua", code=f"System.setClipboardText({text!r})")


async def field_value(ed: Editor, point) -> str:
    """The text of the input at point: focus, select all, copy, read the clipboard."""
    await set_clipboard(ed, "<nothing copied>")
    await send(ed, {"type": "mouse", "action": "click", "x": point[0], "y": point[1]},
               {"type": "key", "key": "a", "mods": ["ctrl"]},
               {"type": "key", "key": "c", "mods": ["ctrl"]})
    return await clipboard(ed)


async def html_ui_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


# -- tests ---------------------------------------------------------------------------------------


async def setup_and_play(ed: Editor):
    """A UI scene with the document and the probe script, playing."""
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiInput", type="UI")
    scene = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Menu")
    menu = ent["entity_id"]
    await ed.call("add_component", scene=scene, entity=menu, component="UIDocument",
                  values={"uidocument": {"filename": DOC}})
    await ed.call("create_script", kind="lua", name="InputProbe", content=PROBE, scene=scene, entity=menu)
    await ed.call("save_scene", scene=scene, path="scenes/HtmlUiInput")

    started = await ed.call("play", action="start", scene=scene)
    ed.check(started, "play started")
    await send(ed, {"type": "wait", "frames": 3})
    await ed.call("eval_lua", code="script('Menu', 'InputProbe'):attach()")


async def test_click_consumed_over_button_not_over_body(ed: Editor):
    await setup_and_play(ed)
    await send(ed, {"type": "mouse", "action": "click", "x": P_BUTTON[0], "y": P_BUTTON[1]})
    ed.check_eq(await probe(ed, "downs"), 0, "a click on the button reached the game's onMouseDown")
    ed.check_eq(await probe(ed, "clicks"), 1, "the button's click listener (Lua) ran once")

    await send(ed, {"type": "mouse", "action": "click", "x": P_BODY[0], "y": P_BODY[1]})
    ed.check_eq(await probe(ed, "downs"), 1, "a click on the transparent body did not reach the game")


async def test_typing_unicode(ed: Editor):
    """Typing "héllo 😀" into an <input>: its value is exactly that string; the game gets no chars."""
    chars = await probe(ed, "chars")
    await send(ed, {"type": "mouse", "action": "click", "x": P_FIRST[0], "y": P_FIRST[1]},
               {"type": "text", "text": TEXT})
    ed.check_eq(await probe(ed, "chars"), chars, "text typed into a focused field reached onCharInput")
    keys = await probe(ed, "keyDowns")
    value = await field_value(ed, P_FIRST)
    ed.check_eq(value, TEXT, "the input's value")
    ed.check_eq(await probe(ed, "keyDowns"), keys, "Ctrl+A / Ctrl+C in a focused field reached onKeyDown")


async def test_ime_sequences(ed: Editor):
    """ime / ime_commit / ime_cancel on the second input."""
    await send(ed, {"type": "mouse", "action": "click", "x": P_SECOND[0], "y": P_SECOND[1]},
               {"type": "text", "text": "x"},
               {"type": "ime", "text": "k"},
               {"type": "ime", "text": "ka"},
               {"type": "ime", "text": "か", "cursor": 1},
               {"type": "ime_commit", "text": "か"})
    ed.check_eq(await field_value(ed, P_SECOND), "xか", "compose ka → か, commit")

    # the selection from field_value is gone after the caret moves to the end
    await send(ed, {"type": "key", "key": "end"},
               {"type": "ime", "text": "ab", "cursor": 2},
               {"type": "ime_cancel"})
    ed.check_eq(await field_value(ed, P_SECOND), "xか", "a cancelled composition leaves the text as before")

    await send(ed, {"type": "key", "key": "end"},
               {"type": "ime", "text": "zz", "cursor": -1},
               {"type": "ime_commit", "text": "Z"})
    ed.check_eq(await field_value(ed, P_SECOND), "xかZ", "a commit replaces the composition")


async def test_copy_paste_between_inputs(ed: Editor):
    """Ctrl+A, Ctrl+C in the first input, Ctrl+V into the (emptied) second."""
    await send(ed, {"type": "mouse", "action": "click", "x": P_FIRST[0], "y": P_FIRST[1]},
               {"type": "key", "key": "a", "mods": ["ctrl"]},
               {"type": "key", "key": "c", "mods": ["ctrl"]})
    ed.check_eq(await clipboard(ed), TEXT, "System.getClipboardText() after Ctrl+C")

    await send(ed, {"type": "mouse", "action": "click", "x": P_SECOND[0], "y": P_SECOND[1]},
               {"type": "key", "key": "a", "mods": ["ctrl"]},
               {"type": "key", "key": "backspace"},
               {"type": "key", "key": "v", "mods": ["ctrl"]})
    ed.check_eq(await field_value(ed, P_SECOND), TEXT, "the second input after Ctrl+V")


async def test_escape_not_consumed(ed: Editor):
    await send(ed, {"type": "mouse", "action": "click", "x": P_FIRST[0], "y": P_FIRST[1]})
    keys = await probe(ed, "keyDowns")
    await send(ed, {"type": "key", "key": "escape"})
    ed.check_eq(await probe(ed, "keyDowns"), keys + 1, "Escape in a focused field reached onKeyDown")
    ed.check_eq(await probe(ed, "lastKey"), 256, "the key the game got (D_KEY_ESCAPE)")
    await send(ed, {"type": "key", "key": "q"})
    ed.check_eq(await probe(ed, "keyDowns"), keys + 1, "a letter in a focused field reached onKeyDown")
    # off the field: letters are the game's again
    await send(ed, {"type": "mouse", "action": "click", "x": P_BODY[0], "y": P_BODY[1]},
               {"type": "key", "key": "q"})
    ed.check_eq(await probe(ed, "keyDowns"), keys + 2, "a letter with no field focused")


async def test_wheel_scrolls(ed: Editor):
    shot = await shoot(ed, "wheel-before")
    check_color(ed, shot, P_SCROLL, RED, "the scroll box before the wheel: first stripe")
    scrolls = await probe(ed, "scrolls")
    await send(ed, {"type": "mouse", "action": "scroll", "x": P_SCROLL[0], "y": P_SCROLL[1], "dy": -1}, frames_after=4)
    shot = await shoot(ed, "wheel-after")
    check_color(ed, shot, P_SCROLL, YELLOW, "the scroll box after one wheel step down: second stripe")
    ed.check_eq(await probe(ed, "scrolls"), scrolls, "a wheel over the scroll box reached onMouseScroll")

    await send(ed, {"type": "mouse", "action": "scroll", "x": P_BODY[0], "y": P_BODY[1], "dy": -1})
    ed.check_eq(await probe(ed, "scrolls"), scrolls + 1, "a wheel over the body did not reach the game")


async def test_two_touch_pointers(ed: Editor):
    touches = await probe(ed, "touches")
    await send(ed, {"type": "touch", "action": "start", "pointer": 0, "x": P_TOUCH_A[0], "y": P_TOUCH_A[1]},
               {"type": "touch", "action": "start", "pointer": 1, "x": P_TOUCH_B[0], "y": P_TOUCH_B[1]}, frames_after=3)
    shot = await shoot(ed, "two-touches")
    check_color(ed, shot, P_TOUCH_A, MAGENTA, "first finger's target :active")
    check_color(ed, shot, P_TOUCH_B, MAGENTA, "second finger's target :active")
    ed.check_eq(await probe(ed, "touches"), touches, "touches on document elements reached onTouchStart")

    await send(ed, {"type": "touch", "action": "end", "pointer": 0, "x": P_TOUCH_A[0], "y": P_TOUCH_A[1]},
               {"type": "touch", "action": "end", "pointer": 1, "x": P_TOUCH_B[0], "y": P_TOUCH_B[1]}, frames_after=3)
    shot = await shoot(ed, "touches-ended")
    check_color(ed, shot, P_TOUCH_A, LIGHT, "first target after the fingers lifted")
    check_color(ed, shot, P_TOUCH_B, LIGHT, "second target after the fingers lifted")

    await send(ed, {"type": "touch", "action": "tap", "pointer": 2, "x": P_BODY[0], "y": P_BODY[1]})
    ed.check_eq(await probe(ed, "touches"), touches + 1, "a touch on the body did not reach the game")


async def test_mouse_leave_clears_hover(ed: Editor):
    await send(ed, {"type": "mouse", "action": "move", "x": P_BUTTON[0], "y": P_BUTTON[1]}, frames_after=3)
    shot = await shoot(ed, "hover")
    check_color(ed, shot, P_BUTTON, GREEN, "the button under the pointer (:hover)")
    await send(ed, {"type": "mouse_leave"}, frames_after=3)
    shot = await shoot(ed, "left")
    check_color(ed, shot, P_BUTTON, GRAY, "the button after mouse_leave")
    await send(ed, {"type": "mouse_enter"},
               {"type": "mouse", "action": "move", "x": P_BUTTON[0] + 2, "y": P_BUTTON[1]}, frames_after=3)
    shot = await shoot(ed, "entered")
    check_color(ed, shot, P_BUTTON, GREEN, "hover again after mouse_enter and a move")

    await ed.call("play", action="stop")
    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")
