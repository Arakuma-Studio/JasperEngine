"""An HTML/CSS document (UIDocument component, phase 0) on screen, through MCP.

Visual (the component registration is test_html_ui_component.py's): writes ui/basic.html +
ui/basic.css, adds the component to a GameObject of a new UI scene and probes Game view
screenshots: box colors, text, the transparent body, enabled on/off, the order of two
documents (sortOrder), the same pixels after save / close / reopen, :hover in play mode
from send_input, clicks consumed over document elements but not over the transparent body,
and a missing file (a Console error naming it, no crash, nothing drawn). The last test
leaves the scene saved as the start scene, so the kept project (JASPER_KEEP_SCRATCH=1) is
ready for tools/smoke-test.sh and the Web export check.

Runs on a scratch copy of the fixture project (canvas 1280 x 720, letterbox). Coordinates
below are canvas pixels; screenshots map them with canvas_rect_in_image.
"""

import struct
import zlib

from harness import Editor

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/basic.html"
OVER = "assets/ui/over.html"

HTML = """<html>
<head>
    <title>basic</title>
    <link type="text/css" href="basic.css"/>
</head>
<body>
    <div id="red" class="box"></div>
    <div id="blue" class="box"></div>
    <div id="label">HTML UI on screen</div>
    <div id="button">Play</div>
</body>
</html>
"""

# RCSS (phase 0): no user-agent stylesheet, so display: block is explicit; colors #RRGGBB(AA)
CSS = """body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 16px; color: #ffffff; }
div { display: block; }
.box { position: absolute; }
#red { left: 40px; top: 40px; width: 200px; height: 100px; background-color: #ff0000; }
#blue { left: 400px; top: 200px; width: 160px; height: 80px; background-color: #0000ff; }
#label { position: absolute; left: 40px; top: 170px; font-size: 32px; color: #ffffff; }
#button { position: absolute; left: 40px; top: 240px; width: 160px; height: 48px; line-height: 48px;
          text-align: center; font-size: 24px; color: #000000; background-color: #808080; }
#button:hover { background-color: #00ff00; }
"""

# a second document over the red box
OVER_HTML = """<html>
<head>
    <title>over</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        #over { display: block; position: absolute; left: 140px; top: 70px; width: 200px; height: 100px;
                background-color: #ff00ff; }
    </style>
</head>
<body>
    <div id="over"></div>
</body>
</html>
"""

COUNTER = """---@class ClickCounter : ScriptInstance
local ClickCounter = {
    properties = {
        { name = "downs", displayName = "Downs", type = "int", default = 0 },
    }
}

function ClickCounter:init()
    RegisterEngineEvent(self, "onMouseDown")
end

function ClickCounter:onMouseDown(button, x, y, mods)
    self.downs = self.downs + 1
end

return ClickCounter
"""

RED = (255, 0, 0)
BLUE = (0, 0, 255)
GREEN = (0, 255, 0)
GRAY = (128, 128, 128)
MAGENTA = (255, 0, 255)

# canvas probes
P_RED = (140, 90)
P_BLUE = (480, 240)
P_BUTTON = (120, 264)
P_EMPTY = (900, 500)
P_OVERLAP = (190, 110)
LABEL = (40, 170, 340, 210)   # x0, y0, x1, y1


# -- PNG and pixels ------------------------------------------------------------------------------


def decode_png(data: bytes):
    """(width, height, channels, rows) of an 8-bit RGB/RGBA PNG."""
    assert data.startswith(b"\x89PNG\r\n\x1a\n"), "not a PNG"
    pos, width, height, color_type, idat = 8, 0, 0, 2, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", chunk[:10])
            assert depth == 8, f"bit depth {depth}"
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    ch = {2: 3, 6: 4}[color_type]
    raw, stride, rows, prev, i = zlib.decompress(idat), width * ch, [], bytearray(width * ch), 0
    for _ in range(height):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, ch, rows


