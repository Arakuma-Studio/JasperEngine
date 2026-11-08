"""S26 (docs/specs/S26-scene-view.md) through MCP: triangle picking and the click flow (AC7, AC8, AC9),
the projection and framing (AC11), camera bookmarks (AC12), layouts and panes (AC13), the Scene
Camera settings (AC14), the Layers filter and per-GameObject hide and lock (AC15), measures (AC16)
and the tools' group (AC19). The Scene view's real input (keys, Ctrl+wheel, drag and drop, the
view gizmo) is in test_scene_view_ui.py.

Every scene looks along -z from a camera at z > 0 unless a test sets it otherwise; points are given
as u, v (0..1) of the Scene view image.
"""

from __future__ import annotations

import math
import os
import struct
import zlib

import harness
import pngdiff
from harness import Editor

CI = True
# background mesh BVH builds stay pending 30 s (only meshes of 256+ triangles have one): a
# click waits for them (R33); a loaded host takes several seconds to import the test's lattice
EDITOR = {"create": True, "name": "Scene View", "env": {"JASPER_PICK_BUILD_DELAY_MS": "30000"}}
TIMEOUT = 1500

GOLDEN_DIR = harness.REPO / "tools" / "mcp" / "tests" / "golden" / "scene_view"
UPDATE_GOLDEN = os.environ.get("JASPER_SCENE_VIEW_UPDATE_GOLDEN") == "1"

S: dict = {}

S26_TOOLS = ["pick_at", "edit_scene_view_bookmarks", "set_scene_view_layout", "measure_scene_view",
             "set_scene_view_filter"]


# -- helpers --------------------------------------------------------------------


def close(a, b, tol=1e-3):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return a == b


async def new_scene(ed: Editor, name: str, kind: str = "3D") -> int:
    made = await ed.call("create_scene", name=name, type=kind, path=f"scenes/{name}")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    return sid


async def cube(ed: Editor, sid: int, name: str, position, scale=(1, 1, 1), parent=None, kind="cube") -> int:
    args = {"scene": sid, "type": kind, "name": name, "position": list(position), "scale": list(scale), "select": False}
    if parent is not None:
        args["parent"] = parent
    return (await ed.call("create_entity", **args))["entity_id"]


async def look(ed: Editor, sid: int, position, target):
    await ed.call("set_scene_camera", scene=sid, position=list(position), target=list(target))


async def selected(ed: Editor, sid: int) -> list[int]:
    got = await ed.call("get_selection", scene=sid)
    return [e["id"] for e in got.get("entities", [])]


async def click(ed: Editor, sid: int, u=0.5, v=0.5, **args) -> dict:
    return await ed.call("pick_at", scene=sid, u=u, v=v, apply="click", **args)


async def clear_selection(ed: Editor, sid: int):
    await ed.call("select_entities", scene=sid, entities=[])


# -- group and schema (AC19) ------------------------------------------------------


async def test_tools_in_editor_ux_group(ed: Editor):
    ed.require_tools(*S26_TOOLS)
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(S26_TOOLS), f"hidden by default: {sorted(default & set(S26_TOOLS))}")
    await ed.call("tool_groups", enable=["editor_ux"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(S26_TOOLS) <= listed, "listed once editor_ux is enabled")
    await ed.call("tool_groups", disable=["editor_ux"])
    for name in S26_TOOLS:
        schema = ed.tool(name).input_schema
        ed.check(schema.get("type") == "object" and "properties" in schema, f"{name}: object schema")
    # set_scene_camera's S26 options
    props = ed.tool("set_scene_camera").input_schema["properties"]
    for key in ("projection", "ortho_size", "view", "frame_all", "camera_settings"):
        ed.check(key in props, f"set_scene_camera.{key}")


# -- picking and the click flow (AC8, AC9) ----------------------------------------


async def test_room_does_not_steal_click(ed: Editor):
    sid = await new_scene(ed, "Room")
    room = await cube(ed, sid, "Room", (0, 0, 0), (10, 10, 10))
    chair = await cube(ed, sid, "Chair", (0, 0, -2))
    await look(ed, sid, (0, 0, 3), (0, 0, -2))
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    hits = [h["entity"] for h in got["hits"]]
    ed.check(hits and hits[0] == chair, f"the chair is the nearest hit, not the room around the camera: {got['hits']}")
    ed.check(room not in hits, "the room's walls face away from the camera: culled, never hit")
    ed.check_eq(got["would_select"]["entity"], chair)
    ed.check(got["hits"][0]["kind"] == "mesh" and got["hits"][0]["distance"] > 0, f"a triangle hit: {got['hits'][0]}")
    # pick_at "none" changes nothing
    ed.check(chair not in await selected(ed, sid), "pick_at without apply selected")


async def build_robot(ed: Editor) -> dict:
    """A prefab instance Robot holding Arm, in front of a Wall; the camera looks at the Arm."""
    if "robot" in S:
        await ed.call("select_scene", scene=S["robot"]["scene"])
        return S["robot"]
    sid = await new_scene(ed, "Drill")
    robot = (await ed.call("create_entity", scene=sid, type="empty", name="Robot", position=[0, 0, 0], select=False))["entity_id"]
    arm = await cube(ed, sid, "Arm", (0, 0, 0), parent=robot)
    wall = await cube(ed, sid, "Wall", (0, 0, -3), (8, 8, 0.2))
    await ed.call("create_prefab", scene=sid, entity=robot, path="prefabs/Robot.prefab")
    await look(ed, sid, (0, 0, 5), (0, 0, 0))
    S["robot"] = {"scene": sid, "robot": robot, "arm": arm, "wall": wall}
    return S["robot"]


async def test_root_then_drill(ed: Editor):
    r = await build_robot(ed)
    sid = r["scene"]
    await clear_selection(ed, sid)
    await ed.call("pick_at", scene=sid, u=0.5, v=0.5, apply="select")
    ed.check_eq(await selected(ed, sid), [r["robot"]], "click 1: the prefab instance root")
    got = await click(ed, sid)
    ed.check_eq(await selected(ed, sid), [r["arm"]], "click 2 at the same spot: the clicked child")
    ed.check(got["cycle"]["count"] == 3, f"candidates root, arm, wall: {got['candidates']}")
    await click(ed, sid)
    ed.check_eq(await selected(ed, sid), [r["wall"]], "click 3: the next hit behind")


async def test_cycle_wraps(ed: Editor):
    r = await build_robot(ed)
    sid = r["scene"]
    await clear_selection(ed, sid)
    seen = []
    await ed.call("pick_at", scene=sid, u=0.5, v=0.5, apply="select")
    seen.append((await selected(ed, sid))[0])
    for _ in range(3):
        await click(ed, sid)
        seen.append((await selected(ed, sid))[0])
    ed.check_eq(seen, [r["robot"], r["arm"], r["wall"], r["robot"]], "the fourth click wraps to the root")
    # a click away from the spot starts over: the wall alone there
    await click(ed, sid, u=0.7, v=0.5)
    ed.check_eq(await selected(ed, sid), [r["wall"]])


async def test_exact_pick_toggle(ed: Editor):
    r = await build_robot(ed)
    sid = r["scene"]
    await clear_selection(ed, sid)
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5, exact=True)
    ed.check_eq(got["would_select"]["entity"], r["arm"], "Exact Pick: no root")
    ed.check(all(c["entity"] != r["robot"] for c in got["candidates"]), "no root among the candidates")
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5, exact=False)
    ed.check_eq(got["would_select"]["entity"], r["robot"])


