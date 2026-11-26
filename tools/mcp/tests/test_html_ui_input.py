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

CI = True
EDITOR = {"create": True, "name": "Html Ui Input CI"}

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
        #mail { left: 40px; top: 260px; }
        #scroller { left: 600px; top: 40px; width: 200px; height: 100px; overflow: auto; background-color: #000000; }
        .stripe { position: relative; width: 180px; height: 100px; }
        #s1 { background-color: #ff0000; }
        #s2 { background-color: #ffff00; }
        #s3 { background-color: #00ffff; }
        #touchA { left: 600px; top: 300px; width: 150px; height: 80px; background-color: #a0a0a0; }
        #touchB { left: 800px; top: 300px; width: 150px; height: 80px; background-color: #a0a0a0; }
        #touchA:active, #touchB:active { background-color: #ff00ff; }
        #draglist { left: 850px; top: 40px; width: 200px; height: 100px; overflow-y: auto; background-color: #000000;
                    scroll-drag: pointer; }
        .dstripe { position: relative; width: 180px; height: 100px; background-color: #404040; }
        #clipview { left: 40px; top: 420px; width: 300px; height: 150px; overflow: hidden; background-color: #202020; }
        #clipbtn { left: 20px; top: 160px; width: 100px; height: 60px; transform: translateY(-120px); background-color: #ff8000; }
    </style>
</head>
<body>
    <div id="button"></div>
    <input type="text" id="first" value=""/>
    <input type="text" id="second" value=""/>
    <input type="email" id="mail" enterkeyhint="send" maxlength="32" autocapitalize="words" value=""/>
    <div id="scroller"><div class="stripe" id="s1"></div><div class="stripe" id="s2"></div><div class="stripe" id="s3"></div></div>
    <div id="touchA"></div>
    <div id="touchB"></div>
    <div id="draglist"><div class="dstripe"></div><div class="dstripe"></div><div class="dstripe"></div></div>
    <div id="clipview"><div id="clipbtn"></div></div>
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
        { name = "touchClicks", type = "int", default = 0 },
        { name = "touchPointers", type = "string", default = "" },
        { name = "touchMoves", type = "int", default = 0 },
        { name = "touchEnds", type = "int", default = 0 },
        { name = "dragClicks", type = "int", default = 0 },
    }
}

function InputProbe:init()
    RegisterEngineEvent(self, "onMouseDown")
    RegisterEngineEvent(self, "onKeyDown")
    RegisterEngineEvent(self, "onCharInput")
    RegisterEngineEvent(self, "onMouseScroll")
    RegisterEngineEvent(self, "onTouchStart")
    RegisterEngineEvent(self, "onTouchMove")
    RegisterEngineEvent(self, "onTouchEnd")
end

function InputProbe:attach()
    local doc = UIDocument(self.scene, self.entity)
    self.doc = doc
    doc:getElementById("button"):addEventListener("click", function(event) self.clicks = self.clicks + 1 end)
    doc:getElementById("draglist"):addEventListener("click", function(event) self.dragClicks = self.dragClicks + 1 end)
    -- ui-multitouch: a click per finger, with its pointer (DOM pointerId = fingerId + 2)
    for _, id in ipairs({"touchA", "touchB"}) do
        doc:getElementById(id):addEventListener("click", function(event)
            self.touchClicks = self.touchClicks + 1
            self.touchPointers = self.touchPointers .. id .. ":" .. event.pointerType .. event.fingerId .. " "
        end)
    end
end

function InputProbe:onMouseDown(button, x, y, mods) self.downs = self.downs + 1 end
function InputProbe:onKeyDown(key, rep, mods) self.keyDowns = self.keyDowns + 1; self.lastKey = key end
function InputProbe:onCharInput(c) self.chars = self.chars + 1 end
function InputProbe:onMouseScroll(x, y, mods) self.scrolls = self.scrolls + 1 end
function InputProbe:onTouchStart(pointer, x, y) self.touches = self.touches + 1 end
function InputProbe:onTouchMove(pointer, x, y) self.touchMoves = self.touchMoves + 1 end
function InputProbe:onTouchEnd(pointer, x, y)
    self.touchEnds = self.touchEnds + 1
    print("InputProbe onTouchEnd " .. self.touchEnds .. " finger " .. pointer)
end

return InputProbe
"""

# S93 AC6: a full-screen popup (a dim page with an OK button), hidden until a script shows it
POPUP_DOC = "assets/ui/popup.html"

POPUP_HTML = """<html>
<head>
    <style>
        html, body { margin: 0; width: 100%; height: 100%; }
        body { background-color: rgba(0, 0, 0, 0.5); }
        #ok { position: absolute; left: 540px; top: 320px; width: 200px; height: 80px; background-color: #808080; }
    </style>
