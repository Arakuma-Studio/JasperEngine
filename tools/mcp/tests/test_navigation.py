"""S19 navigation as an agent sees it (docs/specs/S19-navigation.md R37, R38; AC22-AC25).

Scene Nav (3D): a 40 x 40 floor (plane x 4) with a static 2 m box near its middle, a NavMesh
Surface on the floor, a walker with a NavMesh Agent, and a 16 x 12 Nav Grid. The tools: their
schemas, a bake as a job (get_job reaches done), the stale bake after the box moves, the queries
in edit and play, settings changes and their undo, a painted grid wall a path detours around (and
its undo), the overlay screenshot, and the Lua API in play (onDestinationReached fires).
"""

import asyncio
import time

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Navigation CI"}

TIMEOUT = 1200

TOOLS = ["bake_navigation", "get_navigation_info", "query_navigation", "set_navigation_settings",
         "paint_nav_grid", "visualize_navigation"]

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Nav", type="3D", path="scenes/Nav")
    S["scene"] = made["scene"]["id"]
    floor = await ed.call("create_entity", scene=S["scene"], type="plane", name="Floor", scale=[4, 1, 4])
    S["floor"] = floor["entity_id"]
    box = await ed.call("create_entity", scene=S["scene"], type="cube", name="Block", position=[0, 1, 0], scale=[2, 2, 2])
    S["box"] = box["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["floor"], component="navmeshsurface")
    walker = await ed.call("create_entity", scene=S["scene"], type="capsule", name="Walker", position=[-15, 1, -15])
    S["walker"] = walker["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["walker"], component="navmeshagent",
                  values={"navmeshagent": {"baseOffset": 1.0}})
    grid = await ed.call("create_entity", scene=S["scene"], type="empty", name="Grid", position=[30, 0, 0])
    S["grid"] = grid["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["grid"], component="navgrid",
                  values={"navgrid": {"width": 16, "height": 12, "cellSize": 1}})
    await ed.call("save_scene", scene=S["scene"])
    return S


async def bake(ed: Editor, s: dict, **args) -> dict:
    return await ed.call("bake_navigation", scene=s["scene"], **args)


async def wait_job(ed: Editor, job_id: str, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id)
        if job["status"] != "running" or time.monotonic() > deadline:
            return job
        await asyncio.sleep(0.25)


async def test_tools_listed_with_schemas(ed: Editor):
    """Each tool is in tools/list (group navigation) with an object schema."""
    listed = {tool.name: tool for tool in await ed.tools(groups=["navigation"])}
    for name in TOOLS:
        ed.check(name in listed, f"{name} in tools/list")
        schema = listed[name].inputSchema if hasattr(listed[name], "inputSchema") else listed[name].input_schema
        ed.check_eq(schema.get("type"), "object", f"{name} schema {schema}")


async def test_bake_job_and_info(ed: Editor):
    """bake_navigation wait:false gives a job that get_job sees done; the surface is then baked."""
    s = await setup(ed)
    started = await bake(ed, s, mode="full", wait=False)
    ed.check("job_id" in started, f"a job: {started}")
    done = await wait_job(ed, started["job_id"], timeout=300)
    ed.check_eq(done["status"], "succeeded", f"{done}")
    info = await ed.call("get_navigation_info", scene=s["scene"])
    surface = info["surfaces"][0]
    ed.check_eq(surface["state"], "baked", f"{surface}")
    ed.check(surface["polys"] > 0 and surface["tiles"] > 0, f"{surface}")
    ed.check(surface["asset"].endswith(".navmesh"), f"saved as a .navmesh asset: {surface}")
    ed.check_eq(surface["dirty_tiles"], 0, f"nothing changed since: {surface}")
    # an untouched rebake builds nothing
    again = await bake(ed, s)
    ed.check_eq(again["tiles_built"], 0, f"{again}")


async def test_stale_after_moving_static_geometry(ed: Editor):
    s = await setup(ed)
    await bake(ed, s, mode="full")
    await ed.call("set_transform", scene=s["scene"], entity=s["box"], position=[6, 1, 6])
    info = await ed.call("get_navigation_info", scene=s["scene"])
    ed.check(info["surfaces"][0]["dirty_tiles"] > 0, f"the moved box dirties tiles: {info['surfaces'][0]}")
    rebaked = await bake(ed, s)
    ed.check(0 < rebaked["tiles_built"] <= rebaked["surfaces"][0]["tiles_total"], f"{rebaked}")
    await ed.call("undo", scope="scene", scene=s["scene"])
    await bake(ed, s)


async def test_queries_in_edit_mode(ed: Editor):
    s = await setup(ed)
    await bake(ed, s, mode="full")
    path = await ed.call("query_navigation", scene=s["scene"], op="path", **{"from": [-15, 0, -15]}, to=[15, 0, 15])
    ed.check_eq(path["status"], "complete", f"{path}")
    ed.check(len(path["corners"]) >= 2 and path["length"] > 40, f"{path}")
    sample = await ed.call("query_navigation", scene=s["scene"], op="sample", **{"from": [5, 3, 5]}, max_distance=5)
    # on the floor (y 0), not on the block's top (y 2)
    ed.check(sample["found"] and abs(sample["position"][1]) < 1.0, f"{sample}")
    ray = await ed.call("query_navigation", scene=s["scene"], op="raycast", **{"from": [-6, 0, 0]}, to=[6, 0, 0])
    ed.check(ray["blocked"], f"the box blocks the line: {ray}")
    edge = await ed.call("query_navigation", scene=s["scene"], op="closest_edge", **{"from": [-3, 0, 0]})
    ed.check(edge["found"] and edge["distance"] < 3, f"{edge}")
    reach = await ed.call("query_navigation", scene=s["scene"], op="reachable", **{"from": [-15, 0, -15]}, to=[15, 0, 15])
    ed.check(reach["reachable"], f"{reach}")
    await ed.call("query_navigation", scene=s["scene"], op="teleport", **{"from": [0, 0, 0]}, expect_error=True)


async def test_settings_change_and_undo(ed: Editor):
    s = await setup(ed)
    before = (await ed.call("get_navigation_info", scene=s["scene"]))["settings"]
    changed = await ed.call("set_navigation_settings", areas=[{"index": 3, "name": "Mud", "cost": 5}], crowd_max_agents=64,
                            agents=[{"name": "Humanoid"}, {"name": "Giant", "radius": 1.5, "height": 4}])
    ed.check_eq(changed["areas"][3]["name"], "Mud", f"{changed['areas'][3]}")
    ed.check_eq(changed["crowd_max_agents"], 64, f"{changed}")
    ed.check_eq([a["name"] for a in changed["agents"]], ["Humanoid", "Giant"], f"{changed['agents']}")
    await ed.call("set_navigation_settings", areas=[{"index": 0, "name": "Ground"}], expect_error=True)
    await ed.call("undo", scope="project")
    after = (await ed.call("get_navigation_info", scene=s["scene"]))["settings"]
    ed.check_eq(after, before, "undo restores the settings")


async def test_paint_nav_grid(ed: Editor):
    """AC23: a 3-cell wall painted across the path makes it detour; undo restores it; the .navgrid
    is written on save."""
    s = await setup(ed)
    query = {"scene": s["scene"], "op": "path", "target": s["grid"], "from": [30.5, 0, 5.5], "to": [37.5, 0, 5.5]}
    straight = await ed.call("query_navigation", **query)
    ed.check_eq(straight["status"], "complete", f"{straight}")
    painted = await ed.call("paint_nav_grid", scene=s["scene"], grid=s["grid"],
                            strokes=[{"mode": "paint", "area": "Not Walkable", "cells": [[4, 4], [4, 6]]}])
    ed.check_eq(painted["changed_cells"], 3, f"{painted}")
    ed.check_eq(painted["undo_name"], "Paint Nav Grid", f"{painted}")
    detour = await ed.call("query_navigation", **query)
    ed.check_eq(detour["status"], "complete", f"{detour}")
    ed.check(detour["cost"] > straight["cost"] + 0.5, f"the path detours: {straight['cost']} -> {detour['cost']}")
    ed.check(all(c != [4, 5] for c in detour["cells"]), f"never through the wall: {detour['cells']}")
    await ed.call("undo", scope="scene", scene=s["scene"])
    restored = await ed.call("query_navigation", **query)
    ed.check(abs(restored["cost"] - straight["cost"]) < 1e-3, f"undo restores the path: {restored['cost']}")
    # painted again and saved: the cells go to a .navgrid
    await ed.call("paint_nav_grid", scene=s["scene"], grid=s["grid"], strokes=[{"mode": "paint", "cells": [[2, 2]]}])
    await ed.call("save_scene", scene=s["scene"])
    info = await ed.call("get_navigation_info", scene=s["scene"])
    ed.check(info["grids"][0]["asset"].endswith(".navgrid"), f"{info['grids'][0]}")
    asset = info["grids"][0]["asset"]
    ed.check((ed.project / "assets" / asset).exists() or (ed.project / asset).exists(), f"the .navgrid is on disk: {asset}")


async def test_visualize_screenshot(ed: Editor):
    s = await setup(ed)
    await bake(ed, s)
    r = await ed.call_raw("visualize_navigation", {"scene": s["scene"], "show": {"navmesh": True, "tiles": True},
                                                   "path": {"from": [-15, 0, -15], "to": [15, 0, 15]}, "screenshot": True})
    ed.check(not r.is_error, r.text)
    ed.check(r.images, "a Scene view image")
    ed.check("flat" not in r.text, f"no flat-image note: {r.text}")
    img = pngdiff.decode(r.images[0][0])
    ed.check(not pngdiff.flat(img), "the image is not one colour")


async def test_lua_api_in_play(ed: Editor):
    """AC25: setDestination then onDestinationReached; the queries' shapes; get_runtime_entity."""
    s = await setup(ed)
    await bake(ed, s)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"{started}")
    try:
        got = await ed.call("eval_lua", code=(
            "local agent = NavMeshAgent(scene, entity('Walker')) "
            "arrivedCount = 0 "
            "agent.onDestinationReached = function(a) arrivedCount = arrivedCount + 1 end "
            "return agent:setDestination(Vector3(-15, 0, 15)), agent.isOnNavMesh"))
        ed.check_eq(got["values"], [True, True], f"{got}")
        # ~30 m at 3.5 m/s: game time, however slowly the editor's frames come
        await ed.call("wait", seconds=180, game_seconds=30, **{"until": "return arrivedCount == 1"})
        live = await ed.call("get_runtime_entity", entity=s["walker"], components=["navmeshagent"])
        agent = live["components"]["navmeshagent"]
        ed.check_eq(agent["pathStatus"], "complete", f"{agent}")
        ed.check(agent["isOnNavMesh"] and agent["remainingDistance"] < 0.5, f"{agent}")
        shapes = await ed.call("eval_lua", code=(
            "local nav = scene:getNavigationSystem() "
            "local p = nav:calculatePath(Vector3(-15, 0, -15), Vector3(15, 0, 15), NavMeshQueryFilter{agentType = 'Humanoid', areaMask = NavAreas.All}) "
            "local ok, hit = nav:samplePosition(Vector3(5, 3, 5), 5.0, NavAreas.All) "
            "local blocked, rhit = nav:raycast(Vector3(-6, 0, 0), Vector3(6, 0, 0)) "
            "return p.status == NavMeshPathStatus.Complete, #p.corners >= 2, p.length > 40, ok, hit.position.y < 0.5, blocked, rhit.hit"))
        ed.check_eq(shapes["values"], [True] * 7, f"{shapes}")
        info = await ed.call("query_navigation", op="path", **{"from": [-15, 0, 15]}, to=[15, 0, -15])
        ed.check_eq(info["status"], "complete", f"query in play: {info}")
    finally:
        await ed.call("play", action="stop")
