"""S75 navigation diagnostics as an agent sees them (docs/specs/S75-navigation-diagnostics.md R30-R33,
R35; AC16-AC18, AC20).

Scene NavDiag (3D): a 40 x 40 floor (plane x 4) with a NavMesh Surface, crossed at x = 0 by a 4 m strip
of area 3 "Mud" (a NavMesh Modifier volume); a 6 x 6 platform at x = 30 (its own island); a 60 degree
slab high above the floor (too steep to walk); a NavMesh Link whose end hangs 8 m in the air; a
carving obstacle on a surface without Runtime Carving; agents on the floor (Walker), far off it
(Lost) and of agent type Small (Scout, whose surface is never baked); agent type Tiny has no surface;
and a 16 x 12 Nav Grid. The tools: every path reason, GameObjects as query points, many targets,
the target/agent type conflict, locate and its why, grid reasons, the bake report, coverage, links,
obstacles, modifiers and findings, the overlay's frame, colours and labels, and the Lua API in play.
"""

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Navigation Diagnostics CI"}

TIMEOUT = 1200

S: dict = {}

MUD = 3
NOT_MUD = 0xFFFF & ~(1 << MUD)
# above the middle of the 60 degree slab, 5.6 m over the floor
RAMP_POINT = [-12, 5.6, 12]
PLATFORM = [30, 1, 0]


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    settings = await ed.call("set_navigation_settings", agents=[{"name": "Humanoid"}, {"name": "Small", "radius": 0.3},
                                                                {"name": "Tiny", "radius": 0.2}],
                             areas=[{"index": MUD, "name": "Mud", "cost": 2}])
    ed.check_eq([a["name"] for a in settings["agents"]], ["Humanoid", "Small", "Tiny"], f"{settings['agents']}")
    made = await ed.call("create_scene", name="NavDiag", type="3D", path="scenes/NavDiag")
    scene = S["scene"] = made["scene"]["id"]

    async def entity(name: str, kind: str, **args) -> int:
        made = await ed.call("create_entity", scene=scene, type=kind, name=name, **args)
        S[name] = made["entity_id"]
        return made["entity_id"]

    async def component(name: str, kind: str, values: dict | None = None) -> None:
        await ed.call("add_component", scene=scene, entity=S[name], component=kind, values={kind: values or {}})

    await entity("Floor", "plane", scale=[4, 1, 4])
    await component("Floor", "navmeshsurface")
    await entity("Platform", "cube", position=[30, 0.5, 0], scale=[6, 1, 6])
    await entity("Ramp", "cube", position=[-12, 5, 12], scale=[6, 0.2, 6], rotation={"euler": [60, 0, 0]})
    await entity("Mud Strip", "empty")
    await component("Mud Strip", "navmeshmodifier", {"overrideArea": True, "area": MUD, "useVolume": True,
                                                     "volumeCenter": [0, 0, 0], "volumeSize": [4, 4, 44]})
    await entity("Ledge Link", "empty", position=[-6, 0, -15])
    await component("Ledge Link", "navmeshlink", {"startPoint": [0, 0, 0], "endPoint": [0, 8, 0]})
    await entity("Crate", "cube", position=[10, 0.5, 10])
    await component("Crate", "navmeshobstacle", {"carve": True})
    await entity("Walker", "capsule", position=[-15, 1, -15])
    await component("Walker", "navmeshagent", {"baseOffset": 1.0})
    await entity("Lost", "capsule", position=[-15, 1, 45])
    await component("Lost", "navmeshagent", {"baseOffset": 1.0})
    await entity("Scout", "capsule", position=[-10, 1, -10])
    await component("Scout", "navmeshagent", {"baseOffset": 1.0, "agentType": 1})
    await entity("Small Surface", "empty", position=[0, 0, 0])
    await component("Small Surface", "navmeshsurface", {"agentType": 1})
    await entity("Grid", "empty", position=[60, 0, 0])
    await component("Grid", "navgrid", {"width": 16, "height": 12, "cellSize": 1})
    await ed.call("save_scene", scene=scene)
    baked = await ed.call("bake_navigation", scene=scene, surfaces=[S["Floor"]], mode="full")
    S["bake"] = baked
    ed.check(baked["polys"] > 0, f"{baked}")
    await ed.call("save_scene", scene=scene)
    return S


async def query(ed: Editor, **args) -> dict:
    return await ed.call("query_navigation", scene=S["scene"], **args)


def near(a, b, tolerance=0.3) -> bool:
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


