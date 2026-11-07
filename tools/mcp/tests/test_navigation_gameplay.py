"""S55 navigation gameplay API as an agent sees it (docs/specs/S55-navigation-gameplay-api.md R42-R45;
AC26-AC28, AC30).

Scene Gameplay (3D): a 40 x 40 floor (plane x 4) with a NavMesh Surface, a strip of the "Mud" area
(index 3, cost 4) across it at x in [-1, 1] (a NavMesh Modifier volume), and a walker with a NavMesh
Agent. test_query_ops (S55 P1): query_navigation's random (seeded, around a point, on an island),
move_along, closest_edge with max_distance, and the path's cost, corner areas and flags and lengths
per area, in edit mode and in play.

Scene Doors (S55 P2, nav_doors): two rooms split by a wall with a doorway whose NavMesh Modifier is a
runtime volume, and a bridge link to a platform. test_door_and_bridge_in_play,
test_lua_component_handles and test_overlay_screenshot: the door and the bridge toggled in play, the
Obstacle / Link / Modifier handles and script volumes, and the overlay's area override boxes. Its
Guard agent (S55 P3) walks through the doorway without Auto Repath: closing the door drops its path,
blocked by an area. test_path_failed_callbacks (S55 P3): one- and two-argument onPathFailed, the
reason's name, lastPathFailure and stuck_seconds in get_runtime_entity and get_navigation_info.
"""

import math

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Navigation Gameplay CI"}

TIMEOUT = 1200

S: dict = {}

# the floor's walkable edge: 20 m less the 0.5 m agent radius
EDGE = 19.5
CELL = 0.3


