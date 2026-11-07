"""S55 navigation gameplay API as an agent sees it (docs/specs/S55-navigation-gameplay-api.md R42-R45;
AC26-AC28, AC30).

Scene Gameplay (3D): a 40 x 40 floor (plane x 4) with a NavMesh Surface, a strip of the "Mud" area
(index 3, cost 4) across it at x in [-1, 1] (a NavMesh Modifier volume), and a walker with a NavMesh
Agent. test_query_ops (S55 P1): query_navigation's random (seeded, around a point, on an island),
move_along, closest_edge with max_distance, and the path's cost, corner areas and flags and lengths
per area, in edit mode and in play.
"""

import math

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
    # from the floor into the mud strip (x in [-1, 1])
    opened = await ed.call("query_navigation", op="move_along", **q, **{"from": [-2, 0, -10]}, to=[0.5, 0, -9])
    ed.check(opened["found"] and not opened["blocked"], f"{opened}")
    ed.check(flat_distance(opened["position"], [0.5, 0, -9]) < 0.01, f"{opened}")
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
