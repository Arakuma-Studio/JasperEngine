"""The Lua element API of HTML/CSS documents (phase 1 §3.9), end to end through MCP.

A Lua script on the document's GameObject creates a data model and listeners in init; in play
mode eval_lua drives the element API (queries, text, classList, style, mutation, the detached
pool), send_input clicks a button (a Lua listener counts it) and a data-for row
(data-event-click), the data-bound list grows on table.insert (screenshot probe +
childElementCount), and after stop no Lua reference is left (UIDocument.getLiveLuaRefCount).

Sections owned by other phase-1 packages are appended below (p1-anim: anime.animate /
timelines; p1-vector: Lottie control and its events).

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


# ================================================================================================
# p1-anim section: the anime table (docs/html-ui/phase-1-integration.md §3.11)
# ================================================================================================

ANIME_DOC = "assets/ui/anime.html"
ANIME_HTML = """<html>
<head>
    <title>anime</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        div { display: block; }
        #box { position: absolute; left: 40px; top: 40px; width: 100px; height: 100px; background-color: #ff0000; }
        .dot { position: absolute; top: 300px; width: 40px; height: 40px; background-color: #0000ff; }
        #d1 { left: 40px; } #d2 { left: 140px; } #d3 { left: 240px; }
    </style>
</head>
<body>
    <div id="box"></div>
    <div id="d1" class="dot"></div>
    <div id="d2" class="dot"></div>
    <div id="d3" class="dot"></div>
</body>
</html>
"""


async def anime_shoot(ed: Editor, label: str, scene=None) -> Shot:
    args = {"view": "game", "max_width": 1280}
    if scene is not None:
        args["scene"] = scene
    shot = await ed.call_raw("screenshot", args)
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-lua-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def anime_lua(ed: Editor, code: str, edit=False):
    args = {"code": code}
    if edit:
        args["allow_edit_mode"] = True
    return (await ed.call("eval_lua", **args))["values"]


async def anime_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]

ANIME_RED = (255, 0, 0)


async def test_anime_setup(ed: Editor):
    """A UI scene with a document for the anime checks."""
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiAnime", type="UI")
    S["anime_scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["anime_scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    await ed.call("write_file", path=ANIME_DOC, content=ANIME_HTML)
    ent = await ed.call("create_entity", scene=S["anime_scene"], type="empty", name="AnimeDoc")
    await ed.call("add_component", scene=S["anime_scene"], entity=ent["entity_id"], component="UIDocument",
                  values={"uidocument": {"filename": ANIME_DOC}})
    shot = await anime_shoot(ed, "anime-start", S["anime_scene"])
    ed.check(near(shot.px(90, 90), ANIME_RED), f"the box before animating: {shot.px(90, 90)}")
    S["anime_bg"] = shot.px(900, 500)


async def test_anime_translate_and_stagger(ed: Editor):
    """anime.animate on translateX moves the box (pixels and utils.get); a staggered opacity."""
    await ed.call("play", action="start", scene=S["anime_scene"])
    try:
        await anime_lua(ed, "box = anime.animate('#box', { translateX = 400, duration = 400, ease = 'outQuad' })")
        r = await ed.call_raw("wait", {"until": "box.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the translateX animation completes: {r.text[:300]}")
        ed.check_eq(await anime_lua(ed, "return anime.utils.get('#box', 'translateX')"), ["400px"], "translateX at the end")
        shot = await anime_shoot(ed, "anime-moved")
        ed.check(near(shot.px(490, 90), ANIME_RED), f"the box moved by 400px: {shot.px(490, 90)}")
        ed.check(near(shot.px(90, 90), S["anime_bg"], 6), f"its old place is empty: {shot.px(90, 90)}")
        # the element API sees the transform
        rect = await anime_lua(ed, """
            local r = object('AnimeDoc', 'UIDocument'):getElementById('box'):getBoundingClientRect()
            return r.left, r.top, r.width, r.height
        """)
        ed.check_eq(rect, [440, 40, 100, 100], "getBoundingClientRect after translateX 400")

        # UIElement handles as targets, a UIDocument as the selector root, and function values
        # receiving the elements
        values = await anime_lua(ed, """
            local doc = object('AnimeDoc', 'UIDocument')
            local d2 = doc:getElementById('d2')
            handle = anime.animate(d2, { translateY = 50, duration = 100, ease = 'linear' })
            local scoped = anime.animate('.dot', { opacity = 1, duration = 0, root = doc })
            anime.animate({ doc:getElementById('d1'), doc:getElementById('d3') },
                          { translateX = function(el, i, total) return el.id == 'd1' and 10 or total end, duration = 0 })
            return #handle.targets, handle.targets[1] == d2, #scoped.targets
        """)
        ed.check_eq(values, [1, True, 3], "element handles, root and function values")
        r = await ed.call_raw("wait", {"until": "handle.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the handle animation completes: {r.text[:300]}")
        await ed.call("wait", frames=2)
        rects = await anime_lua(ed, """
            local doc = object('AnimeDoc', 'UIDocument')
            return doc:getElementById('d1'):getBoundingClientRect().left, doc:getElementById('d2'):getBoundingClientRect().top,
                   doc:getElementById('d3'):getBoundingClientRect().left
        """)
        ed.check_eq(rects, [50, 350, 242], "the elements moved")

        await anime_lua(ed, "dots = anime.animate('.dot', { opacity = 0, delay = anime.stagger(100), duration = 200, ease = 'linear' })")
        ed.check_eq(await anime_lua(ed, "return dots.duration, #dots.targets, dots.targets[1].id, dots.targets[3].id"),
                    [400, 3, "d1", "d3"], "three staggered UIElement targets from the selector")
        r = await ed.call_raw("wait", {"until": "dots.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the staggered animation completes: {r.text[:300]}")
        shot = await anime_shoot(ed, "anime-faded")
        for x in (60, 160, 260):
            ed.check(near(shot.px(x, 320), S["anime_bg"], 8), f"dot at {x} faded out: {shot.px(x, 320)}")
    finally:
        await ed.call("play", action="stop")


async def test_anime_timeline_labels_and_clocks(ed: Editor):
    """A timeline with labels and calls on a plain table; clock = 'game' freezes with the game,
    the default UI clock does not; play stop cancels what is left and frees the callbacks."""
    await ed.call("play", action="start", scene=S["anime_scene"])
    try:
        values = await anime_lua(ed, """
            t = { v = 0 }
            log = {}
            tl = anime.createTimeline({ defaults = { duration = 200, ease = 'linear' } })
            tl:add(t, { v = 100 })
              :label('mid')
              :add(t, { v = 0 }, 'mid+=100')
              :call(function() log[#log + 1] = 'called' end, 'mid')
            return tl.labels.mid, tl.duration
        """)
        ed.check_eq(values, [200, 500], "label and duration")
        r = await ed.call_raw("wait", {"until": "tl.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the timeline completes: {r.text[:300]}")
        ed.check_eq(await anime_lua(ed, "return t.v, log[1]"), [0, "called"], "timeline end state")

        await anime_lua(ed, """
            Engine.timeScale = 0
            game = { v = 0 }; ui = { v = 0 }
            ga = anime.animate(game, { v = 100, duration = 300, ease = 'linear', clock = 'game' })
            ua = anime.animate(ui, { v = 100, duration = 300, ease = 'linear' })
        """)
        r = await ed.call_raw("wait", {"until": "ua.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the UI-clock animation runs at time scale 0: {r.text[:300]}")
        ed.check_eq(await anime_lua(ed, "return game.v"), [0], "the game-clock animation is frozen")
        await anime_lua(ed, "Engine.timeScale = 1")
        r = await ed.call_raw("wait", {"until": "ga.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the game-clock animation runs again: {r.text[:300]}")

        bad = await ed.call_raw("eval_lua", {"code": "anime.createTimer({ clock = 'wall' })"})
        ed.check("clock must be" in bad.text, f"an invalid clock is an error: {bad.text[:300]}")

        await anime_lua(ed, "forever = anime.animate(t, { v = 1, loop = true, alternate = true, onUpdate = function() end })")
    finally:
        await ed.call("play", action="stop")
        await anime_lua(ed, "Engine.timeScale = 1", edit=True)

    # the Lua state outlives the play: the handle is still there, cancelled
    ed.check_eq(await anime_lua(ed, "return forever.cancelled, forever.paused", edit=True), [True, True],
                "play stop cancelled the endless animation")
    errors = await anime_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")


# ================================================================================================
# p1-vector section: <lottie> control from Lua and its events (phase-1 §3.10.3)
# ================================================================================================

LOTTIE_DOC = "assets/ui/lottie.html"
LOTTIE_JSON = "assets/ui/lottie/shapes.json"
# 200 x 200, 30 fps, 60 frames: a red box moving x 50 -> 150 (centre y 60) and a circle at
# (100, 145) whose fill goes blue -> green (the C++ tests' fixture)
LOTTIE_SOURCE = (Path(__file__).resolve().parents[3] / "engine/tests/htmlui/fixtures/p1/lottie/shapes.json").read_text()
LOTTIE_HTML = """<html>
<head>
    <title>lottie</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
               font-family: sans-serif; font-size: 16px; color: #ffffff; }
        div { display: block; }
        #anim { position: absolute; left: 100px; top: 100px; width: 200px; height: 200px; }
        #broken { position: absolute; left: 400px; top: 100px; width: 50px; height: 50px; }
        #label { position: absolute; left: 400px; top: 300px; }
    </style>
</head>
<body>
    <lottie id="anim" src="lottie/shapes.json"/>
    <lottie id="broken" src="lottie/missing.json"/>
    <div id="label">not a lottie</div>
</body>
</html>
"""

LOTTIE_SCRIPT = """---@class LottieScript : ScriptInstance
local LottieScript = { properties = {} }

function LottieScript:init()
    local doc = UIDocument(self.scene, self.entity)
    lottieEvents = {}
    local anim = doc:getElementById("anim")
    for _, name in ipairs({ "load", "play", "pause", "loop", "complete" }) do
        anim:addEventListener(name, function(event) lottieEvents[#lottieEvents + 1] = event.type end)
    end
    -- the document loaded (in the editor, before play): its load/error events are past;
    -- a new src loads again and fires them
    doc:getElementById("broken"):addEventListener("error", function(event)
        lottieError = event.detail and event.detail.message
    end)
    doc:getElementById("broken"):setAttribute("src", "lottie/missing2.json")
end

return LottieScript
"""

LOTTIE_RED = (230, 51, 26)
LOTTIE_TEAL = (26, 140, 140)


async def test_lottie_setup(ed: Editor):
    """A UI scene with a <lottie> document and a script listening to its events."""
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiLottie", type="UI")
    S["lottie_scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["lottie_scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    await ed.call("write_file", path=LOTTIE_JSON, content=LOTTIE_SOURCE)
    await ed.call("write_file", path=LOTTIE_DOC, content=LOTTIE_HTML)
    ent = await ed.call("create_entity", scene=S["lottie_scene"], type="empty", name="LottieDoc")
    await ed.call("add_component", scene=S["lottie_scene"], entity=ent["entity_id"], component="UIDocument",
                  values={"uidocument": {"filename": LOTTIE_DOC}})
    await ed.call("create_script", kind="lua", name="LottieScript", content=LOTTIE_SCRIPT,
                  scene=S["lottie_scene"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["lottie_scene"], path="scenes/HtmlUiLottie")
    started = await ed.call("play", action="start", scene=S["lottie_scene"])
    ed.check(started, "play started")
    await ed.call("wait", frames=3)


async def test_lottie_control_and_events(ed: Editor):
    """play/pause/seek/setSegment from Lua, the load/error/play/pause/complete events, and the
    animation on screen in the Game view."""
    try:
        r = await ed.call_raw("wait", {"until": "lottieError ~= nil", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the error event reached init's listener: {r.text[:300]}")
        values = await anime_lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            return lottieError, lottieEvents[1], anim:isPlaying(), anim:getTotalFrames(), anim:getDuration(),
                   anim:getCurrentFrame()
        """)
        ed.check("could not read 'ui/lottie/missing2.json'" in str(values[0]), f"event.detail.message: {values[0]}")
        ed.check_eq(values[1:], [None, False, 60, 2, 0], "loaded (no event since play started), paused on the first frame")

        # play, then pause: the frame stands still while paused
        await anime_lua(ed, "object('LottieDoc', 'UIDocument'):getElementById('anim'):play()")
        await ed.call("wait", frames=10)
        values = await anime_lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            local playing = anim:isPlaying()
            anim:pause()
            pausedAt = anim:getCurrentFrame()
            return playing, anim:isPlaying(), pausedAt > 0
        """)
        ed.check_eq(values, [True, False, True], "play then pause")
        await ed.call("wait", frames=10)
        ed.check_eq(await anime_lua(ed, "return object('LottieDoc', 'UIDocument'):getElementById('anim'):getCurrentFrame() == pausedAt"),
                    [True], "paused: the frame does not move")

        # seek to 1 s (frame 30): the box at x 100, the circle half way to green
        values = await anime_lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            anim:seek(1.0)
            return anim:getCurrentFrame()
        """)
        ed.check_eq(values, [30], "seek(1.0)")
        await ed.call("wait", frames=3)
        shot = await anime_shoot(ed, "lottie-seek")
        ed.check(near(shot.px(200, 160), LOTTIE_RED, 12), f"the box at frame 30: {shot.px(200, 160)}")
        ed.check(near(shot.px(175, 160), LOTTIE_RED, 12), f"the box spans x 170..230: {shot.px(175, 160)}")
        ed.check(not near(shot.px(240, 160), LOTTIE_RED, 40), f"right of the box: {shot.px(240, 160)}")
        ed.check(not near(shot.px(140, 160), LOTTIE_RED, 40), f"not at its frame-0 place: {shot.px(140, 160)}")
        ed.check(near(shot.px(200, 245), LOTTIE_TEAL, 12), f"the circle at frame 30: {shot.px(200, 245)}")

        # a segment to its end: complete
        await anime_lua(ed, """
            completed = false
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            anim:addEventListener('complete', function() completed = true end)
            anim:setSegment(50, 60)
            anim:play()
        """)
        r = await ed.call_raw("wait", {"until": "completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the complete event: {r.text[:300]}")
        values = await anime_lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            local count = {}
            for _, name in ipairs(lottieEvents) do count[name] = (count[name] or 0) + 1 end
            return anim:isPlaying(), anim:getCurrentFrame(), count.load, count.play, count.pause, count.complete
        """)
        ed.check_eq(values, [False, 59, None, 2, 1, 1], "complete on the last frame; event counts")

        # another element: an error in the Console, nothing raised
        await ed.console_since(None, limit=1)
        await anime_lua(ed, "object('LottieDoc', 'UIDocument'):getElementById('label'):play()")
        errors = await anime_errors(ed)
        ed.check(any("is not a <lottie> element" in e for e in errors), f"play() on a <div>: {errors}")
    finally:
        await ed.call("play", action="stop")
