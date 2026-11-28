"""The app lifecycle around HTML UI documents (docs/specs/S95-lua-runtime-gaps.md, REQ-15 to REQ-19).

- A view loss (an Android return after the EGL context went, a lost WebGL context), simulated in
  Play with send_input {type: "app", action: "view_lost" / "view_restored"}: the running scenes'
  GPU objects go and come back, and the document keeps what its script did to it (text, class,
  scroll, listeners), with no second onLoad and no onUnload. The frame after is the one before.
- Play started and stopped three times with a document logs no "outside a frame" error (S28).
"""

from harness import Editor
from htmlui_pixels import image_difference

EDITOR = {"create": True, "name": "App Lifecycle"}
CI = True

DOC = "assets/ui/lifecycle.html"
HTML = """<html>
<head>
    <title>lifecycle</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
               font-family: sans-serif; font-size: 24px; color: #ffffff; }
        div { display: block; }
        #label { position: absolute; left: 40px; top: 40px; width: 300px; height: 40px; }
        #btn { position: absolute; left: 40px; top: 120px; width: 200px; height: 60px; background-color: #3060c0; }
        #btn.on { background-color: #20a040; }
        #list { position: absolute; left: 400px; top: 40px; width: 200px; height: 100px; overflow: auto; }
        #list div { height: 50px; }
    </style>
</head>
<body>
    <div id="label">From the file</div>
    <div id="btn"></div>
    <div id="list"><div>1</div><div>2</div><div>3</div><div>4</div><div>5</div></div>
</body>
</html>
"""

SCRIPT = """---@class LifecycleUI : ScriptInstance
local LifecycleUI = { properties = {} }

function LifecycleUI:init()
    local doc = UIDocument(self.scene, self.entity)
    uiDoc = doc
    uiLoads, uiUnloads, uiClicks = 0, 0, 0
    doc.onLoad = function() uiLoads = uiLoads + 1 end
    doc.onUnload = function() uiUnloads = uiUnloads + 1 end
end

return LifecycleUI
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="LifecycleUI", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Hud")
    S["entity"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["entity"], component="UIDocument",
                  values={"uidocument": {"filename": DOC}})
    await ed.call("create_script", kind="lua", name="LifecycleUI", content=SCRIPT, scene=S["scene"], entity=S["entity"])
    await ed.call("save_scene", scene=S["scene"], path="scenes/LifecycleUI")
    return S


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def game_shot(ed: Editor) -> bytes:
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 640})
    ed.check(not shot.is_error and shot.images, f"a Game view screenshot: {shot.text[:300]}")
    return shot.images[0][0]


async def test_view_loss_keeps_ui(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        r = await ed.call_raw("wait", {"until": "uiDoc ~= nil and uiDoc.loaded", "game_seconds": 10})
        ed.check(not r.is_error, f"the document loaded in Play: {r.text[:300]}")
        await ed.call("wait", frames=3)
        # (Play starts with the document the editor loaded: no onLoad may have fired since init)
        await lua(ed, """
            loadsBefore = uiLoads
            local doc = uiDoc
            keptLabel = doc:getElementById("label")
            keptLabel.textContent = "Set by the script"
            local btn = doc:getElementById("btn")
            btn:setClass("on", true)
            btn:addEventListener("click", function() uiClicks = uiClicks + 1 end)
            doc:getElementById("list").scrollTop = 50
        """)
        await ed.call("wait", frames=3)
        before = await game_shot(ed)

        result = await ed.call("send_input", events=[
            {"type": "app", "action": "view_lost"}, {"type": "wait", "frames": 3},
            {"type": "app", "action": "view_restored"}], frames_after=3)
        ed.check(result.get("state") == "playing", f"still playing after the view came back: {result}")
        lifecycle = (await ed.call("get_play_state"))["platform"]["lifecycle"]
        ed.check_eq(lifecycle, {"view_lost": 1, "view_restored": 1, "view": "loaded"}, "platform.lifecycle")
        ed.check(not (await ed.call("get_play_state"))["platform"]["paused"], "the restore resumed the app view_lost paused")

        values = await lua(ed, """
            local doc = uiDoc
            local label = doc:getElementById("label")
            return uiLoads - loadsBefore, uiUnloads, label == keptLabel, keptLabel:isValid(), label.textContent,
                   doc:getElementById("btn"):hasClass("on"), doc:getElementById("list").scrollTop
        """)
        ed.check_eq(values, [0, 0, True, True, "Set by the script", True, 50],
                    "the document as the script left it, with no onLoad or onUnload")
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "target": "#btn"}], frames_after=2)
        ed.check_eq(await lua(ed, "return uiClicks"), [1], "the listener added before the loss still runs")

        after = await game_shot(ed)
        difference = image_difference(before, after)
        ed.check(difference < 1.0, f"the frame after the return is the one before (mean difference {difference:.2f})")
    finally:
        await ed.call("play", action="stop")


async def test_view_loss_errors_and_reset(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        bad = await ed.call_raw("send_input", {"events": [{"type": "app", "action": "view_restored"}]})
        ed.check(bad.is_error and "view_lost" in bad.text, f"a restore without a loss names the fix: {bad.text[:300]}")
        await ed.call("send_input", events=[{"type": "app", "action": "view_lost"}])
        state = (await ed.call("get_play_state"))["platform"]
        ed.check(state["lifecycle"]["view"] == "lost" and state["paused"], f"lost and paused: {state}")
        shot = await ed.call_raw("screenshot", {"view": "game"})
        ed.check(shot.is_error and "view_restored" in shot.text, f"nothing draws while the view is lost: {shot.text[:300]}")
        again = await ed.call_raw("send_input", {"events": [{"type": "app", "action": "view_lost"}]})
        ed.check(again.is_error and "view_restored" in again.text, f"a second loss names the fix: {again.text[:300]}")
    finally:
        # Play ends with the view still lost: the next Play starts with a view
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        state = (await ed.call("get_play_state"))["platform"]
        ed.check_eq(state["lifecycle"], {"view_lost": 0, "view_restored": 0, "view": "loaded"}, "a new Play")
        r = await ed.call_raw("wait", {"until": "uiDoc ~= nil and uiDoc.loaded", "game_seconds": 10})
        ed.check(not r.is_error, f"the document loads in the next Play: {r.text[:300]}")
        await game_shot(ed)
    finally:
        await ed.call("play", action="stop")


async def test_play_cycle_no_outside_frame_error(ed: Editor):
    s = await setup(ed)
    await ed.console_since(None, limit=1)
    for _ in range(3):
        await ed.call("play", action="start", scene=s["scene"])
        r = await ed.call_raw("wait", {"until": "uiDoc ~= nil and uiDoc.loaded", "timeout_seconds": 30})
        ed.check(not r.is_error, f"the document loaded in Play: {r.text[:300]}")
        await ed.call("wait", frames=5)
        await ed.call("play", action="stop")
        await ed.call("wait", frames=5)
    entries = await ed.console_since(limit=500)
    outside = [e["message"] for e in entries if "outside a frame" in e.get("message", "")]
    ed.check(not outside, f"no 'outside a frame' error over three Play cycles: {outside[:3]}")