</head>
<body><div id="ok"></div></body>
</html>
"""

POPUP_PROBE = """---@class PopupProbe : ScriptInstance
local PopupProbe = { properties = { { name = "clicks", type = "int", default = 0 } } }

function PopupProbe:show()
    local doc = UIDocument(self.scene, self.entity)
    doc:getElementById("ok"):addEventListener("click", function() self.clicks = self.clicks + 1 end)
    doc:show()
end

return PopupProbe
"""

P_POPUP_OK = (640, 360)

GRAY = (128, 128, 128)
GREEN = (0, 255, 0)
RED = (255, 0, 0)
YELLOW = (255, 255, 0)
MAGENTA = (255, 0, 255)
LIGHT = (160, 160, 160)

P_BUTTON = (140, 70)
P_FIRST = (60, 158)
P_SECOND = (60, 218)
P_MAIL = (60, 278)
P_SCROLL = (700, 90)
P_TOUCH_A = (675, 340)
P_TOUCH_B = (875, 340)
P_BODY = (1100, 600)

TEXT = "héllo \U0001F600"   # héllo 😀

# the scene and the document's GameObject, set by setup_and_play
S = {}


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
    await ed.call("write_file", path=POPUP_DOC, content=POPUP_HTML)
    popup = (await ed.call("create_entity", scene=scene, type="empty", name="Popup"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=popup, component="UIDocument",
                  values={"uidocument": {"filename": POPUP_DOC, "sortOrder": 10, "enabled": False}})
    await ed.call("create_script", kind="lua", name="PopupProbe", content=POPUP_PROBE, scene=scene, entity=popup)
    await ed.call("save_scene", scene=scene, path="scenes/HtmlUiInput")
    S.update(scene=scene, menu=menu)

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


async def test_text_entry_traits(ed: Editor):
    """text-entry AC20: inspect_ui reports the focused field's soft keyboard."""
    await send(ed, {"type": "mouse", "action": "click", "x": P_MAIL[0], "y": P_MAIL[1]})
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["menu"], selector="#mail", depth=0)
    focus = r.get("focus")
    ed.check(focus is not None, f"inspect_ui has no focus while #mail is focused: {sorted(r)}")
    ed.check_eq(focus["ref"], "#mail", "the focused field")
    ed.check_eq(focus["keyboard"], "email", "type=email: the e-mail keyboard")
    ed.check_eq(focus["enter_key"], "send", "enterkeyhint=send")
    ed.check_eq(focus["max_length"], 32, "maxlength")
    ed.check_eq(focus["autocapitalize"], "words", "autocapitalize")
    ed.check_eq(focus["secure"], False, "not a password")
    ed.check_eq(focus["keyboard_open"], True, "the keyboard is shown for it")

    # blurred: no focus
    await send(ed, {"type": "mouse", "action": "click", "x": P_BODY[0], "y": P_BODY[1]})
    r = await ed.call("inspect_ui", scene=S["scene"], entity=S["menu"], selector="#mail", depth=0)
    ed.check("focus" not in r, f"inspect_ui reports a focus with nothing focused: {r.get('focus')}")


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

    # ui-multitouch: lifting one finger releases only its own target
    await send(ed, {"type": "touch", "action": "end", "pointer": 0, "x": P_TOUCH_A[0], "y": P_TOUCH_A[1]}, frames_after=3)
    shot = await shoot(ed, "first-touch-ended")
    check_color(ed, shot, P_TOUCH_A, LIGHT, "first target after its finger lifted")
    check_color(ed, shot, P_TOUCH_B, MAGENTA, "second finger's target still :active")

    await send(ed, {"type": "touch", "action": "end", "pointer": 1, "x": P_TOUCH_B[0], "y": P_TOUCH_B[1]}, frames_after=3)
    shot = await shoot(ed, "touches-ended")
    check_color(ed, shot, P_TOUCH_A, LIGHT, "first target after the fingers lifted")
    check_color(ed, shot, P_TOUCH_B, LIGHT, "second target after the fingers lifted")
    ed.check_eq(await probe(ed, "touchClicks"), 2, "each finger clicked its own target")
    ed.check_eq(await probe(ed, "touchPointers"), "touchA:touch0 touchB:touch1 ",
                "the clicks carry each finger (pointerType touch, fingerId)")

    await send(ed, {"type": "touch", "action": "tap", "pointer": 2, "x": P_BODY[0], "y": P_BODY[1]})
    ed.check_eq(await probe(ed, "touches"), touches + 1, "a touch on the body did not reach the game")


