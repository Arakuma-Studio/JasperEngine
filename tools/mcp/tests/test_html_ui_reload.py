"""Hot reload of HTML UI documents (phase 1, p1-editor, spec §3.8.6), through MCP.

The editor's HtmlUIWatcher hands changed files to UIDocumentSystem::reloadChangedFiles every
0.25 s, in edit and play mode. write_file changes them here, like an external editor would:
- edit mode: a CSS colour change shows in the Game view and the Scene view within 2 s;
- play mode: a CSS change shows within 2 s and keeps the DOM (an element handle stored in a
  Lua global stays valid); an HTML change reloads the document keeping the input's value,
  the scroll position and the focus, and a listener on an element with an id still fires;
  onLoad runs once more.
The Lua checks need p1-lua's element API (UIDocument in Lua); without it they are skipped.

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import time

from harness import Editor, skip
from htmlui_pixels import Shot, near, count_near

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/reload.html"
CSS_PATH = "assets/ui/reload.css"

HTML = """<html>
<head>
    <title>reload</title>
    <link type="text/css" href="reload.css"/>
</head>
<body>
    <div id="box"></div>
    <input id="name" type="text" value=""/>
    <div id="list"><p>1</p><p>2</p><p>3</p><p>4</p><p>5</p><p>6</p></div>
    <div id="go">Go</div>
</body>
</html>
"""

CSS = """body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 16px; color: #ffffff; }
div, p, input { display: block; }
#box { position: absolute; left: 40px; top: 40px; width: 300px; height: 200px; background-color: #ff0000; }
#name { position: absolute; left: 400px; top: 40px; width: 200px; height: 30px; background-color: #333333; }
#list { position: absolute; left: 400px; top: 100px; width: 200px; height: 80px; overflow: auto; }
#list p { height: 40px; }
#go { position: absolute; left: 40px; top: 300px; width: 120px; height: 40px; background-color: #808080; }
"""

PROBE = """---@class ReloadProbe : ScriptInstance
local ReloadProbe = {
    properties = {
        { name = "loads", displayName = "Loads", type = "int", default = 0 },
        { name = "clicks", displayName = "Clicks", type = "int", default = 0 },
    }
}

function ReloadProbe:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    local probe = self
    self.doc:getElementById("go"):addEventListener("click", function(event) probe.clicks = probe.clicks + 1 end)
    _G.reloadBox = self.doc:getElementById("box")
end

function ReloadProbe:onDocumentLoad()
    self.loads = self.loads + 1
end

