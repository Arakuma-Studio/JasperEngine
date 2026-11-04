"""Input actions while a UI has focus (input spec P7, acceptance 6): a focused HTML text field keeps the
keyboard from driving actions (typing "d" does not move Horizontal), a document with gamepad focus
keeps the left stick and A (the d-pad too) to itself; raw queries still read the device, and the
actions work again once the focus goes.
"""

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900

DOC = "assets/ui/input_focus.html"

HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Focus</title>
  <style>
    html, body { margin: 0; background-color: #1e1e1e; font-family: sans-serif; color: #ffffff; }
    .menu { display: flex; flex-direction: column; gap: 10px; width: 300px; padding: 20px; }
    input, button { height: 40px; font: inherit; color: #ffffff; background-color: #404040; border: 0; }
  </style>
</head>
<body>
  <div class="menu">
    <input id="name" type="text" value="">
    <button id="ok">OK</button>
  </div>
</body>
</html>
"""


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def send(ed: Editor, *events, frames_after=2):
    r = await ed.call_raw("send_input", {"events": list(events), "frames_after": frames_after})
    ed.check(not r.is_error, f"send_input {events}: {r.text[:300]}")


async def horizontal(ed: Editor) -> float:
    return (await lua(ed, "return Input.getAxis('Horizontal')"))[0]


async def test_focused_ui_keeps_its_controls(ed: Editor):
    made = await ed.call("create_scene", name="InputFocus", type="UI")
    scene = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=scene, type="ui_document", name="Focus")
    await ed.call("set_properties", scene=scene, entity=ent["entity_id"], values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=scene, path="scenes/InputFocus")
    await ed.call("wait", frames=10)

    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=10)
        doc = "object('Focus', 'UIDocument')"

        # a focused text field: typing d does not move Horizontal, the key itself is down
        await lua(ed, f"{doc}:getElementById('name'):focus()")
        # a key press and the character it types, as a keyboard sends them
        await send(ed, {"type": "key", "key": "d", "action": "down"}, {"type": "text", "text": "d"}, frames_after=25)
        ed.check_eq(await horizontal(ed), 0, "typing d into the field leaves Horizontal at rest")
        ed.check_eq(await lua(ed, "return Input.getKey('d')"), [True], "getKey still reads the keyboard")
        await send(ed, {"type": "key", "key": "d", "action": "up"}, frames_after=2)
        typed = (await lua(ed, f"return {doc}:getElementById('name').value"))[0]
        ed.check_in("d", typed, "the field got the key")

        # no focus: the same key drives the action
        await lua(ed, f"{doc}:getElementById('name'):blur()")
        await send(ed, {"type": "key", "key": "d", "action": "down"}, frames_after=25)
        ed.check(await horizontal(ed) > 0.99, "without focus d moves Horizontal")
        await send(ed, {"type": "key", "key": "d", "action": "up"}, frames_after=40)
        ed.check_eq(await horizontal(ed), 0, "back to rest")

        # a focused button (gamepad navigation): the stick and A are the menu's
        await lua(ed, f"{doc}:getElementById('ok'):focus()")
        await send(ed, {"type": "gamepad", "axis": "left_x", "value": 1.0},
                   {"type": "gamepad", "button": "a", "action": "down"}, frames_after=3)
        ed.check_eq(await lua(ed, "return Engine.wantsGamepadNavigation()"), [True], "the document has gamepad focus")
        ed.check_eq(await horizontal(ed), 0, "the stick navigates, it does not move Horizontal")
        ed.check_eq(await lua(ed, "return Input.getButton('Submit'), Input.getButton('Fire1')"), [False, False],
                    "A is the menu's")
        ed.check_eq(await lua(ed, "return Input.isGamepadButtonPressed(0, Input.GAMEPAD_BUTTON_A)"), [True], "raw A")
        await send(ed, {"type": "gamepad", "button": "a", "action": "up"},
                   {"type": "gamepad", "axis": "left_x", "value": 0.0}, frames_after=2)

        await lua(ed, f"{doc}:querySelector(':focus'):blur()")
        await send(ed, {"type": "gamepad", "axis": "left_x", "value": 1.0}, frames_after=2)
        ed.check_eq(await horizontal(ed), 1, "without focus the stick moves Horizontal")
        await send(ed, {"type": "release_all"})
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