async def setup(ed: Editor) -> dict:
    """The scene, baked once per editor (the tests share it)."""
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("set_navigation_settings", areas=[{"index": 3, "name": "Mud", "cost": 4}])
    made = await ed.call("create_scene", name="Gameplay", type="3D", path="scenes/Gameplay")
    S["scene"] = made["scene"]["id"]
    floor = await ed.call("create_entity", scene=S["scene"], type="plane", name="Floor", scale=[4, 1, 4])
    S["floor"] = floor["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["floor"], component="navmeshsurface")
    mud = await ed.call("create_entity", scene=S["scene"], type="empty", name="Mud", position=[0, 0, 0])
    S["mud"] = mud["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["mud"], component="navmeshmodifier",
                  values={"navmeshmodifier": {"useVolume": True, "overrideArea": True, "area": 3,
                                              "volumeCenter": [0, 0, 0], "volumeSize": [2, 4, 44]}})
    walker = await ed.call("create_entity", scene=S["scene"], type="capsule", name="Walker", position=[-15, 1, -15])
    S["walker"] = walker["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["walker"], component="navmeshagent",
                  values={"navmeshagent": {"baseOffset": 1.0}})
    await ed.call("save_scene", scene=S["scene"])
    baked = await ed.call("bake_navigation", scene=S["scene"], mode="full")
    ed.check(baked["surfaces"][0]["polys"] > 0, f"baked: {baked}")
    return S


def flat_distance(a, b) -> float:
    return math.hypot(a[0] - b[0], a[2] - b[2])


async def check_queries(ed: Editor, scene=None):
    """The S55 query ops (scene: the edited scene, or None for the running one)."""
    q = {"scene": scene} if scene is not None else {}

    # a path across the mud prices it at 4 per metre and reports its length per area name
    path = await ed.call("query_navigation", op="path", **q, **{"from": [-15, 0, 0]}, to=[15, 0, 0])
    ed.check_eq(path["status"], "complete", f"{path}")
    ed.check_eq(len(path["corner_areas"]), len(path["corners"]), f"{path}")
    ed.check_eq(len(path["corner_flags"]), len(path["corners"]), f"{path}")
    ed.check(path["corner_flags"][0] & 1 and path["corner_flags"][-1] & 2, f"start and end flags: {path['corner_flags']}")
    lengths = path["area_lengths"]
    ed.check(abs(lengths.get("Mud", 0) - 2.0) < CELL, f"2 m of mud: {lengths}")
    ed.check(abs(sum(lengths.values()) - path["length"]) < 0.01, f"the lengths add up: {lengths} vs {path['length']}")
    ed.check(abs(path["cost"] - (path["length"] + 3.0 * lengths.get("Mud", 0))) < 0.01, f"cost: {path}")

    # a raycast across the strip carries its cost
    ray = await ed.call("query_navigation", op="raycast", **q, **{"from": [-5, 0, 0]}, to=[5, 0, 0])
    ed.check(not ray["blocked"] and abs(ray["cost"] - 16.0) < 3 * CELL, f"8 m of floor and 2 of mud: {ray}")

    # random: the same seed gives the same point; another seed another one; all on the floor
    first = await ed.call("query_navigation", op="random", **q, seed=42)
    again = await ed.call("query_navigation", op="random", **q, seed=42)
    other = await ed.call("query_navigation", op="random", **q, seed=43)
    ed.check(first["found"] and first["seed"] == 42, f"{first}")
    ed.check_eq(first["position"], again["position"], "seeded twice: the same point")
    ed.check(other["position"] != first["position"], f"another seed: {other} vs {first}")
    for hit in (first, other):
        ed.check(abs(hit["position"][0]) <= EDGE + CELL and abs(hit["position"][2]) <= EDGE + CELL, f"on the floor: {hit}")
    # around a point within max_distance
    centre = [-10, 0, -10]
    for seed in range(5):
        near = await ed.call("query_navigation", op="random", **q, **{"from": centre}, max_distance=3, seed=seed)
        ed.check(near["found"] and flat_distance(near["position"], centre) <= 3.0 + 1e-3, f"within 3 m of {centre}: {near}")
    # on the island of a point (the floor is one island)
    island = await ed.call("query_navigation", op="random", **q, **{"from": [5, 0, 5]}, seed=7)
    ed.check(island["found"], f"a point on the floor's island: {island}")
    # off every navmesh: nothing
    none = await ed.call("query_navigation", op="random", **q, **{"from": [0, 0, 80]}, seed=7)
    ed.check(not none["found"], f"no island at from: {none}")

    # move_along: a step into the floor's edge stops on it; an open one reaches its end
    blocked = await ed.call("query_navigation", op="move_along", **q, **{"from": [17, 0, -10]}, to=[25, 0, -10])
    ed.check(blocked["found"] and blocked["blocked"], f"{blocked}")
    ed.check(abs(blocked["position"][0] - EDGE) < CELL and abs(blocked["distance"] - (EDGE - 17)) < CELL, f"{blocked}")
    opened = await ed.call("query_navigation", op="move_along", **q, **{"from": [0, 0, -10]}, to=[1.5, 0, -9])
    ed.check(opened["found"] and not opened["blocked"], f"{opened}")
    ed.check(flat_distance(opened["position"], [1.5, 0, -9]) < 0.01, f"{opened}")
    ed.check_eq(opened["area"], 3, f"it ends in the mud: {opened}")

    # closest_edge within max_distance: the edge is 2.5 m away
    near_edge = {"from": [17, 0, -10]}
    short = await ed.call("query_navigation", op="closest_edge", **q, **near_edge, max_distance=1)
    ed.check(not short["found"] and abs(short["distance"] - 1.0) < 1e-3, f"no edge within 1 m: {short}")
    far = await ed.call("query_navigation", op="closest_edge", **q, **near_edge, max_distance=5)
    ed.check(far["found"] and abs(far["distance"] - (EDGE - 17)) < CELL, f"{far}")
    unlimited = await ed.call("query_navigation", op="closest_edge", **q, **near_edge)
    ed.check(unlimited["found"] and abs(unlimited["distance"] - far["distance"]) < 1e-3, f"{unlimited}")

    # from is required by every op but random
    await ed.call("query_navigation", op="path", **q, to=[1, 0, 1], expect_error=True)
    await ed.call("query_navigation", op="move_along", **q, **{"from": [0, 0, 0]}, expect_error=True)


async def test_query_ops(ed: Editor):
    """AC27: random, move_along, closest_edge with max_distance and the path's costs, in edit and play."""
    s = await setup(ed)
    await check_queries(ed, s["scene"])
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"{started}")
    try:
        await check_queries(ed)
        # the Lua side of the same queries (NavMeshQueryFilter costs, the path table's keys)
        got = await ed.call("eval_lua", code=(
            "local nav = scene:getNavigationSystem() "
            "local f = NavMeshQueryFilter{agentType = 'Humanoid', areaMask = NavAreas.All} "
            "f:setAreaCost(NavAreas.getAreaFromName('Mud'), 20) "
            "local p = nav:calculatePath(Vector3(-15, 0, 0), Vector3(15, 0, 0), f, {areaCrossings = true}) "
            "local found, hit = nav:randomPoint(nil, {seed = 42}) "
            "local ok, move = nav:moveAlongSurface(Vector3(17, 0, -10), Vector3(25, 0, -10)) "
            "local edge, e = nav:findClosestEdge(Vector3(17, 0, -10), nil, 1.0) "
            "return p.status == NavMeshPathStatus.Complete, p.cost > p.length, p.corridor ~= nil, "
            "#p.cornerAreas == #p.corners, p.cornerFlags[1] == NavCornerFlags.START, NavAreas.getAreaName(3), "
            "found, ok and move.hit, edge, e.distance"))
        values = got["values"]
        ed.check_eq(values[:9], [True, True, True, True, True, "Mud", True, True, False], f"{got}")
        ed.check(abs(values[9] - 1.0) < 1e-3, f"{got}")
    finally:
        await ed.call("play", action="stop")