return ReloadProbe
"""

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
YELLOW = (255, 255, 0)
P_BOX = (190, 140)


async def game_shot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    (ed.scratch.root / f"html-ui-reload-{label}.png").write_bytes(shot.images[0][0])
    return Shot(shot.images[0][0], shot.value)


async def wait_for_color(ed: Editor, label: str, point, color, seconds: float = 2.0) -> Shot:
    """Game view screenshots until `point` shows `color`; fails after `seconds` and at least
    three screenshots (a screenshot itself takes a while on a loaded host)."""
    deadline = time.monotonic() + seconds
    attempts = 0
    while True:
        shot = await game_shot(ed, label)
        attempts += 1
        if near(shot.px(*point), color, 12):
            return shot
        if time.monotonic() > deadline and attempts >= 3:
            ed.fail(f"{label}: pixel at canvas {point} is {shot.px(*point)} after {seconds} s, expected {color}")
        await asyncio.sleep(0.1)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def has_element_api(ed: Editor) -> bool:
    values = (await ed.call("eval_lua", code="return UIDocument ~= nil",
                            allow_edit_mode=True))["values"]
    return values == [True]


async def test_setup(ed: Editor):
    await ed.console_since(None, limit=1)
    await ed.call("write_file", path=DOC, content=HTML)
    await ed.call("write_file", path=CSS_PATH, content=CSS)
    made = await ed.call("create_scene", name="HtmlUiReload", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiReload")
    await wait_for_color(ed, "setup", P_BOX, RED)


async def test_edit_mode_css_change(ed: Editor):
    await ed.call("write_file", path=CSS_PATH, content=CSS.replace("#ff0000", "#00ff00"))
    await wait_for_color(ed, "edit-css-game", P_BOX, GREEN)

    # the Scene view of the UI scene redraws with it (p1-render draws documents there)
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("set_scene_camera", scene=S["scene"], center=[640, 360], zoom=2.0)
    deadline = time.monotonic() + 2.0
    while True:
        shot = await ed.call_raw("screenshot", {"view": "scene", "scene": S["scene"], "max_width": 800})
        ed.check(not shot.is_error and shot.images, f"scene view: {shot.text[:300]}")
        png = shot.images[0][0]
        (ed.scratch.root / "html-ui-reload-edit-css-scene.png").write_bytes(png)
        greens, reds = count_near(png, GREEN), count_near(png, RED)
        if greens > 50 and reds < 10:
            break
        if time.monotonic() > deadline:
            ed.fail(f"the Scene view shows no restyled document: {greens} green, {reds} red samples")
        await asyncio.sleep(0.1)

    # a hot reload is not an edit: nothing to save
    info = await ed.call("list_scenes")
    for scene in info.get("scenes", []):
        if scene.get("id") == S["scene"]:
            ed.check(not scene.get("modified", False), f"scene marked modified by a reload: {scene}")


async def test_play_mode_css_change_keeps_dom(ed: Editor):
    S["playing_lua"] = await has_element_api(ed)
    if S["playing_lua"]:
        await ed.call("create_script", kind="lua", name="ReloadProbe", content=PROBE, scene=S["scene"], entity=S["menu"])
    await ed.call("play", action="start", scene=S["scene"])
    try:
        loads = (await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0] if S["playing_lua"] else None
        await ed.call("write_file", path=CSS_PATH, content=CSS.replace("#ff0000", "#0000ff"))
        await wait_for_color(ed, "play-css", P_BOX, BLUE)
        if S["playing_lua"]:
            ed.check_eq(await lua(ed, "return reloadBox:isValid()"), [True], "the element handle survived a stylesheet reload")
            ed.check_eq((await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0], loads, "a stylesheet reload is not a load")
    except BaseException:
        await ed.call("play", action="stop")
        raise


async def test_play_mode_html_change_keeps_state(ed: Editor):
    try:
        if not S.get("playing_lua"):
            # without the element API: the document still reloads (a new element shows)
            await ed.call("write_file", path=DOC, content=HTML.replace(
                '<div id="go">Go</div>', '<div id="go">Go</div>\n    <div id="added" style="position: absolute; left: 700px; top: 300px; '
                'width: 100px; height: 100px; background-color: #ffff00;"></div>'))
            await wait_for_color(ed, "play-html", (750, 350), YELLOW)
            skip("needs p1-lua's element API for the state and listener checks")

        await lua(ed, "local d = script('Menu', 'ReloadProbe').doc; d:getElementById('name').value = 'kept'; "
                      "d:getElementById('list').scrollTop = 40; d:getElementById('name'):focus()")
        loads_before = (await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0]

        await ed.call("write_file", path=DOC, content=HTML.replace(
            '<div id="go">Go</div>', '<div id="go">Go</div>\n    <div id="added" style="position: absolute; left: 700px; top: 300px; '
            'width: 100px; height: 100px; background-color: #ffff00;"></div>'))
        await wait_for_color(ed, "play-html", (750, 350), YELLOW)

        values = await lua(ed, "local d = script('Menu', 'ReloadProbe').doc; local n = d:getElementById('name'); "
                               "return n.value, d:getElementById('list').scrollTop, n:matches(':focus')")
        ed.check_eq(values[0], "kept", "the input's value survived the reload")
        ed.check(abs(values[1] - 40) < 1, f"scrollTop after the reload: {values[1]}")
        ed.check_eq(values[2], True, "the input kept the focus")
        ed.check_eq((await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0], loads_before + 1, "onLoad ran again")

        # the listener added in init on #go moved to the new #go
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 100, "y": 320}], space="canvas", frames_after=2)
        ed.check_eq(await lua(ed, "return script('Menu', 'ReloadProbe').clicks"), [1], "the listener on #go fires after the reload")
    finally:
        await ed.call("play", action="stop")
        await ed.call("write_file", path=DOC, content=HTML)


async def test_no_errors(ed: Editor):
    entries = await ed.console_since(None, types=["error"], limit=1000)
    errors = [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "") or "Lua" in e.get("message", "")]
    ed.check(not errors, f"errors: {errors}")
