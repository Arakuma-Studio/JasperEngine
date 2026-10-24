"""HTML UI phase 1 as a whole (p1-integrate), in the real editor.

A 3D scene with a UI child scene (the HUD) plays every phase-1 feature at once:
- a screen document in the HUD: a rounded overflow clip, a grayscale filter, a gradient, an inline SVG,
  a Lottie, a backdrop-filtered panel, a data model with a data-event, a text field, a button
  with a Lua click listener and an outlined label, an element moved by `anime`;
- a TEXTURE-mode document of the 3D scene shown on a wall (its target texture on the wall's
  material), with a Lua click listener;
- a component UI Button of the HUD drawn over the wall.
Lua scripts (HudGame, WorldGame) bind the documents and count what reaches whom. Checks: probe
colours in the Game view, input routing (screen document, component UI, document on the
wall, game), text input, anime and Lottie on the UI clock, hot reload of the playing document
(CSS and HTML), and play stop releasing every Lua reference with no [HTML UI] error.

The scene is saved as the start scene; with JASPER_KEEP_SCRATCH=1 the scratch project is the
phase-1 project p1-integrate exports for the Linux smoke test and the Web check.

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import time

from harness import Editor, REPO
from htmlui_pixels import Shot, near, check_color

CI = True
EDITOR = {"create": True, "name": "Html Ui Integration CI"}

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/p1.html"
CSS = "assets/ui/p1.css"
WORLD = "assets/ui/world.html"
LOTTIE = "assets/ui/anim/shapes.json"

HTML = """<html>
<head>
    <title>phase 1</title>
    <link type="text/css" href="p1.css"/>
</head>
<body>
    <div id="clip"><div id="clipfill"></div></div>
    <div id="gray"></div>
    <div id="grad"></div>
    <svg id="icon" width="64" height="64" viewBox="0 0 64 64"><rect x="0" y="0" width="64" height="64" fill="#00ff00"/></svg>
    <lottie id="spin" src="anim/shapes.json" autoplay="autoplay" loop="loop"/>
    <div id="hud" data-model="hud">Gold: <span id="gold">{{ gold }}</span></div>
    <div id="buy" data-model="hud" data-event-click="buy()">Buy</div>
    <input id="name" type="text" value=""/>
    <div id="box"></div>
    <div id="glass">Glass</div>
</body>
</html>
"""

STYLE = """body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 24px; color: #ffffff; }
div, input { display: block; position: absolute; }
#clip { left: 20px; top: 20px; width: 200px; height: 120px; overflow: hidden; border-radius: 40px; }
#clipfill { left: -50px; top: -50px; width: 300px; height: 220px; background-color: #ff8000; }
#gray { left: 20px; top: 160px; width: 120px; height: 60px; background-color: #ff0000; filter: grayscale(1); }
#grad { left: 160px; top: 160px; width: 140px; height: 60px; decorator: linear-gradient(90deg, #ff0000, #0000ff); }
#icon { position: absolute; left: 20px; top: 240px; width: 64px; height: 64px; }
#spin { position: absolute; left: 100px; top: 240px; width: 100px; height: 100px; }
#hud { left: 20px; top: 350px; }
#buy { left: 20px; top: 400px; width: 140px; height: 50px; background-color: #808080; font-effect: outline(2px #000000); }
#buy.pressed { background-color: #00ff00; }
#name { left: 20px; top: 470px; width: 260px; height: 36px; background-color: #303030; color: #ffffff; }
#box { left: 20px; top: 530px; width: 40px; height: 40px; background-color: #ffff00; }
#glass { left: 20px; top: 600px; width: 260px; height: 60px; backdrop-filter: blur(6px); background-color: #ffffff40; }
"""

WORLD_HTML = """<html>
<head>
    <title>world</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        #target { display: block; position: absolute; left: 30%; top: 30%; width: 40%; height: 40%;
                  background-color: #ff0000; border-radius: 16px; }
    </style>
</head>
<body>
    <div id="target"></div>
</body>
</html>
"""

HUD_GAME = """---@class HudGame : ScriptInstance
local HudGame = {
    properties = {
        { name = "clicks", type = "int", default = 0 },
        { name = "presses", type = "int", default = 0 },
        { name = "loops", type = "int", default = 0 },
        { name = "loads", type = "int", default = 0 },
    }
}