async def test_shift_ctrl_modifiers(ed: Editor):
    r = await build_robot(ed)
    sid = r["scene"]
    await clear_selection(ed, sid)
    await ed.call("pick_at", scene=sid, u=0.5, v=0.5, apply="select", exact=True)
    ed.check_eq(await selected(ed, sid), [r["arm"]])
    # Shift adds (the wall, beside the arm)
    await click(ed, sid, u=0.7, v=0.5, shift=True)
    ed.check_eq(sorted(await selected(ed, sid)), sorted([r["arm"], r["wall"]]), "Shift adds")
    # Ctrl toggles: on the arm, the exact hit, since it is selected (not its root)
    await click(ed, sid, u=0.5, v=0.5, ctrl=True)
    ed.check_eq(await selected(ed, sid), [r["wall"]], "Ctrl toggled the selected exact hit off")
    await click(ed, sid, u=0.5, v=0.5, ctrl=True)
    ed.check(r["robot"] in await selected(ed, sid) or r["arm"] in await selected(ed, sid), "Ctrl toggles it back on")
    # a miss clears; with a modifier it does not (the camera looks away from everything)
    await look(ed, sid, (0, 0, 5), (0, 0, 10))
    before = await selected(ed, sid)
    await click(ed, sid, u=0.02, v=0.02, shift=True)
    ed.check_eq(await selected(ed, sid), before, "a modified miss keeps the selection")
    await click(ed, sid, u=0.02, v=0.02)
    ed.check_eq(await selected(ed, sid), [], "a miss clears the selection")
    await look(ed, sid, (0, 0, 5), (0, 0, 0))


async def test_selection_undo(ed: Editor):
    r = await build_robot(ed)
    sid = r["scene"]
    await clear_selection(ed, sid)
    await ed.call("wait", frames=2)
    states = []
    await ed.call("pick_at", scene=sid, u=0.5, v=0.5, apply="select")
    states.append(await selected(ed, sid))
    for _ in range(2):
        await click(ed, sid)
        states.append(await selected(ed, sid))
    ed.check_eq(states, [[r["robot"]], [r["arm"]], [r["wall"]]])
    # one Ctrl+Z per click
    for expected in ([r["arm"]], [r["robot"]], []):
        await ed.call("undo", scene=sid)
        ed.check_eq(await selected(ed, sid), expected, "one undo step per click")


async def test_prefab_mode_root_not_swallowing(ed: Editor):
    await build_robot(ed)
    stage = (await ed.call("prefab_mode", action="open", prefab="prefabs/Robot.prefab", auto_save=False))["scene_id"]
    try:
        await ed.call("wait", frames=10)
        await ed.call("set_scene_camera", scene=stage, position=[0, 0, 5], target=[0, 0, 0])
        got = await ed.call("pick_at", scene=stage, u=0.5, v=0.5)
        ed.check(got["would_select"] is not None, f"the stage's arm is hit: {got}")
        ed.check(got["would_select"]["path"].endswith("Arm"), f"in Prefab Mode the stage root is never a pick root: {got['would_select']}")
    finally:
        await ed.call("prefab_mode", action="close", unsaved="discard")


async def scene_view_state(ed: Editor, sid: int) -> dict:
    import json  # noqa: PLC0415
    result = await ed.session.read_resource(f"jasper://scene-view/{sid}")
    return json.loads(result.contents[0].text)