# ---- S55 P2: runtime areas and links, the component handles, the overlay (AC26, AC28, AC30)
#
# Scene Doors (nav_doors): two rooms of a 40 x 12 floor split by a wall at x = 0 with a 2.5 m doorway,
# a NavMesh Modifier "Door" over the gap with Use Volume and Apply At Runtime (Not Walkable, disabled:
# the door is open), and a platform beyond a 3 m gap reached only by the "Bridge" link.

D: dict = {}

WEST = [-10, 0, 0]
EAST = [14, 0, 0]
ISLAND = [28, 0, 0]


async def setup_doors(ed: Editor) -> dict:
    """The Doors scene, baked once per editor."""
    if D.get("ed") is ed:
        return D
    D.clear()
    D["ed"] = ed
    made = await ed.call("create_scene", name="Doors", type="3D", path="scenes/Doors")
    D["scene"] = sc = made["scene"]["id"]
    floor = await ed.call("create_entity", scene=sc, type="plane", name="Floor", scale=[4, 1, 1.2])
    D["floor"] = floor["entity_id"]
    await ed.call("create_entity", scene=sc, type="plane", name="Platform", position=[27, 0, 0], scale=[0.8, 1, 0.8])
    for name, z in (("Wall South", -3.625), ("Wall North", 3.625)):
        await ed.call("create_entity", scene=sc, type="cube", name=name, position=[0, 1.5, z], scale=[1, 3, 4.75])
    door = await ed.call("create_entity", scene=sc, type="empty", name="Door", position=[0, 1.5, 0])
    D["door"] = door["entity_id"]
    await ed.call("add_component", scene=sc, entity=D["door"], component="navmeshmodifier",
                  values={"navmeshmodifier": {"enabled": False, "useVolume": True, "applyAtRuntime": True, "area": 1,
                                              "volumeCenter": [0, 0, 0], "volumeSize": [2, 3, 3]}})
    bridge = await ed.call("create_entity", scene=sc, type="empty", name="Bridge", position=[21.5, 0, 0])
    D["bridge"] = bridge["entity_id"]
    await ed.call("add_component", scene=sc, entity=D["bridge"], component="navmeshlink",
                  values={"navmeshlink": {"startPoint": [-2.5, 0, 0], "endPoint": [2.5, 0, 0]}})
    crate = await ed.call("create_entity", scene=sc, type="empty", name="Crate", position=[-6, 0.5, 4])
    D["crate"] = crate["entity_id"]
    await ed.call("add_component", scene=sc, entity=D["crate"], component="navmeshobstacle")
    # S55 P3: an agent walking through the doorway, without Auto Repath, its watchdog on
    guard = await ed.call("create_entity", scene=sc, type="capsule", name="Guard", position=[-10, 1, -2])
    D["guard"] = guard["entity_id"]
    await ed.call("add_component", scene=sc, entity=D["guard"], component="navmeshagent",
                  values={"navmeshagent": {"baseOffset": 1.0, "autoRepath": False, "stuckDetection": True}})
    await ed.call("add_component", scene=sc, entity=D["floor"], component="navmeshsurface")
    await ed.call("save_scene", scene=sc)
    baked = await ed.call("bake_navigation", scene=sc, mode="full")
    ed.check(baked["surfaces"][0]["polys"] > 0, f"baked: {baked}")
    return D


