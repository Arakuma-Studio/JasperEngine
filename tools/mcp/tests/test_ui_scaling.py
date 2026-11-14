"""UI scaling and text (docs/superpowers/specs/2026-10-31-ui-scaling-text-design.md): the Canvas
Scaler of a UI scene (Expand at 1920x1080) seen through the Game view's fixed resolutions (4:3 and
20:9: the UI size, a stretch-anchored panel filling the view, a centred element hit at the view's
centre), the HTML document laid out at the same UI size, the project and scene settings over MCP,
create_ui's outline and shadow, a Text Effect added with add_component animating its text, and a
UI Particle System playing at time scale 0, the Game view's fixed resolutions (render size, Screen.dpi in
edit mode and Play, aspect, custom ones in the workspace).
"""

from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "UI Scaling"}
CI = True

S: dict = {}

HTML = """<!DOCTYPE html>
<html><head><style>
html, body { margin: 0; width: 100%; height: 100%; }
</style></head><body><p id="label">HUD</p></body></html>
"""


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={
        "canvas_scaler": {"mode": "scale_with_screen_size", "reference": [1920, 1080], "match_mode": "expand"}})
    await ed.call("create_ui", scene=S["scene"], root={
        "type": "image", "name": "Backdrop", "color": [1, 0, 0, 1], "layout": {"anchor": "stretch"},
        "children": [{"type": "image", "name": "Centre", "color": [0, 1, 0, 1],
                      "layout": {"anchor": "center", "size": [200, 100]}},
                     {"type": "text", "name": "Title", "text": "WAVES", "font_size": 64,
                      "outline": {"width": 3, "color": [0, 0, 0, 1]},
                      "shadow": {"offset": [2, 2], "color": [0, 0, 0, 0.5], "softness": 4},
                      "layout": {"anchor": "top_left", "position": [40, 40]}}]})
    (ed.project / "assets/ui").mkdir(parents=True, exist_ok=True)
    (ed.project / "assets/ui/hud.html").write_text(HTML)
    await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/hud.html", name="HudDocument")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


def component_values(entity: dict, component: str) -> dict:
    components = entity.get("components", entity)
    if isinstance(components, list):
        for item in components:
            if item.get("type") == component or item.get("name") == component:
                return item.get("properties", item.get("values", item))
    return components[component]


def pixel(rows, channels, x, y):
    return tuple(rows[y][x * channels:x * channels + 3])


async def test_settings_round_trip_and_defaults(ed: Editor):
    s = await setup(ed)
    settings = (await ed.call("get_scene_settings", scene=s["scene"]))
    scaler = settings["settings"]["canvas_scaler"] if "settings" in settings else settings["canvas_scaler"]
    ed.check_eq(scaler["mode"], "scale_with_screen_size")
    ed.check_eq(scaler["match_mode"], "expand")
    ed.check_eq(scaler["use_project_default"], False, "fields given override the project default")

    # the project default: absent from project.yaml until set (old projects unchanged)
    project = await ed.call("get_project_settings", keys=["graphics.ui_scale"])
    ed.check("follow_game_canvas" in str(project), f"the default follows the game canvas: {project}")
    await ed.call("set_project_settings", values={"graphics.ui_scale": {"mode": "constant_pixel_size", "scale_factor": 2}})
    project = await ed.call("get_project_settings", keys=["graphics.ui_scale"])
    ed.check("constant_pixel_size" in str(project), f"set: {project}")
    await ed.check_error("set_project_settings", values={"graphics.ui_scale": {"mode": "fit"}}, contains="follow_game_canvas")
    await ed.check_error("set_scene_settings", scene=s["scene"], settings={"canvas_scaler": {"match": 3}}, contains="match")
    await ed.call("set_project_settings", values={"graphics.ui_scale": {"mode": "follow_game_canvas", "scale_factor": 1}})

    # create_ui's outline and shadow reached the Text
    props = await ed.call("get_entity", scene=s["scene"], entity="Title", components=["TextComponent"])
    values = component_values(props, "text")
    ed.check_eq(values["outlineWidth"], 3)
    ed.check_eq(values["shadowEnabled"], True)
    ed.check_eq(values["shadowSoftness"], 4)


