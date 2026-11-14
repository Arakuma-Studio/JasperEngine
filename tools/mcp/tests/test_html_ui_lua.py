"""Lottie controls and callbacks through the public Lua bindings in a running project.

C++ tests own frame math and rendering. This scenario catches missing or misbound Lua
methods and callbacks that direct UIElement calls cannot exercise.
"""

from pathlib import Path

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


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
    </style>
</head>
<body>
    <lottie id="anim" src="lottie/shapes.json"/>
    <lottie id="broken" src="lottie/missing.json"/>
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


async def test_lottie_control_and_events(ed: Editor):
    """Lua play, pause, seek and setSegment update state and deliver callbacks to the script."""
    try:
        await ed.console_since(None, limit=1)
        made = await ed.call("create_scene", name="HtmlUiLottie", type="UI")
        scene = made["scene"]["id"]
        await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
        await ed.call("write_file", path=LOTTIE_JSON, content=LOTTIE_SOURCE)
        await ed.call("write_file", path=LOTTIE_DOC, content=LOTTIE_HTML)
        ent = await ed.call("create_entity", scene=scene, type="empty", name="LottieDoc")
        await ed.call("add_component", scene=scene, entity=ent["entity_id"], component="UIDocument",
                      values={"uidocument": {"filename": LOTTIE_DOC}})
        await ed.call("create_script", kind="lua", name="LottieScript", content=LOTTIE_SCRIPT,
                      scene=scene, entity=ent["entity_id"])
        await ed.call("save_scene", scene=scene, path="scenes/HtmlUiLottie")
        # A fixed UI clock, frozen between the calls: the checks below count frames ("10 frames
        # after play() the animation is still playing"), and on the measured clock a loaded host's
        # frames last up to 0.25 s (Engine::maxDeltatime), so the 2 s animation was over within
        # those 10 frames and "play then pause" failed now and then.
        started = await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
        ed.check(started, "play started")
        await ed.call("wait", frames=3)
        r = await ed.call_raw("wait", {"until": "lottieError ~= nil", "game_seconds": 10})
        ed.check(not r.is_error, f"the error event reached init's listener: {r.text[:300]}")
        values = await lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            return lottieError, lottieEvents[1], anim:isPlaying(), anim:getTotalFrames(), anim:getDuration(),
                   anim:getCurrentFrame()
        """)
        ed.check("ui/lottie/missing2.json" in str(values[0]), f"event.detail.message: {values[0]}")
        ed.check_eq(values[1:], [None, False, 60, 2, 0], "loaded (no event since play started), paused on the first frame")

        # play, then pause: the frame stands still while paused
        await lua(ed, "object('LottieDoc', 'UIDocument'):getElementById('anim'):play()")
        await ed.call("wait", frames=10)
        values = await lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            local playing = anim:isPlaying()
            anim:pause()
            pausedAt = anim:getCurrentFrame()
            return playing, anim:isPlaying(), pausedAt > 0
        """)
        ed.check_eq(values, [True, False, True], "play then pause")
        await ed.call("wait", frames=10)
        ed.check_eq(await lua(ed, "return object('LottieDoc', 'UIDocument'):getElementById('anim'):getCurrentFrame() == pausedAt"),
                    [True], "paused: the frame does not move")

        # The fixture is 30 fps, so seeking to one second selects frame 30.
        values = await lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            anim:seek(1.0)
            return anim:getCurrentFrame()
        """)
        ed.check_eq(values, [30], "seek(1.0)")
        # a segment to its end: complete
        await lua(ed, """
            completed = false
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            anim:addEventListener('complete', function() completed = true end)
            anim:setSegment(50, 60)
            anim:play()
        """)
        r = await ed.call_raw("wait", {"until": "completed", "game_seconds": 10})
        ed.check(not r.is_error, f"the complete event: {r.text[:300]}")
        values = await lua(ed, """
            local anim = object('LottieDoc', 'UIDocument'):getElementById('anim')
            local count = {}
            for _, name in ipairs(lottieEvents) do count[name] = (count[name] or 0) + 1 end
            return anim:isPlaying(), anim:getCurrentFrame(), count.load, count.play, count.pause, count.complete
        """)
        ed.check_eq(values, [False, 59, None, 2, 1, 1], "complete on the last frame; event counts")

    finally:
        # (the clock options outlive a stop: back to the measured clock for the tests after)
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


LINKS_DOC = "assets/ui/links.html"
LINKS_HTML = """<html>
<head><title>links</title></head>
<body>
    <a id="ext" href="https://example.com/page">out</a>
    <a id="kept" href="https://example.com/kept">kept</a>