function HudGame:init()
    -- the data model first (the document reloads to bind it), then the listeners: on elements
    -- with an id they move to the new elements on every state-preserving reload
    local doc = UIDocument(self.scene, self.entity)
    self.model = doc:createDataModel("hud", { gold = 10, buy = function(event) self.model.gold = self.model.gold + 1 end })
    doc:getElementById("buy"):addEventListener("click", function(event)
        self.clicks = self.clicks + 1
        event.currentTarget.classList:add("pressed")
    end)
    doc:getElementById("spin"):addEventListener("loop", function(event) self.loops = self.loops + 1 end)
    doc.onLoad = function() self.loads = self.loads + 1 end
    anime.animate("#box", { translateX = 200, duration = 1000, ease = "linear" })

    -- a component button of this HUD over the middle of the 3D scene's wall (owned by this
    -- Lua handle: collected after the play stop deleted the HUD's copy, see test_lua_gc_lifetime)
    self.button = Button(self.scene)
    self.button:setPosition(600, 340)
    self.button:setSize(80, 40)
    self.button:getButtonComponent().onPress = function() self.presses = self.presses + 1 end
end

return HudGame
"""

WORLD_GAME = """---@class WorldGame : ScriptInstance
local WorldGame = {
    properties = {
        { name = "downs", type = "int", default = 0 },
        { name = "worldClicks", type = "int", default = 0 },
        { name = "worldBound", type = "bool", default = false },
    }
}

function WorldGame:init()
    RegisterEngineEvent(self, "onMouseDown")
    self.camera = Camera(self.scene)
    self.camera:setPosition(0, 0, 5)
    self.camera:setTarget(0, 0, 0)
    self.camera:activate()
    self.wall = Primitive(self.scene)
    self.wall:createWall(4, 4)

    -- a document rendered into a texture shown on the wall, with pointer input through it
    local doc = UIDocument(self.scene, self.entity)
    local component = doc:getUIDocumentComponent()
    component.renderMode = UIDocumentRenderMode.TEXTURE
    component.textureWidth = 256
    component.textureHeight = 256
    component.inputSurface = self.wall.entity
    component.needReload = true
    doc.onLoad = function() self:bind() end
end

function WorldGame:bind()
    local doc = UIDocument(self.scene, self.entity)
    if doc:getUIDocumentComponent().renderMode ~= UIDocumentRenderMode.TEXTURE then return end
    self.wall:setTexture(doc:getTargetTexture())
    doc:getElementById("target"):addEventListener("click", function(event) self.worldClicks = self.worldClicks + 1 end)
    self.worldBound = true
end

function WorldGame:onMouseDown(button, x, y, mods) self.downs = self.downs + 1 end