async def path_status(ed: Editor, start, end, scene=None) -> str:
    q = {"scene": scene} if scene is not None else {}
    path = await ed.call("query_navigation", op="path", **q, **{"from": start}, to=end)
    return path["status"]


def door_override(info: dict, door: int) -> dict:
    found = [o for o in info["area_overrides"] if o["source"].get("entity", {}).get("id") == door]
    assert len(found) == 1, f"one area override for the door: {info['area_overrides']}"
    return found[0]


async def test_door_and_bridge_in_play(ed: Editor):
    """AC28 (R43-R45): the door and the bridge toggled through set_runtime_property in play; the
    path status, get_navigation_info and get_runtime_entity follow; in edit mode too."""
    d = await setup_doors(ed)
    # edit mode: the open door, then closed in the Inspector's field
    ed.check_eq(await path_status(ed, WEST, EAST, d["scene"]), "complete", "the door is open")
    info = await ed.call("get_navigation_info", scene=d["scene"])
    ed.check(not door_override(info, d["door"])["applied"], f"{info['area_overrides']}")
    bridge = [l for l in info["links"] if l["entity"]["id"] == d["bridge"]][0]
    ed.check(bridge["activated"] and "occupied" not in bridge, f"{bridge}")
    door = [m for m in info["modifiers"] if m["entity"]["id"] == d["door"]][0]
    ed.check(door["apply_at_runtime"] is True, f"{door}")

    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=d["scene"], fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"{started}")
    try:
        # S55 P3: the guard walks east through the doorway
        sent = await ed.call("eval_lua", code=(
            "local guard = NavMeshAgent(scene, entity('Guard')) guard.speed = 1 "
            "return guard:setDestination(Vector3(14, 0, -2))"))
        ed.check_eq(sent["values"], [True], f"{sent}")
        await ed.call("wait", seconds=60, game_seconds=2, **{"until": "return not NavMeshAgent(scene, entity('Guard')).pathPending"})
        await ed.call("set_runtime_property", entity="Door", component="navmeshmodifier", property="enabled", value=True)
        await ed.call("wait", seconds=60, game_seconds=0.1)
        ed.check_eq(await path_status(ed, WEST, EAST), "partial", "the closed door blocks the only way east")
        # S55 P3: its path crossed the door: dropped (no Auto Repath), blocked by an area
        await ed.call("wait", seconds=60, game_seconds=1,
                      **{"until": "return NavMeshAgent(scene, entity('Guard')).lastPathFailure ~= 'none'"})
        live = await ed.call("get_runtime_entity", entity=d["guard"], components=["navmeshagent"])
        guard = live["components"]["navmeshagent"]
        ed.check(guard["lastPathFailure"] == "blocked_by_area" and guard["stuckTime"] == 0, f"{guard}")
        info = await ed.call("get_navigation_info")
        listed = [a for a in info["agents"] if a["entity"]["id"] == d["guard"]]
        ed.check(listed and listed[0]["last_failure"] == "blocked_by_area" and listed[0]["stuck_seconds"] == 0, f"{listed}")
        info = await ed.call("get_navigation_info")
        closed = door_override(info, d["door"])
        ed.check(closed["applied"] and closed["polygons"] > 0 and closed["area"] == 1, f"{closed}")
        live = await ed.call("get_runtime_entity", entity=d["door"], components=["navmeshmodifier"])
        ed.check(live["components"]["navmeshmodifier"]["appliedPolygons"] == closed["polygons"], f"{live}")

        await ed.call("set_runtime_property", entity="Door", component="navmeshmodifier", property="enabled", value=False)
        await ed.call("wait", seconds=60, game_seconds=0.1)
        ed.check_eq(await path_status(ed, WEST, EAST), "complete", "opened again")
        info = await ed.call("get_navigation_info")
        ed.check(not door_override(info, d["door"])["applied"], f"{info['area_overrides']}")

        ed.check_eq(await path_status(ed, EAST, ISLAND), "complete", "the bridge leads to the platform")
        await ed.call("set_runtime_property", entity="Bridge", component="navmeshlink", property="activated", value=False)
        await ed.call("wait", seconds=60, game_seconds=0.1)
        ed.check_eq(await path_status(ed, EAST, ISLAND), "partial", "a deactivated bridge is ignored")
        info = await ed.call("get_navigation_info")
        bridge = [l for l in info["links"] if l["entity"]["id"] == d["bridge"]][0]
        ed.check(not bridge["activated"] and bridge["occupied"] is False, f"{bridge}")
        live = await ed.call("get_runtime_entity", entity=d["bridge"], components=["navmeshlink"])
        ed.check(live["components"]["navmeshlink"]["occupied"] is False, f"{live}")
        live = await ed.call("get_runtime_entity", entity=d["crate"], components=["navmeshobstacle"])
        ed.check_eq(live["components"]["navmeshobstacle"]["velocity"], [0, 0, 0], f"a crate standing still: {live}")
    finally:
        await ed.call("play", action="stop")
    # Stop restores the edited scene: the bridge is active, the door open
    ed.check_eq(await path_status(ed, EAST, ISLAND, d["scene"]), "complete", "after Stop")


