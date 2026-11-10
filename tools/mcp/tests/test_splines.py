"""S18 splines through MCP (docs/specs/S18-splines.md R27, AC10-AC14, AC18): edit_spline,
sample_spline and build_spline, the splines group, a road drawn on a fractal terrain and baked
into it (one undo step), a scene with all four spline components saved byte-identically after a
reload with no chunk buffers in it, the Lua Spline API agreeing with sample_spline, a Spline
Animate reaching its end in Play, knot edits undone one by one (a batch of drags one step), 2D
shapes (a filled concave shape drawn, an edge a ball rests on) and the tools' budget.

Runs in one new project; tests run in order and share the editor.
"""

from __future__ import annotations

import asyncio
import difflib
import importlib.util
import math
import sys
from pathlib import Path

from harness import Editor, find_key

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import tool_budget  # noqa: E402

_spec = importlib.util.spec_from_file_location("splines_test_play", HERE / "test_play.py")
_play = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_play)
decode_png = _play.decode_png

CI = True
EDITOR = {"create": True, "name": "Splines CI"}
TIMEOUT = 1500

TOOLS = ("edit_spline", "sample_spline", "build_spline")
MAX_HEIGHT = 20.0

S: dict = {}


def distinct_colors(png: bytes, box=None) -> int:
    w, h, ch, rows = decode_png(png)
    x0, y0, x1, y1 = box or (0, 0, w, h)
    seen = set()
    for y in range(y0, y1, 5):
        row = rows[y]
        for x in range(x0, x1, 5):
            seen.add(tuple(row[x * ch:x * ch + 3]))
    return len(seen)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code, allow_edit_mode=True))["values"]


async def setup(ed: Editor) -> dict:
    """A 3D scene with a 129 x 129 fractal terrain (max height 20) and the splines group."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["splines", "terrain"])
    made = await ed.call("create_scene", name="Roads", type="3D", path="scenes/Roads")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    terrain = await ed.call("create_entity", name="Ground", type="terrain")
    S["terrain"] = terrain["entity_id"]
    await ed.call("set_properties", entity=S["terrain"], values={"terrain": {"terrainSize": 128, "maxHeight": MAX_HEIGHT}})
    await ed.call("set_terrain_maps", entity=S["terrain"],
                  heightmap={"generate": "fractal_noise", "resolution": 129, "seed": 11, "amplitude": 0.25})
    return S


async def heights(ed: Editor, points) -> list[float]:
    probe = await ed.call("sample_terrain", entity=S["terrain"], points=[[p[0], p[2]] for p in points])
    return [s["height"] for s in probe["samples"]]


async def test_group(ed: Editor):
    """The spline tools are the splines group's: hidden by default, listed once enabled."""
    ed.require_tools("tool_groups", *TOOLS)
    await ed.call("tool_groups", disable=["splines"])
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"hidden by default: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["splines"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(TOOLS) <= listed, f"listed once enabled: {sorted(set(TOOLS) - listed)}")


async def test_budget(ed: Editor):
    """AC18: the three tools add at most 6,000 characters to the listing."""
    tools = {t.name: t for t in await ed.tools(groups=["splines"])}
    total = sum(tool_budget.measure(tools[name])["total"] for name in TOOLS)
    print(f"     [budget] splines tools: {total} chars")
    ed.check(total <= 6000, f"edit_spline + sample_spline + build_spline: {total} chars, over 6,000")


async def test_road_on_terrain(ed: Editor):
    """AC10: a 4-point stroke snapped to the terrain, the road preset baked into it: the terrain
    on the centreline is the road bed (the terrain under it less the crown); one undo restores it."""
    s = await setup(ed)
    stroke = [[-40, 50, -10], [-12, 50, 6], [14, 50, -6], [40, 50, 8]]
    made = await ed.call("edit_spline", create={"name": "Road"}, stroke={"points": stroke, "snap": "terrain", "tolerance": 0.25})
    s["road"] = made["entity"]["id"]
    ed.check(len(made["knots"]) >= 2 and made["length"] > 60, f"the stroke made the road: {made}")
    centre = await ed.call("sample_spline", entity=s["road"], count=12)
    points = [f["position"] for f in centre["samples"][1:-1]]
    ground = await heights(ed, points)
    for p, g in zip(points, ground):
        ed.check(abs(p[1] - g) < 0.05 * MAX_HEIGHT, f"the knots were snapped to the terrain: {p} over {g}")

    built = await ed.call("build_spline", entity=s["road"], preset="road", apply_terrain=True)
    extrude = built.get("extrude", {})
    ed.check(extrude.get("vertices", 0) > 0 and extrude.get("chunks", 0) >= 1, f"a road mesh: {built}")
    ed.check(built["terrain"]["heights"] is not None, f"the bake changed heights: {built}")
    bed = await heights(ed, points)
    for g, b in zip(ground, bed):
        # the bed: the conformed centreline less the crown (0.12), within 0.02 * maxHeight
        ed.check(abs(b - (g - 0.12)) <= 0.02 * MAX_HEIGHT, f"the bed {b} under the road over {g}")

    shot = await ed.screenshot("scene", path=ed.scratch.root / "spline_road.png" if ed.scratch else None)
    ed.check(distinct_colors(shot) > 20, "the road region is drawn")

    await ed.call("undo")
    again = await heights(ed, points)
    for g, a in zip(ground, again):
        ed.check(abs(g - a) < 1e-4, f"one undo restores the terrain: {a} vs {g}")
    await ed.call("redo")


