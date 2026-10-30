"""UI layout in 2D scenes: create_ui / set_rect_layout give the same on-screen placement in a 2D
scene (bottom-up orthographic camera) as in a UI scene (top-down UI camera).

The engine lays UI out in a top-down canvas space whatever the camera, so in a 2D scene every
vertical anchor and offset comes out mirrored; the MCP tools map the layout so agents always get
the top-down semantics they asked for, and read it back the same way. Layout groups still stack
bottom-up in a 2D scene (no mapping fixes that): create_ui warns.

Also: 3D friction / restitution are per body, so edit_colliders writes a 3D collider's friction /
restitution to the Rigidbody (with a note).

Each placement test builds one board of colored images (one color per anchor case), screenshots
the stopped game view and finds every color's box in the image. Runs on an
owned empty project (canvas 1280 x 720).
"""

from harness import Editor, ToolError
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Coordinate Contracts"}
CI = True
SCENES = {}

# (name, color, layout, expected top-down canvas rect [x, y, w, h]); canvas 1280 x 720
ROOTS = [
    ("TL", [1, 0, 0], {"anchor": "top_left", "size": [80, 50], "position": [20, 30]}, [20, 30, 80, 50]),
    ("TC", [0, 1, 0], {"anchor": "top_center", "size": [80, 50], "position": [0, 30]}, [600, 30, 80, 50]),
    ("TR", [0, 0, 1], {"anchor": "top_right", "size": [80, 50], "position": [-20, 30]}, [1180, 30, 80, 50]),
    ("CL", [1, 1, 0], {"anchor": "center_left", "size": [80, 50], "position": [20, 40]}, [20, 375, 80, 50]),
    ("C", [1, 0, 1], {"anchor": "center", "size": [80, 50], "position": [0, 40]}, [600, 375, 80, 50]),
    ("CR", [0, 1, 1], {"anchor": "center_right", "size": [80, 50], "position": [-20, 40]}, [1180, 375, 80, 50]),
    ("BL", [1, 0.5, 0], {"anchor": "bottom_left", "size": [80, 50], "position": [20, -30]}, [20, 640, 80, 50]),
    ("BC", [0, 0.5, 1], {"anchor": "bottom_center", "size": [80, 50], "position": [0, -30]}, [600, 640, 80, 50]),
    ("BR", [0.5, 0, 1], {"anchor": "bottom_right", "size": [80, 50], "position": [-20, -30]}, [1180, 640, 80, 50]),
    ("Abs", [1, 0, 0.5], {"position": [300, 120], "size": [100, 60]}, [300, 120, 100, 60]),
    ("Piv", [0.5, 0.5, 0], {"anchor": "top_left", "size": [80, 50], "position": [700, 120], "pivot": [1, 1]},
     [740, 145, 40, 25]),
]

# Containers (invisible) with colored children: [parent layout, [(name, color, layout, expected rect)]]
GROUPS = [
    ("Box", {"position": [400, 200], "size": [240, 120]}, [
        ("BoxTL", [0, 1, 0.5], {"anchor": "top_left", "size": [40, 30], "position": [10, 10]}, [410, 210, 40, 30]),
        ("BoxAbs", [0.5, 1, 0], {"position": [150, 70], "size": [60, 40]}, [550, 270, 60, 40]),
    ]),
    ("Box2", {"position": [800, 200], "size": [300, 120]}, [
        ("Stretch", [0, 0.5, 0], {"anchor": "stretch", "margins": [10, 20, 30, 40]}, [810, 220, 260, 60]),
    ]),
    ("Box3", {"position": [300, 460], "size": [300, 140]}, [
        ("TopWide", [0.5, 0, 0], {"anchor": "top_wide", "size": [10, 20], "position": [0, 5]}, [300, 465, 300, 20]),
        ("BottomWide", [0, 0, 0.5], {"anchor": "bottom_wide", "size": [10, 20], "position": [0, -5]}, [300, 575, 300, 20]),
    ]),
]


def level(v: int) -> int:
    return 0 if v < 50 else (2 if v > 210 else 1)


def color_key(color) -> tuple:
    return tuple(0 if c == 0 else (2 if c == 1 else 1) for c in color[:3])


def color_boxes(png: bytes) -> dict:
    """{(r, g, b) level key: [x0, y0, x1, y1, count]} over the image."""
    w, h, ch, rows = decode_png(png)
    boxes = {}
    for y in range(h):
        row = rows[y]
        for x in range(w):
            i = x * ch
            key = (level(row[i]), level(row[i + 1]), level(row[i + 2]))
            b = boxes.get(key)
            if b is None:
                boxes[key] = [x, y, x, y, 1]
            else:
                if x < b[0]: b[0] = x
                if x > b[2]: b[2] = x
                if y < b[1]: b[1] = y
                if y > b[3]: b[3] = y
                b[4] += 1
    return boxes