class Shot:
    """A Game view screenshot addressed in canvas pixels."""

    def __init__(self, png: bytes, info: dict):
        self.png = png
        self.w, self.h, self.ch, self.rows = decode_png(png)
        rect, canvas = info["canvas_rect_in_image"], info["canvas"]
        self.rx, self.ry = rect["x"], rect["y"]
        self.sx, self.sy = rect["width"] / canvas["width"], rect["height"] / canvas["height"]

    def px(self, cx: float, cy: float) -> tuple:
        x = min(self.w - 1, max(0, int(self.rx + (cx + 0.5) * self.sx)))
        y = min(self.h - 1, max(0, int(self.ry + (cy + 0.5) * self.sy)))
        i = x * self.ch
        return tuple(self.rows[y][i:i + 3])

    def region(self, x0, y0, x1, y1):
        """Image pixels of a canvas rectangle."""
        ix0, iy0 = int(self.rx + x0 * self.sx), int(self.ry + y0 * self.sy)
        ix1, iy1 = int(self.rx + x1 * self.sx), int(self.ry + y1 * self.sy)
        for y in range(max(0, iy0), min(self.h, iy1)):
            row = self.rows[y]
            for x in range(max(0, ix0), min(self.w, ix1)):
                yield tuple(row[x * self.ch:x * self.ch + 3])


def near(a, b, tol=12) -> bool:
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def check_color(ed: Editor, shot: Shot, point, want, what: str, tol=12):
    got = shot.px(*point)
    ed.check(near(got, want, tol), f"{what}: pixel at canvas {point} is {got}, expected {want} +-{tol}")


def ink_count(shot: Shot, rect, background) -> int:
    """Pixels of a canvas rectangle away from the background color."""
    return sum(1 for p in shot.region(*rect) if not near(p, background, 40))


async def shoot(ed: Editor, label: str, scene=None) -> Shot:
    args = {"view": "game", "max_width": 1280}
    if scene is not None:
        args["scene"] = scene
    shot = await ed.call_raw("screenshot", args)
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def html_ui_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


async def doc_entity(ed: Editor, name: str):
    found = await ed.call("find_entities", scene=S["scene"], name=name)
    ed.check_eq(found["count"], 1, f"one {name}")
    return found["entities"][0]["id"]


# -- tests ---------------------------------------------------------------------------------------


async def test_document_on_screen(ed: Editor):
    """Steps 1-3: files, scene, component; boxes, text and the transparent body on screen."""
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiBasic", type="UI")
    S["scene"] = made["scene"]["id"]
    # a dark background: the UI scene's default gray is too close to the button's #808080
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    for path, content in ((DOC, HTML), ("assets/ui/basic.css", CSS), (OVER, OVER_HTML)):
        await ed.call("write_file", path=path, content=content)

    # the empty scene: its background is what the transparent body must show
    S["empty"] = await shoot(ed, "empty", S["scene"])
    S["background"] = S["empty"].px(*P_EMPTY)
    ed.check(near(S["empty"].px(*P_RED), S["background"], 4), "the empty scene is flat")
    ed.check(near(S["background"], (30, 30, 30), 6), f"the dark background: {S['background']}")

    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["menu"], component="UIDocument",
                  values={"uidocument": {"filename": DOC}})

    shot = await shoot(ed, "step3", S["scene"])
    check_color(ed, shot, P_RED, RED, "red box center")
    check_color(ed, shot, P_BLUE, BLUE, "blue box center")
    check_color(ed, shot, P_BUTTON, GRAY, "button (normal)")
    # edges in canvas pixels: just inside and just outside the red box
    check_color(ed, shot, (44, 44), RED, "red box top-left corner")
    check_color(ed, shot, (236, 136), RED, "red box bottom-right corner")
    check_color(ed, shot, (36, 90), S["background"], "left of the red box")
    check_color(ed, shot, (140, 146), S["background"], "below the red box")
    # the text line: white glyphs over the background
    ink = ink_count(shot, LABEL, S["background"])
    ed.check(ink > 300, f"the text line has {ink} non-background pixels")
    white = sum(1 for p in shot.region(*LABEL) if near(p, (255, 255, 255), 20))
    ed.check(white > 50, f"the text is white: {white} white pixels")
    # the transparent body shows the scene everywhere else
    for point in (P_EMPTY, (700, 100), (1200, 650), (300, 400)):
        check_color(ed, shot, point, S["background"], f"background at {point}", tol=4)
    S["step3"] = shot

    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")