async def test_lua_component_handles(ed: Editor):
    """AC26 (R30-R33): the NavMeshObstacle, NavMeshLink and NavMeshModifier handles read and write each
    member with Unity's names, setAreaInVolume's NavAreaOverride, and jasper://lua-api lists them."""
    d = await setup_doors(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=d["scene"], fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"{started}")
    try:
        got = await ed.call("eval_lua", code=(
            "local o = NavMeshObstacle(scene, entity('Crate')) "
            "o.shape = NavMeshObstacleShape.CAPSULE o.radius = 0.75 o.height = 1.5 o.center = Vector3(0, 0.5, 0) "
            "o.size = Vector3(2, 1, 2) o.carving = true o.carveOnlyStationary = false o.carvingMoveThreshold = 0.3 "
            "o.carvingTimeToStationary = 0.25 "
            "local obstacle = {o.valid, o.enabled, o.shape == NavMeshObstacleShape.CAPSULE, o.radius, o.height, o.center.y, "
            "o.size.x, o.carving, o.carveOnlyStationary, o.carvingMoveThreshold, o.carvingTimeToStationary, o.velocity.x} "
            "o.carving = false "
            "local l = NavMeshLink(scene, entity('Bridge')) "
            "l.width = 1 l.costModifier = 3 l.bidirectional = false l.area = 2 l.autoUpdate = true l.agentTypeID = 0 "
            "l.startPoint = Vector3(-2.5, 0, 0) l.endPoint = Vector3(2.5, 0, 0) "
            "local link = {l.valid, l.enabled, l.width, l.costModifier, l.bidirectional, l.area, l.autoUpdate, l.agentTypeID, "
            "l.startPoint.x, l.endPoint.x, l.activated, l.occupied} "
            "l.width = 0 l.costModifier = -1 l.bidirectional = true l.autoUpdate = false "
            "l.activated = false local off = scene:getNavigationSystem():calculatePath(Vector3(14, 0, 0), Vector3(28, 0, 0)).status "
            "l.activated = true local on = scene:getNavigationSystem():calculatePath(Vector3(14, 0, 0), Vector3(28, 0, 0)).status "
            "local m = NavMeshModifier(scene, entity('Door')) "
            "local before = m.appliedPolygons m.enabled = true local applied = m.appliedPolygons "
            "local shut = scene:getNavigationSystem():calculatePath(Vector3(-10, 0, 0), Vector3(14, 0, 0)).status "
            "m.area = 4 local muddy = scene:getNavigationSystem():calculatePath(Vector3(-10, 0, 0), Vector3(14, 0, 0)).status "
            "m.enabled = false m.area = 1 m.center = Vector3(0, 0, 0) m.size = Vector3(2, 3, 3) "
            "local modifier = {m.valid, m.useVolume, m.applyAtRuntime, m.overrideArea, m.applyToChildren, before, applied > 0, "
            "shut == NavMeshPathStatus.Partial, muddy == NavMeshPathStatus.Complete, m.area, m.size.z} "
            "local nav = scene:getNavigationSystem() "
            "local v = nav:setAreaInVolume(Vector3(0, 1.5, 0), Vector3(2, 3, 3), NavAreas.getAreaFromName('Not Walkable'), {rotation = Quaternion()}) "
            "local blocked = nav:calculatePath(Vector3(-10, 0, 0), Vector3(14, 0, 0)).status "
            "local volume = {v.valid, v.area, v.size.x, v.appliedPolygons > 0, blocked == NavMeshPathStatus.Partial} "
            "v.area = 0 local walked = nav:calculatePath(Vector3(-10, 0, 0), Vector3(14, 0, 0)).status "
            "v:remove() "
            "local bad = nav:setAreaInVolume(Vector3(0, 0, 0), Vector3(1, 1, 1), 16) "
            "return obstacle, link, off == NavMeshPathStatus.Partial, on == NavMeshPathStatus.Complete, modifier, volume, "
            "walked == NavMeshPathStatus.Complete, v.valid, bad == nil, l:updateLink()"))
        values = got["values"]
        obstacle = values[0]
        ed.check_eq(obstacle[:3] + obstacle[7:9], [True, True, True, True, False], f"obstacle: {got}")
        for got_value, want in zip(obstacle[3:7] + obstacle[9:], [0.75, 1.5, 0.5, 2, 0.3, 0.25, 0]):
            ed.check(abs(got_value - want) < 1e-5, f"obstacle {want}: {obstacle}")
        ed.check_eq(values[1], [True, True, 1, 3, False, 2, True, 0, -2.5, 2.5, True, False], f"link: {got}")
        ed.check_eq(values[2:4], [True, True], f"activated: {got}")
        ed.check_eq(values[4], [True, True, True, False, True, 0, True, True, True, 1, 3], f"modifier: {got}")
        ed.check_eq(values[5], [True, 1, 2, True, True], f"script volume: {got}")
        ed.check_eq(values[6:], [True, False, True, True], f"{got}")
    finally:
        await ed.call("play", action="stop")
    for cls, members in (("NavMeshObstacle", ["carving", "carvingMoveThreshold", "carvingTimeToStationary", "velocity"]),
                         ("NavMeshLink", ["activated", "occupied", "autoUpdate", "agentTypeID", "updateLink"]),
                         ("NavMeshModifier", ["applyAtRuntime", "appliedPolygons", "useVolume"]),
                         ("NavAreaOverride", ["area", "remove", "valid", "appliedPolygons"]),
                         ("NavigationSystem", ["setAreaInVolume"])):
        got = await ed.session.read_resource(f"jasper://lua-api?class={cls}")
        text = "".join(getattr(c, "text", "") or "" for c in got.contents)
        for member in members:
            ed.check(member in text, f"jasper://lua-api?class={cls} lists {member}")