return WorldGame
"""

P_CLIP_CORNER = (24, 24)        # outside the 40 px radius
P_CLIP = (120, 80)
P_GRAY = (80, 190)
P_GRAD_LEFT = (164, 190)
P_GRAD_RIGHT = (296, 190)
P_SVG = (52, 272)
P_BUY = (90, 425)
P_NAME = (40, 488)
P_COMPONENT = (640, 360)        # the component button over the wall's middle
P_WORLD = (560, 300)            # the wall document's #target, beside the component button
P_WORLD_BODY = (380, 360)       # the wall document's transparent body (uv 0.1, 0.5)


async def shoot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-integration-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def send(ed: Editor, *events, frames_after=2):
    r = await ed.call_raw("send_input", {"events": list(events), "space": "canvas", "frames_after": frames_after})
    ed.check(not r.is_error, f"send_input {events}: {r.text[:300]}")
    return r


async def hud(ed: Editor, field: str):
    return (await ed.call("eval_lua", code=f"return script('Hud', 'HudGame', 'Phase1Hud').{field}"))["values"][0]


async def world(ed: Editor, field: str):
    return (await ed.call("eval_lua", code=f"return script('Terminal', 'WorldGame', 'Phase1').{field}"))["values"][0]


# the HUD's document from eval_lua
DOC_LUA = "UIDocument(script('Hud', 'HudGame', 'Phase1Hud').scene, script('Hud', 'HudGame', 'Phase1Hud').entity)"


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def html_ui_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


async def test_setup_and_play(ed: Editor):
    """A 3D scene (the wall and its document) with a UI child scene (the HUD document and a
    component button), saved, the 3D scene as the start scene, playing."""
    await ed.console_since(None, limit=1)
    await ed.call("write_file", path=DOC, content=HTML)
    await ed.call("write_file", path=CSS, content=STYLE)
    await ed.call("write_file", path=WORLD, content=WORLD_HTML)
    lottie = (REPO / "engine/tests/htmlui/fixtures/p1/lottie/shapes.json").read_text()
    await ed.call("write_file", path=LOTTIE, content=lottie)

    made = await ed.call("create_scene", name="Phase1Hud", type="UI", path="scenes/Phase1Hud")
    S["hud"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["hud"], type="ui_document", name="Hud")
    await ed.call("set_properties", scene=S["hud"], entity=ent["entity_id"], values={"uidocument": {"filename": DOC}})
    await ed.call("create_script", kind="lua", name="HudGame", content=HUD_GAME, scene=S["hud"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["hud"])

    made = await ed.call("create_scene", name="Phase1", type="3D", path="scenes/Phase1")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Terminal")
    await ed.call("set_properties", scene=S["scene"], entity=ent["entity_id"], values={"uidocument": {"filename": WORLD}})
    await ed.call("create_script", kind="lua", name="WorldGame", content=WORLD_GAME, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("set_child_scene", scene=S["scene"], child=S["hud"], start_active=True)
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("edit_build_scenes", action="add", scene="scenes/Phase1Hud.scene")
    await ed.call("edit_build_scenes", action="set_start", scene=S["scene"])
    await ed.call("save_project")

    ed.check(await ed.call("play", action="start", scene=S["scene"]), "play started")
    await ed.call("wait", frames=5)
    ed.check(await world(ed, "worldBound"), "the wall's document reloaded in TEXTURE mode and bound its texture and listener")


async def test_features_draw(ed: Editor):
    """Probe colours of the rounded clip, filter, gradient, SVG, Lottie, the wall's document
    and the component button."""
    shot = await shoot(ed, "play")
    r, g, b = shot.px(*P_CLIP_CORNER)
    ed.check(not near((r, g, b), (255, 128, 0), 40), f"the rounded clip does not cut its corner: {(r, g, b)}")
    check_color(ed, shot, P_CLIP, (255, 128, 0), "inside the rounded clip")
    check_color(ed, shot, P_GRAY, (54, 54, 54), "grayscale(1) of red", tol=8)
    r, g, b = shot.px(*P_GRAD_LEFT)
    ed.check(r > 200 and b < 60, f"the gradient's left end is not red: {(r, g, b)}")
    r, g, b = shot.px(*P_GRAD_RIGHT)
    ed.check(b > 200 and r < 60, f"the gradient's right end is not blue: {(r, g, b)}")
    check_color(ed, shot, P_SVG, (0, 255, 0), "the inline SVG")
    # the wall is lit, so its red is darker than the document's
    r, g, b = shot.px(*P_WORLD)
    ed.check(r > 90 and r > g + 60 and r > b + 60, f"the wall does not show the document's red #target: {(r, g, b)}")
    r, g, b = shot.px(*P_COMPONENT)
    ed.check(r > 200 and g > 200 and b > 200, f"the HUD's component button is not drawn over the wall: {(r, g, b)}")
    reds = sum(1 for p in shot.region(100, 240, 200, 340) if p[0] > 180 and p[1] < 90 and p[2] < 90)
    ed.check(reds > 50, f"the Lottie draws nothing: {reds} red pixels")


async def test_input_routing(ed: Editor):
    """HUD document, then the HUD's component UI, then the document on the wall, then the game."""
    await send(ed, {"type": "mouse", "action": "click", "x": P_BUY[0], "y": P_BUY[1]}, frames_after=3)
    ed.check_eq(await hud(ed, "clicks"), 1, "the Lua listener on the HUD document's button")
    ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('gold').textContent"))[0], "11",
                "the data-event buy() through the model")
    shot = await shoot(ed, "clicked")
    check_color(ed, shot, (150, 405), (0, 255, 0), "the button's class set by the listener")

    await send(ed, {"type": "mouse", "action": "click", "x": P_COMPONENT[0], "y": P_COMPONENT[1]})
    ed.check_eq(await hud(ed, "presses"), 1, "the component button over the wall")
    ed.check_eq(await world(ed, "worldClicks"), 0, "the wall's document under the component button got the click")

    await send(ed, {"type": "mouse", "action": "click", "x": P_WORLD[0], "y": P_WORLD[1]})
    ed.check_eq(await world(ed, "worldClicks"), 1, "the wall's document")
    ed.check_eq(await world(ed, "downs"), 0, "a click taken by UI reached the game")

    await send(ed, {"type": "mouse", "action": "click", "x": P_WORLD_BODY[0], "y": P_WORLD_BODY[1]})
    ed.check_eq(await world(ed, "downs"), 1, "a click on the wall document's transparent body did not reach the game")