async def test_enabled_toggle(ed: Editor):
    """Step 4: enabled false hides the document, true shows it again."""
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"enabled": False}})
    shot = await shoot(ed, "step4-disabled", S["scene"])
    check_color(ed, shot, P_RED, S["background"], "disabled: red box gone", tol=4)
    check_color(ed, shot, P_BLUE, S["background"], "disabled: blue box gone", tol=4)
    ed.check(ink_count(shot, LABEL, S["background"]) == 0, "disabled: the text is gone")

    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"enabled": True}})
    shot = await shoot(ed, "step4-enabled", S["scene"])
    check_color(ed, shot, P_RED, RED, "enabled again: red box")
    check_color(ed, shot, P_BLUE, BLUE, "enabled again: blue box")


async def test_sort_order(ed: Editor):
    """Step 5: a second document with sortOrder 1 is on top of the first; swapped, under it."""
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Overlay")
    S["over"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["over"], component="UIDocument",
                  values={"uidocument": {"filename": OVER, "sortOrder": 1}})
    shot = await shoot(ed, "step5-over", S["scene"])
    check_color(ed, shot, P_OVERLAP, MAGENTA, "sortOrder 1 over 0: the second document on top")
    check_color(ed, shot, (60, 60), RED, "the red box outside the overlap")
    check_color(ed, shot, (320, 160), MAGENTA, "the second document outside the overlap")

    await ed.call("set_properties", scene=S["scene"], entity=S["over"], values={"uidocument": {"sortOrder": 0}})
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"sortOrder": 1}})
    shot = await shoot(ed, "step5-under", S["scene"])
    check_color(ed, shot, P_OVERLAP, RED, "swapped: the first document on top")
    check_color(ed, shot, (320, 160), MAGENTA, "swapped: the second document still drawn")

    # back to one document for the next steps
    await ed.call("delete_entities", scene=S["scene"], entities=[S["over"]])
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"sortOrder": 0}})
    shot = await shoot(ed, "step5-removed", S["scene"])
    check_color(ed, shot, P_OVERLAP, RED, "the second document's GameObject deleted")


async def test_save_and_reopen(ed: Editor):
    """Step 6: save, close, reopen: the same pixels as step 3."""
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiBasic")
    S["path"] = saved["scene"]["path"]
    text = (ed.project / S["path"]).read_text()
    ed.check("uidocument" in text and "ui/basic.html" in text, f"saved scene lacks the document: {text[:500]}")

    await ed.call("close_scene", scene=S["scene"])
    opened = await ed.call("open_scene", scene=S["path"])
    S["scene"] = opened["scene"]["id"] if isinstance(opened, dict) and "scene" in opened else S["scene"]
    S["menu"] = await doc_entity(ed, "Menu")

    shot = await shoot(ed, "step6", S["scene"])
    before = S["step3"]
    for point in (P_RED, P_BLUE, P_BUTTON, P_EMPTY, (44, 44), (36, 90), (700, 100)):
        a, b = before.px(*point), shot.px(*point)
        ed.check(near(a, b, 2), f"reopened: pixel at canvas {point} is {b}, was {a}")
    a, b = ink_count(before, LABEL, S["background"]), ink_count(shot, LABEL, S["background"])
    ed.check(abs(a - b) <= max(10, a // 20), f"reopened: text pixels {b}, were {a}")


async def test_hover_and_clicks_in_play_mode(ed: Editor):
    """Step 7: :hover from send_input in play mode; clicks over document elements are the
    document's, clicks over the transparent body reach the game."""
    await ed.call("create_script", kind="lua", name="ClickCounter", content=COUNTER, scene=S["scene"], entity=S["menu"])

    started = await ed.call("play", action="start", scene=S["scene"])
    ed.check(started, "play started")
    try:
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "move", "x": P_BUTTON[0], "y": P_BUTTON[1]}],
                                             "space": "canvas", "frames_after": 3, "screenshot": "game", "max_width": 1280})
        ed.check(not r.is_error, f"send_input: {r.text[:300]}")
        shot = await shoot(ed, "step7-hover")
        check_color(ed, shot, P_BUTTON, GREEN, "button under the pointer (:hover)")
        check_color(ed, shot, P_RED, RED, "red box while playing")

        await ed.call("send_input", events=[{"type": "mouse", "action": "move", "x": P_EMPTY[0], "y": P_EMPTY[1]}],
                      space="canvas", frames_after=3)
        shot = await shoot(ed, "step7-away")
        check_color(ed, shot, P_BUTTON, GRAY, "button after the pointer left (normal)")

        # the game's onMouseDown: not over the button (the document takes it), yes over the body
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_BUTTON[0], "y": P_BUTTON[1]}],
                      space="canvas", frames_after=2)
        over = (await ed.call("eval_lua", code="return script('Menu', 'ClickCounter').downs"))["values"]
        ed.check_eq(over, [0], "a click over the button reached the game")
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_EMPTY[0], "y": P_EMPTY[1]}],
                      space="canvas", frames_after=2)
        body = (await ed.call("eval_lua", code="return script('Menu', 'ClickCounter').downs"))["values"]
        ed.check_eq(body, [1], "a click over the transparent body did not reach the game")
    finally:
        await ed.call("play", action="stop")

    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors while playing: {errors}")