async def test_overlay_screenshot(ed: Editor):
    """AC30 (R41): with show.area_overrides the Scene view shows the door's volume: a PNG that is not
    one colour, and differs from the same view without it."""
    d = await setup_doors(ed)
    await ed.call("select_entities", scene=d["scene"], entities=[])
    await ed.call("set_scene_camera", {"scene": d["scene"], "position": [0.0, 12.0, 10.0], "target": [0.0, 0.0, 0.0],
                                       "display": {"show_grid_3d": False, "hide_light_icons": True}})
    shown = await ed.call("visualize_navigation", scene=d["scene"], show={"navmesh": False, "area_overrides": True})
    ed.check(shown["overlay"]["area_overrides"] is True, f"{shown}")
    r = await ed.call_raw("visualize_navigation", {"scene": d["scene"], "show": {"navmesh": False, "area_overrides": True},
                                                   "screenshot": True})
    ed.check(not r.is_error and r.images, r.text)
    with_volume = pngdiff.decode(r.images[0][0])
    ed.check(not pngdiff.flat(with_volume), "the image is not one colour")
    await ed.call("visualize_navigation", scene=d["scene"], show={"area_overrides": False})
    without = pngdiff.decode(await ed.screenshot("scene", scene=d["scene"], max_width=1024))
    same, fraction, _ = pngdiff.compare(with_volume, without, max_fraction=0.0005)
    ed.check(not same, f"the door's volume draws ({fraction:.5f} of the pixels differ)")
    await ed.call("visualize_navigation", scene=d["scene"], show={"area_overrides": True})