async def test_text_anime_and_lottie(ed: Editor):
    await send(ed, {"type": "mouse", "action": "click", "x": P_NAME[0], "y": P_NAME[1]},
               {"type": "text", "text": "héllo ✓"})
    ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('name').value"))[0], "héllo ✓", "typed text")
    # 3 s of play: the 1 s animation is done, the 2 s Lottie wrapped
    await ed.call("wait", game_seconds=3)
    left = (await lua(ed, f"return {DOC_LUA}:getElementById('box'):getBoundingClientRect().left"))[0]
    ed.check(abs(left - 220) < 2, f"anime moved #box by 200 px: left = {left}")
    ed.check(await hud(ed, "loops") >= 1, "the Lottie's loop listener")


async def test_hot_reload_while_playing(ed: Editor):
    """A CSS change restyles in place; an HTML change reloads, keeping the model, the field
    and the listeners of elements with an id (onLoad runs once more)."""
    await ed.call("write_file", path=CSS, content=STYLE.replace("#ff8000", "#0080ff"))
    deadline = time.monotonic() + 4
    got = None
    while time.monotonic() < deadline:
        await ed.call("wait", frames=2)
        got = (await shoot(ed, "css")).px(*P_CLIP)
        if near(got, (0, 128, 255), 12):
            break
    ed.check(near(got, (0, 128, 255), 12), f"the CSS change did not show: {got}")
    loads = await hud(ed, "loads")
    clicks = await hud(ed, "clicks")

    await ed.call("write_file", path=DOC, content=HTML.replace('<div id="glass">', '<div id="extra">new</div>\n    <div id="glass">'))
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline and await hud(ed, "loads") == loads:
        await ed.call("wait", frames=2)
    ed.check_eq(await hud(ed, "loads"), loads + 1, "onLoad after the HTML change")
    ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('extra') ~= nil"))[0], True, "the new element")
    ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('gold').textContent"))[0], "11", "the model kept its value")
    ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('name').value"))[0], "héllo ✓", "the field kept its value")
    await send(ed, {"type": "mouse", "action": "click", "x": P_BUY[0], "y": P_BUY[1]}, frames_after=3)
    ed.check_eq(await hud(ed, "clicks"), clicks + 1, "the button's listener, moved by the reload (once)")
    ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('gold').textContent"))[0], "12",
                "the model's data-event after the reload")
    shot = await shoot(ed, "reloaded")
    check_color(ed, shot, P_CLIP, (0, 128, 255), "the reloaded document keeps the edited stylesheet")


async def test_stop_releases_everything(ed: Editor):
    held = await lua(ed, "return UIDocument.getLiveLuaRefCount()")
    ed.check(held[0] >= 4, f"listeners and model callbacks hold Lua references: {held}")
    await ed.call("play", action="stop")
    deadline = time.monotonic() + 30
    left = None
    while time.monotonic() < deadline:
        state = await ed.call("get_play_state")
        left = (await ed.call("eval_lua", code="return UIDocument.getLiveLuaRefCount()", allow_edit_mode=True))["values"]
        if left == [0] and not state.get("playing", False):
            break
        await asyncio.sleep(0.5)
    ed.check_eq(left, [0], "Lua references left after stop")
    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")