async def test_pick_waits_for_pending_bvh(ed: Editor):
    """AC7: a large imported mesh whose BVH is still building (held pending by
    JASPER_PICK_BUILD_DELAY_MS) is clicked at once: the click waits for (or builds) its tree and
    selects triangle-exact, never through its bounds. A lattice of bars: its bounds cover its holes."""
    sid = await new_scene(ed, "Pending")
    path = ed.project / "assets" / "models" / "lattice.obj"
    path.parent.mkdir(parents=True, exist_ok=True)
    n = 400
    c = n * 0.05
    lines = []
    for i in range(n + 1):
        for j in range(n + 1):
            lines.append(f"v {i * 0.1 - c:.3f} {j * 0.1 - c:.3f} 0")
    for i in range(n):
        for j in range(n):
            if i % 4 == 0 or j % 4 == 0:   # bars every 4 cells, square holes between them
                a = i * (n + 1) + j + 1
                lines.append(f"f {a} {a + n + 1} {a + 1}")
                lines.append(f"f {a + 1} {a + n + 1} {a + n + 2}")
    path.write_text("\n".join(lines) + "\n")
    await ed.call("sync_files")
    behind = await cube(ed, sid, "Behind", (0, 0, -2), (60, 60, 0.1))
    made = await ed.call("create_entity", scene=sid, type="model", name="Lattice", model="assets/models/lattice.obj", select=False)
    lattice = made["entity_id"]
    hole = 0.25 - c      # the centre of the hole of cells 1..3
    await look(ed, sid, (hole, hole, 3), (hole, hole, 0))
    await ed.screenshot("scene", scene=sid, max_width=256)   # loaded and drawn
    state = await scene_view_state(ed, sid)
    ed.check(state["bvh_cache"]["pending"] >= 1, f"the lattice's tree is still building: {state['bvh_cache']}")
    # through a hole: the wall behind, not the lattice's bounds
    got = await click(ed, sid, u=0.5, v=0.5)
    ed.check(got["would_select"] is not None and got["would_select"]["entity"] == behind,
             f"the click through a hole selects what is behind it: {got['hits'][:3]}")
    ed.check(all(not h["pending"] for h in got["hits"]), "no pending (bounds) hit is reported")
    # on a bar: the lattice (its model root)
    bar = 0.05 - c
    await look(ed, sid, (bar, hole, 3), (bar, hole, 0))
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check(got["would_select"] is not None and got["would_select"]["entity"] == lattice, f"a bar hits the lattice: {got['hits'][:3]}")


async def test_2d_draw_order_kept(ed: Editor):
    sid = await new_scene(ed, "Flat", "2D")
    first = (await ed.call("create_entity", scene=sid, type="mesh_polygon", name="Under", select=False))["entity_id"]
    await ed.call("set_shape_points", scene=sid, entity=first, points=[[0, 0], [100, 0], [100, 100], [0, 100]], color=[1, 0, 0])
    second = (await ed.call("create_entity", scene=sid, type="mesh_polygon", name="Over", select=False))["entity_id"]
    await ed.call("set_shape_points", scene=sid, entity=second, points=[[40, 40], [140, 40], [140, 140], [40, 140]], color=[0, 0, 1])
    await ed.call("set_scene_camera", scene=sid, center=[70, 70], zoom=0.25)
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check(got["would_select"] is not None and got["would_select"]["entity"] == second,
             f"2D: the last drawn (higher Transform index) first: {got['hits']}")
    ed.check_eq([h["entity"] for h in got["hits"]][:2], [second, first])


async def test_ui_rect_pick_kept(ed: Editor):
    sid = await new_scene(ed, "Hud", "UI")
    await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Back", "color": [1, 0, 0, 1],
                                                 "layout": {"anchor": "center", "size": [300, 200], "position": [0, 0]}})
    await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Front", "color": [0, 1, 0, 1],
                                                 "layout": {"anchor": "center", "size": [100, 100], "position": [0, 0]}})
    width, height = (await ed.call("configure_game_view"))["canvas"]
    await ed.call("set_scene_camera", scene=sid, center=[width / 2, height / 2], zoom=1)
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    names = [h["path"] for h in got["hits"]]
    ed.check(names[:2] == ["Front", "Back"], f"UI: the rects under the point, the last drawn first: {names}")
    ed.check(all(h["kind"] == "ui" for h in got["hits"][:2]), f"UI rect hits: {got['hits'][:2]}")


# -- projection and framing (AC11) -----------------------------------------------


async def edge_u(ed: Editor, sid: int, entity: int) -> float:
    """The u where the entity stops being hit, right of the view centre (bisection on v = 0.5)."""
    lo, hi = 0.5, 1.0
    for _ in range(14):
        mid = (lo + hi) / 2
        got = await ed.call("pick_at", scene=sid, u=mid, v=0.5, exact=True)
        if any(h["entity"] == entity for h in got["hits"]):
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