async def test_conform_follows_sculpt(ed: Editor):
    """T1 AC18: a sculpt keeps the terrain's node tree, and a road conformed to it still follows."""
    s = await setup(ed)
    made = await ed.call("edit_spline", create={"name": "Follower"}, space="world",
                         stroke={"points": [[-30, 50, 40], [0, 50, 40], [30, 50, 40]], "snap": "terrain", "tolerance": 0.25})
    road = made["entity"]["id"]
    await ed.call("build_spline", entity=road, extrude={"shape": "crown", "conform": "terrain_height"})

    async def road_height():
        for _ in range(200):
            hits = (await ed.call("raycast_meshes", origin=[0.0, 200.0, 40.0], direction=[0.0, -1.0, 0.0], all=True))["hits"]
            built = [h for h in hits if h["entity"] != s["terrain"] and not h["pending"]]
            if built:
                return built[0]["point"][1]
            await asyncio.sleep(0.1)
        raise AssertionError("the road is not under the probe")

    before = await road_height()
    sculpted = await ed.call("sculpt_terrain", entity=s["terrain"], space="world", strokes=[
        {"mode": "raise", "points": [[0, 40]], "size": 12.0, "strength": 1.0, "seconds_per_point": 3.0}])
    ed.check(sculpted["strokes"], f"the sculpt ran: {sculpted}")
    info = (await ed.call("sample_terrain", entity=s["terrain"], grid=0))["info"]
    ed.check(info.get("mesh_built"), f"no rebuild is pending: {info}")

    after = before
    for _ in range(200):
        after = await road_height()
        if after > before + 1.0:
            break
        await asyncio.sleep(0.1)
    ed.check(after > before + 1.0, f"the road followed the raised ground: {before} to {after}")

    await ed.call("undo")
    await ed.call("delete_entities", entities=[road])


async def test_edit_undo(ed: Editor):
    """AC13: insert, update and remove undo one by one to the exact knots; ten updates in one batch
    are one undo step."""
    s = await setup(ed)
    made = await ed.call("edit_spline", create={"name": "Path"}, space="local",
                         set_knots=[[0, 0, 0], [10, 0, 0], [20, 0, 5]])
    path = made["entity"]["id"]
    k0 = made["knots"]
    k1 = (await ed.call("edit_spline", entity=path, space="local", insert=[{"index": 1, "knot": {"position": [5, 1, 1], "radius": 3}}]))["knots"]
    ed.check_eq(len(k1), 4, "inserted")
    k2 = (await ed.call("edit_spline", entity=path, space="local", update=[{"index": 2, "position": [10, 2, 0], "roll": 15}]))["knots"]
    ed.check_eq(k2[2]["roll"], 15, "updated")
    k3 = (await ed.call("edit_spline", entity=path, space="local", remove=[0]))["knots"]
    ed.check_eq(len(k3), 3, "removed")
    refused = await ed.call_raw("edit_spline", {"entity": path, "remove": [0, 1]})
    ed.check(refused.is_error, "two knots stay")

    for expected in (k2, k1, k0):
        await ed.call("undo")
        now = (await ed.call("edit_spline", entity=path, space="local"))["knots"]
        ed.check_eq(now, expected, "undo restores the previous knots exactly")

    calls = [{"tool": "edit_spline", "arguments": {"entity": path, "space": "local",
                                                   "update": [{"index": 1, "position": [10, 0, 0.1 * i]}]}} for i in range(1, 11)]
    await ed.call("batch", calls=calls, undo_group=True)
    moved = (await ed.call("edit_spline", entity=path, space="local"))["knots"]
    ed.check(abs(moved[1]["position"][2] - 1.0) < 1e-4, f"the drag moved the knot: {moved[1]}")
    await ed.call("undo")
    back = (await ed.call("edit_spline", entity=path, space="local"))["knots"]
    ed.check_eq(back, k0, "the ten updates were one undo step")