async def test_joystick_survives_popup(ed: Editor):
    """S93 AC6 (U29): a finger the game got at its press (a joystick) stays the game's when a script
    shows a full-screen popup under it: its moves and its release over the popup's button reach the
    game's onTouchMove / onTouchEnd, and the button is not clicked."""
    moves = await probe(ed, "touchMoves")
    ends = await probe(ed, "touchEnds")
    try:
        await send(ed, {"type": "touch", "action": "start", "pointer": 3, "x": P_BODY[0], "y": P_BODY[1]})
        await ed.call("eval_lua", code="script('Popup', 'PopupProbe'):show()")
        await send(ed, {"type": "wait", "frames": 2},
                   {"type": "touch", "action": "move", "pointer": 3, "x": P_POPUP_OK[0], "y": P_POPUP_OK[1]},
                   {"type": "touch", "action": "end", "pointer": 3, "x": P_POPUP_OK[0], "y": P_POPUP_OK[1]})
        ed.check_eq(await probe(ed, "touchMoves"), moves + 1, "the game's finger moving over the popup reached onTouchMove")
        ed.check_eq(await probe(ed, "touchEnds"), ends + 1, "the game's finger lifted over the popup reached onTouchEnd")
        entries = await ed.console_since(None, limit=1000)
        lines = [e.get("message", "") for e in entries
                 if "InputProbe onTouchEnd" in e.get("message", "") and e.get("message", "").endswith("finger 3")]
        ed.check_eq(len(lines), 1, f"one onTouchEnd line of finger 3 in the Console: {lines}")
        ed.check_eq((await ed.call("eval_lua", code="return script('Popup', 'PopupProbe').clicks"))["values"][0], 0,
                    "the popup's button was clicked by a finger it never got")
        ed.check_eq((await ed.call("eval_lua", code="return Input.touchCount"))["values"][0], 0, "a finger stayed down in Input")
    finally:
        # the popup goes, so the next test sees the menu
        await ed.call("eval_lua", code="UIDocument(script('Popup', 'PopupProbe').scene, script('Popup', 'PopupProbe').entity):hide()")
        await send(ed, {"type": "wait", "frames": 2})


# -- S93 P2: hit testing through transforms, drag scrolling -----------------------------------------


P_DRAGLIST = (950, 120)
P_CLIPBTN = (90, 490)          # #clipbtn where it is drawn; its layout box is below #clipview's clip


async def test_hit_follows_transform_in_clip(ed: Editor):
    """S93 R6/R51 (AC7): inspect_ui's hit test finds a button translated into view inside an
    `overflow: hidden` box where it is drawn, not the box (6.3 tested its layout position)."""
    # (entity: the popup of test_joystick_survives_popup is a second document)
    r = await ed.call("inspect_ui", entity="Menu", at=list(P_CLIPBTN))
    ed.check_eq(r["hit"][0]["ref"], "#clipbtn", f"the translated button is hit where it is drawn: {r['hit']}")
    r = await ed.call("inspect_ui", entity="Menu", at=[P_CLIPBTN[0], 555])
    ed.check_eq(r["hit"][0]["ref"], "#clipview", f"the box under the button's layout area: {r['hit']}")


async def test_scroll_drag_pointer(ed: Editor):
    """S93 R8/R9 (AC12): a mouse drag on a list with `scroll-drag: pointer` scrolls it and clicks
    nothing; a still click on it clicks."""
    top = "return script('Menu', 'InputProbe').doc:getElementById('draglist').scrollTop"
    ed.check_eq((await ed.call("eval_lua", code=top))["values"][0], 0, "the list starts at the top")
    await send(ed, {"type": "mouse", "action": "drag", "x": P_DRAGLIST[0], "y": P_DRAGLIST[1],
                    "to": [P_DRAGLIST[0], P_DRAGLIST[1] - 70], "steps": 7}, frames_after=3)
    scrolled = (await ed.call("eval_lua", code=top))["values"][0]
    ed.check(scrolled >= 50, f"the mouse drag scrolled the list: scrollTop {scrolled}")
    ed.check_eq(await probe(ed, "dragClicks"), 0, "the panned press clicked nothing")
    await send(ed, {"type": "mouse", "action": "click", "x": P_DRAGLIST[0], "y": P_DRAGLIST[1]})
    ed.check_eq(await probe(ed, "dragClicks"), 1, "a still click on the list clicks")


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