async def test_projection_toggle(ed: Editor):
    sid = await new_scene(ed, "Projection")
    card = await cube(ed, sid, "Card", (0, 0, 0), (3, 3, 0.01))
    await look(ed, sid, (0, 0, 12), (0, 0, 0))
    persp = await ed.call("set_scene_camera", scene=sid)
    ed.check_eq(persp["camera"]["projection"], "perspective")
    before = await edge_u(ed, sid, card) - 0.5
    ortho = await ed.call("set_scene_camera", scene=sid, projection="orthographic")
    ed.check_eq(ortho["camera"]["projection"], "orthographic")
    fov = ortho["camera"]["fov"]
    ed.check(close(ortho["camera"]["ortho_size"], 12 * math.tan(math.radians(fov) / 2), 1e-2),
             f"ortho half height = distance * tan(fov/2): {ortho['camera']}")
    after = await edge_u(ed, sid, card) - 0.5
    ed.check(abs(after - before) <= 0.02 * before, f"the framing is kept: {before:.4f} vs {after:.4f}")
    back = await ed.call("set_scene_camera", scene=sid, projection="perspective")
    ed.check_eq(back["camera"]["projection"], "perspective")
    again = await edge_u(ed, sid, card) - 0.5
    ed.check(abs(again - before) <= 0.02 * before, f"and back: {before:.4f} vs {again:.4f}")
    # the wheel scales the half height in ortho (here: ortho_size)
    await ed.call("set_scene_camera", scene=sid, projection="orthographic", ortho_size=2.0)
    small = await edge_u(ed, sid, card) - 0.5
    ed.check(small > after, "a smaller half height shows the card bigger")
    await ed.call("set_scene_camera", scene=sid, projection="perspective")


async def test_axis_view_goes_ortho(ed: Editor):
    sid = await new_scene(ed, "Axis")
    await cube(ed, sid, "Box", (0, 0, 0))
    await look(ed, sid, (6, 4, 6), (0, 0, 0))
    got = await ed.call("set_scene_camera", scene=sid, view="top")
    ed.check_eq(got["camera"]["projection"], "orthographic", "an axis view is orthographic while aligned")
    ed.check_eq(got["camera"]["view"], "top")
    ed.check(close(got["camera"]["position"][0], 0, 1e-3) and close(got["camera"]["position"][2], 0, 1e-3), f"above the target: {got['camera']}")
    # rotating out of the axis returns to the user's projection
    await ed.call("set_scene_camera", scene=sid, orbit={"pitch": -30})
    await ed.call("wait", frames=3)
    got = await ed.call("set_scene_camera", scene=sid)
    ed.check_eq(got["camera"]["view"], "free")
    ed.check_eq(got["camera"]["projection"], "perspective", "out of the axis: perspective again")
    # an explicit Iso stays when leaving the axis
    await ed.call("set_scene_camera", scene=sid, projection="orthographic")
    await ed.call("set_scene_camera", scene=sid, view="front")
    await ed.call("set_scene_camera", scene=sid, orbit={"yaw": 25})
    await ed.call("wait", frames=3)
    got = await ed.call("set_scene_camera", scene=sid)
    ed.check_eq(got["camera"]["projection"], "orthographic", "the user's Iso is kept")
    await ed.call("set_scene_camera", scene=sid, projection="perspective")