async def new_scene(ed: Editor, kind: str, name: str):
    """Create and select the named shared scene; return its id."""
    if not SCENES:
        await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720, "graphics.scaling_mode": "letterbox"})
    if name not in SCENES:
        made = await ed.call("create_scene", name=name, type=kind)
        scene = made["scene"]
        SCENES[name] = scene["id"]
    return SCENES[name]


def image_node(name, color, layout):
    return {"type": "image", "name": name, "color": color + [1], "layout": layout}


async def build_board(ed: Editor, sid):
    for name, color, layout, _ in ROOTS:
        await ed.call("create_ui", scene=sid, root=image_node(name, color, layout))
    for name, layout, children in GROUPS:
        await ed.call("create_ui", scene=sid, root={
            "type": "container", "name": name, "layout": layout,
            "children": [image_node(n, c, lay) for n, c, lay, _ in children]})
    await ed.call("set_properties", scene=sid, entity="Piv", values={"transform": {"scale": [0.5, 0.5, 1]}})


async def placements(ed: Editor, sid, label: str) -> dict:
    """{element name: [x, y, w, h] in canvas pixels} measured from a game-view screenshot."""
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": sid, "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"ui-anchors-{label}.png").write_bytes(png)
    info = shot.value
    rect = info["canvas_rect_in_image"]
    canvas = info["canvas"]
    sx = rect["width"] / canvas["width"]
    sy = rect["height"] / canvas["height"]
    boxes = color_boxes(png)
    out = {}
    for name, color, _, _ in ROOTS + [c for _, _, children in GROUPS for c in children]:
        b = boxes.get(color_key(color))
        if not b:
            out[name] = None
            continue
        out[name] = [round((b[0] - rect["x"]) / sx), round((b[1] - rect["y"]) / sy),
                     round((b[2] - b[0] + 1) / sx), round((b[3] - b[1] + 1) / sy)]
    return out


def close(a, b, tol=4) -> bool:
    return a is not None and all(abs(x - y) <= tol for x, y in zip(a, b))


async def check_board(ed: Editor, sid, label: str):
    got = await placements(ed, sid, label)
    want = {name: rect for name, _, _, rect in ROOTS + [c for _, _, children in GROUPS for c in children]}
    wrong = {n: (got.get(n), r) for n, r in want.items() if not close(got.get(n), r)}
    ed.check(not wrong, f"{label}: misplaced elements (measured, expected): {wrong}")


# -- placement -------------------------------------------------------------------------------


async def test_ui_scene_placement(ed: Editor):
    """Baseline: in a UI scene the layout is top-down as asked."""
    sid = await new_scene(ed, "ui", "AnchorsUI")
    await build_board(ed, sid)
    await check_board(ed, sid, "ui")


async def test_2d_scene_placement_matches_ui_scene(ed: Editor):
    sid = await new_scene(ed, "2d", "Anchors2D")
    await build_board(ed, sid)
    await check_board(ed, sid, "2d")


# -- read-back ---------------------------------------------------------------------------------


async def test_2d_read_back_is_top_down(ed: Editor):
    sid = await new_scene(ed, "2d", "Anchors2D")
    if not await _exists(ed, sid, "TL"):
        await build_board(ed, sid)
    laid = await ed.call("set_rect_layout", scene=sid, items=[
        {"entity": "TL", "anchor": "top_left", "size": [80, 50], "position": [20, 30]},
        {"entity": "BR", "anchor": "bottom_right", "size": [80, 50], "position": [-20, -30]},
        {"entity": "C", "anchor": "center", "size": [80, 50], "position": [0, 40]},
        {"entity": "Abs", "position": [300, 120], "size": [100, 60]},
        {"entity": "Box2/Stretch", "anchor": "stretch", "margins": [10, 20, 30, 40]},
        {"entity": "Box3/TopWide", "anchor": "top_wide", "size": [10, 20], "position": [0, 5]},
        {"entity": "Piv", "pivot": [1, 1]},
    ])
    items = {i["name"]: i["layout"] for i in laid["items"]}
    ed.check_eq(items["Piv"]["pivot"], [1, 1], "the bottom-right pivot reads back in the caller's coordinate space")
    ed.check_eq(items["TL"]["anchor"], "top_left")
    ed.check_eq(items["TL"]["position_offset"], [20, 30])
    ed.check_eq(items["BR"]["anchor"], "bottom_right")
    ed.check_eq(items["BR"]["position_offset"], [-20, -30])
    ed.check_eq(items["C"]["anchor"], "center")
    ed.check_eq(items["C"]["position_offset"], [0, 40])
    ed.check_eq(items["Abs"]["position"], [300, 120])
    ed.check_eq(items["Abs"]["size"], [100, 60])
    ed.check_eq(items["Stretch"]["anchor_offsets"], [10, 20, -30, -40], "margins read back as asked")
    ed.check_eq(items["Stretch"]["size"], [260, 60])
    ed.check_eq(items["TopWide"]["anchor"], "top_wide")
    ed.check_eq(items["TopWide"]["position_offset"], [0, 5])
    # the board is unchanged on screen (same values re-applied)
    await check_board(ed, sid, "2d-relaid")

    # absolute, size only: the top edge stays where it was
    laid = await ed.call("set_rect_layout", scene=sid, items=[{"entity": "Abs", "size": [100, 30]}])
    ed.check_eq(laid["items"][0]["layout"]["position"], [300, 120], "resizing keeps the top-left")
    got = await placements(ed, sid, "2d-resized")
    ed.check(close(got["Abs"], [300, 120, 100, 30]), f"resized on screen: {got['Abs']}")
    await ed.call("set_rect_layout", scene=sid, items=[{"entity": "Abs", "size": [100, 60]}])


