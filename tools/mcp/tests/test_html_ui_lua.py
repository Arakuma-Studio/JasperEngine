"""HTML UI scripting through MCP (phase 1): the Lua element API and data binding (p1-lua),
Lottie control (p1-vector) and the anime animation API (p1-anim), each in its own section.

Runs on a scratch copy of the fixture project (canvas 1280 x 720, letterbox).
"""

import struct
import zlib

from harness import Editor

TIMEOUT = 900

S: dict = {}


# -- PNG and pixels (as test_html_ui_basic.py) ---------------------------------------------------


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
        self.w, self.h, self.ch, self.rows = decode_png(png)
        rect, canvas = info["canvas_rect_in_image"], info["canvas"]
        self.rx, self.ry = rect["x"], rect["y"]
        self.sx, self.sy = rect["width"] / canvas["width"], rect["height"] / canvas["height"]

    def px(self, cx: float, cy: float) -> tuple:
        x = min(self.w - 1, max(0, int(self.rx + (cx + 0.5) * self.sx)))
        y = min(self.h - 1, max(0, int(self.ry + (cy + 0.5) * self.sy)))
        i = x * self.ch
        return tuple(self.rows[y][i:i + 3])


def near(a, b, tol=12) -> bool:
    return all(abs(x - y) <= tol for x, y in zip(a, b))


async def shoot(ed: Editor, label: str, scene=None) -> Shot:
    args = {"view": "game", "max_width": 1280}
    if scene is not None:
        args["scene"] = scene
    shot = await ed.call_raw("screenshot", args)
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-lua-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def html_ui_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


async def lua(ed: Editor, code: str, edit=False):
    args = {"code": code}
    if edit:
        args["allow_edit_mode"] = True
    return (await ed.call("eval_lua", **args))["values"]


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

RED = (255, 0, 0)
BLUE = (0, 0, 255)


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
    shot = await shoot(ed, "anime-start", S["anime_scene"])
    ed.check(near(shot.px(90, 90), RED), f"the box before animating: {shot.px(90, 90)}")
    S["anime_bg"] = shot.px(900, 500)


async def test_anime_translate_and_stagger(ed: Editor):
    """anime.animate on translateX moves the box (pixels and utils.get); a staggered opacity."""
    await ed.call("play", action="start", scene=S["anime_scene"])
    try:
        await lua(ed, "box = anime.animate('#box', { translateX = 400, duration = 400, ease = 'outQuad' })")
        r = await ed.call_raw("wait", {"until": "box.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the translateX animation completes: {r.text[:300]}")
        ed.check_eq(await lua(ed, "return anime.utils.get('#box', 'translateX')"), ["400px"], "translateX at the end")
        shot = await shoot(ed, "anime-moved")
        ed.check(near(shot.px(490, 90), RED), f"the box moved by 400px: {shot.px(490, 90)}")
        ed.check(near(shot.px(90, 90), S["anime_bg"], 6), f"its old place is empty: {shot.px(90, 90)}")

        await lua(ed, "dots = anime.animate('.dot', { opacity = 0, delay = anime.stagger(100), duration = 200, ease = 'linear' })")
        ed.check_eq(await lua(ed, "return dots.duration"), [400], "three staggered targets from the selector")
        r = await ed.call_raw("wait", {"until": "dots.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the staggered animation completes: {r.text[:300]}")
        shot = await shoot(ed, "anime-faded")
        for x in (60, 160, 260):
            ed.check(near(shot.px(x, 320), S["anime_bg"], 8), f"dot at {x} faded out: {shot.px(x, 320)}")
    finally:
        await ed.call("play", action="stop")


async def test_anime_timeline_labels_and_clocks(ed: Editor):
    """A timeline with labels and calls on a plain table; clock = 'game' freezes with the game,
    the default UI clock does not; play stop cancels what is left and frees the callbacks."""
    await ed.call("play", action="start", scene=S["anime_scene"])
    try:
        values = await lua(ed, """
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
        ed.check_eq(await lua(ed, "return t.v, log[1]"), [0, "called"], "timeline end state")

        await lua(ed, """
            Engine.timeScale = 0
            game = { v = 0 }; ui = { v = 0 }
            ga = anime.animate(game, { v = 100, duration = 300, ease = 'linear', clock = 'game' })
            ua = anime.animate(ui, { v = 100, duration = 300, ease = 'linear' })
        """)
        r = await ed.call_raw("wait", {"until": "ua.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the UI-clock animation runs at time scale 0: {r.text[:300]}")
        ed.check_eq(await lua(ed, "return game.v"), [0], "the game-clock animation is frozen")
        await lua(ed, "Engine.timeScale = 1")
        r = await ed.call_raw("wait", {"until": "ga.completed", "timeout_seconds": 10})
        ed.check(not r.is_error, f"the game-clock animation runs again: {r.text[:300]}")

        bad = await ed.call_raw("eval_lua", {"code": "anime.createTimer({ clock = 'wall' })"})
        ed.check("clock must be" in bad.text, f"an invalid clock is an error: {bad.text[:300]}")

        await lua(ed, "forever = anime.animate(t, { v = 1, loop = true, alternate = true, onUpdate = function() end })")
    finally:
        await ed.call("play", action="stop")
        await lua(ed, "Engine.timeScale = 1", edit=True)

    # the Lua state outlives the play: the handle is still there, cancelled
    ed.check_eq(await lua(ed, "return forever.cancelled, forever.paused", edit=True), [True, True],
                "play stop cancelled the endless animation")
    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")
