"""The Lua element API of HTML/CSS documents (phase 1 §3.9), end to end through MCP.

A Lua script on the document's GameObject creates a data model and listeners in init; in play
mode eval_lua drives the element API (queries, text, classList, style, mutation, the detached
pool), send_input clicks a button (a Lua listener counts it) and a data-for row
(data-event-click), the data-bound list grows on table.insert (screenshot probe +
childElementCount), and after stop no Lua reference is left (UIDocument.getLiveLuaRefCount).

Sections owned by other phase-1 packages are appended below (p1-vector: Lottie control and
its events; p1-anim: anime.animate / timelines).

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import sys
import time
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_html_ui_basic import Shot, near  # noqa: E402  (PNG probes shared with the phase-0 test)

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/lua.html"

# RML/RCSS (phase 1): no user-agent stylesheet, display: block is explicit
HTML = """<html>
<head>
    <title>lua</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
               font-family: sans-serif; font-size: 16px; color: #ffffff; }
        div { display: block; }
        #button { position: absolute; left: 40px; top: 40px; width: 160px; height: 48px; background-color: #808080; }
        #button.hot { background-color: #ff0000; }
        #gold { position: absolute; left: 400px; top: 40px; width: 100px; height: 30px; }
        #list { position: absolute; left: 40px; top: 120px; width: 300px; }
        .row { height: 30px; margin-bottom: 10px; background-color: #0000ff; }
    </style>
</head>
<body>
    <div id="button">Click</div>
    <div id="shop" data-model="shop">
        <div id="gold">{{ gold }}</div>
        <div id="list"><div class="row" data-for="item, i : items" data-event-click="buy(i)">{{ item.name }}</div></div>
    </div>
</body>
</html>
"""

SCRIPT = """---@class MenuScript : ScriptInstance
local MenuScript = { properties = {} }

function MenuScript:init()
    local doc = UIDocument(self.scene, self.entity)
    -- data models first: the document reloads to bind them
    shop = doc:createDataModel("shop", {
        gold = 10,
        items = { {name = "Sword"}, {name = "Shield"} },
        buy = function(event, index) bought = index end,
    })
    clicks = 0
    doc:getElementById("button"):addEventListener("click", function(event)
        clicks = clicks + 1
        lastButton = event.button
        lastType = event.type
    end)
end

return MenuScript
"""

BLUE = (0, 0, 255)
P_BUTTON = (120, 64)
P_ROW2 = (100, 175)
P_ROW3 = (100, 215)


async def shoot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-lua-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def lua(ed: Editor, code: str, **args) -> list:
    r = await ed.call("eval_lua", code=code, **args)
    return r["values"]


async def html_ui_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries
            if "[HTML UI]" in e.get("message", "") or "Lua" in e.get("message", "")]


async def test_setup(ed: Editor):
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiLua", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["menu"], component="UIDocument",
                  values={"uidocument": {"filename": DOC}})
    await ed.call("create_script", kind="lua", name="MenuScript", content=SCRIPT, scene=S["scene"], entity=S["menu"])
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiLua")
    started = await ed.call("play", action="start", scene=S["scene"])
    ed.check(started, "play started")
    await ed.call("wait", frames=3)


async def test_element_api(ed: Editor):
    values = await lua(ed, """
        local doc = object('Menu', 'UIDocument')
        local b = doc:getElementById('button')
        b.classList:add('hot')
        b.style.width = '170px'
        -- data-for keeps its element in the DOM, hidden, as the template of the rows
        return b.id, b.tagName, b.textContent, b.classList:contains('hot'), b.style.width,
               doc:getElementById('list').childElementCount - 1, #doc:querySelectorAll('.row') - 1,
               doc:querySelector('#gold').textContent, doc:getElementById('nope') == nil, doc.title
    """)
    ed.check_eq(values, ["button", "div", "Click", True, "170px", 2, 2, "10", True, "lua"], "element API")

    values = await lua(ed, """
        local doc = object('Menu', 'UIDocument')
        local made = doc:createElement('div')
        made.id = 'made'
        made.textContent = 'made by Lua'
        doc.body:appendChild(made)
        local found = doc:getElementById('made')
        local removed = doc.body:removeChild(found)
        local again = doc.body:appendChild(removed)
        local r = doc:getElementById('button'):getBoundingClientRect()
        return found == made, removed:isValid(), again.textContent, r.left, r.top, r.width
    """)
    ed.check_eq(values, [True, True, "made by Lua", 40, 40, 170], "mutation and geometry")

    shot = await shoot(ed, "hot-button")
    got = shot.px(*P_BUTTON)
    ed.check(near(got, (255, 0, 0), 12), f"classList.add('hot') shows: button pixel {got}")


async def test_click_listener(ed: Editor):
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_BUTTON[0], "y": P_BUTTON[1]}],
                  space="canvas", frames_after=2)
    ed.check_eq(await lua(ed, "return clicks, lastButton, lastType"), [1, 0, "click"], "the Lua click listener")


async def test_data_bound_list_grows(ed: Editor):
    shot = await shoot(ed, "list-2")
    ed.check(not near(shot.px(*P_ROW3), BLUE, 30), f"no third row yet: {shot.px(*P_ROW3)}")

    await lua(ed, "table.insert(shop.items, {name = 'Bow'}); shop.gold = 25")
    await ed.call("wait", frames=3)
    values = await lua(ed, """
        local doc = object('Menu', 'UIDocument')
        return doc:getElementById('list').childElementCount - 1, doc:getElementById('gold').textContent,
               #shop.items, shop.items[3].name
    """)
    ed.check_eq(values, [3, "25", 3, "Bow"], "the list grew")
    shot = await shoot(ed, "list-3")
    ed.check(near(shot.px(*P_ROW3), BLUE, 12), f"the third row on screen: {shot.px(*P_ROW3)}")

    # data-event-click="buy(i)": the row's 0-based index
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_ROW2[0], "y": P_ROW2[1]}],
                  space="canvas", frames_after=2)
    ed.check_eq(await lua(ed, "return bought"), [1], "data-event-click on the second row")

    # errors are Lua errors with a clear message
    r = await ed.call_raw("eval_lua", {"code": "shop.silver = 1"})
    ed.check("has no variable 'silver'" in r.text, f"a new top-level key: {r.text[:300]}")


async def test_lua_refs_released_after_stop(ed: Editor):
    held = await lua(ed, "return UIDocument.getLiveLuaRefCount()")
    ed.check(held[0] >= 2, f"listener and data-model callbacks hold Lua references: {held}")
    await ed.call("play", action="stop")
    # the stop finishes asynchronously (the runtime scene and its documents are destroyed last)
    deadline = time.monotonic() + 30
    left = None
    while time.monotonic() < deadline:
        state = await ed.call("get_play_state")
        left = await lua(ed, "return UIDocument.getLiveLuaRefCount()", allow_edit_mode=True)
        if left == [0] and not state.get("playing", False):
            break
        await asyncio.sleep(0.5)
    ed.check_eq(left, [0], "Lua references left after stop")

    errors = [e for e in await html_ui_errors(ed) if "silver" not in e]
    ed.check(not errors, f"errors: {errors}")