async def test_round_trip(ed: Editor):
    """AC11: a scene with all four components saves, reloads and saves again byte-identically;
    it holds no chunk buffers; a scene written before S18 loads with no difference."""
    s = await setup(ed)
    fence = await ed.call("edit_spline", create={"name": "Fence"}, space="local",
                          set_knots=[[-20, 0, 20], [0, 0, 20], [12, 0, 28]])
    await ed.call("build_spline", entity=fence["entity"]["id"], preset="fence")
    cart = await ed.call("create_entity", name="Cart", type="box")
    s["cart"] = cart["entity_id"]
    await ed.call("add_component", entity=s["cart"], component="splineanimate")
    await ed.call("set_properties", entity=s["cart"],
                  values={"splineanimate": {"spline": s["road"], "duration": 2, "loop": "once"}})
    await ed.call("save_scene", scene=s["scene"])
    first = (ed.project / "scenes/Roads.scene").read_text()
    for key in ("spline:", "splineextrude:", "splineinstantiate:", "splineanimate:"):
        ed.check(key in first, f"the scene holds {key}")
    ed.check("- {p: [" in first, "knots are flow maps")
    road_part = first[first.index("name: Road"):]
    road_part = road_part[:road_part.find("- type: Entity") if "- type: Entity" in road_part else len(road_part)]
    ed.check("buffer" not in road_part and "vertices:" not in road_part, "no chunk buffers in the scene")
    await ed.call("close_scene", scene=s["scene"])
    opened = await ed.call("open_scene", scene="scenes/Roads.scene")
    s["scene"] = find_key(opened, "id")
    await ed.call("save_scene", scene=s["scene"])
    second = (ed.project / "scenes/Roads.scene").read_text()
    if first != second:
        diff = "".join(difflib.unified_diff(first.splitlines(True), second.splitlines(True), "first", "second", n=2))
        ed.fail(f"saved, reloaded and saved again: byte-identical, but:\n{diff[:3000]}")

    # a scene as an editor before S18 wrote it: format 3, the same content
    older = await ed.call("create_scene", name="Before", type="3D", path="scenes/Before")
    await ed.call("create_entity", scene=older["scene"]["id"], name="Box", type="box", position=[1, 2, 3])
    await ed.call("save_scene", scene=older["scene"]["id"])
    await ed.call("close_scene", scene=older["scene"]["id"])
    current = (ed.project / "scenes/Before.scene").read_text()
    ed.check(current.startswith("formatVersion: 8\n"), "this editor writes format 8")
    pre = "formatVersion: 3\n" + current.split("\n", 1)[1]
    (ed.project / "scenes/Before.scene").write_text(pre)
    await ed.call("sync_files")
    before = await ed.call("open_scene", scene="scenes/Before.scene")
    report = await ed.call("get_load_report", scene=find_key(before, "id"))
    # only the version note (info: read as format 3, the next save writes 4); nothing dropped or kept aside
    issues = [i for i in (report.get("issues") or []) if not (i.get("fate") == "info" and i.get("kind") == "migrated")]
    ed.check(not issues and not report.get("dropped") and not report.get("preserved"),
             f"a pre-S18 scene loads with nothing but its version note: {report}")
    await ed.call("save_scene", scene=find_key(before, "id"))
    after = (ed.project / "scenes/Before.scene").read_text()
    ed.check(after in (current, pre), "the pre-S18 scene saves with nothing but its version line changed")
    await ed.call("select_scene", scene=s["scene"])


async def test_sample_and_follow(ed: Editor):
    """AC12: sample_spline matches Lua getLength/getClosestPoint within 1e-4; a Spline Animate of
    2 s, Once, has reached the end of the road after 2.1 s of Play."""
    s = await setup(ed)
    sampled = await ed.call("sample_spline", entity=s["road"], query_points=[[3, 9, 4]])
    values = await lua(ed, "local s = GameObject(scene, scene:findEntity('Road')):getSpline(); "
                           "local h = s:getClosestPoint(Vector3(3, 9, 4)); "
                           "return s:getLength(), h.t, h.distance, h.position.x, h.position.y, h.position.z")
    ed.check(abs(values[0] - sampled["length"]) < 1e-4, f"length: Lua {values[0]} vs {sampled['length']}")
    closest = sampled["queries"][0]["closest"]
    ed.check(abs(values[1] - closest["t"]) < 1e-4 and abs(values[2] - closest["distance"]) < 1e-4, f"closest: {values} vs {closest}")
    for a, b in zip(values[3:6], closest["position"]):
        ed.check(abs(a - b) < 1e-4, f"closest point: {values[3:6]} vs {closest['position']}")
    end = (await ed.call("sample_spline", entity=s["road"], t=[1.0]))["at_t"][0]["position"]

    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", seconds=2.1)
        live = await ed.call("get_runtime_entity", entity=s["cart"], components=["Transform"])
        position = live["components"]["transform"]["position"]
        gap = math.dist(position, end)
        ed.check(gap <= 0.01 * sampled["length"], f"the follower reached the end: {position} vs {end} ({gap})")
    finally:
        await ed.call("play", action="stop")


