"""Line Renderer and Trail Renderer (lines and trails spec 9, docs/mcp/guide/lines-trails.md):

- an old-format hairline LineRenderer renders as on main (golden/lines_trails_hairline.png, made
  with the main checkout's editor: JASPER_UPDATE_GOLDEN=1 JASPER_EDITOR=<main editor>);
- create_entity line_renderer / trail_renderer defaults (Unity's: a 2-point world-space polyline
  of width 0.1, a fading trail), set_properties / get_entity with the curve and gradient shapes
  (number, keys array, object; one colour, keys with "#RRGGBB" and linear colours), their
  errors, undo, the texture shortcut, the ribbon read-back;
- pixels: a 0.5-wide red polyline (its thickness against its length at a known distance, the
  same seen from a camera turned 90 degrees around it: View alignment; edge-on with Transform Z
  under 2 pixels), a 6-pixel line at two distances, round caps past the end vs butt caps, a
  two-colour texture stretched vs tiled;
- Play: a trail behind an object moving at a known speed spans the last `time` seconds of its path
  and fades to the tail, clear() empties it; Lua round trips (positions, curve and gradient
  copies, startWidth/startColor, the 0-based index error);
- a 2D scene: a trail drawn in the hierarchy order with sprites;
- check: line-texture-no-uv and trail-zero-time.
"""

from __future__ import annotations

import os
import struct
import zlib
from pathlib import Path

from harness import Editor, find_key, skip

import pngdiff
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "LinesTrailsTest"}
TIMEOUT = 900

HERE = Path(__file__).resolve().parent
GOLDEN = HERE / "golden" / "lines_trails_hairline.png"
SCENE = "scenes/Lines.scene"
SCENE_2D = "scenes/Lines2D.scene"
S: dict = {}

RED = (255, 0, 0)
TEXTURE = "assets/textures/halves.png"


