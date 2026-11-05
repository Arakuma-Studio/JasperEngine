"""Safe area (docs/superpowers/specs/2026-10-31-safe-area-design.md §9): the Game view's simulated
device feeds Screen.safeArea (canvas units) in Play and edit mode, a rotation fires one
onSafeAreaChanged, the Safe Area component drives a panel (create_ui type safe_area) whose
children stay inside the safe rectangle and whose driven RectTransform fields are refused,
extra insets shrink it, screenshots carry the device overlay and safe_area_in_image, HTML UI
env(safe-area-inset-top) is the live inset, get_play_state reports safe_area, and "none" means
zero insets.

Runs on an owned empty project with a 390 x 844 canvas (an iPhone 13 in points) fitted to the
width, so the iPhone 13's 141 / 102 pixel portrait insets are 47 / 34 canvas units.
"""

from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Safe Area"}
CI = True

CANVAS = (390, 844)
IPHONE_13 = {"width": 1170, "height": 2532, "portrait": (0, 141, 0, 102), "landscape": (141, 0, 141, 63)}

COUNTER = """---@class SafeCounter : ScriptInstance
local SafeCounter = { properties = {} }
function SafeCounter:init()
    safeEvents = 0
    RegisterEngineEvent(self, "onSafeAreaChanged")
end
function SafeCounter:onSafeAreaChanged() safeEvents = safeEvents + 1 end
return SafeCounter
"""