def project(camera: dict, size: tuple[int, int], point) -> tuple[float, float, float]:
    """(u, v, depth) of a world point through the reported camera (perspective or orthographic)."""
    pos, target, up = camera["position"], camera["target"], camera.get("up", [0, 1, 0])
    f = [target[i] - pos[i] for i in range(3)]
    fl = math.sqrt(sum(c * c for c in f))
    f = [c / fl for c in f]
    r = [f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]]
    rl = math.sqrt(sum(c * c for c in r)) or 1.0
    r = [c / rl for c in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    d = [point[i] - pos[i] for i in range(3)]
    x, y, z = sum(d[i] * r[i] for i in range(3)), sum(d[i] * u[i] for i in range(3)), sum(d[i] * f[i] for i in range(3))
    aspect = size[0] / size[1]
    if camera["projection"] == "orthographic":
        half_h = camera["ortho_size"]
        return 0.5 + x / (2 * half_h * aspect), 0.5 - y / (2 * half_h), z
    t = math.tan(math.radians(camera["fov"]) / 2)
    return 0.5 + x / (2 * z * t * aspect), 0.5 - y / (2 * z * t), z


async def view_size(ed: Editor, sid: int) -> tuple[int, int]:
    layout = await ed.call("set_scene_view_layout", scene=sid)
    active = [p for p in layout["panes"] if p["active"]][0]
    return tuple(active["size"])


async def test_alt_f_frames_all(ed: Editor):
    sid = await new_scene(ed, "Frame")
    spots = [((-20, 0, 0), (1, 1, 1)), ((15, 3, -4), (2, 2, 2)), ((0, 9, 12), (1, 3, 1)), ((5, -6, 30), (1, 1, 1))]
    for i, (pos, scale) in enumerate(spots):
        await cube(ed, sid, f"Box{i}", pos, scale)
    await ed.call("create_entity", scene=sid, type="terrain", name="Ground", select=False)
    await look(ed, sid, (200, 200, 200), (100, 0, 0))
    got = await ed.call("set_scene_camera", scene=sid, frame_all=True)
    cam = got["camera"]
    size = await view_size(ed, sid)
    for pos, scale in spots:
        for corner in range(8):
            p = [pos[k] + (0.5 if corner >> k & 1 else -0.5) * scale[k] for k in range(3)]
            u, v, depth = project(cam, size, p)
            ed.check(depth > 0 and -0.01 <= u <= 1.01 and -0.01 <= v <= 1.01, f"{p} projects inside the view: {(u, v, depth)} {cam}")
    # the terrain (large) does not count: the boxes fill a good part of the view
    uvs = [project(cam, size, pos)[:2] for pos, _ in spots]
    spread = max(max(u for u, _ in uvs) - min(u for u, _ in uvs), max(v for _, v in uvs) - min(v for _, v in uvs))
    ed.check(spread > 0.3, f"framed on the boxes, not the terrain: {uvs}")


async def test_ortho_top_golden(ed: Editor):
    """The grid lies in the plane an orthographic axis view looks at (golden, glcore)."""
    sid = await new_scene(ed, "OrthoTop")
    # the default light's icon would sit in the middle of the image: the grid and the marker only
    lights = await ed.call("find_entities", scene=sid, name="Directional Light")
    if lights["entities"]:
        await ed.call("delete_entities", scene=sid, entities=[e["id"] for e in lights["entities"]])
    await cube(ed, sid, "Marker", (2, 0, -1))
    await look(ed, sid, (0, 10, 0.001), (0, 0, 0))
    await ed.call("set_scene_camera", scene=sid, view="top", ortho_size=6)
    await ed.call("wait", frames=4)
    png = await ed.screenshot("scene", scene=sid, max_width=0)
    img = pngdiff.decode(png)              # (width, height, rgba)
    img = pngdiff.downscale(img, 320, int(320 * img[1] / img[0]))
    golden = GOLDEN_DIR / "scene_view_ortho_top.png"
    if UPDATE_GOLDEN or not golden.exists():
        golden.parent.mkdir(parents=True, exist_ok=True)
        golden.write_bytes(pngdiff.encode(img))
        if not UPDATE_GOLDEN:
            ed.fail(f"no golden yet: wrote {golden} (review and commit it)")
        return
    ref = pngdiff.decode(golden.read_bytes())
    img, ref = pngdiff.same_frame(img, ref)
    ok, fraction, _diff = pngdiff.compare(img, ref, channel_tolerance=24, max_fraction=0.03)
    (ed.scratch.root / "scene_view_ortho_top.png").write_bytes(pngdiff.encode(img))
    ed.check(ok, f"ortho top view differs from its golden: {fraction:.4f} of the pixels")


# -- bookmarks (AC12) -------------------------------------------------------------


async def test_bookmarks(ed: Editor):
    sid = await new_scene(ed, "Marks")
    await cube(ed, sid, "Box", (0, 0, 0))
    await look(ed, sid, (3, 2, 7), (0, 0.5, 0))
    stored = await ed.call("edit_scene_view_bookmarks", scene=sid, action="store", slot=3)
    pose = [b for b in stored["bookmarks"] if b["slot"] == 3][0]
    ed.check(pose["defined"] and "preview_path" in pose, f"slot 3 stored with a preview: {pose}")
    preview = pose["preview_path"]
    data = open(preview, "rb").read()
    ed.check(data[:8] == b"\x89PNG\r\n\x1a\n", "the preview is a PNG")
    w, h = struct.unpack(">II", data[16:24])
    ed.check_eq((w, h), (160, 90), "160x90 preview")
    ed.check(f"bookmarks" in preview and preview.endswith("/3.png"), f"at .jasperengine/user/bookmarks/<scene>/3.png: {preview}")
    await ed.call("edit_scene_view_bookmarks", scene=sid, action="rename", slot=3, name="Entrance")
    await look(ed, sid, (-9, 6, -2), (4, 0, 4))
    recalled = await ed.call("edit_scene_view_bookmarks", scene=sid, action="recall", slot=3)
    cam = recalled["camera"]
    ed.check(close(cam["position"], [3, 2, 7], 1e-4) and close(cam["target"], [0, 0.5, 0], 1e-4), f"recall restores the pose: {cam}")
    listed = await ed.call("edit_scene_view_bookmarks", scene=sid)
    ed.check([b["name"] for b in listed["bookmarks"] if b["slot"] == 3] == ["Entrance"], "renamed")
    # it survives a restart (the workspace) -- the reopen is checked by test_bookmark_survives_restart
    await ed.call("save_scene", scene=sid)
    S["marks"] = {"scene_path": "scenes/Marks.scene"}
    deleted = await ed.call("edit_scene_view_bookmarks", scene=sid, action="delete", slot=3)
    ed.check(not [b for b in deleted["bookmarks"] if b["slot"] == 3][0]["defined"], "deleted")
    ed.check(not os.path.exists(preview), "its preview went with it")
    await ed.call("edit_scene_view_bookmarks", scene=sid, action="store", slot=3)
    await ed.call("save_project")


async def test_bookmark_survives_restart(ed: Editor):
    if "marks" not in S:
        harness.skip("test_bookmarks did not run")
    project = ed.project
    await ed.call("save_project")
    async with Editor(project, name="Scene View Again", env={"JASPER_PICK_BUILD_DELAY_MS": "0"}) as again:
        opened = await again.call("open_scene", scene=S["marks"]["scene_path"])
        sid = opened["scene"]["id"]
        listed = await again.call("edit_scene_view_bookmarks", scene=sid)
        slot = [b for b in listed["bookmarks"] if b["slot"] == 3][0]
        again.check(slot["defined"], f"slot 3 after a restart: {listed}")
        again.check(close(slot["pose"]["position"], [3, 2, 7], 1e-3), f"its pose: {slot}")


# -- layouts (AC13) ---------------------------------------------------------------


async def test_quad_layout(ed: Editor):
    sid = await new_scene(ed, "Quad")
    await cube(ed, sid, "Box", (0, 0, 0))
    await cube(ed, sid, "Tall", (3, 2, 0), (1, 4, 1))
    await look(ed, sid, (8, 6, 8), (0, 0, 0))
    got = await ed.call("set_scene_view_layout", scene=sid, layout="quad", capture=True)
    ed.check_eq(got["layout"], "quad")
    axes = [p["axis"] for p in got["panes"]]
    ed.check_eq(axes, ["free", "top", "front", "right"], "Persp, Top, Front, Side")
    ed.check(all(p["size"][0] > 0 and p["size"][1] > 0 for p in got["panes"]), f"every pane laid out: {got['panes']}")
    raw = await ed.call_raw("set_scene_view_layout", {"scene": sid, "capture": True})
    ed.check(len(raw.images) == 4, f"an image per pane: {len(raw.images)}")
    for i, (data, _) in enumerate(raw.images):
        img = pngdiff.decode(data)
        ed.check(not pngdiff.flat(img), f"pane {i} drew something")
        (ed.scratch.root / f"quad-pane-{i}.png").write_bytes(data)
    # picking in the Top pane: its own (orthographic, from above) ray
    top = await ed.call("pick_at", scene=sid, pane=1, u=0.5, v=0.5, exact=True)
    # (the Directional Light's icon may be first along that ray: the first mesh)
    meshes = [h for h in top["hits"] if h["kind"] == "mesh"]
    ed.check(meshes and meshes[0]["normal"][1] > 0.9, f"from above: the top face {top['hits'][:2]}")
    layout = await ed.call("set_scene_view_layout", scene=sid)
    ed.check_eq(layout["active_pane"], 1, "the picked pane is the active one")
    # idle panes do not redraw: once the editor is idle (the Project window's thumbnails and the
    # new meshes' loading make the view redraw for a while), 60 frames redraw no inactive pane
    draws = {p["pane"]: p["draws"] for p in layout["panes"]}
    later = None
    for _ in range(5):
        await ed.call("wait", frames=60)
        later = {p["pane"]: p["draws"] for p in (await ed.call("set_scene_view_layout", scene=sid))["panes"]}
        if later == draws:
            break
        draws = later
    ed.check_eq(later, draws, "nothing changed: no inactive pane redrew over 60 frames (within 300)")
    # a change in the scene redraws them
    await ed.call("set_transform", scene=sid, entity="Box", position=[0, 1, 0])
    await ed.call("wait", frames=4)
    moved = {p["pane"]: p["draws"] for p in (await ed.call("set_scene_view_layout", scene=sid))["panes"]}
    ed.check(any(moved[p] > draws[p] for p in moved if p != 1), f"a scene change redraws the inactive panes: {draws} -> {moved}")
    # pane settings and maximize
    got = await ed.call("set_scene_view_layout", scene=sid, pane_settings=[{"pane": 2, "axis": "back", "shading": "shaded_shadows"}],
                        maximized_pane=3)
    ed.check_eq(got["panes"][2]["axis"], "back")
    ed.check_eq(got["panes"][2]["shading"], "shaded_shadows")
    ed.check_eq(got["maximized_pane"], 3)
    got = await ed.call("set_scene_view_layout", scene=sid, maximized_pane=-1, layout="split")
    ed.check_eq([p["axis"] for p in got["panes"]], ["free", "top"], "Split: Persp | Top")
    got = await ed.call("set_scene_view_layout", scene=sid, layout="single")
    ed.check_eq(len(got["panes"]), 1)


# -- camera settings (AC14) -------------------------------------------------------


async def test_camera_settings(ed: Editor):
    sid = await new_scene(ed, "Camera")
    await cube(ed, sid, "Box", (0, 0, 0))
    got = await ed.call("set_scene_camera", scene=sid,
                        camera_settings={"fov": 45, "near": 0.05, "far": 900, "speed": 22, "easing": False, "acceleration": False,
                                         "dynamic_clipping": False})
    cs = got["camera"]["camera_settings"]
    ed.check(close(got["camera"]["fov"], 45) and close(cs["speed"], 22) and cs["easing"] is False and cs["acceleration"] is False,
             f"settings applied: {got['camera']}")
    ed.check(close(got["camera"]["near"], 0.05, 1e-4) and close(got["camera"]["far"], 900), "near and far on the camera")
    await ed.call("save_scene", scene=sid)
    await ed.call("save_project")
    workspace = (ed.project / ".jasperengine" / "user" / "workspace.yaml").read_text()
    ed.check("cameraSettings:" in workspace and "speed: 22" in workspace, "kept in workspace.yaml")
    # dynamic clipping: near and far follow the target distance
    got = await ed.call("set_scene_camera", scene=sid, camera_settings={"dynamic_clipping": True}, position=[0, 0, 400], target=[0, 0, 0])
    far_view = got["camera"]
    got = await ed.call("set_scene_camera", scene=sid, position=[0, 0, 4], target=[0, 0, 0])
    await ed.call("wait", frames=2)
    near_view = (await ed.call("set_scene_camera", scene=sid))["camera"]
    ed.check(near_view["near"] < far_view["near"], f"near follows the distance: {near_view['near']} vs {far_view['near']}")
    await ed.call("set_scene_camera", scene=sid, camera_settings={"dynamic_clipping": False})


async def test_legacy_walk_speed_migrates(ed: Editor):
    """A workspace written before S26 (editorCamera.walkSpeed 5, none of the S26 keys) loads as
    Camera Speed 15 in a second editor (on a copy of this project)."""
    import yaml  # noqa: PLC0415 - the MCP requirements have PyYAML
    sid = await new_scene(ed, "Legacy")
    await ed.call("save_scene", scene=sid)
    await ed.call("save_project")
    path = ed.project / ".jasperengine" / "user" / "workspace.yaml"
    doc = yaml.safe_load(path.read_text())
    entry = None
    for key, value in (doc.get("scenes") or {}).items():
        if str(key).endswith("Legacy.scene"):
            entry = value
    ed.check(entry is not None, f"a workspace entry for the scene: {list((doc.get('scenes') or {}).keys())}")
    for key in ("cameraSettings", "view", "bookmarks", "measures", "layerFilter", "hiddenEntities", "pickLockedEntities"):
        entry.pop(key, None)
    entry.setdefault("editorCamera", {})["walkSpeed"] = 5
    original = path.read_text()
    path.write_text(yaml.safe_dump(doc))
    try:
        async with Editor(ed.project, name="Legacy", env={"JASPER_PICK_BUILD_DELAY_MS": "0"}) as again:
            opened = await again.call("open_scene", scene="scenes/Legacy.scene")
            got = await again.call("set_scene_camera", scene=opened["scene"]["id"])
            again.check(close(got["camera"]["camera_settings"]["speed"], 15), f"walkSpeed 5 = Camera Speed 15: {got['camera']}")
    finally:
        path.write_text(original)


# -- filters (AC15) ---------------------------------------------------------------


async def test_layer_filter(ed: Editor):
    sid = await new_scene(ed, "Layers")
    near = await cube(ed, sid, "Near", (0, 0, 0))
    far = await cube(ed, sid, "Far", (0, 0, -4), (4, 4, 0.2))
    await ed.call("set_entity_state", scene=sid, entity=near, layer=8)
    await look(ed, sid, (0, 0, 5), (0, 0, 0))
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(got["would_select"]["entity"], near)
    # a locked layer is drawn but not picked
    await ed.call("set_scene_view_filter", scene=sid, pickable_layers=0xFFFFFFFF & ~(1 << 8))
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(got["would_select"]["entity"], far, "the locked layer's cube is skipped")
    # a hidden layer is not drawn in the Scene view (and not picked), the Game view still draws it
    await ed.call("set_scene_view_filter", scene=sid, pickable_layers=0xFFFFFFFF, visible_layers=0xFFFFFFFF & ~(1 << 8))
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(got["would_select"]["entity"], far, "a hidden layer is not picked")
    state = await ed.call("set_scene_view_filter", scene=sid)
    ed.check_eq(state["visible_layers"], 0xFFFFFFFF & ~(1 << 8))
    await ed.call("set_scene_view_filter", scene=sid, visible_layers=0xFFFFFFFF)


async def test_hide_and_lock_entity(ed: Editor):
    sid = await new_scene(ed, "Hide")
    group = (await ed.call("create_entity", scene=sid, type="empty", name="Group", select=False))["entity_id"]
    inner = await cube(ed, sid, "Inner", (0, 0, 0), parent=group)
    back = await cube(ed, sid, "Back", (0, 0, -4), (4, 4, 0.2))
    await look(ed, sid, (0, 0, 5), (0, 0, 0))
    got = await ed.call("set_scene_view_filter", scene=sid, hide=[group])
    ed.check_eq(sorted(e["id"] for e in got["hidden"]), sorted([group, inner]), "hide takes the children")
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(got["would_select"]["entity"], back, "a hidden GameObject is not picked")
    # the scene file never holds it
    await ed.call("save_scene", scene=sid)
    text = (ed.project / "scenes" / "Hide.scene").read_text()
    ed.check("hidden" not in text.lower() or "hiddenEntities" not in text, "the scene file is unchanged by hiding")
    got = await ed.call("set_scene_view_filter", scene=sid, show_all=True)
    ed.check_eq(got["hidden"], [])
    got = await ed.call("set_scene_view_filter", scene=sid, lock=[inner], children=False)
    ed.check_eq([e["id"] for e in got["locked"]], [inner])
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5, exact=True)
    ed.check_eq(got["would_select"]["entity"], back, "a locked GameObject is drawn but not picked")
    await ed.call("set_scene_view_filter", scene=sid, unlock=[inner])
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5, exact=True)
    ed.check_eq(got["would_select"]["entity"], inner)