async def _exists(ed: Editor, sid, entity) -> bool:
    try:
        await ed.call("get_entity", scene=sid, entity=entity)
        return True
    except ToolError:
        return False


# -- layout groups in 2D scenes ---------------------------------------------------------------


async def test_layout_group_in_2d_scene_warns(ed: Editor):
    sid = await new_scene(ed, "2d", "Anchors2D")
    r = await ed.call_raw("create_ui", {"scene": sid, "root": {
        "type": "layout_group", "name": "Menu", "direction": "vertical", "layout": {"anchor": "center", "size": [200, 100]},
        "children": [{"type": "button", "text": "A"}, {"type": "button", "text": "B"}]}})
    ed.check(not r.is_error, r.text)
    ed.check(r.value.get("warnings"), f"structured warnings: {r.value}")

    uid = await new_scene(ed, "ui", "AnchorsUI")
    r = await ed.call_raw("create_ui", {"scene": uid, "root": {
        "type": "layout_group", "name": "Menu", "direction": "vertical",
        "children": [{"type": "button", "text": "A"}]}})
    ed.check(not r.is_error, r.text)
    ed.check(not r.value.get("warnings"), "a UI scene needs no coordinate-fallback warning")


# -- 3D friction / restitution per body ----------------------------------------------------------


async def test_3d_collider_friction_goes_to_body(ed: Editor):
    sid = await new_scene(ed, "3d", "Bodies3D")
    made = await ed.call("create_entity", scene=sid, name="Ball", type="sphere")
    ball = made["entity_id"]
    r = await ed.call_raw("edit_colliders", {"scene": sid, "entity": ball, "body_type": "dynamic",
                                             "add": [{"shape": "sphere", "friction": 0.8, "restitution": 0.4}]})
    ed.check(not r.is_error, f"3D friction on a collider: {r.text}")
    ed.check(abs(r.value["body"]["friction"] - 0.8) < 1e-4, f"body friction in the result: {r.value}")
    got = await ed.call("get_entity", scene=sid, entity=ball, components=["rigidbody"], properties=["friction", "restitution"])
    props = got["components"]["rigidbody"]
    ed.check(abs(float(props["friction"]) - 0.8) < 1e-4, f"friction on the body: {props}")
    ed.check(abs(float(props["restitution"]) - 0.4) < 1e-4, f"restitution on the body: {props}")

    r = await ed.call_raw("edit_colliders", {"scene": sid, "entity": ball, "update": [{"index": 0, "restitution": 0.9}]})
    ed.check(not r.is_error, r.text)
    got = await ed.call("get_entity", scene=sid, entity=ball, components=["rigidbody"], properties=["friction", "restitution"])
    props = got["components"]["rigidbody"]
    ed.check(abs(float(props["restitution"]) - 0.9) < 1e-4, f"update restitution on the body: {props}")
    ed.check(abs(float(props["friction"]) - 0.8) < 1e-4, f"friction kept: {props}")

    err = await ed.call("edit_colliders", scene=sid, entity=ball, update=[{"index": 0, "restitution": 2}], expect_error=True)
    ed.check_in("restitution", err)

    # one undo step: the collider edit and the body friction go together
    await ed.call("undo", scene=sid, steps=1)
    got = await ed.call("get_entity", scene=sid, entity=ball, components=["rigidbody"], properties=["restitution"])
    ed.check(abs(float(got["components"]["rigidbody"]["restitution"]) - 0.4) < 1e-4, f"undo: {got}")