async def test_missing_file(ed: Editor):
    """Step 8: a missing file logs an error naming it, nothing crashes, nothing is drawn."""
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"],
                  values={"uidocument": {"filename": "assets/ui/missing.html"}})
    shot = await shoot(ed, "step8-missing", S["scene"])
    await ed.check_alive()
    errors = await html_ui_errors(ed)
    ed.check(any("ui/missing.html" in e for e in errors), f"no [HTML UI] error naming the file: {errors}")
    check_color(ed, shot, P_RED, S["background"], "missing file: no red box", tol=4)
    check_color(ed, shot, P_BLUE, S["background"], "missing file: no blue box", tol=4)
    ed.check(ink_count(shot, LABEL, S["background"]) == 0, "missing file: no text")

    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": DOC}})
    shot = await shoot(ed, "step8-fixed", S["scene"])
    check_color(ed, shot, P_RED, RED, "the document is back")
    errors = await html_ui_errors(ed)
    ed.check(not errors, f"errors after fixing the filename: {errors}")


async def test_edited_css_shows_after_reload(ed: Editor):
    """An edited stylesheet shows once the document loads again (here: its filename set
    again), although RmlUi stays initialised and caches parsed stylesheets."""
    orange = CSS.replace("#red { left: 40px; top: 40px; width: 200px; height: 100px; background-color: #ff0000; }",
                         "#red { left: 40px; top: 40px; width: 200px; height: 100px; background-color: #ff8000; }")
    ed.check(orange != CSS, "the CSS edit applies")
    await ed.call("write_file", path="assets/ui/basic.css", content=orange)
    for value in ("", DOC):
        await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": value}})
    shot = await shoot(ed, "edited-css", S["scene"])
    check_color(ed, shot, P_RED, (255, 128, 0), "the edited stylesheet after a reload")

    await ed.call("write_file", path="assets/ui/basic.css", content=CSS)
    for value in ("", DOC):
        await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": value}})
    shot = await shoot(ed, "restored-css", S["scene"])
    check_color(ed, shot, P_RED, RED, "the restored stylesheet after a reload")


async def test_saved_as_start_scene(ed: Editor):
    """Leaves the project ready for the exported-game and Web checks: the document scene
    saved and first in Scenes in Build."""
    await ed.call("save_scene", scene=S["scene"])
    built = await ed.call("edit_build_scenes", action="set_start", scene=S["path"])
    start = [s for s in built["scenes_in_build"] if s.get("start")]
    ed.check(start and start[0]["path"] == S["path"], f"start scene: {built}")
    await ed.call("save_project")
    ed.check("ui/basic.html" in (ed.project / S["path"]).read_text(), "saved scene keeps the document")