async def test_expand_lays_the_ui_out_at_each_resolution(ed: Editor):
    s = await setup(ed)
    for resolution, expected in (("1440x1080", [1920, 1440]), ("2400x1080", [2400, 1080])):
        view = await ed.call("configure_game_view", resolution=resolution)
        ed.check_eq(view["resolution"]["width"], int(resolution.split("x")[0]), f"fixed resolution: {view}")
        await ed.call("play", action="start", scene=s["scene"])
        try:
            await ed.call("wait", frames=5)
            size = (await lua(ed, "local c = scene:getCanvasScaler().canvasSize; return {c.x, c.y}"))[0]
            ed.check_eq([round(v) for v in size], expected, f"UI size at {resolution}")
            factor = (await lua(ed, "return scene:getCanvasScaler().appliedScaleFactor"))[0]
            ed.check(abs(factor - (0.75 if resolution == "1440x1080" else 1.0)) < 1e-3, f"scale {factor}")

            # the HTML document lays out at the UI size too
            rect = (await lua(ed, "local d = object('HudDocument', 'UIDocument'); local r = d.body:getBoundingClientRect(); return {r.width, r.height}"))[0]
            ed.check_eq([round(v) for v in rect], expected, f"body at {resolution}")

            # the stretch-anchored backdrop fills the view: red corners, the centred element green
            shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 480})
            width, height, channels, rows = decode_png(shot.images[0][0])
            for x, y in ((2, 2), (width - 3, 2), (2, height - 3), (width - 3, height - 3)):
                r, g, b = pixel(rows, channels, x, y)
                ed.check(r > 200 and g < 60, f"backdrop at ({x},{y}) {resolution}: {(r, g, b)}")
            r, g, b = pixel(rows, channels, width // 2, height // 2)
            ed.check(g > 200 and r < 60, f"centre element at {resolution}: {(r, g, b)}")
        finally:
            await ed.call("play", action="stop")
    await ed.call("configure_game_view", resolution="free")


async def test_text_effect_animates_the_title(ed: Editor):
    s = await setup(ed)
    await ed.call("add_component", scene=s["scene"], entity="Title", component="TextEffect")
    await ed.call("set_properties", scene=s["scene"], entity="Title",
                  values={"TextEffectComponent": {"layers[0].type": "wave", "layers[0].amplitude": 0.3, "layers[0].duration": 1.0}})
    props = await ed.call("get_entity", scene=s["scene"], entity="Title", components=["TextEffectComponent"])
    ed.check_eq(component_values(props, "texteffect")["layers[0].type"], "wave")
    await ed.call("configure_game_view", resolution="1920x1080")
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=30, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        first = decode_png((await ed.call_raw("screenshot", {"view": "game", "max_width": 960})).images[0][0])
        await ed.call("wait", frames=8)
        second = decode_png((await ed.call_raw("screenshot", {"view": "game", "max_width": 960})).images[0][0])
        width, height, channels, rows_a = first
        rows_b = second[3]
        # the title (top left, 64 px text at (40, 40) of 1920 wide) moves; the rest does not
        title = [(x, y) for y in range(0, height // 5) for x in range(0, width // 3)]
        rest = [(x, y) for y in range(height // 2, height) for x in range(0, width)]
        changed = sum(1 for x, y in title if pixel(rows_a, channels, x, y) != pixel(rows_b, channels, x, y))
        elsewhere = sum(1 for x, y in rest if pixel(rows_a, channels, x, y) != pixel(rows_b, channels, x, y))
        ed.check(changed > 20, f"the title region changed ({changed} pixels)")
        ed.check_eq(elsewhere, 0, "nothing else changed")
        # Lua sees the same component
        state = (await lua(ed, "local fx = TextEffect(scene, object('Title').entity); return fx:getLayer(1).type == TextEffectType.Wave"))[0]
        ed.check(state, "TextEffect wrapper reads the layer")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", resolution="free")


async def test_ui_particles_play_while_paused(ed: Editor):
    s = await setup(ed)
    made = await ed.call("create_particles", scene=s["scene"], parent="Backdrop", preset="magic")
    system = made.get("entity") or made.get("particle_system") or made
    ed.check(system is not None, f"created: {made}")
    ed.check_eq(made.get("ui"), True, f"UI particles under a UI element (drawn between siblings, clipped by panels): {made}")
    ed.check_eq(made.get("unscaled_time"), True, f"UI particles on unscaled time: {made}")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        unscaled = (await lua(ed, "for _, ps in ipairs({object('Particle System', 'ParticleSystem')}) do return ps.useUnscaledTime end"))[0]
        ed.check_eq(unscaled, True, "UI particles run on unscaled time")
        await lua(ed, "Engine.timeScale = 0")
        await ed.call("wait", seconds=0.5)
        count = (await lua(ed, "return object('Particle System', 'ParticleSystem').particleCount"))[0]
        ed.check(count and count > 0, f"particles spawned at time scale 0: {count}")
    finally:
        await lua(ed, "Engine.timeScale = 1")
        await ed.call("play", action="stop")


async def test_game_view_resolutions_render_size_and_dpi(ed: Editor):
    """a fixed resolution sets the render size and Screen.dpi in Play (the preset's, else the host's), the
    edit-mode screen_dpi equals Play's; an aspect clears the fixed resolution; a custom resolution is kept in
    the editor workspace"""
    s = await setup(ed)
    try:
        await ed.call("configure_game_view", resolution="2400x1080")
        await ed.call("wait", frames=2)
        view = await ed.call("configure_game_view")
        ed.check_eq(view["render_size"], [2400, 1080], f"render size of the preset (Game tab shown or hidden): {view}")
        ed.check_eq(view["screen_dpi"], 400, "the 20:9 phone preset's dpi")
        await ed.call("play", action="start", scene=s["scene"])
        try:
            await ed.call("wait", frames=3)
            ed.check_eq((await lua(ed, "return Screen.dpi"))[0], 400, "Screen.dpi in Play")
        finally:
            await ed.call("play", action="stop")

        # no dpi given: the host's, the same in edit mode and in Play
        view = await ed.call("configure_game_view", resolution="1440x1080")
        edit_dpi = view["screen_dpi"]
        ed.check(edit_dpi > 0, f"the host's dpi in edit mode: {view}")
        await ed.call("play", action="start", scene=s["scene"])
        try:
            await ed.call("wait", frames=3)
            ed.check_eq((await lua(ed, "return Screen.dpi"))[0], edit_dpi, "Play reads the dpi edit mode laid the UI out with")
        finally:
            await ed.call("play", action="stop")

        # an aspect replaces the fixed resolution (as the toolbar's Aspect entries)
        view = await ed.call("configure_game_view", aspect="16:9")
        ed.check_eq(view["resolution"], "free", f"the aspect cleared the resolution: {view}")

        # a named one is saved as a custom resolution in the workspace
        view = await ed.call("configure_game_view", resolution={"width": 1280, "height": 800, "dpi": 206, "name": "Deck"})
        ed.check(any(r.get("name") == "Deck" for r in view["custom_resolutions"]), f"listed: {view}")
        workspace = (ed.project / ".jasperengine/user/workspace.yaml").read_text()
        ed.check("Deck" in workspace and "1280" in workspace, f"kept in the workspace file: {workspace[:400]}")
    finally:
        await ed.call("configure_game_view", resolution="free")


async def test_text_alignment_golden(ed: Editor):
    """S93 R37 / AC43: a Text in a fixed 800 x 400 box drawn Left/Top, Center/Middle and Right/Bottom
    lands where TextMeshPro puts it (ink box against the box edges in the Game view), each set
    through the generic component tools; Auto Size grows a short title to the box."""
    made = await ed.call("create_scene", name="TextAlign", type="UI", path="scenes/TextAlign")
    scene = made["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={
        "type": "image", "name": "Back", "color": [0, 0, 0, 1], "layout": {"anchor": "stretch"},
        "children": [{"type": "text", "name": "Label", "text": "HELLO", "font_size": 64, "text_color": [1, 1, 1, 1],
                      "layout": {"anchor": "center", "size": [800, 400]}}]})
    await ed.call("set_properties", scene=scene, entity="Label",
                  values={"TextComponent": {"fixedWidth": True, "fixedHeight": True}})
    await ed.call("save_scene", scene=scene)
    await ed.call("configure_game_view", resolution="1920x1080")

    async def ink() -> tuple:
        shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 960})
        width, height, channels, rows = decode_png(shot.images[0][0])
        xs, ys = [], []
        for y in range(height):
            for x in range(width):
                r, g, b = pixel(rows, channels, x, y)
                if r > 128 and g > 128 and b > 128:
                    xs.append(x)
                    ys.append(y)
        ed.check(xs, "the text is drawn")
        return min(xs), min(ys), max(xs), max(ys)

    try:
        await ed.call("play", action="start", scene=scene)
        # the 800 x 400 box, centred on the canvas, in the 960 px wide shot of the 1920 x 1080 view
        cw, ch = (await lua(ed, "return {Engine.canvasWidth, Engine.canvasHeight}"))[0]
        f = 960 / cw
        box = ((cw - 800) / 2 * f, (ch - 400) / 2 * f, (cw + 800) / 2 * f, (ch + 400) / 2 * f)
        middle = ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
        placements = {}
        for horizontal, vertical in (("left", "top"), ("center", "middle"), ("right", "bottom")):
            await ed.call("set_properties", scene=scene, entity="Label",
                          values={"TextComponent": {"horizontalAlignment": horizontal, "verticalAlignment": vertical}})
            await ed.call("wait", frames=3)
            placements[horizontal] = await ink()
        x0, y0, x1, y1 = placements["left"]
        ed.check(abs(x0 - box[0]) <= 6 and abs(y0 - box[1]) <= 12, f"Left / Top at the box's top-left {box}: {placements['left']}")
        x0, y0, x1, y1 = placements["center"]
        ed.check(abs((x0 + x1) / 2 - middle[0]) <= 4 and abs((y0 + y1) / 2 - middle[1]) <= 10, f"Center / Middle in the box's middle {middle}: {placements['center']}")
        x0, y0, x1, y1 = placements["right"]
        ed.check(abs(x1 - box[2]) <= 6 and box[3] - 24 <= y1 <= box[3] + 1, f"Right / Bottom at the box's bottom-right {box}: {placements['right']}")
        ed.check_eq(placements["left"][3] - placements["left"][1], placements["right"][3] - placements["right"][1], "same glyphs, moved")

        # Auto Size: the largest whole size whose text fits the box
        await ed.call("set_properties", scene=scene, entity="Label",
                      values={"TextComponent": {"autoSize": True, "fontSizeMin": 10, "fontSizeMax": 400,
                                                "horizontalAlignment": "left", "verticalAlignment": "top"}})
        await ed.call("wait", frames=3)
        size = (await lua(ed, "return object('Label', 'Text').effectiveFontSize"))[0]
        ed.check(size > 64, f"grown past its fontSize: {size}")
        x0, y0, x1, y1 = await ink()
        ed.check(x1 - x0 > 0.8 * (box[2] - box[0]) and x1 <= box[2] + 2 and y1 <= box[3] + 2,
                 f"it fills the box and stays in it: {(x0, y0, x1, y1)}")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", resolution="free")
