"""On-screen controls with several fingers (ui-multitouch spec 14, acceptance 1): the game's layout
(a Floating stick on the left, a Touch Area on the right, a Fire button) driven by three fingers at
once through send_input, in the component UI (create_ui onscreen_stick / touch_area /
onscreen_button) and in an HTML document (<onscreen-stick>, <touch-area>, <onscreen-button>).
A probe script records, every frame, the largest Horizontal and Mouse Y values and the frames
Fire1 went down, so the per-frame deltas of the touch area are seen in the frame of the move.

Runs on a scratch copy of the html-ui fixture project (canvas 1280 x 720, canvas px = send_input
canvas coordinates).
"""

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900

PROBE = """---@class ControlsProbe : ScriptInstance
local ControlsProbe = {}
function ControlsProbe:init()
    self.maxHorizontal = 0
    self.maxMouseY = 0
    self.fireDowns = 0
    self.overUI = false
    RegisterEngineEvent(self, "onUpdate")
end
function ControlsProbe:onUpdate()
    self.maxHorizontal = math.max(self.maxHorizontal, Input.getAxis("Horizontal"))
    self.maxMouseY = math.max(self.maxMouseY, Input.getAxis("Mouse Y"))
    if Input.getButtonDown("Fire1") then self.fireDowns = self.fireDowns + 1 end
    if Input.isPointerOverUI(1) then self.overUI = true end
end
return ControlsProbe
"""

HTML = """<!DOCTYPE html>
<html>
<head>
  <title>controls</title>
  <style>
    html, body { margin: 0; width: 100%; height: 100%; }
    #stick { position: absolute; left: 0; top: 420px; width: 300px; height: 300px; }
    #look { position: absolute; left: 640px; top: 0; width: 640px; height: 720px; }
    #fire { position: absolute; left: 1160px; top: 600px; width: 120px; height: 120px; }
  </style>
</head>
<body>
  <onscreen-stick id="stick" mode="floating"></onscreen-stick>
  <touch-area id="look"></touch-area>
  <onscreen-button id="fire" action="Fire1">Fire</onscreen-button>
</body>
</html>
"""

# stick area bottom-left (0, 420)-(300, 720); touch area the right half; button bottom-right
STICK = (150, 570)
LOOK = (800, 300)
FIRE = (1220, 660)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def probe(ed: Editor, field: str):
    return (await lua(ed, f"return script('Probe', 'ControlsProbe').{field}"))[0]


async def three_fingers(ed: Editor, label: str):
    """Finger 0 drags the stick 80 px right over 5 frames, finger 1 drags the area 40 px up, finger
    2 taps the button while both are down; the actions are checked while the fingers hold."""
    events = [{"type": "touch", "action": "start", "pointer": 0, "x": STICK[0], "y": STICK[1]},
              {"type": "touch", "action": "start", "pointer": 1, "x": LOOK[0], "y": LOOK[1]}]
    for step in range(1, 6):
        events.append({"type": "touch", "action": "move", "pointer": 0, "x": STICK[0] + 16 * step, "y": STICK[1]})
        events.append({"type": "touch", "action": "move", "pointer": 1, "x": LOOK[0], "y": LOOK[1] - 8 * step})
    events.append({"type": "touch", "action": "tap", "pointer": 2, "x": FIRE[0], "y": FIRE[1]})
    r = await ed.call_raw("send_input", {"events": events, "space": "canvas", "frames_after": 3})
    ed.check(not r.is_error, f"{label}: send_input: {r.text[:300]}")

    horizontal = (await lua(ed, "return Input.getAxis('Horizontal')"))[0]
    ed.check(horizontal > 0.9, f"{label}: the stick held right: Horizontal {horizontal}")
    ed.check(await probe(ed, "maxMouseY") > 0, f"{label}: the touch area moved Mouse Y up in the frames of its moves")
    ed.check_eq(await probe(ed, "fireDowns"), 1, f"{label}: the button pressed Fire1 once while two fingers held")
    ed.check_eq((await lua(ed, "return Input.isPointerOverUI(0)"))[0], True, f"{label}: finger 0 is on UI")

    # lifting the look finger keeps the stick (no finger releases another's control)
    r = await ed.call_raw("send_input", {"events": [
        {"type": "touch", "action": "end", "pointer": 1, "x": LOOK[0], "y": LOOK[1] - 40}], "space": "canvas", "frames_after": 2})
    ed.check(not r.is_error, r.text[:300])
    horizontal = (await lua(ed, "return Input.getAxis('Horizontal')"))[0]
    ed.check(horizontal > 0.9, f"{label}: the stick still held after the other finger lifted: {horizontal}")

    r = await ed.call_raw("send_input", {"events": [
        {"type": "touch", "action": "end", "pointer": 0, "x": STICK[0] + 80, "y": STICK[1]}], "space": "canvas", "frames_after": 2})
    ed.check(not r.is_error, r.text[:300])
    horizontal = (await lua(ed, "return Input.getAxis('Horizontal')"))[0]
    ed.check(abs(horizontal) < 0.05, f"{label}: released: Horizontal {horizontal}")