def png_rgb(width: int, height: int, pixels) -> bytes:
    """A small RGB PNG (rows of (r, g, b))."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


async def shot(ed: Editor, label: str) -> tuple:
    r = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1024})
    ed.check(not r.is_error and r.images, f"{label}: no screenshot: {r.text[:300]}")
    data = r.images[0][0]
    if ed.scratch:
        (ed.scratch.root / f"lines-{label}.png").write_bytes(data)
    return data, r.value or {}


def red_runs(data: bytes, column: int | None = None, row: int | None = None, tol: int = 60):
    """Lengths of the red runs of an image column (or row), longest first."""
    w, h, ch, rows = decode_png(data)
    if column is not None:
        samples = [tuple(rows[y][column * ch:column * ch + 3]) for y in range(h)]
    else:
        samples = [tuple(rows[row][x * ch:x * ch + 3]) for x in range(w)]
    runs, n = [], 0
    for px in samples + [(0, 0, 0)]:
        if px[0] > 255 - tol and px[1] < tol and px[2] < tol:
            n += 1
        elif n:
            runs.append(n)
            n = 0
    return sorted(runs, reverse=True)


def red_extent(data: bytes, tol: int = 60):
    """(x0, y0, x1, y1) of the red pixels."""
    w, h, ch, rows = decode_png(data)
    xs, ys = [], []
    for y in range(0, h):
        line = rows[y]
        for x in range(0, w):
            r, g, b = line[x * ch], line[x * ch + 1], line[x * ch + 2]
            if r > 255 - tol and g < tol and b < tol:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


async def props(ed: Editor, entity, component: str, names: list[str]) -> dict:
    got = await ed.call("get_entity", entity=entity, components=[component], properties=names)
    return got["components"][component]


async def set_line(ed: Editor, entity, values: dict, component: str = "linerenderer"):
    return await ed.call("set_properties", entity=entity, values={component: values})


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "get_entity", "screenshot", "set_main_camera",
                     "delete_entities", "get_scene_tree", "undo", "redo", "play", "eval_lua", "wait", "check_shader", "set_transform")
    await ed.call("create_scene", name="Lines", path=SCENE)
    # a black view: no sky, no sun
    tree = await ed.call("get_scene_tree", format="json")
    def walk(nodes):
        for node in nodes or []:
            yield node
            yield from walk(node.get("children"))
    nodes = find_key(tree, "entities", "roots", "children") or []
    for node in list(walk(nodes)):
        if node.get("name") not in ("View",):
            await ed.call("delete_entities", entities=[str(node["id"])])
    cam = find_key(await ed.call("create_entity", type="camera", name="View", position=[0, 0, 10]), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)
    S["scene"] = SCENE
    S["camera"] = cam
    # a red | blue texture for the texture modes
    texture = ed.project / "assets" / "textures" / "halves.png"
    texture.parent.mkdir(parents=True, exist_ok=True)
    texture.write_bytes(png_rgb(2, 1, [[(255, 0, 0), (0, 0, 255)]]))


async def test_old_hairline_matches_main(ed: Editor):
    """A Line Renderer as main wrote it: Segments, no width, two coloured segments."""
    made = await ed.call("create_entity", type="line_renderer", name="Hairline")
    line = find_key(made, "entity_id", "id")
    current = await props(ed, line, "linerenderer", ["*"])
    values = {"lines": [{"pointA": [-3, -1, 0], "colorA": [1, 0, 0, 1], "pointB": [3, -1, 0], "colorB": [0, 1, 0, 1]},
                        {"pointA": [-3, 1, 0], "colorA": [0, 0, 1, 1], "pointB": [3, 2, 0], "colorB": [1, 1, 1, 1]}]}
    # this editor's new lines are Unity's polylines: make it the old component again
    if "widthMultiplier" in current:
        values.update({"mode": "segments", "widthMultiplier": 0, "useWorldSpace": False, "positions": []})
    await set_line(ed, line, values)
    data, _ = await shot(ed, "hairline")
    if os.environ.get("JASPER_UPDATE_GOLDEN") == "1":
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_bytes(data)
        print(f"     [golden] wrote {GOLDEN}")
    else:
        ed.check(GOLDEN.is_file(), f"missing committed golden image: {GOLDEN}")
        ok, fraction, diff = pngdiff.compare(pngdiff.decode(GOLDEN.read_bytes()), pngdiff.decode(data), max_fraction=0.001)
        if not ok and ed.scratch:
            (ed.scratch.root / "lines-hairline-diff.png").write_bytes(diff)
        ed.check(ok, f"the hairline differs from main's ({GOLDEN.name}) in {fraction:.4f} of the pixels")
    await ed.call("delete_entities", entity=line)
    if "widthMultiplier" not in current:
        skip("the main editor made the golden image; the rest needs widths")


async def test_create_defaults_and_shapes(ed: Editor):
    made = await ed.call("create_entity", type="line_renderer", name="Laser")
    laser = find_key(made, "entity_id", "id")
    values = await props(ed, laser, "linerenderer", ["*"])
    ed.check_eq(values["mode"], "polyline", "a new line is Unity's polyline")
    ed.check_eq(values["useWorldSpace"], True, "in world space")
    ed.check(abs(values["widthMultiplier"] - 0.1) < 1e-6, f"width 0.1: {values['widthMultiplier']}")
    ed.check_eq(values["positions"], [[0, 0, 0], [0, 0, 1]], "two points")
    ed.check_eq(values["widthCurve"]["keys"], [[0, 1]], "a constant curve")

    # the curve and gradient shapes
    await set_line(ed, laser, {"widthCurve": [[0, 1], [1, 0.25, -0.75, -0.75]],
                               "colorGradient": {"mode": "blend", "colors": [[0, "#FF9600"], [1, [0.5, 0.25, 0]]], "alphas": [[0, 1], [1, 0]]}})
    values = await props(ed, laser, "linerenderer", ["widthCurve", "colorGradient"])
    ed.check_eq(values["widthCurve"]["keys"], [[0, 1], [1, 0.25, -0.75, -0.75]], "keys read back")
    ed.check_eq(values["widthCurve"]["pre"], "clamp", "pre wrap")
    ed.check_eq(values["colorGradient"]["colors"][0], [0, "#FF9600"], "an exact sRGB colour reads back as hex")
    ed.check_eq(values["colorGradient"]["colors"][1], [1, [0.5, 0.25, 0]], "a linear colour as numbers")
    ed.check_eq(values["colorGradient"]["alphas"], [[0, 1], [1, 0]], "alphas")

    await set_line(ed, laser, {"widthCurve": 0.5, "colorGradient": "#FF000080"})
    values = await props(ed, laser, "linerenderer", ["widthCurve", "colorGradient"])
    ed.check_eq(values["widthCurve"]["keys"], [[0, 0.5]], "a number is a constant curve")
    ed.check_eq(values["colorGradient"]["colors"], [[0, "#FF0000"]], "one colour")
    ed.check(abs(values["colorGradient"]["alphas"][0][1] - 128 / 255) < 1e-3, f"its alpha: {values['colorGradient']}")
    await set_line(ed, laser, {"widthCurve": {"keys": [[0, 0, 0, "inf"], [1, 1]], "post": "loop"}})
    values = await props(ed, laser, "linerenderer", ["widthCurve"])
    ed.check_eq(values["widthCurve"]["keys"][0], [0, 0, 0, "inf"], "a step tangent")
    ed.check_eq(values["widthCurve"]["post"], "loop", "post wrap")

    # wrong shapes are refused with the expected shape
    for bad, what in (({"widthCurve": {"keys": [[0]]}}, "keys[0]"), ({"widthCurve": "x"}, "curve"),
                      ({"colorGradient": {"colors": [[2, "#FFFFFF"]]}}, "0..1"), ({"colorGradient": {"oops": 1}}, "unknown key"),
                      ({"textureMode": "spiral"}, "stretch")):
        r = await ed.call_raw("set_properties", {"entity": laser, "values": {"linerenderer": bad}})
        ed.check(r.is_error and what in r.text, f"{bad} refused naming {what}: {r.text[:300]}")

    # undo puts the curve back
    await set_line(ed, laser, {"widthCurve": [[0, 3], [1, 3]]})
    await ed.call("undo")
    values = await props(ed, laser, "linerenderer", ["widthCurve"])
    ed.check_eq(values["widthCurve"]["keys"], [[0, 0, 0, "inf"], [1, 1]], "undo restores the curve")

    # the texture shortcut is shaderTextures.u_baseTexture; the ribbon is read back
    await set_line(ed, laser, {"texture": TEXTURE, "widthCurve": 1})
    got = await ed.call("get_entity", entity=laser, components=["linerenderer"], properties=["shaderTextures"])
    textures = got["components"]["linerenderer"]["shaderTextures"]
    ed.check("u_baseTexture" in str(textures) and "halves.png" in str(textures), f"the base texture: {textures!r}")
    full = await ed.call("get_entity", entity=laser)
    ribbon = find_key(full, "ribbon")
    ed.check(isinstance(ribbon, dict) and ribbon.get("vertices", 0) >= 4, f"the ribbon: {ribbon!r}")
    await ed.call("delete_entities", entity=laser)

    trail = find_key(await ed.call("create_entity", type="trail_renderer", name="Trail"), "entity_id", "id")
    values = await props(ed, trail, "trailrenderer", ["*"])
    ed.check(abs(values["time"] - 5) < 1e-6 and abs(values["widthMultiplier"] - 0.2) < 1e-6, f"trail defaults: {values!r}")
    ed.check_eq(values["colorGradient"]["alphas"], [[0, 1], [1, 0]], "fading out")
    await ed.call("delete_entities", entity=trail)


async def make_line(ed: Editor, name: str, values: dict) -> int:
    line = find_key(await ed.call("create_entity", type="line_renderer", name=name), "entity_id", "id")
    base = {"positions": [[-2, 0, 0], [2, 0, 0]], "colorGradient": "#FF0000", "widthMultiplier": 0.5, "blendMode": "opaque"}
    base.update(values)
    await set_line(ed, line, base)
    return line


async def test_width_alignment_and_units(ed: Editor):
    line = await make_line(ed, "Wide", {})
    data, _ = await shot(ed, "wide")
    box = red_extent(data)
    ed.check(box is not None, "the wide line draws")
    x0, y0, x1, y1 = box
    length, thickness = x1 - x0 + 1, y1 - y0 + 1
    # 4 long, 0.5 wide: thickness / length = 1/8 (+- a pixel)
    ed.check(abs(thickness - length / 8) <= 1.5, f"0.5 of 4: {thickness} px of {length} px")

    # a camera turned 90 degrees around the line (looking from +Y): View alignment faces it
    await ed.call("set_transform", entity=S["camera"], position=[0, 10, 0], rotation={"euler": [-90, 0, 0]})
    turned, _ = await shot(ed, "wide-turned")
    tb = red_extent(turned)
    ed.check(tb is not None and abs((tb[3] - tb[1] + 1) - thickness) <= 1.5, f"the same width from above: {tb} vs {thickness}")

    # Transform Z alignment: a strip across the GameObject's Z (here facing +Z), edge-on from above
    await set_line(ed, line, {"alignment": "transform_z"})
    edge, _ = await shot(ed, "wide-edge")
    eb = red_extent(edge)
    ed.check(eb is None or (eb[3] - eb[1] + 1) <= 2, f"edge-on under 2 px: {eb}")
    await ed.call("set_transform", entity=S["camera"], position=[0, 0, 10], rotation={"euler": [0, 0, 0]})

    # 6 pixels at two distances
    await set_line(ed, line, {"alignment": "view", "widthUnit": "pixels", "widthMultiplier": 6})
    near, info = await shot(ed, "pixels-near")
    view = await ed.call("configure_game_view")
    rect = (info or {}).get("canvas_rect_in_image") or {}
    render = find_key(view, "render_size")
    scale = rect.get("height", 0) / render[1] if render and rect.get("height") else 1.0
    await ed.call("set_transform", entity=S["camera"], position=[0, 0, 20])
    far, _ = await shot(ed, "pixels-far")
    nb, fb = red_extent(near), red_extent(far)
    ed.check(nb is not None and fb is not None, "pixel-wide lines draw")
    tn, tf = nb[3] - nb[1] + 1, fb[3] - fb[1] + 1
    ed.check(abs(tn - tf) <= 1, f"the same pixel width at 10 and 20: {tn} vs {tf}")
    ed.check(abs(tn - 6 * scale) <= 1.5, f"6 render pixels ({scale:.2f} image px each): {tn}")
    await ed.call("set_transform", entity=S["camera"], position=[0, 0, 10])
    await ed.call("delete_entities", entity=line)


async def test_caps_and_texture(ed: Editor):
    line = await make_line(ed, "Capped", {"widthMultiplier": 1.0})
    butt, _ = await shot(ed, "butt")
    bb = red_extent(butt)
    await set_line(ed, line, {"numCapVertices": 16})
    round_, _ = await shot(ed, "round")
    rb = red_extent(round_)
    ed.check(bb is not None and rb is not None, "both draw")
    # round caps reach half a width (1/8 of the 4-unit length each side) past the ends
    length = bb[2] - bb[0] + 1
    ed.check(rb[0] < bb[0] - length / 16 and rb[2] > bb[2] + length / 16, f"round caps past the ends: {rb} vs {bb}")

    # a red | blue texture: stretched, the colour changes at the middle; tiled every 1 / 0.5 = 2 units
    await set_line(ed, line, {"numCapVertices": 0, "texture": TEXTURE, "colorGradient": "#FFFFFF", "textureMode": "stretch"})
    stretched, _ = await shot(ed, "stretch")
    w, h, ch, rows = decode_png(stretched)
    y = (bb[1] + bb[3]) // 2
    def color_changes(rowdata):
        # (colour, x) at each change, x halfway between the last pixel of the previous colour and
        # the first of the next (the texture is filtered: a blend between the two)
        seq, last = [], None
        for x in range(bb[0] + 2, bb[2] - 1):
            r, g, b = rowdata[x * ch:x * ch + 3]
            c = "r" if r > b + 40 else "b" if b > r + 40 else None
            if c and (not seq or seq[-1][0] != c):
                seq.append((c, x if last is None else (last + x) / 2))
            if c:
                last = x
        return seq
    seq = color_changes(rows[y])
    ed.check(len(seq) == 2 and abs(seq[1][1] - (bb[0] + bb[2]) / 2) <= length * 0.05, f"stretch: one change at the middle: {seq}")
    await set_line(ed, line, {"textureMode": "tile", "textureScale": [0.5, 1]})
    tiled, _ = await shot(ed, "tile")
    w, h, ch, rows = decode_png(tiled)
    seq = color_changes(rows[y])
    ed.check(len(seq) >= 4, f"tile: the texture repeats along 4 units at 2 per repeat: {seq}")
    await ed.call("delete_entities", entity=line)


async def test_trail_in_play(ed: Editor):
    mover = find_key(await ed.call("create_entity", type="empty", name="Mover", position=[-4, 0, 0]), "entity_id", "id")
    trail = find_key(await ed.call("create_entity", type="trail_renderer", name="MoverTrail", parent="Mover"), "entity_id", "id")
    await set_line(ed, trail, {"time": 0.3, "widthMultiplier": 0.4, "widthCurve": 1, "minVertexDistance": 0.05,
                               "colorGradient": {"colors": [[0, "#FF0000"], [1, "#FF0000"]], "alphas": [[0, 1], [1, 0]]}},
                   component="trailrenderer")
    await ed.call("play", action="start", fixed_fps=60)
    # 4 units per second along x
    await ed.call("eval_lua", code="local o = object('Mover'); local s = {}; _G.mover_mover = s; "
                                   "function s:onUpdate() o.position = o.position + Vector3(4 * Engine.deltatime, 0, 0) end; "
                                   "RegisterEngineEvent(s, 'onUpdate'); return true")
    await ed.call("wait", game_seconds=1.0)
    stats = find_key(await ed.call("get_entity", entity="MoverTrail", runtime=True), "trail")
    ed.check(isinstance(stats, dict) and stats.get("points", 0) > 3, f"points while moving: {stats!r}")
    ed.check(abs(stats["length"] - 4 * 0.3) <= 4 * 0.3 * 0.15, f"0.3 s of 4 units/s: {stats['length']}")
    data, _ = await shot(ed, "trail")
    box = red_extent(data, tol=110)
    ed.check(box is not None, "the trail draws")
    # alpha falls towards the tail: the tail half is darker than the head half
    w, h, ch, rows = decode_png(data)
    y = (box[1] + box[3]) // 2
    xs = range(box[0], box[2] + 1)
    reds = [rows[y][x * ch] for x in xs]
    half = len(reds) // 2
    ed.check(sum(reds[:half]) < sum(reds[half:]), f"fading to the tail (left): {reds[:5]}...{reds[-5:]}")

    values = (await ed.call("eval_lua", code="local t = object('MoverTrail', 'TrailRenderer'); local n = t.positionCount; "
                                             "t:clear(); return n, t.positionCount"))["values"]
    ed.check(values[0] > 0 and values[1] == 0, f"clear() empties it: {values}")
    await ed.call("play", action="stop")


async def test_lua_round_trips(ed: Editor):
    await ed.call("play", action="start")
    code = """
