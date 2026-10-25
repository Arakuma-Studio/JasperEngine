"""Critical HTML UI workflow through the running editor's MCP interface.

A screen document, a component button and a texture document on a 3D wall route input to
Lua scripts before the game. Editing CSS and HTML preserves model values, typed text and
listeners. Screenshots retain artifacts of the running composition and its reload.

Renderer, animation and Lua API details are covered at their direct public seams.
"""

import time

from harness import Editor, REPO
from htmlui_pixels import Shot, near, check_color

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

TIMEOUT = 900

DOC = "assets/ui/p1.html"
CSS = "assets/ui/p1.css"
WORLD = "assets/ui/world.html"

HTML = """<html>
<head>
    <title>phase 1</title>
    <link type="text/css" href="p1.css"/>
</head>
<body>
    <div id="clip"><div id="clipfill"></div></div>
    <div id="hud" data-model="hud">Gold: <span id="gold">{{ gold }}</span></div>
    <div id="buy" data-model="hud" data-event-click="buy()">Buy</div>
    <input id="name" type="text" value=""/>
</body>
</html>
"""

STYLE = """body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 24px; color: #ffffff; }
div, input { display: block; position: absolute; }
#clip { left: 20px; top: 20px; width: 200px; height: 120px; overflow: hidden; border-radius: 40px; }
#clipfill { left: -50px; top: -50px; width: 300px; height: 220px; background-color: #ff8000; }
#hud { left: 20px; top: 350px; }
#buy { left: 20px; top: 400px; width: 140px; height: 50px; background-color: #808080; font-effect: outline(2px #000000); }
#buy.pressed { background-color: #00ff00; }
#name { left: 20px; top: 470px; width: 260px; height: 36px; background-color: #303030; color: #ffffff; }
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
    doc.onLoad = function() self.loads = self.loads + 1 end

    -- a component button of this HUD over the middle of the 3D scene's wall (owned by this
    -- Lua handle: collected after the play stop deleted the HUD's copy, see TestEntityHandleLifetime.cpp::LuaHandlesCollectedAfterTheirSceneWasDeleted)
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

P_CLIP = (120, 80)
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


async def test_screen_component_world_routing_and_reload(ed: Editor):
    """A running project routes clicks across UI layers and preserves script/form state on reload."""
    try:
        await ed.console_since(None, limit=1)
        await ed.call("write_file", path=DOC, content=HTML)
        await ed.call("write_file", path=CSS, content=STYLE)
        await ed.call("write_file", path=WORLD, content=WORLD_HTML)

        made = await ed.call("create_scene", name="Phase1Hud", type="UI", path="scenes/Phase1Hud")
        hud_scene = made["scene"]["id"]
        ent = await ed.call("create_entity", scene=hud_scene, type="ui_document", name="Hud")
        await ed.call("set_properties", scene=hud_scene, entity=ent["entity_id"], values={"uidocument": {"filename": DOC}})
        await ed.call("create_script", kind="lua", name="HudGame", content=HUD_GAME, scene=hud_scene, entity=ent["entity_id"])
        await ed.call("save_scene", scene=hud_scene)

        made = await ed.call("create_scene", name="Phase1", type="3D", path="scenes/Phase1")
        scene = made["scene"]["id"]
        ent = await ed.call("create_entity", scene=scene, type="ui_document", name="Terminal")
        await ed.call("set_properties", scene=scene, entity=ent["entity_id"], values={"uidocument": {"filename": WORLD}})
        await ed.call("create_script", kind="lua", name="WorldGame", content=WORLD_GAME, scene=scene, entity=ent["entity_id"])
        await ed.call("set_child_scene", scene=scene, child=hud_scene, start_active=True)
        await ed.call("save_scene", scene=scene)
        await ed.call("edit_build_scenes", action="add", scene="scenes/Phase1Hud.scene")
        await ed.call("edit_build_scenes", action="set_start", scene=scene)
        await ed.call("save_project")

        ed.check(await ed.call("play", action="start", scene=scene), "play started")
        await ed.call("wait", frames=5)
        ed.check(await world(ed, "worldBound"), "the wall's document reloaded in TEXTURE mode and bound its texture and listener")

        await send(ed, {"type": "mouse", "action": "click", "x": P_BUY[0], "y": P_BUY[1]}, frames_after=3)
        ed.check_eq(await hud(ed, "clicks"), 1, "the Lua listener on the HUD document's button")
        ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('gold').textContent"))[0], "11",
                    "the data-event buy() through the model")
        shot = await shoot(ed, "clicked")
        check_color(ed, shot, (150, 405), (0, 255, 0), "the button's class set by the listener")
        r, g, b = shot.px(*P_WORLD)
        ed.check(r > 80 and r > g + 40 and r > b + 40,
                 f"the wall shows its document's red target: {(r, g, b)}")

        await send(ed, {"type": "mouse", "action": "click", "x": P_COMPONENT[0], "y": P_COMPONENT[1]})
        ed.check_eq(await hud(ed, "presses"), 1, "the component button over the wall")
        ed.check_eq(await world(ed, "worldClicks"), 0, "the wall's document under the component button got the click")

        await send(ed, {"type": "mouse", "action": "click", "x": P_WORLD[0], "y": P_WORLD[1]})
        ed.check_eq(await world(ed, "worldClicks"), 1, "the wall's document")
        ed.check_eq(await world(ed, "downs"), 0, "a click taken by UI reached the game")

        await send(ed, {"type": "mouse", "action": "click", "x": P_WORLD_BODY[0], "y": P_WORLD_BODY[1]})
        ed.check_eq(await world(ed, "downs"), 1, "a click on the wall document's transparent body did not reach the game")

        # A click that misses the wall reaches the game.
        await send(ed, {"type": "mouse", "action": "click", "x": 1200, "y": 650})
        ed.check_eq(await world(ed, "downs"), 2, "a click beside the wall reaches the game")
        ed.check_eq(await world(ed, "worldClicks"), 1, "a click beside the wall misses the document")
        await send(ed, {"type": "mouse", "action": "click", "x": P_NAME[0], "y": P_NAME[1]},
                   {"type": "text", "text": "héllo ✓"})
        ed.check_eq((await lua(ed, f"return {DOC_LUA}:getElementById('name').value"))[0], "héllo ✓", "typed text")
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

        await ed.call("write_file", path=DOC, content=HTML.replace('</body>', '<div id="extra">new</div></body>'))
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

    finally:
        await ed.call("play", action="stop")
    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")