async def test_path_failed_callbacks(ed: Editor):
    """AC26 (R27, R28): onPathFailed with one argument and with two both run; the reason, and
    lastPathFailure, are S54's snake_case names; get_runtime_entity and get_navigation_info show them."""
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"{started}")
    try:
        cursor = (await ed.call("read_console", limit=1))["next_cursor"]
        # the walker may not enter the mud, which crosses the whole floor: the far side is
        # blocked by an area its mask leaves out
        got = await ed.call("eval_lua", code=(
            "local agent = NavMeshAgent(scene, entity('Walker')) "
            "failedOne, failedTwo, failedReason = 0, 0, nil "
            "agent.onPathFailed:add('one', function(a) failedOne = failedOne + 1 print('S55 one-argument onPathFailed') end) "
            "agent.onPathFailed:add('two', function(a, reason) failedTwo = failedTwo + 1 failedReason = reason "
            "print('S55 two-argument onPathFailed: ' .. reason) end) "
            "agent.areaMask = NavAreas.All - NavAreas.maskOf(NavAreas.getAreaFromName('Mud')) "
            "return agent:setDestination(Vector3(15, 0, -15)), agent.lastPathFailure"))
        ed.check_eq(got["values"], [True, "none"], f"{got}")
        await ed.call("wait", seconds=200, game_seconds=10, **{"until": "return failedOne == 1 and failedTwo == 1"})
        state = await ed.call("eval_lua", code=(
            "local agent = NavMeshAgent(scene, entity('Walker')) "
            "return failedOne, failedTwo, failedReason, agent.lastPathFailure"))
        ed.check_eq(state["values"], [1, 1, "blocked_by_area", "blocked_by_area"], f"{state}")
        text = "\n".join(str(entry.get("message")) for entry in await ed.console_since(cursor, contains="S55 "))
        ed.check("S55 one-argument onPathFailed" in text, f"one-argument marker: {text[-2000:]}")
        ed.check("S55 two-argument onPathFailed: blocked_by_area" in text, f"two-argument marker: {text[-2000:]}")

        live = await ed.call("get_runtime_entity", entity=s["walker"], components=["navmeshagent"])
        agent = live["components"]["navmeshagent"]
        ed.check_eq(agent["lastPathFailure"], "blocked_by_area", f"{agent}")
        ed.check("stuckTime" in agent and "isPathStale" in agent and len(agent["steeringTarget"]) == 3, f"{agent}")
        info = await ed.call("get_navigation_info")
        walker = [a for a in info["agents"] if a["entity"]["id"] == s["walker"]]
        ed.check(walker and walker[0]["last_failure"] == "blocked_by_area" and "stuck_seconds" not in walker[0], f"{walker}")

        # a reachable destination clears it; with stuck detection on, info shows the watchdog
        cleared = await ed.call("eval_lua", code=(
            "local agent = NavMeshAgent(scene, entity('Walker')) "
            "agent.areaMask = NavAreas.All agent.stuckDetection = true "
            "return agent:setDestination(Vector3(-15, 0, 10))"))
        ed.check_eq(cleared["values"], [True], f"{cleared}")
        await ed.call("wait", seconds=200, game_seconds=5, **{"until": (
            "local agent = NavMeshAgent(scene, entity('Walker')) "
            "return not agent.pathPending and agent.lastPathFailure == 'none'")})
        info = await ed.call("get_navigation_info")
        walker = [a for a in info["agents"] if a["entity"]["id"] == s["walker"]]
        ed.check(walker and walker[0]["last_failure"] == "none" and "stuck_seconds" in walker[0], f"{walker}")
    finally:
        await ed.call("play", action="stop")