local l = LineRenderer(scene)
l.entityOwned = false
l.positionCount = 3
l:setPosition(0, Vector3(0, 0, 0)); l:setPosition(1, Vector3(1, 0, 0)); l:setPosition(2, Vector3(1, 1, 0))
local ok, err = pcall(function() l:setPosition(3, Vector3()) end)
local c = l.widthCurve
local idx = c:addKey(0.5, 2)
local before = l.widthCurve.keyCount
l.widthCurve = c
local g = Gradient()
g:setKeys({{0, Vector3(1, 0.5, 0)}, {1, Vector3(0, 0, 1)}}, {{0, 1}, {1, 0}})
l.colorGradient = g
l.startWidth = 0.35
l.endWidth = 0.1
local sc = l.startColor
return l.mode == LineRendererMode.Polyline, l.positionCount, ok, tostring(err), idx, before, l.widthCurve.keyCount,
       l.colorGradient.colorKeyCount, math.floor(sc.y * 100 + 0.5), l.startWidth, l.endWidth, l.widthMultiplier,
       l:getPosition(2).y, #l:getPositions()
"""
    v = (await ed.call("eval_lua", code=code))["values"]
    ed.check_eq(v[0], True, "the polyline API switches to Polyline")
    ed.check_eq(v[1], 3, "positionCount")
    ed.check(v[2] is False and "positionCount is 3" in v[3], f"an index out of range names the count: {v[3]}")
    ed.check_eq(v[4], 2, "addKey: the 1-based index")
    ed.check_eq(v[5], 1, "the property is a copy: unchanged before assigning it back")
    ed.check_eq(v[6], 2, "assigned back")
    ed.check_eq(v[7], 2, "gradient keys")
    ed.check_eq(v[8], 50, "startColor in sRGB")
    ed.check(abs(v[9] - 0.35) < 1e-5 and abs(v[10] - 0.1) < 1e-5 and abs(v[11] - 1) < 1e-6, f"start/end width: {v[9:12]}")
    ed.check_eq(v[12], 1, "getPosition is 0-based")
    ed.check_eq(v[13], 3, "getPositions")
    v = (await ed.call("eval_lua", code="local t = TrailRenderer(scene); t.entityOwned = false; t.time = -1; t.numCapVertices = 500; "
                                        "return t.time, t.numCapVertices, scene ~= nil and object('MoverTrail'):getTrailRenderer() ~= nil"))["values"]
    ed.check_eq(v[:2], [0, 90], "values are clamped")
    ed.check_eq(v[2], True, "getTrailRenderer")
    await ed.call("play", action="stop")


async def test_check_findings(ed: Editor):
    hair = find_key(await ed.call("create_entity", type="line_renderer", name="TexturedHairline"), "entity_id", "id")
    await set_line(ed, hair, {"widthMultiplier": 0, "texture": TEXTURE})
    zero = find_key(await ed.call("create_entity", type="trail_renderer", name="NoTime"), "entity_id", "id")
    await set_line(ed, zero, {"time": 0}, component="trailrenderer")
    r = await ed.call("check_shader")
    text = str(r)
    ed.check("line-texture-no-uv" in text, f"line-texture-no-uv: {text[:400]}")
    ed.check("trail-zero-time" in text, f"trail-zero-time: {text[:400]}")
    await ed.call("delete_entities", entity=hair)
    await ed.call("delete_entities", entity=zero)


async def test_2d_hierarchy_order(ed: Editor):
    await ed.call("create_scene", name="Lines2D", path=SCENE_2D, type="2D")
    S["scene"] = SCENE_2D
    # a red trail, then a later white sprite over it: the sprite wins where they overlap
    mover = find_key(await ed.call("create_entity", type="empty", name="Runner", position=[100, 250, 0]), "entity_id", "id")
    trail = find_key(await ed.call("create_entity", type="trail_renderer", name="RunnerTrail", parent="Runner"), "entity_id", "id")
    await set_line(ed, trail, {"time": 2, "widthMultiplier": 40, "widthCurve": 1, "colorGradient": "#FF0000", "blendMode": "opaque"},
                   component="trailrenderer")
    cover = find_key(await ed.call("create_entity", type="sprite", name="Cover", position=[200, 250, 0]), "entity_id", "id")
    await ed.call("set_properties", entity=cover, values={"sprite": {"width": 80, "height": 80}})
    await ed.call("play", action="start", fixed_fps=60)
    await ed.call("eval_lua", code="local o = object('Runner'); local s = {}; _G.mover_runner = s; "
                                   "function s:onUpdate() if o.position.x < 300 then "
                                   "o.position = o.position + Vector3(200 * Engine.deltatime, 0, 0) end end; "
                                   "RegisterEngineEvent(s, 'onUpdate'); return true")
    await ed.call("wait", game_seconds=1.2)
    stats = find_key(await ed.call("get_entity", entity="RunnerTrail", runtime=True), "trail")
    ed.check(isinstance(stats, dict) and stats.get("points", 0) > 3, f"the 2D trail keeps points: {stats!r}")
    data, _ = await shot(ed, "2d")
    ed.check(red_extent(data) is not None, "the 2D trail draws")
    # the sprite (white, from its bottom-left corner at (200, 250)) overlaps the top of the
    # trail's band (250 +- 20): where they overlap, the later sprite wins
    w, h, ch, rows = decode_png(data)
    whites = [(x, y) for y in range(h) for x in range(w)
              if min(rows[y][x * ch:x * ch + 3]) > 230]
    ed.check(bool(whites), "the sprite draws")
    sx0, sx1 = min(p[0] for p in whites), max(p[0] for p in whites)
    sy0, sy1 = min(p[1] for p in whites), max(p[1] for p in whites)
    tb = red_extent(data)
    ed.check(tb[1] < sy1 and sy1 < tb[3], f"the sprite's bottom edge crosses the trail band: sprite y {sy0}-{sy1}, trail {tb}")
    y = (sy1 + tb[1]) // 2
    cx = (sx0 + sx1) // 2
    over = tuple(rows[y][cx * ch:cx * ch + 3])
    left = tuple(rows[y][(tb[0] + 2) * ch:(tb[0] + 2) * ch + 3])
    ed.check(min(over) > 230, f"the later sprite draws over the trail: {over} at ({cx}, {y})")
    ed.check(left[0] > 180 and left[1] < 80 and left[2] < 80, f"the trail beside it: {left}")
    await ed.call("play", action="stop")