</body>
</html>
"""

UPDATE_MODE_HTML = """<html>
<head>
    <title>update mode</title>
    <style>
        body { position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        @keyframes slide { from { left: 0px; } to { left: 400px; } }
        #runner { position: absolute; left: 0px; top: 50px; width: 40px; height: 40px; background-color: #ff8800;
                  animation: slide 4s linear infinite; }
    </style>
</head>
<body><div id="runner"></div></body>
</html>
"""


async def test_external_link_opens_url(ed: Editor):
    """S93 R32 (AC36): a click on a remote <a href> opens it with System.openURL (recorded; a headless editor opens
    nothing); an htmlnavigate listener that calls preventDefault() keeps it in the game."""
    try:
        made = await ed.call("create_scene", name="HtmlUiLinks", type="UI")
        scene = made["scene"]["id"]
        await ed.call("write_file", path=LINKS_DOC, content=LINKS_HTML)
        ent = await ed.call("create_entity", scene=scene, type="empty", name="LinkDoc")
        await ed.call("add_component", scene=scene, entity=ent["entity_id"], component="UIDocument",
                      values={"uidocument": {"filename": LINKS_DOC}})
        await ed.call("save_scene", scene=scene, path="scenes/HtmlUiLinks")
        await ed.call("play", action="start", scene=scene)
        await ed.call("wait", frames=3)
        await lua(ed, "object('LinkDoc', 'UIDocument'):getElementById('ext'):click()")
        await lua(ed, """
            local doc = object('LinkDoc', 'UIDocument')
            doc:addEventListener('htmlnavigate', function(event) event:preventDefault() end)
            doc:getElementById('kept'):click()
        """)
        urls = (await ed.call("get_play_state"))["platform"]["opened_urls"]
        ed.check_eq([u["url"] for u in urls], ["https://example.com/page"])
    finally:
        await ed.call("play", action="stop")


async def test_update_mode_property(ed: Editor):
    """S93 R35 / AC40: create_ui_document takes the Update Mode (snake_case keys); at
    Engine.timeScale = 0 a Game Time document's @keyframes hold, an Unscaled Time one's run on."""
    try:
        made = await ed.call("create_scene", name="HtmlUiUpdateMode", type="UI")
        scene = made["scene"]["id"]
        await ed.call("write_file", path="assets/ui/update_mode.html", content=UPDATE_MODE_HTML)
        game = await ed.call("create_ui_document", scene=scene, html="assets/ui/update_mode.html", name="GameDoc",
                             properties={"update_mode": "game"})
        unscaled = await ed.call("create_ui_document", scene=scene, html="assets/ui/update_mode.html", name="UnscaledDoc")
        got = await ed.call("get_entity", scene=scene, entity=game["entity"]["id"], components=["uidocument"])
        ed.check_eq(got["components"]["uidocument"]["updateMode"], "game", "create_ui_document set the Update Mode")
        await ed.call("save_scene", scene=scene, path="scenes/HtmlUiUpdateMode")

        await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
        await ed.call("wait", frames=5)
        await lua(ed, "Engine.timeScale = 0")
        await ed.call("wait", frames=2)

        async def left(entity) -> float:
            r = await ed.call("inspect_ui", scene=scene, entity=entity, selector="#runner", detail="full", styles=["left"])
            styles = {x["property"]: x for x in r["details"][0]["styles"]}
            return float(str(styles["left"]["value"]).replace("px", ""))

        game_before, unscaled_before = await left(game["entity"]["id"]), await left(unscaled["entity"]["id"])
        await ed.call("wait", frames=30)
        game_after, unscaled_after = await left(game["entity"]["id"]), await left(unscaled["entity"]["id"])
        ed.check_eq(game_after, game_before, "a Game Time document's @keyframes hold at time scale 0")
        ed.check(unscaled_after > unscaled_before + 20, f"an Unscaled Time one runs on: {unscaled_before} -> {unscaled_after}")

        # Lua reads and switches it
        values = await lua(ed, """
            local doc = object('GameDoc', 'UIDocument')
            local was = doc.updateMode == UIDocumentUpdateMode.GAME_TIME
            doc.updateMode = UIDocumentUpdateMode.UNSCALED_TIME
            return was, doc.updateMode == UIDocumentUpdateMode.UNSCALED_TIME
        """)
        ed.check_eq(values, [True, True], "doc.updateMode")
        await ed.call("wait", frames=30)
        ed.check(await left(game["entity"]["id"]) > game_after + 20, "switched to Unscaled Time, it runs at time scale 0")
    finally:
        await lua(ed, "Engine.timeScale = 1")
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