async def add_probe(ed: Editor, scene):
    host = (await ed.call("create_entity", scene=scene, name="Probe", type="empty"))["entity_id"]
    await ed.call("create_script", kind="lua", name="ControlsProbe", content=PROBE, entity=host, overwrite=True)


async def test_native_controls_three_fingers(ed: Editor):
    scene = (await ed.call("create_scene", name="Controls", type="UI"))["scene"]["id"]
    made = await ed.call("create_ui", scene=scene, root={"type": "onscreen_stick", "name": "Stick", "mode": "floating"})
    ed.check(made["root"].get("background") and made["root"].get("handle"), f"the stick's parts: {made}")
    await ed.call("create_ui", scene=scene, root={"type": "touch_area", "name": "Look"})
    await ed.call("create_ui", scene=scene, root={"type": "onscreen_button", "name": "Fire", "action": "Fire1", "text": "Fire"})
    await add_probe(ed, scene)

    stick = await ed.call("get_entity", scene=scene, entity="Stick", components=["onscreenstick"], detail="full")
    ed.check("floating" in str(harness.find_key(stick, "mode")), f"create_ui mode: {harness.find_key(stick, 'mode')}")
    ed.check("Horizontal" in str(harness.find_key(stick, "horizontalAction")), "the default horizontal action")
    await ed.call("save_scene", scene=scene, path="scenes/Controls")
    ed.check((ed.project / "assets" / "ui" / "onscreen" / "stick_base.png").exists(), "the stick's circle images were written")

    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=5)
        await three_fingers(ed, "component UI")
        console = await ed.console_since(None)
        ed.check(not [line for line in console if "error" in str(line).lower()], f"no errors: {console}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    await ed.check_alive()


async def test_html_controls_three_fingers(ed: Editor):
    scene = (await ed.call("create_scene", name="HtmlControls", type="UI"))["scene"]["id"]
    await ed.call("write_file", path="assets/ui/controls.html", content=HTML)
    doc = await ed.call("create_entity", scene=scene, type="ui_document", name="Controls")
    await ed.call("set_properties", scene=scene, entity=doc["entity_id"], values={"uidocument": {"filename": "assets/ui/controls.html"}})
    await add_probe(ed, scene)
    await ed.call("save_scene", scene=scene, path="scenes/HtmlControls")
    await ed.call("wait", frames=10)

    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=10)
        await three_fingers(ed, "HTML UI")
        # the stick element shows its state (inspect_ui / assert_ui see the attributes)
        r = await ed.call_raw("send_input", {"events": [
            {"type": "touch", "action": "start", "pointer": 0, "x": STICK[0], "y": STICK[1]},
            {"type": "touch", "action": "move", "pointer": 0, "x": STICK[0] + 80, "y": STICK[1]}], "space": "canvas", "frames_after": 2})
        ed.check(not r.is_error, r.text[:300])
        state = await lua(ed, "local d = UIDocument(object('Controls').scene, object('Controls').entity); "
                              "local s = d:getElementById('stick'); return s:getAttribute('data-pressed'), s:getAttribute('data-x')")
        ed.check_eq(state, ["true", "1.000"], "data-pressed / data-x while held")
        await ed.call_raw("send_input", {"events": [{"type": "touch", "action": "end", "pointer": 0, "x": STICK[0] + 80, "y": STICK[1]}],
                                         "space": "canvas", "frames_after": 2})
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    await ed.check_alive()