async def test_2d(ed: Editor):
    """AC14: in a 2D scene a filled concave shape is drawn; an edge with a Chain2D carries a ball."""
    made = await ed.call("create_scene", name="Shapes", type="2D", path="scenes/Shapes")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    shape = await ed.call("edit_spline", create={"name": "Shape", "dimension": "2d"}, space="local", type="linear", closed=True,
                          set_knots=[[-60, -40, 0], [60, -40, 0], [60, 40, 0], [0, 5, 0], [-60, 40, 0]])
    built = await ed.call("build_spline", entity=shape["entity"]["id"], preset="fill2d")
    ed.check(built["extrude"]["vertices"] > 0 and not built["extrude"]["warnings"], f"the concave fill: {built}")
    shot = await ed.screenshot("scene", path=ed.scratch.root / "spline_fill2d.png" if ed.scratch else None)
    ed.check(distinct_colors(shot) > 3, "the fill is drawn")

    edge = await ed.call("edit_spline", create={"name": "Edge", "dimension": "2d"}, space="local",
                         set_knots=[[-200, -100, 0], [0, -110, 0], [200, -100, 0]])
    await ed.call("build_spline", entity=edge["entity"]["id"], preset="edge2d")
    ball = await ed.call("create_entity", name="Ball", type="sprite", position=[0, 50, 0])
    await ed.call("edit_colliders", entity=ball["entity_id"], body_type="dynamic", add=[{"shape": "circle", "radius": 10, "center": [0, 0]}])
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", seconds=3)
        live = await ed.call("get_runtime_entity", entity=ball["entity_id"], components=["Transform"])
        y = live["components"]["transform"]["position"][1]
        surface = (await ed.call("sample_spline", entity=edge["entity"]["id"], query_points=[[0, y, 0]]))["queries"][0]["closest"]["position"][1]
        # resting on the edge: its centre one radius above the chain (within 0.1 of it)
        ed.check(abs(y - (surface + 10)) <= 0.1 * 10 + 0.1, f"the ball rests on the edge: y {y}, edge {surface}")
    finally:
        await ed.call("play", action="stop")


async def test_pick_selects_the_spline(ed: Editor):
    """R25 (with S26 picking): a click on what a spline built selects the GameObject with the
    Spline, not the engine-owned chunk; Surface snap lands a stroke on a box's top face."""
    made = await ed.call("create_scene", name="Pick", type="3D", path="scenes/Pick")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    lane = await ed.call("edit_spline", create={"name": "Lane"}, space="local", set_knots=[[-10, 0, 0], [0, 0, 0], [10, 0, 0]])
    lane_id = lane["entity"]["id"]
    built = await ed.call("build_spline", entity=lane_id, preset="road")
    ed.check(built["extrude"]["vertices"] > 0, f"the lane is built: {built}")
    await ed.call("set_scene_camera", scene=sid, position=[0, 8, 6], target=[0, 0, 0])
    await ed.call("wait", frames=5)
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check(got["hits"], f"the lane is hit: {got}")
    ed.check_eq(got["would_select"]["entity"], lane_id, f"a click selects the Lane: {got}")

    # Surface snap: a stroke across a box lands on its top face (y = 2 + 1 = 3 at its middle)
    box = await ed.call("create_entity", scene=sid, name="Block", type="box", position=[0, 2, 10])
    await ed.call("set_properties", entity=box["entity_id"], values={"transform": {"scale": [4, 2, 4]}})
    await ed.call("wait", frames=3)
    drawn = await ed.call("edit_spline", create={"name": "OverBlock"},
                          stroke={"points": [[-1, 9, 10], [0, 9, 10], [1, 9, 10]], "snap": "surface", "tolerance": 0.05})
    ys = [k["position"][1] if isinstance(k, dict) else k[1] for k in drawn["knots"]]
    ed.check(all(abs(y - 3.0) < 0.05 for y in ys), f"the knots sit on the box's top face (y 3): {ys}")