async def test_hidden_entities_play_and_export(ed: Editor):
    """Hidden in the Scene view only: the scene's saved bytes are the same, and the game (Game
    view) still draws the GameObject."""
    sid = await new_scene(ed, "HideGame")
    box = await cube(ed, sid, "Box", (0, 0, 0))
    await ed.call("create_entity", scene=sid, type="camera", name="Cam", position=[0, 0, 6], select=False)
    await ed.call("save_scene", scene=sid)
    before = (ed.project / "scenes" / "HideGame.scene").read_bytes()
    await ed.call("set_scene_view_filter", scene=sid, hide=[box])
    await ed.call("save_scene", scene=sid)
    after = (ed.project / "scenes" / "HideGame.scene").read_bytes()
    ed.check(before == after, "hiding changed the scene file")
    ray = await ed.call("raycast_meshes", scene=sid, origin=[0, 0, 6], direction=[0, 0, -1])
    ed.check(ray["hits"] and ray["hits"][0]["entity"] == box, "the runtime raycast ignores editor visibility")
    await ed.call("set_scene_view_filter", scene=sid, show_all=True)


# -- measures (AC16) --------------------------------------------------------------


async def test_measure(ed: Editor):
    sid = await new_scene(ed, "Measure")
    await cube(ed, sid, "Left", (-3, 0, 0), (2, 2, 2))
    await cube(ed, sid, "Right", (3, 0, 0), (2, 2, 2))
    # faces at x = -2 and x = 2: 4 m apart; looking along +z from the front, a little above
    got = await ed.call("measure_scene_view", scene=sid, action="add", **{"from": [-2, 0, 0], "to": [2, 0, 0]})
    ed.check(close(got["length"], 4.0, 1e-4) and close(got["delta"], [4, 0, 0], 1e-4), f"world measure: {got}")
    # surface snap from the screen: the camera looks at the gap between the faces from above
    await look(ed, sid, (0, 0.5, 12), (0, 0.5, 0))
    size = await view_size(ed, sid)
    cam = (await ed.call("set_scene_camera", scene=sid))["camera"]
    ua, va, _ = project(cam, size, (-2.0, 0.5, 0.9))
    ub, vb, _ = project(cam, size, (2.0, 0.5, 0.9))
    # just inside the front faces' x range (the cube's face toward the camera at z = 1)
    pa = [(ua - 0.004) * size[0], va * size[1]]
    pb = [(ub + 0.004) * size[0], vb * size[1]]
    surf = await ed.call("measure_scene_view", scene=sid, action="add", from_screen=pa, to_screen=pb, snap="surface")
    ed.check(abs(surf["from"][2] - 1.0) < 1e-3 and abs(surf["to"][2] - 1.0) < 1e-3, f"on the front faces (z = 1): {surf}")
    # vertex snap: the nearest corner of the hit triangle
    corner_u, corner_v, _ = project(cam, size, (-2.0, 1.0, 1.0))
    near_corner = [(corner_u - 0.003) * size[0], (corner_v + 0.003) * size[1]]
    vert = await ed.call("measure_scene_view", scene=sid, action="add", from_screen=near_corner, to=[0, 0, 0], snap="vertex")
    ed.check(close(vert["from"], [-2, 1, 1], 1e-3), f"vertex snap hits the corner: {vert['from']}")
    listed = await ed.call("measure_scene_view", scene=sid, action="list")
    ed.check_eq(len(listed["measures"]), 3)
    # kept in the workspace
    await ed.call("save_scene", scene=sid)
    await ed.call("save_project")
    workspace = (ed.project / ".jasperengine" / "user" / "workspace.yaml").read_text()
    ed.check("measures:" in workspace, "measures are workspace data")
    await ed.call("measure_scene_view", scene=sid, action="delete", id=got["id"])
    ed.check_eq(len((await ed.call("measure_scene_view", scene=sid, action="list"))["measures"]), 2)
    await ed.call("measure_scene_view", scene=sid, action="clear")
    ed.check_eq((await ed.call("measure_scene_view", scene=sid, action="list"))["measures"], [])