async def test_query_reasons(ed: Editor):
    """AC16: each path reason the fixture raises, with its status, surface, closest point, gap and
    next step (out_of_nodes needs a 3,000-polygon comb: NavFailureTest.OutOfNodes covers it)."""
    s = await setup(ed)
    cases = [
        ("no_surface", "invalid", {"from": [-10, 0, 5], "to": [10, 0, 5], "agent_type": "Tiny"}),
        ("not_baked", "invalid", {"from": [-10, 0, 5], "to": [10, 0, 5], "agent_type": "Small"}),
        ("start_off_mesh", "invalid", {"from": [0, 0, 60], "to": [10, 0, 5]}),
        ("end_off_mesh", "invalid", {"from": [-10, 0, 5], "to": [0, 0, 60]}),
        ("start_filtered", "invalid", {"from": [0, 0, 5], "to": [-10, 0, 5], "area_mask": 1}),
        ("end_filtered", "invalid", {"from": [-10, 0, 5], "to": [0, 0, 5], "area_mask": 1}),
        ("disconnected", "partial", {"from": [-10, 0, 5], "to": PLATFORM}),
        ("blocked_by_area", "partial", {"from": [-10, 0, 5], "to": [10, 0, 5], "area_mask": NOT_MUD}),
    ]
    for reason, status, args in cases:
        path = await query(ed, op="path", **args)
        ed.check_eq((path["status"], path.get("reason")), (status, reason), f"{reason}: {path}")
        ed.check(path["next"] and "closest_point" in path and "gap" in path, f"{reason} has its next step: {path}")
        ed.check({"position", "snap_distance", "island"} <= set(path["start"]), f"{reason}: the snapped start {path['start']}")
        if status == "partial":
            ed.check_eq(path["surface"]["id"], s["Floor"], f"{reason}: the floor answered: {path}")
            ed.check(path["gap"] > 1 and near(path["closest_point"], path["corners"][-1], 1e-3), f"{reason}: {path}")
    # the islands are current for a diagnostic: the ends name theirs, and they differ
    apart = await query(ed, op="path", **{"from": [-10, 0, 5]}, to=PLATFORM)
    ed.check(apart["start"]["island"] >= 0 and apart["end"]["island"] >= 0 and apart["start"]["island"] != apart["end"]["island"],
             f"{apart['start']} {apart['end']}")
    complete = await query(ed, op="path", **{"from": [-10, 0, 5]}, to=[10, 0, 5])
    ed.check_eq(complete["status"], "complete", f"{complete}")
    ed.check("reason" not in complete and complete["start"]["island"] == complete["end"]["island"], f"{complete}")
    reach = await query(ed, op="reachable", **{"from": [-10, 0, 5]}, to=PLATFORM)
    ed.check_eq((reach["reachable"], reach["reason"]), (False, "disconnected"), f"{reach}")
    raw = await ed.call_raw("query_navigation", {"scene": s["scene"], "from": [-10, 0, 5], "to": [10, 0, 5], "area_mask": NOT_MUD})
    ed.check("blocked_by_area" in raw.text and "area_mask" in raw.text, f"the text names the reason and the fix: {raw.text}")


async def test_query_entity_refs(ed: Editor):
    """AC16: from/to as GameObjects: an agent's feet, its agent type and area mask."""
    s = await setup(ed)
    walk = await query(ed, op="path", **{"from": "Walker"}, to="Crate")
    ed.check_eq((walk["status"], walk["agent_type"]), ("complete", 0), f"{walk}")
    ed.check(near(walk["start"]["position"], [-15, 0, -15]) and walk["start"]["snap_distance"] < 0.3,
             f"from Walker's feet (1 m below its position): {walk['start']}")
    ed.check(near(walk["end"]["position"], [10, 0, 10], 0.6), f"to the Crate: {walk['end']}")
    scout = await query(ed, op="path", **{"from": "Scout"}, to=[10, 0, 10])
    ed.check_eq((scout["agent_type"], scout["reason"]), (1, "not_baked"), f"Scout's type Small, never baked: {scout}")
    typed = await query(ed, op="path", **{"from": "Scout"}, to=[10, 0, 10], agent_type="Humanoid")
    ed.check_eq((typed["agent_type"], typed["status"]), (0, "complete"), f"an explicit agent_type wins: {typed}")
    lost = await query(ed, op="reachable", **{"from": "Lost"}, to="Walker")
    ed.check_eq((lost["reachable"], lost["reason"]), (False, "start_off_mesh"), f"{lost}")
    await ed.check_error("query_navigation", scene=s["scene"], op="path", **{"from": "Nobody Here"}, to=[0, 0, 0], contains="from")