HTML = """<!DOCTYPE html>
<html>
<head><link rel="stylesheet" href="safe.css"></head>
<body><div id="hud"><div id="coins">0</div></div></body>
</html>
"""
CSS = """html, body { margin: 0; }
#hud { padding-top: env(safe-area-inset-top); }
#coins { height: 20px; }
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("set_project_settings", values={"graphics.canvas_width": CANVAS[0], "graphics.canvas_height": CANVAS[1],
                                                  "graphics.scaling_mode": "fit_width"})
    made = await ed.call("create_scene", name="SafeMain", type="UI", path="scenes/SafeMain")
    S["scene"] = made["scene"]["id"]
    # a white background: the overlay's black corners show on it
    await ed.call("create_ui", scene=S["scene"], root={"type": "image", "name": "Background", "color": [1, 1, 1, 1],
                                                       "layout": {"anchor": "stretch"}})
    ui = await ed.call("create_ui", scene=S["scene"], root={
        "type": "safe_area", "name": "Safe",
        "children": [{"type": "button", "name": "Pause", "text": "II",
                      "layout": {"anchor": "top_right", "size": [60, 40], "position": [-8, 8]}}]})
    S["safe"] = ui["root"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Counter")
    await ed.call("create_script", kind="lua", name="SafeCounter", content=COUNTER, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["scene"])
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


def expected_safe(view: dict, insets: tuple, device: tuple) -> list:
    """Screen.safeArea for the Game view's render size: the device's insets scaled to the render
    pixels (rounded, as the Game view pushes them) and fitted to the canvas width."""
    rw, rh = view["render_size"]
    dw, dh = device
    left, top, right, bottom = (round(insets[0] * rw / dw), round(insets[1] * rh / dh),
                                round(insets[2] * rw / dw), round(insets[3] * rh / dh))
    k = CANVAS[0] / rw
    ch = rh * k
    return [left * k, top * k, CANVAS[0] - (left + right) * k, ch - (top + bottom) * k]


def edit_expected(insets: tuple, device: tuple) -> list:
    """Screen.safeArea in edit mode: the project canvas, each edge the same fraction of it as on the
    device (whatever size the Game view has now)."""
    dw, dh = device
    left, top, right, bottom = insets[0] / dw * CANVAS[0], insets[1] / dh * CANVAS[1], insets[2] / dw * CANVAS[0], insets[3] / dh * CANVAS[1]
    return [left, top, CANVAS[0] - left - right, CANVAS[1] - top - bottom]


def near(ed: Editor, got, want, what: str, tolerance: float = 1.0):
    ed.check(len(got) == len(want) and all(abs(a - b) <= tolerance for a, b in zip(got, want)),
             f"{what}: {got}, expected {want}")


def rect_values(rect) -> list:
    if isinstance(rect, dict):
        return [rect["x"], rect["y"], rect["width"], rect["height"]]
    return list(rect)


async def test_device_safe_area_play_rotation_and_event(ed: Editor):
    s = await setup(ed)
    view = await ed.call("configure_game_view", device="iphone_13", orientation="portrait")
    ed.check_in("custom", view["devices"])
    near(ed, view["safe_area"], edit_expected(IPHONE_13["portrait"], (IPHONE_13["width"], IPHONE_13["height"])),
         "edit mode: the project canvas laid out as on the phone", 1.5)
    near(ed, view["safe_area"], [0, 47, 390, 763], "about the iPhone 13's 47 / 34 points", 1.5)
    try:
        await ed.call("play", action="start", scene=s["scene"])
        await ed.call("wait", frames=5)
        # the Game view shows the game now: its size is the device's aspect
        view = await ed.call("configure_game_view")
        portrait = expected_safe(view, IPHONE_13["portrait"], (IPHONE_13["width"], IPHONE_13["height"]))
        safe = await lua(ed, "local r = Screen.safeArea; return {r.x, r.y, r.width, r.height}")
        near(ed, safe[0], portrait, "Screen.safeArea in Play", 1.5)
        insets = await lua(ed, "local i = Screen.safeAreaInsets; return {i.left, i.top, i.right, i.bottom}")
        near(ed, insets[0], [0, 47, 0, 34], "Screen.safeAreaInsets", 1.0)
        cutouts = await lua(ed, "return #Screen.cutouts")
        ed.check_eq(cutouts, [1], "the notch is a cutout")
        await lua(ed, "safeEvents = 0")

        # rotate the phone while playing: the values swap, one event
        view = await ed.call("configure_game_view", orientation="landscape_left")
        await ed.call("wait", frames=5)
        landscape = expected_safe(view, IPHONE_13["landscape"], (IPHONE_13["height"], IPHONE_13["width"]))
        safe = await lua(ed, "local r = Screen.safeArea; return {r.x, r.y, r.width, r.height}")
        near(ed, safe[0], landscape, "Screen.safeArea in landscape", 1.0)
        ed.check(safe[0][0] > 10 and abs(safe[0][1]) < 0.01, f"landscape: the notch on the left, nothing on top: {safe}")
        ed.check_eq(await lua(ed, "return safeEvents"), [1], "one onSafeAreaChanged for the rotation")

        state = (await ed.call("get_play_state"))["safe_area"]
        ed.check_eq(state["device"], "iphone_13")
        near(ed, state["rect"], landscape, "get_play_state.safe_area.rect", 1.0)
        ed.check_eq(state["extra"], [0, 0, 0, 0])
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", device="iphone_13", orientation="portrait")


async def test_safe_area_component_drives_the_panel(ed: Editor):
    s = await setup(ed)
    await ed.call("configure_game_view", device="iphone_13", orientation="portrait")
    # edit mode: the Scene view's layout follows the simulated device
    got = await ed.call("get_entity", scene=s["scene"], entity="Safe", components=["recttransform", "safearea"])
    rect = got["components"]["recttransform"]
    ed.check(abs(float(rect["anchorPointTop"]) - 47 / 844) < 0.003, f"top anchor at the notch: {rect}")
    ed.check(abs(float(rect["anchorPointBottom"]) - (844 - 34) / 844) < 0.003, f"bottom anchor at the home bar: {rect}")
    ed.check(float(rect["anchorPointLeft"]) == 0 and float(rect["anchorPointRight"]) == 1, f"no side insets: {rect}")
    ed.check("safearea" in got["components"], f"the component is listed: {list(got['components'])}")

    # driven fields are refused, the component's own fields are not
    bad = await ed.call_raw("set_properties", {"scene": s["scene"], "entity": "Safe", "values": {"recttransform": {"width": 10}}})
    ed.check(bad.is_error and "driven by the Safe Area component" in bad.text, bad.text)
    bad = await ed.call_raw("set_rect_layout", {"scene": s["scene"], "items": [{"entity": "Safe", "anchor": "center"}]})
    ed.check(bad.is_error and "driven by the Safe Area component" in bad.text, bad.text)
    await ed.call("set_properties", scene=s["scene"], entity="Safe", values={"safearea": {"left": False}})
    await ed.call("set_properties", scene=s["scene"], entity="Safe", values={"safearea": {"left": True}})
    bad = await ed.call_raw("create_ui", {"scene": s["scene"], "root": {"type": "safe_area", "layout": {"anchor": "center"}}})
    ed.check(bad.is_error and "laid out by its Safe Area component" in bad.text, bad.text)

    try:
        await ed.call("play", action="start", scene=s["scene"])
        await ed.call("wait", frames=5)
        box = await lua(ed, "local b = object('Safe/Pause', 'RectTransform'); local p = b:getWorldPosition(); "
                            "return {p.x, p.y, b.width, b.height}")
        x, y, w, h = box[0]
        safe = await lua(ed, "local r = Screen.safeArea; return {r.x, r.y, r.width, r.height}")
        sx, sy, sw, sh = safe[0]
        ed.check(x >= sx - 0.5 and x + w <= sx + sw + 0.5 and y >= sy - 0.5 and y + h <= sy + sh + 0.5,
                 f"the top-right button keeps inside the safe area: button {box[0]}, safe {safe[0]}")
        ed.check(abs(y - (sy + 8)) <= 1, f"8 below the notch: y {y}, safe top {sy}")

        # a banner at the bottom: the panel shrinks by it
        before = await lua(ed, "return object('Safe', 'RectTransform').height")
        await lua(ed, "Screen.setExtraInsets(0, 0, 0, 50)")
        await ed.call("wait", frames=3)
        after = await lua(ed, "return object('Safe', 'RectTransform').height")
        ed.check(abs((before[0] - after[0]) - 50) <= 1, f"the panel is 50 shorter: {before} -> {after}")
        state = (await ed.call("get_play_state"))["safe_area"]
        ed.check_eq(state["extra"], [0, 0, 0, 50])
        bad = await ed.call_raw("eval_lua", {"code": "Screen.setExtraInsets(0, 0, 0, 'x')"})
        ed.check(bad.is_error and "must be a number" in bad.text, bad.text)
        bad = await ed.call_raw("eval_lua", {"code": "Screen.setExtraInsets(0, 0, 0, 1, 'inches')"})
        ed.check(bad.is_error and "canvas" in bad.text, bad.text)
    finally:
        await ed.call("play", action="stop")
    # Play stop clears the extras
    state = (await ed.call("get_play_state"))["safe_area"]
    ed.check_eq(state["extra"], [0, 0, 0, 0], "extras end with the Play")


async def test_screenshot_overlay_and_safe_area_in_image(ed: Editor):
    s = await setup(ed)
    await ed.call("configure_game_view", device="iphone_13", orientation="portrait", device_overlay=True)
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 300})
    ed.check(not shot.is_error and shot.images, shot.text)
    info = shot.structured
    ed.check_eq(info.get("device"), "iphone_13")
    safe = info["safe_area_in_image"]
    width, height, ch, rows = decode_png(shot.images[0][0])
    ed.check(abs(safe["y"] / height - 141 / 2532) < 0.01, f"safe top in the image: {safe}, image {width}x{height}")
    corner = rows[0][0:3]
    ed.check(max(corner) < 10, f"the rounded corner is black: {list(corner)}")
    middle = rows[height // 2][(width // 2) * ch:(width // 2) * ch + 3]
    ed.check(min(middle) > 200, f"the white background shows in the middle: {list(middle)}")
    notch = rows[2][(width // 2) * ch:(width // 2) * ch + 3]
    ed.check(max(notch) < 10, f"the notch is black: {list(notch)}")

    plain = await ed.call_raw("screenshot", {"view": "game", "max_width": 300, "device_overlay": False})
    _, _, ch2, rows2 = decode_png(plain.images[0][0])
    ed.check(min(rows2[0][0:3]) > 200, f"no overlay: the corner shows the game: {list(rows2[0][0:3])}")
    ed.check("safe_area_in_image" in plain.structured, "safe_area_in_image without the overlay too")

    # Device Overlay off, Safe Area outline on: only the outline is drawn
    await ed.call("configure_game_view", device_overlay=False, safe_area_outline=True)
    try:
        outlined = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
        ed.check(not outlined.is_error and outlined.images, outlined.text)
        ed.check_eq(outlined.structured.get("device_overlay"), False, "the screen is not drawn")
        w3, h3, ch3, rows3 = decode_png(outlined.images[0][0])
        ed.check(min(rows3[0][0:3]) > 200, f"no corner: {list(rows3[0][0:3])}")
        notch3 = rows3[2][(w3 // 2) * ch3:(w3 // 2) * ch3 + 3]
        ed.check(min(notch3) > 200, f"no notch without the device overlay: {list(notch3)}")
        top = round(outlined.structured["safe_area_in_image"]["y"])
        greens = sum(1 for x in range(w3) for y in (top - 1, top, top + 1) if 0 <= y < h3
                     and rows3[y][x * ch3 + 1] > 200 and rows3[y][x * ch3] < 60 and rows3[y][x * ch3 + 2] < 60)
        ed.check(greens > w3 // 4, f"the dashed green outline along the safe top ({greens} green pixels)")
    finally:
        await ed.call("configure_game_view", device_overlay=True, safe_area_outline=False)


async def test_html_env_is_the_live_inset(ed: Editor):
    s = await setup(ed)
    (ed.project / "assets/ui").mkdir(parents=True, exist_ok=True)
    (ed.project / "assets/ui/safe.html").write_text(HTML)
    (ed.project / "assets/ui/safe.css").write_text(CSS)
    made = await ed.call("create_ui_document", scene=s["scene"], html="assets/ui/safe.html")
    doc = made["entity"]["id"]
    await ed.call("configure_game_view", device="iphone_13", orientation="portrait")

    async def padding_top() -> float:
        r = await ed.call("inspect_ui", scene=s["scene"], entity=doc, selector="#hud", detail="full", styles=["padding-top"])
        styles = {x["property"]: x for x in r["details"][0]["styles"]}
        return float(str(styles["padding-top"]["value"]).replace("px", ""))

    top = await padding_top()
    ed.check(abs(top - 47) <= 1, f"edit mode: env(safe-area-inset-top) is the notch's 47 px: {top}")
    try:
        await ed.call("play", action="start", scene=s["scene"])
        await ed.call("wait", frames=5)
        top = await padding_top()
        ed.check(abs(top - 47) <= 1, f"Play: {top}")
        await lua(ed, "Screen.setExtraInsets(0, 10, 0, 0)")
        await ed.call("wait", frames=3)
        top = await padding_top()
        ed.check(abs(top - 57) <= 1, f"the extra inset shows without a reload: {top}")
    finally:
        await ed.call("play", action="stop")


async def test_custom_device_and_none(ed: Editor):
    s = await setup(ed)
    # a resolution preset is a size without insets; a device's screen wins over it
    view = await ed.call("configure_game_view", device="none", resolution="1080x2400")
    near(ed, view["safe_area"], [0, 0, CANVAS[0], CANVAS[1]], "a resolution preset: no insets", 0.01)
    view = await ed.call("configure_game_view", custom_device={"width": 1000, "height": 2000, "dpi": 420,
                                                              "insets": {"top": 200, "bottom": 100}, "corner_radius": 50})
    ed.check_eq(view["device"], "custom")
    ed.check_eq(view["custom_device"]["insets"]["top"], 200)
    ed.check_eq(view["custom_device"]["dpi"], 420)
    ed.check_eq(view["render_size"], [1000, 2000])
    expected = edit_expected((0, 200, 0, 100), (1000, 2000))
    near(ed, view["safe_area"], expected, "the custom device's safe area", 1.5)
    bad = await ed.call_raw("configure_game_view", {"custom_device": {"width": "wide"}})
    ed.check(bad.is_error and "custom_device" in bad.text, bad.text)
    try:
        await ed.call("play", action="start", scene=s["scene"])
        await ed.call("wait", frames=3)
        ed.check_eq(await lua(ed, "return Screen.dpi"), [420], "the custom device's dpi is Screen.dpi")
    finally:
        await ed.call("play", action="stop")

    view = await ed.call("configure_game_view", device="none", resolution="free")
    near(ed, view["safe_area"], [0, 0, CANVAS[0], CANVAS[1]], "no device: the whole canvas", 0.01)
    try:
        await ed.call("play", action="start", scene=s["scene"])
        await ed.call("wait", frames=3)
        insets = await lua(ed, "return Screen.safeAreaInsets:isZero(), #Screen.cutouts, tostring(Screen.extraInsets)")
        ed.check_eq(insets, [True, 0, "EdgeInsets(0, 0, 0, 0)"])
        ed.check_eq((await ed.call("get_play_state"))["safe_area"]["device"], None)
    finally:
        await ed.call("play", action="stop")