# -- resource ---------------------------------------------------------------------


async def test_scene_view_resource(ed: Editor):
    sid = await new_scene(ed, "Resource")
    await cube(ed, sid, "Box", (0, 0, 0))
    await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    data = await scene_view_state(ed, sid)
    for key in ("camera", "layout", "bookmarks", "measures", "filters", "bvh_cache"):
        ed.check(key in data, f"jasper://scene-view has {key}")
    for key in ("built", "pending", "bytes", "build_ms_total"):
        ed.check(key in data["bvh_cache"], f"bvh_cache.{key}")


# -- straight down (agent-authoring) -----------------------------------------------


async def test_camera_straight_down_and_degenerate_aims(ed: Editor):
    """red before the fix: aimed straight down the camera kept up (0, 1, 0), parallel to the view
    (lookAt's side vector was zero); a target equal to the position was accepted"""
    sid = await new_scene(ed, "TopDown")
    # away from the new scene's camera and light, whose icons the centre would pick
    floor = await cube(ed, sid, "Floor", (40, 0, 40), (6, 0.2, 6))
    got = await ed.call("set_scene_camera", scene=sid, position=[40, 25, 40], target=[40, 0, 40])
    ed.check(close(got["camera"]["up"], [0, 0, 1]), f"the Top view's up: {got['camera']}")
    picked = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(picked["would_select"]["entity"], floor, "the view's centre is the floor under it")
    got = await ed.call("set_scene_camera", scene=sid, position=[0, -25, 0], target=[0, 0, 0])
    ed.check(close(got["camera"]["up"], [0, 0, -1]), f"the Bottom view's up: {got['camera']}")
    got = await ed.call("set_scene_camera", scene=sid, position=[0, 5, -10], target=[0, 0, 0])
    ed.check(close(got["camera"]["up"], [0, 1, 0]), f"an oblique view keeps the world up: {got['camera']}")
    err = await ed.call("set_scene_camera", scene=sid, position=[1, 2, 3], target=[1, 2, 3], expect_error=True)
    ed.check_in("target must differ from position", err, "a zero view direction is refused")
    err = await ed.call("set_scene_camera", scene=sid, position=[0, 0, 0], expect_error=True)
    ed.check_in("target must differ", err, "a position on the current target is refused")
    got = await ed.call("set_scene_camera", scene=sid)
    ed.check(close(got["camera"]["position"], [0, 5, -10]), f"nothing changed: {got['camera']}")