async def test_query_multi_target(ed: Editor):
    """AC16: up to 64 targets answer one entry each (no corners); 65 are refused."""
    s = await setup(ed)
    targets = [[-17 + 2 * (i % 8), 0, -17 + 2 * (i // 8)] for i in range(64)]
    targets[-1] = [0, 0, 60]
    targets[-2] = "Crate"
    got = await query(ed, op="path", **{"from": [-15, 0, -15]}, to=targets)
    results = got["results"]
    ed.check_eq(len(results), 64, "one entry per target")
    ed.check(all("corners" not in r and "corner_count" in r for r in results), f"summaries: {results[0]}")
    ed.check_eq(results[-1]["reason"], "end_off_mesh", f"{results[-1]}")
    ed.check_eq(results[-2]["to"]["id"], s["Crate"], f"a GameObject target is echoed: {results[-2]}")
    ed.check(sum(1 for r in results if r["status"] == "complete") >= 50, f"{[r['status'] for r in results]}")
    reach = await query(ed, op="reachable", **{"from": [-15, 0, -15]}, to=[[10, 0, 10], PLATFORM])
    ed.check_eq([(r["reachable"], r.get("reason")) for r in reach["results"]], [(True, None), (False, "disconnected")], f"{reach}")
    rays = await query(ed, op="raycast", **{"from": [-15, 0, -15]}, to=[[-15, 0, 15], [-15, 0, 40]])
    ed.check_eq([r["blocked"] for r in rays["results"]], [False, True], f"{rays}")
    await ed.check_error("query_navigation", scene=s["scene"], op="path", **{"from": [0, 0, 0]}, to=targets + [[1, 0, 1]],
                         contains="64")


async def test_query_target_agent_type_conflict(ed: Editor):
    """AC16: a target surface of another agent type than agent_type (or from's agent) is an error
    naming both (S19 let the target's type win silently)."""
    s = await setup(ed)
    msg = await ed.check_error("query_navigation", scene=s["scene"], op="path", **{"from": [0, 0, 0]}, to=[5, 0, 5],
                               target="Small Surface", agent_type="Humanoid")
    ed.check("'Small'" in msg and "'Humanoid'" in msg, f"names both types: {msg}")
    msg = await ed.check_error("query_navigation", scene=s["scene"], op="path", **{"from": "Walker"}, to=[5, 0, 5],
                               target="Small Surface")
    ed.check("'Small'" in msg and "'Humanoid'" in msg and "Agent" in msg, f"from's agent type: {msg}")
    same = await query(ed, op="path", **{"from": [0, 0, 0]}, to=[5, 0, 5], target="Small Surface")
    ed.check_eq((same["agent_type"], same["reason"]), (1, "not_baked"), f"the target's type without agent_type: {same}")


async def test_locate_why(ed: Editor):
    """AC16: locate on the floor, and off it the probe's reasons (the slab is too steep)."""
    await setup(ed)
    on = await query(ed, op="locate", **{"from": [5, 0.1, 5]})
    ed.check(on["on_mesh"] and on["found"] and on["island"] >= 0 and on["area_name"] == "Walkable", f"{on}")
    ed.check("why" not in on, f"no reasons on the mesh: {on}")
    steep = await query(ed, op="locate", **{"from": RAMP_POINT})
    ed.check(not steep["on_mesh"], f"{steep}")
    reasons = {r["code"]: r for r in steep["why"]}
    ed.check("too_steep" in reasons, f"the slab is too steep: {steep['why']}")
    slope = reasons["too_steep"]
    ed.check(55 <= slope["value"] <= 65 and slope["limit"] == 45 and slope["message"], f"{slope}")
    mud = await query(ed, op="locate", **{"from": [0, 0.1, 5]}, area_mask=1)
    ed.check(not mud["on_mesh"] and "filtered" in {r["code"] for r in mud["why"]}, f"Mud is outside the mask: {mud}")


async def test_grid_reasons(ed: Editor):
    """AC16: a grid answer's reason: outside or a Not Walkable cell is end_off_mesh, a masked cell
    end_filtered, a walled-in cell disconnected."""
    s = await setup(ed)
    ring = [[x, y] for x in (2, 3, 4) for y in (8, 9, 10) if [x, y] != [3, 9]]
    await ed.call("paint_nav_grid", scene=s["scene"], grid=s["Grid"],
                  strokes=[{"area": "Not Walkable", "cells": [[10, 5]]}, {"area": MUD, "cells": [[12, 5]]}]
                  + [{"area": "Not Walkable", "cells": [c]} for c in ring])
    origin = [60.5, 0, 5.5]

    def cell(x, y):
        return [60.5 + x, 0, 0.5 + y]

    cases = [("end_off_mesh", [90, 0, 5], {}), ("end_off_mesh", cell(10, 5), {}), ("end_filtered", cell(12, 5), {"area_mask": 1}),
             ("disconnected", cell(3, 9), {})]
    for reason, to, extra in cases:
        path = await query(ed, op="path", target="grid", **{"from": origin}, to=to, **extra)
        ed.check(path["status"] != "complete" and path["reason"] == reason, f"{reason}: {path}")
        reach = await query(ed, op="reachable", target="grid", **{"from": origin}, to=to, **extra)
        ed.check_eq((reach["reachable"], reach["reason"]), (False, reason), f"reachable {to}: {reach}")
    start = await query(ed, op="reachable", target="grid", **{"from": [90, 0, 5]}, to=origin)
    ed.check_eq(start["reason"], "start_off_mesh", f"{start}")
    ok = await query(ed, op="path", target="grid", **{"from": origin}, to=cell(14, 1))
    ed.check(ok["status"] == "complete" and "reason" not in ok, f"{ok}")


async def test_bake_report(ed: Editor):
    """AC17: a bake's sources, islands and slowest tiles; an empty surface fails with
    nothing_collected and its hint."""
    s = await setup(ed)
    floor = next(r for r in s["bake"]["surfaces"] if r["entity"]["id"] == s["Floor"])
    sources = floor["sources"]
    ed.check_eq(set(sources), {"meshes", "instances", "terrains", "colliders", "colliders_2d", "volumes", "links", "excluded",
                               "triangles"}, f"{sources}")
    ed.check(sources["meshes"] >= 3 and sources["volumes"] == 1 and sources["links"] == 1 and sources["excluded"] >= 1
             and sources["triangles"] > 0, f"Floor, Platform, Ramp; the strip; the link; the agents and the crate: {sources}")
    ed.check(floor["island_count"] >= 2 and 1000 < floor["walkable_area_m2"] < 1700, f"{floor}")
    ed.check(len(floor["islands"]) == min(16, floor["island_count"]), f"{floor['islands']}")
    ed.check({"id", "area_m2", "polys", "bounds"} <= set(floor["islands"][0]), f"{floor['islands'][0]}")
    ed.check(0 < len(floor["slowest_tiles"]) <= 5 and {"tile", "ms", "polys", "kb"} <= set(floor["slowest_tiles"][0]),
             f"{floor['slowest_tiles']}")
    ms = [t["ms"] for t in floor["slowest_tiles"]]
    ed.check_eq(ms, sorted(ms, reverse=True), "slowest first")

    made = await ed.call("create_scene", name="NavEmpty", type="3D", path="scenes/NavEmpty")
    empty = await ed.call("create_entity", scene=made["scene"]["id"], type="empty", name="Nothing Here")
    await ed.call("add_component", scene=made["scene"]["id"], entity=empty["entity_id"], component="navmeshsurface")
    r = await ed.call_raw("bake_navigation", {"scene": made["scene"]["id"]})
    ed.check(r.is_error, f"nothing to bake fails: {r.text}")
    failed = r.structured["surfaces"][0]
    ed.check_eq(failed["error_code"], "nothing_collected", f"{failed}")
    ed.check(failed["hint"] and failed["hint"] in r.text, f"the hint, in the text too: {failed} / {r.text}")
    await ed.call("select_scene", scene=s["scene"])


async def test_info_coverage_findings(ed: Editor):
    """AC17: coverage, islands and last bake per surface; link connection; obstacles, modifiers;
    the findings; entities restricts every list."""
    s = await setup(ed)
    info = await ed.call("get_navigation_info", scene=s["scene"])
    floor = next(x for x in info["surfaces"] if x["entity"]["id"] == s["Floor"])
    coverage = floor["coverage"]
    ed.check_eq(set(coverage), {"walkable_area_m2", "island_count", "largest_island_m2", "tiles_area_m2", "fraction"}, f"{coverage}")
    ed.check(coverage["island_count"] >= 2 and 0 < coverage["fraction"] <= 1
             and coverage["largest_island_m2"] < coverage["walkable_area_m2"] <= coverage["tiles_area_m2"], f"{coverage}")
    ed.check(floor["islands"] and floor["last_bake"]["ok"] and "sources" in floor["last_bake"], f"{floor}")
    small = next(x for x in info["surfaces"] if x["entity"]["id"] == s["Small Surface"])
    ed.check("coverage" not in small, f"not baked, no coverage: {small}")
    link = info["links"][0]
    ed.check_eq((link["connected"], link["surface"]["id"], link["island_end"]), (False, s["Floor"], -1), f"the end hangs in the air: {link}")
    ed.check(link["connections"] >= 1 and link["island_start"] >= 0, f"{link}")
    ed.check_eq([(o["entity"]["id"], o["carve"], o["shape"]) for o in info["obstacles"]], [(s["Crate"], True, "box")], f"{info['obstacles']}")
    mod = info["modifiers"][0]
    ed.check_eq((mod["entity"]["id"], mod["area"], mod["area_name"]), (s["Mud Strip"], MUD, "Mud"), f"{mod}")
    ed.check("volume" in mod, f"{mod}")
    findings = info["findings"]
    codes = {(f["code"], (f["entity"] or {}).get("id")) for f in findings}
    for code, entity in [("link_unconnected", s["Ledge Link"]), ("agent_off_mesh", s["Lost"]), ("not_baked", s["Small Surface"]),
                         ("obstacle_carve_ignored", s["Crate"]), ("small_island", s["Floor"]), ("agent_type_unused", None)]:
        ed.check((code, entity) in codes, f"{code} on {entity}: {sorted(codes, key=str)}")
    ed.check(("agent_off_mesh", s["Walker"]) not in codes, "Walker stands on the floor")
    for f in findings:
        ed.check({"code", "severity", "entity", "message", "details"} <= set(f) and f["severity"] in ("info", "warning", "error"), f"{f}")
    link_finding = next(f for f in findings if f["code"] == "link_unconnected")
    ed.check_eq(link_finding["details"]["island_end"], -1, f"{link_finding}")
    order = ["error", "warning", "info"]
    ed.check_eq([f["severity"] for f in findings], sorted((f["severity"] for f in findings), key=order.index), "error first")

    only = await ed.call("get_navigation_info", scene=s["scene"], entities=["Ledge Link"])
    ed.check_eq((len(only["links"]), only["surfaces"], only["agents"], only["obstacles"], only["modifiers"], only["grids"]),
                (1, [], [], [], [], []), f"{only}")
    ed.check_eq({f["code"] for f in only["findings"]}, {"link_unconnected"}, f"{only['findings']}")


async def test_visualize_frame_and_flags(ed: Editor):
    """AC18: frame moves the Scene camera (navmesh, a GameObject); islands / tile_colors set Color
    By; tile labels are listed; a drawn path reports its reason; screenshot game in play."""
    s = await setup(ed)
    scene = s["scene"]
    await ed.call("select_scene", scene=scene)
    before = (await ed.call("set_scene_camera", scene=scene))["camera"]
    framed = await ed.call("visualize_navigation", scene=scene, frame="navmesh")
    ed.check_eq(framed["framed"], "navmesh", f"{framed}")
    ed.check(not near(framed["camera"]["position"], before["position"], 0.01), f"the camera moved: {before} -> {framed['camera']}")
    after = (await ed.call("set_scene_camera", scene=scene))["camera"]
    ed.check(near(after["position"], framed["camera"]["position"], 0.01), f"it stays there: {after}")
    lost = await ed.call("visualize_navigation", scene=scene, frame="Lost")
    ed.check(not near(lost["camera"]["target"], framed["camera"]["target"], 0.5), f"framed Lost: {lost['camera']}")
    ed.check(near(lost["camera"]["target"], [-15, 1, 45], 2.0), f"around Lost: {lost['camera']}")

    islands = await ed.call("visualize_navigation", scene=scene, show={"islands": True, "slope": True})
    ed.check_eq((islands["overlay"]["color_by"], islands["overlay"]["slope"]), ("islands", True), f"{islands['overlay']}")
    tiles = await ed.call("visualize_navigation", scene=scene, show={"tile_colors": True})
    ed.check_eq(tiles["overlay"]["color_by"], "tiles", "tile_colors replaces islands")
    plain = await ed.call("visualize_navigation", scene=scene, show={"tile_colors": False, "slope": False})
    ed.check_eq(plain["overlay"]["color_by"], "areas", "off goes back to areas")
    await ed.check_error("visualize_navigation", scene=scene, show={"islands": True, "tile_colors": True}, contains="one")

    await ed.call("visualize_navigation", scene=scene, frame="navmesh")
    labelled = await ed.call("visualize_navigation", scene=scene, show={"navmesh": True, "tile_labels": True})
    labels = labelled["tile_labels"]
    ed.check(labels and all("polys" in label["text"] and len(label["tile"]) == 2 for label in labels), f"{labels[:3]}")
    ed.check({label["surface"]["id"] for label in labels} == {s["Floor"]}, f"{labels[:3]}")
    await ed.call("visualize_navigation", scene=scene, show={"tile_labels": False})

    drawn = await ed.call("visualize_navigation", scene=scene, path={"from": [-10, 0, 5], "to": PLATFORM})
    ed.check_eq((drawn["path"]["status"], drawn["path"]["reason"], drawn["path"]["surface"]["id"]), ("partial", "disconnected", s["Floor"]),
                f"{drawn['path']}")
    ed.check(drawn["path"]["gap"] > 1 and len(drawn["path"]["closest_point"]) == 3, f"{drawn['path']}")
    walker = await ed.call("visualize_navigation", scene=scene, path={"from": "Walker", "to": "Crate"})
    ed.check_eq(walker["path"]["status"], "complete", f"GameObjects as path ends: {walker['path']}")

    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=scene, fixed_fps=60)
    try:
        r = await ed.call_raw("visualize_navigation", {"screenshot": "game"})
        ed.check(not r.is_error and r.images, r.text)
        ed.check("Game view" in r.text and r.structured["image"].get("view") == "game", f"{r.text} {r.structured.get('image')}")
        ed.check(not pngdiff.flat(pngdiff.decode(r.images[0][0])), "the Game view image is not one colour")
    finally:
        await ed.call("play", action="stop")


async def test_lua_diagnostics(ed: Editor):
    """AC20: in play, a path's reason, surface, closestPoint and gap; isReachable's reason; locate,
    getCoverage, getIslands; NavMeshSurface.lastBakeResult after the game rebakes (before that it is
    the editor's bake of this session, or nil)."""
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60)
    try:
        got = await ed.call("eval_lua", code=(
            "local nav = scene:getNavigationSystem() "
            "local floor = entity('Floor') "
            "local p = nav:calculatePath(Vector3(-10, 0, 5), Vector3(30, 1, 0)) "
            "local ok, why = nav:isReachable(Vector3(-10, 0, 5), Vector3(30, 1, 0)) "
            "local here = nav:locate(Vector3(5, 0.1, 5)) "
            "local cov = nav:getCoverage(floor) "
            "local islands = nav:getIslands(floor) "
            "local before = NavMeshSurface(scene, floor).lastBakeResult "
            "local last = p.corners[#p.corners] "
            "return p.reason, p.surface == floor, p.gap > 1, "
            "math.abs(p.closestPoint.x - last.x) + math.abs(p.closestPoint.z - last.z) < 0.01, "
            "ok, why, here.onMesh, here.island >= 0, cov.islandCount >= 2, cov.walkableArea > 1000, #islands == cov.islandCount, "
            "islands[1].polys > 0, before == nil or before.ok"))
        ed.check_eq(got["values"], ["disconnected", True, True, True, False, "disconnected", True, True, True, True, True, True, True],
                    f"{got}")
        started = await ed.call("eval_lua", code=(
            "return NavMeshSurface(scene, entity('Floor')):bakeAsync()"))
        ed.check_eq(started["values"], [True], f"the game's bake starts: {started}")
        await ed.call("wait", seconds=300, game_seconds=120,
                      **{"until": "local surf = NavMeshSurface(scene, entity('Floor')) "
                                  "return not surf.isBaking and surf.lastBakeResult ~= nil"})
        report = await ed.call("eval_lua", code=(
            "local r = NavMeshSurface(scene, entity('Floor')).lastBakeResult "
            "return r.ok, r.errorCode, r.sources.meshes >= 3, r.sources.colliders2D, r.islandCount >= 2, #r.islands >= 2, "
            "r.walkableArea > 1000, type(r.slowestTiles) == 'table', r.tilesTotal > 0"))
        ed.check_eq(report["values"], [True, "none", True, 0, True, True, True, True, True], f"{report}")
    finally:
        await ed.call("play", action="stop")
