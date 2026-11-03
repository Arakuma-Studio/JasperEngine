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
