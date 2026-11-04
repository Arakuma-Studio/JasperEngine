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
CI_EXCLUDED = ("long-running (over 2 minutes on a loaded server: 14 tests compared in pixels); "
               "in the full suite (MCP_SUITE=all) on the build servers")
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


def hairline_features(image) -> dict:
    """Where an image's hairline ends are (the mean position of its nearly pure red, green and
    blue pixels, x and y from the centre over the height: the sky or the black behind them is
    neither) and how thick the red-green line is at the centre column."""
    w, h, px = image
    def at(x, y):
        i = (y * w + x) * 4
        return px[i], px[i + 1], px[i + 2]
    found = {"red": [], "green": [], "blue": []}
    for y in range(h):
        for x in range(w):
            r, g, b = at(x, y)
            if r > 200 and g < 60 and b < 60:
                found["red"].append(((x - w / 2) / h, (y - h / 2) / h))
            elif g > 200 and r < 60 and b < 60:
                found["green"].append(((x - w / 2) / h, (y - h / 2) / h))
            elif b > 200 and r < 60 and g < 60:
                found["blue"].append(((x - w / 2) / h, (y - h / 2) / h))
    # the red-green line (yellow at its middle, no blue): its pixels down the centre column
    thick = sum(1 for y in range(h) if (lambda c: c[2] < 90 and max(c[0], c[1]) > 110)(at(w // 2, y)))
    out = {"thickest": thick}
    for key, points in found.items():
        out[key] = (sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points)) if points else None
    return out


async def test_old_hairline_matches_main(ed: Editor):
    """A Line Renderer as main wrote it: Segments, no width, two coloured segments."""
    made = await ed.call("create_entity", type="line_renderer", name="Hairline")
    line = find_key(made, "entity_id", "id")
    current = await props(ed, line, "linerenderer", ["*"])
    values = {"lines": [{"pointA": [-3, -1, 0], "colorA": [1, 0, 0, 1], "pointB": [3, -1, 0], "colorB": [0, 1, 0, 1]},
                        {"pointA": [-3, 1, 0], "colorA": [0, 0, 1, 1], "pointB": [3, 2, 0], "colorB": [1, 1, 1, 1]}]}
    # writing the segment list makes it Segments in local space (as main drew it); this editor's
    # new lines have a width, main's had none
    if "widthMultiplier" in current:
        values["widthMultiplier"] = 0
    await set_line(ed, line, values)
    try:
        data, _ = await shot(ed, "hairline")
        if os.environ.get("JASPER_UPDATE_GOLDEN") == "1":
            GOLDEN.parent.mkdir(parents=True, exist_ok=True)
            GOLDEN.write_bytes(data)
            print(f"     [golden] wrote {GOLDEN}")
        else:
            ed.check(GOLDEN.is_file(), f"missing committed golden image: {GOLDEN}")
            golden, now = pngdiff.decode(GOLDEN.read_bytes()), pngdiff.decode(data)
            if golden[:2] == now[:2]:
                ok, fraction, diff = pngdiff.compare(golden, now, max_fraction=0.001)
                if not ok and ed.scratch and diff:
                    (ed.scratch.root / "lines-hairline-diff.png").write_bytes(diff)
                ed.check(ok, f"the hairline differs from main's ({GOLDEN.name}) in {fraction:.4f} of the pixels")
            else:
                # the Game view's size is the editor layout's: the same lines where main drew
                # them (by the view's height: the vertical field of view), still one pixel thick
                want, got = hairline_features(golden), hairline_features(now)
                ed.check(1 <= got["thickest"] <= 2, f"a hairline (1 or 2 px thick): {got['thickest']} px")
                for key in ("red", "green", "blue"):
                    a, b = want[key], got[key]
                    ed.check(a is not None and b is not None and all(abs(x - y) <= 0.012 for x, y in zip(a, b)),
                             f"the {key} end where main drew it ({golden[0]}x{golden[1]} -> {now[0]}x{now[1]}): {b} vs {a}")
    finally:
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
    # S27's curve (docs/specs/S27-editor-tools.md): [[time, value, inTangent, outTangent]] keys
    ed.check_eq(values["widthCurve"], [[0, 1, 0, 0]], "a constant curve")

    # the curve and gradient shapes: origin/main's form is taken and read back in S27's
    await set_line(ed, laser, {"widthCurve": [[0, 1], [1, 0.25, -0.75, -0.75]],
                               "colorGradient": {"mode": "blend", "colors": [[0, "#FF9600"], [1, [0.5, 0.25, 0]]], "alphas": [[0, 1], [1, 0]]}})
    values = await props(ed, laser, "linerenderer", ["widthCurve", "colorGradient"])
    ed.check_eq(values["widthCurve"], [[0, 1, 0, 0], [1, 0.25, -0.75, -0.75]], "keys read back")
    colors = values["colorGradient"]["colorKeys"]
    ed.check(close(colors[0], [0, 1, 150 / 255, 0]), f"an sRGB hex colour is the key itself: {colors[0]}")
    ed.check(close(colors[1], [1, srgb(0.5), srgb(0.25), 0]), f"a linear colour as its sRGB encoding: {colors[1]}")
    ed.check_eq(values["colorGradient"]["alphaKeys"], [[0, 1], [1, 0]], "alphas")
    # S27's own form, the same
    await set_line(ed, laser, {"colorGradient": {"colorKeys": [[0, 1, 0, 0], [1, 0, 0, 1]], "alphaKeys": [[0, 1]], "mode": "fixed"}})
    values = await props(ed, laser, "linerenderer", ["colorGradient"])
    ed.check_eq(values["colorGradient"].get("mode"), "fixed", "S27's form")

    await set_line(ed, laser, {"widthCurve": 0.5, "colorGradient": "#FF000080"})
    values = await props(ed, laser, "linerenderer", ["widthCurve", "colorGradient"])
    ed.check_eq(values["widthCurve"], [[0, 0.5, 0, 0]], "a number is a constant curve")
    ed.check_eq(values["colorGradient"]["colorKeys"], [[0, 1, 0, 0]], "one colour")
    ed.check(abs(values["colorGradient"]["alphaKeys"][0][1] - 128 / 255) < 1e-3, f"its alpha: {values['colorGradient']}")
    await set_line(ed, laser, {"widthCurve": {"keys": [[0, 0, 0, "inf"], [1, 1]], "post": "loop"}})
    values = await props(ed, laser, "linerenderer", ["widthCurve"])
    ed.check_eq(values["widthCurve"]["keys"][0], [0, 0, 0, None], "a step tangent (null)")
    ed.check_eq(values["widthCurve"]["postWrapMode"], "loop", "post wrap")

    # wrong shapes are refused with the expected shape
    for bad, what in (({"widthCurve": {"keys": [[0]]}}, "keys[0]"), ({"widthCurve": "x"}, "curve"),
                      ({"colorGradient": {"colors": [[2, "#FFFFFF"]]}}, "0..1"), ({"colorGradient": {"oops": 1}}, "unknown"),
                      ({"textureMode": "spiral"}, "stretch")):
        r = await ed.call_raw("set_properties", {"entity": laser, "values": {"linerenderer": bad}})
        ed.check(r.is_error and what in r.text, f"{bad} refused naming {what}: {r.text[:300]}")

    # undo puts the curve back
    await set_line(ed, laser, {"widthCurve": [[0, 3], [1, 3]]})
    await ed.call("undo")
    values = await props(ed, laser, "linerenderer", ["widthCurve"])
    ed.check_eq(values["widthCurve"]["keys"], [[0, 0, 0, None], [1, 1, 0, 0]], "undo restores the curve")

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
    ed.check_eq(values["colorGradient"]["alphaKeys"], [[0, 1], [1, 0]], "fading out")
    await ed.call("delete_entities", entity=trail)


def srgb(linear: float) -> float:
    return linear ** (1 / 2.2)   # the engine's Color::linearTosRGB (gamma 2.2)


def close(a: list, b: list, tol: float = 1e-4) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


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
    # image pixels per world unit at the line's depth (the later tests' scale)
    S["ppu"] = length / 4.0
    S["center"] = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)

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


def colored_extent(data: bytes):
    """(x0, y0, x1, y1) of the red or blue pixels (the halves texture)."""
    w, h, ch, rows = decode_png(data)
    xs, ys = [], []
    for y in range(h):
        line = rows[y]
        for x in range(w):
            r, g, b = line[x * ch], line[x * ch + 1], line[x * ch + 2]
            if g < 80 and (r > 150 or b > 150) and abs(r - b) > 60:
                xs.append(x)
                ys.append(y)
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None


def color_changes(data: bytes, y: int, x0: int, x1: int):
    """(colour, x) of each red/blue change along a row, x halfway between the two colours."""
    w, h, ch, rows = decode_png(data)
    seq, last = [], None
    for x in range(x0, x1 + 1):
        r, g, b = rows[y][x * ch:x * ch + 3]
        c = "r" if r > b + 40 else "b" if b > r + 40 else None
        if c and (not seq or seq[-1][0] != c):
            seq.append((c, x if last is None else (last + x) / 2))
        if c:
            last = x
    return seq


async def test_texture_modes_per_segment(ed: Editor):
    """DistributePerSegment and RepeatPerSegment on segments of 1 and 5 units."""
    ppu = S["ppu"]
    line = await make_line(ed, "PerSegment", {"positions": [[-3, 0, 0], [-2, 0, 0], [3, 0, 0]], "texture": TEXTURE,
                                              "colorGradient": "#FFFFFF", "textureMode": "distribute_per_segment"})
    data, _ = await shot(ed, "distribute")
    box = colored_extent(data)
    ed.check(box is not None, "the textured line draws")
    x_of = lambda wx: box[0] + (wx + 3) * ppu
    y = (box[1] + box[3]) // 2
    seq = color_changes(data, y, box[0] + 2, box[2] - 2)
    # each segment half the texture: red to blue where the first (short) segment ends
    ed.check(len(seq) == 2 and abs(seq[1][1] - x_of(-2)) <= 0.2 * ppu + 2, f"distribute: the change at x=-2: {seq}, expected {x_of(-2):.0f}")

    await set_line(ed, line, {"textureMode": "repeat_per_segment"})
    data, _ = await shot(ed, "repeat")
    seq = color_changes(data, y, box[0] + 2, box[2] - 2)
    # the whole texture on each segment: blue from its middle, red again at the next one
    expected = [("b", x_of(-2.5)), ("r", x_of(-2)), ("b", x_of(0.5))]
    got = seq[1:]
    ed.check(len(got) == 3 and all(c == e[0] and abs(x - e[1]) <= 0.2 * ppu + 2 for (c, x), e in zip(got, expected)),
             f"repeat: changes at -2.5, -2, 0.5: {seq} vs {[(c, round(x)) for c, x in expected]}")
    await ed.call("delete_entities", entity=line)


async def test_round_joints_and_loop(ed: Editor):
    ppu = S["ppu"]
    # a right-angle V, 1 wide: the miter's outer tip is sqrt(2)/2 below the corner, a round
    # joint's half a width
    line = await make_line(ed, "Vee", {"positions": [[-2, 1, 0], [0, -1, 0], [2, 1, 0]], "widthMultiplier": 1.0})
    miter, _ = await shot(ed, "miter")
    await set_line(ed, line, {"numCornerVertices": 8})
    rounded, _ = await shot(ed, "round-joint")
    mb, rb = red_extent(miter), red_extent(rounded)
    ed.check(mb is not None and rb is not None, "both draw")
    lift = (mb[3] - rb[3]) / ppu
    ed.check(abs(lift - (0.7071 - 0.5)) <= 0.06 + 1.5 / ppu, f"the round joint is {lift:.3f} units above the miter tip (0.207)")
    # a round joint's outline below the corner: as wide as the line there, not a point
    w, h, ch, rows = decode_png(rounded)
    row = rows[rb[3] - int(0.1 * ppu)]
    reds = [x for x in range(w) if row[x * ch] > 195 and row[x * ch + 1] < 60]
    ed.check(len(reds) > 0.3 * ppu, f"a rounded bottom ({len(reds)} px wide 0.1 above it)")
    await ed.call("delete_entities", entity=line)

    # a square: open, its left side is missing; loop closes it
    square = [[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]]
    line = await make_line(ed, "Square", {"positions": square, "widthMultiplier": 0.2})
    open_, _ = await shot(ed, "square-open")
    await set_line(ed, line, {"loop": True})
    closed, _ = await shot(ed, "square-loop")
    ob, cb = red_extent(open_), red_extent(closed)
    ed.check(cb is not None and ob is not None, "the square draws")
    def red_at(data, x, y):
        w, h, ch, rows = decode_png(data)
        px = rows[y][x * ch:x * ch + 3]
        return px[0] > 195 and px[1] < 60 and px[2] < 60
    left_x, mid_y = cb[0] + max(1, int(0.1 * ppu)), (cb[1] + cb[3]) // 2
    ed.check(red_at(closed, left_x, mid_y), "loop: the closing side draws")
    ed.check(not red_at(open_, left_x, mid_y), "open: no closing side")
    await ed.call("delete_entities", entity=line)


async def test_local_and_world_space_under_parents(ed: Editor):
    ppu = S["ppu"]
    cx = S["center"][0]
    for scale in ([2, 1, 1], [-2, 1, 1]):
        parent = find_key(await ed.call("create_entity", type="empty", name="Holder", position=[1, 0, 0], scale=scale), "entity_id", "id")
        line = find_key(await ed.call("create_entity", type="line_renderer", name="Held", parent=parent), "entity_id", "id")
        await set_line(ed, line, {"positions": [[-1, 0, 0], [1, 0, 0]], "useWorldSpace": False, "widthMultiplier": 0.5,
                                  "colorGradient": "#FF0000", "blendMode": "opaque"})
        data, _ = await shot(ed, f"local-{scale[0]}")
        box = red_extent(data)
        ed.check(box is not None, f"the local line under scale {scale} draws")
        # the points scale and move with the parent (world -1..3), the width does not
        length, thickness = (box[2] - box[0] + 1) / ppu, (box[3] - box[1] + 1) / ppu
        middle = ((box[0] + box[2]) / 2 - cx) / ppu
        ed.check(abs(length - 4) <= 0.1 and abs(middle - 1) <= 0.1, f"scale {scale}: 4 long around x=1: {length:.2f}, {middle:.2f}")
        ed.check(abs(thickness - 0.5) <= 0.05 + 1.5 / ppu, f"scale {scale}: still 0.5 wide: {thickness:.3f}")
        # world space: the same numbers are world points, whatever the parents
        await set_line(ed, line, {"useWorldSpace": True})
        data, _ = await shot(ed, f"world-{scale[0]}")
        box = red_extent(data)
        length = (box[2] - box[0] + 1) / ppu
        middle = ((box[0] + box[2]) / 2 - cx) / ppu
        ed.check(abs(length - 2) <= 0.1 and abs(middle) <= 0.1, f"world space: 2 long around 0: {length:.2f}, {middle:.2f}")
        await ed.call("delete_entities", entities=[str(parent)])


async def test_pixel_width_through_the_near_plane(ed: Editor):
    """A pixel-wide line from in front of the camera to behind it is cut at the near plane."""
    line = await make_line(ed, "ToCamera", {"positions": [[0, -1, 0], [0, -1, 15]], "widthUnit": "pixels", "widthMultiplier": 6})
    data, info = await shot(ed, "near-plane")
    box = red_extent(data)
    ed.check(box is not None, "the visible part draws")
    w, h, ch, rows = decode_png(data)
    # below the centre (y -1, coming towards the camera: down to the bottom edge), one strip
    ed.check(box[1] >= S["center"][1] - 2, f"nothing above the line's far end: {box}, centre {S['center']}")
    ed.check(box[2] - box[0] <= 12, f"a 6-pixel strip, no mirrored triangles: {box}")
    await ed.call("delete_entities", entity=line)


async def test_set_shape_points_on_new_lines(ed: Editor):
    line = find_key(await ed.call("create_entity", type="line_renderer", name="Shaped", position=[1, 0, 0]), "entity_id", "id")
    # (the transform's matrix is made by the next frame)
    await ed.call("wait", frames=2)
    await ed.call("set_shape_points", entity=line, component="line_renderer", points=[[-1, 0, 0], [1, 0, 0], [1, 1, 0]], closed=True)
    v = await props(ed, line, "linerenderer", ["mode", "positions", "loop", "useWorldSpace"])
    ed.check_eq(v["mode"], "polyline", "points: a polyline")
    ed.check_eq(v["loop"], True, "closed: a loop")
    # local points, a world-space line: moved by the GameObject's position
    ed.check_eq(v["positions"], [[0, 0, 0], [2, 0, 0], [2, 1, 0]], "the points in world space")
    await ed.call("set_shape_points", entity=line, component="line_renderer", lines=[{"a": [0, 0, 0], "b": [1, 0, 0]}])
    v = await props(ed, line, "linerenderer", ["mode", "lines", "useWorldSpace"])
    ed.check_eq(v["mode"], "segments", "lines: segments")
    ed.check_eq(v["useWorldSpace"], False, "segments in the GameObject's space")
    ed.check_eq(len(v["lines"]), 1, "one segment")
    await ed.call("undo")
    v = await props(ed, line, "linerenderer", ["mode"])
    ed.check_eq(v["mode"], "polyline", "one undo step")
    await ed.call("delete_entities", entity=line)


async def test_trail_in_play(ed: Editor):
    mover = find_key(await ed.call("create_entity", type="empty", name="Mover", position=[-4, 0, 0]), "entity_id", "id")
    trail = find_key(await ed.call("create_entity", type="trail_renderer", name="MoverTrail", parent="Mover"), "entity_id", "id")
    await set_line(ed, trail, {"time": 0.3, "widthMultiplier": 0.4, "widthCurve": 1, "minVertexDistance": 0.05,
                               "colorGradient": "#FF0000"}, component="trailrenderer")
    await ed.call("play", action="start", fixed_fps=60)
    # 4 units per second along x
    await ed.call("eval_lua", code="local o = object('Mover'); local s = {}; _G.mover_mover = s; "
                                   "function s:onUpdate() o.position = o.position + Vector3(4 * Engine.deltatime, 0, 0) end; "
                                   "RegisterEngineEvent(s, 'onUpdate'); return true")
    await ed.call("wait", game_seconds=1.0)
    stats = find_key(await ed.call("get_entity", entity="MoverTrail", runtime=True), "trail")
    ed.check(isinstance(stats, dict) and stats.get("points", 0) > 3, f"points while moving: {stats!r}")
    ed.check(abs(stats["length"] - 4 * 0.3) <= 4 * 0.3 * 0.15, f"0.3 s of 4 units/s: {stats['length']}")
    # opaque first: the lit pixels span the last 0.3 s of the path (1.2 units) +- 10 %
    solid, _ = await shot(ed, "trail-solid")
    sb = red_extent(solid)
    ed.check(sb is not None, "the opaque trail draws")
    span = (sb[2] - sb[0] + 1) / S["ppu"]
    ed.check(abs(span - 1.2) <= 0.12 + 2 / S["ppu"], f"the trail spans 0.3 s of 4 units/s: {span:.3f} units")
    # then fading out by age
    await ed.call("eval_lua", code="local t = object('MoverTrail', 'TrailRenderer'); local g = Gradient(); "
                                   "g:setKeys({{0, Vector3(1, 0, 0)}, {1, Vector3(1, 0, 0)}}, {{0, 1}, {1, 0}}); "
                                   "t.colorGradient = g; return true")
    await ed.call("wait", game_seconds=0.1)
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
    # a handle adds nothing; addTrailRenderer adds; more than 8 gradient keys is an error
    code = """
local g = GameObject(scene); g.entityOwned = false; g.name = "NoTrailYet"
pcall(function() return object("NoTrailYet", "TrailRenderer") end)
local had = g:hasTrailRenderer()
local t = g:addTrailRenderer()
local keys = {}
for i = 0, 8 do keys[#keys + 1] = {i / 8, Vector3(1, 1, 1)} end
local gradient = Gradient()
local set = gradient:setKeys(keys, {{0, 1}})
return had, g:hasTrailRenderer(), t ~= nil, set, gradient.colorKeyCount
"""
    v = (await ed.call("eval_lua", code=code))["values"]
    ed.check_eq(v[:3], [False, True, True], "object(..., 'TrailRenderer') adds nothing, addTrailRenderer adds")
    # S27's setKeys: false (and a warning) past 8 keys, the gradient unchanged
    ed.check(v[3] is False and v[4] == 0, f"9 colour keys refused: {v[3:]}")
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
