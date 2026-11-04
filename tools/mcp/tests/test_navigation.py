"""S19 navigation as an agent sees it (docs/specs/S19-navigation.md R37, R38; AC22-AC25), with the
S53 fixes (docs/specs/S53-navigation-fixes.md R2, R3, R4, R17, R23, R24, R28; AC5, AC7, AC26-AC28) and
S54's tool rules (docs/specs/S54-navigation-diagnostics.md R34, AC19; the diagnostics themselves are
in test_navigation_diagnostics.py).

Scene Nav (3D): a 40 x 40 floor (plane x 4) with a static 2 m box near its middle, a NavMesh
Surface on the floor, a walker with a NavMesh Agent, and a 16 x 12 Nav Grid. The tools: their
schemas, a bake as a job (get_job reaches done), the stale bake after the box moves, the queries
in edit and play, settings changes and their undo, a painted grid wall a path detours around (and
its undo), the overlay screenshot, and the Lua API in play (onDestinationReached fires).
"""

import asyncio
import os
import time
from pathlib import Path

import pngdiff
from harness import Editor, tool_meta

CI = True
EDITOR = {"create": True, "name": "Navigation CI"}

TIMEOUT = 1200

TOOLS = ["bake_navigation", "get_navigation_info", "query_navigation", "set_navigation_settings",
         "paint_nav_grid", "visualize_navigation"]

S: dict = {}

# the AI Navigation overlay over the test scene (JASPER_UPDATE_GOLDEN=1 rewrites it)
GOLDEN = Path(__file__).resolve().parent / "golden" / "navigation_overlay.png"


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
    """Each tool is in tools/list (group navigation) with an object schema; the group is hidden from
    a new session until tool_groups enables it (S54 AC19)."""
    listed = {tool.name: tool for tool in await ed.tools(groups=["navigation"])}
    for name in TOOLS:
        ed.check(name in listed, f"{name} in tools/list")
        schema = listed[name].inputSchema if hasattr(listed[name], "inputSchema") else listed[name].input_schema
        ed.check_eq(schema.get("type"), "object", f"{name} schema {schema}")
    default = {tool.name for tool in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"hidden until navigation is enabled: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["navigation"])
    try:
        enabled = {tool.name for tool in await ed.tools(groups=None)}
        ed.check(set(TOOLS) <= enabled, f"listed once enabled: {sorted(set(TOOLS) - enabled)}")
    finally:
        await ed.call("tool_groups", disable=["navigation"])
    # S54 R34: the read tools are read-only and idempotent; visualize_navigation is idempotent, not read-only
    annotations = {name: listed[name].annotations for name in TOOLS}
    for name in ("get_navigation_info", "query_navigation"):
        ed.check(annotations[name].read_only_hint and annotations[name].idempotent_hint, f"{name}: {annotations[name]}")
    visualize = annotations["visualize_navigation"]
    ed.check(visualize.idempotent_hint and not visualize.read_only_hint, f"visualize_navigation: {visualize}")


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
            "agent.speed = 6 agent.acceleration = 20 return agent:setDestination(Vector3(-15, 0, -3)), agent.isOnNavMesh"))
        ed.check_eq(got["values"], [True, True], f"{got}")
        # 12 m at 6 m/s (2 s of game time); a loaded host runs few frames a second
        await ed.call("wait", seconds=400, game_seconds=30, **{"until": "return arrivedCount == 1"})
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


async def test_overlay_golden(ed: Editor):
    """The overlay draws the navmesh in its area colours, its tile bounds and a query path: on and off
    differ, and the image matches tools/mcp/tests/golden/navigation_overlay.png."""
    s = await setup(ed)
    await bake(ed, s, mode="full")
    await ed.call("select_entities", scene=s["scene"], entities=[])
    cam = {"scene": s["scene"], "position": [0.0, 45.0, 30.0], "target": [0.0, 0.0, 0.0],
           "display": {"show_grid_3d": False, "hide_light_icons": True}}
    await ed.call("set_scene_camera", cam)
    await ed.call("visualize_navigation", scene=s["scene"], show={"navmesh": True, "tiles": True, "links": True},
                  path={"from": [-15, 0, -15], "to": [15, 0, 15]})
    shot = await ed.screenshot("scene", scene=s["scene"], max_width=640)
    img = pngdiff.decode(shot)
    await ed.call("visualize_navigation", scene=s["scene"], show={"navmesh": False, "tiles": False})
    plain = pngdiff.decode(await ed.screenshot("scene", scene=s["scene"], max_width=640))
    same, fraction, _ = pngdiff.compare(img, plain, max_fraction=0.002)
    ed.check(not same, f"the overlay draws ({fraction:.4f} of the pixels differ)")
    await ed.call("visualize_navigation", scene=s["scene"], show={"navmesh": True, "tiles": False})
    if os.environ.get("JASPER_UPDATE_GOLDEN") == "1":
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_bytes(shot)
        print(f"     [golden] wrote {GOLDEN}")
    else:
        # a missing golden is a failure, never a silent rewrite (S53 R28)
        ed.check(GOLDEN.exists(), f"{GOLDEN} is missing: JASPER_UPDATE_GOLDEN=1 writes it")
        ok, fraction, diff = pngdiff.compare(pngdiff.decode(GOLDEN.read_bytes()), img, max_fraction=0.01)
        if not ok and ed.scratch:
            (ed.scratch.root / "navigation_overlay_diff.png").write_bytes(diff)
        ed.check(ok, f"the screenshot differs from {GOLDEN.name} in {fraction:.4f} of the pixels")


async def test_tool_play_policy(ed: Editor):
    """S53 AC27: in play the server refuses bake_navigation and paint_nav_grid; visualize_navigation
    resolves the running scene (after a scene load too) and its path takes agent_type and area_mask.
    bake_navigation is listed as destructive (clear deletes the .navmesh)."""
    s = await setup(ed)
    listed = {tool.name: tool for tool in await ed.tools(groups=["navigation"])}
    bake_tool = listed["bake_navigation"]
    ed.check(bake_tool.annotations and bake_tool.annotations.destructive_hint, f"bake_navigation destructive: {bake_tool.annotations}")
    playing = {name: tool_meta(listed[name]).get("jasper/while_playing") for name in TOOLS}
    ed.check_eq(playing, {"bake_navigation": False, "get_navigation_info": True, "query_navigation": True,
                          "set_navigation_settings": True, "paint_nav_grid": False, "visualize_navigation": True},
                "the play policy of each tool")

    # a surface for agent type 1 only: a path of the default type (0) finds no navmesh
    before = await ed.call("set_navigation_settings", agents=[{"name": "Humanoid"}, {"name": "Small", "radius": 0.3}])
    small = next(a["id"] for a in before["agents"] if a["name"] == "Small")
    ed.check_eq(small, 1, f"{before['agents']}")
    made = await ed.call("create_scene", name="NavPolicy", type="3D", path="scenes/NavPolicy")
    scene = made["scene"]["id"]
    ground = await ed.call("create_entity", scene=scene, type="plane", name="Ground", scale=[2, 1, 2])
    await ed.call("add_component", scene=scene, entity=ground["entity_id"], component="navmeshsurface",
                  values={"navmeshsurface": {"agentType": small}})
    baked = await ed.call("bake_navigation", scene=scene, mode="full")
    ed.check(baked["polys"] > 0, f"{baked}")
    await ed.call("save_scene", scene=scene)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=scene, fixed_fps=60)
    try:
        await ed.check_error("bake_navigation", scene=scene, contains="while a scene is playing")
        await ed.check_error("paint_nav_grid", scene=s["scene"], grid=s["grid"],
                             strokes=[{"cells": [[1, 1]]}], contains="while a scene is playing")
        # a scene load replaces the played copy: the tools follow the one the game shows now
        await ed.call("eval_lua", code="SceneManager.loadScene(SceneManager.currentSceneName)")
        await ed.call("wait", frames=3)
        running = (await ed.call("get_navigation_info"))["scene_id"]
        ed.check(running != scene, f"the running copy is its own scene ({running} vs {scene})")
        path = {"from": [-8, 0, -8], "to": [8, 0, 8], "agent_type": "Small", "area_mask": 1}
        drawn = await ed.call("visualize_navigation", path=path)
        ed.check_eq(drawn["scene_id"], running, f"drawn on the running scene: {drawn}")
        ed.check_eq(drawn["path"]["status"], "complete", f"the Small surface's path: {drawn}")
        ed.check_eq((drawn["path"]["agent_type"], drawn["path"]["area_mask"]), (small, 1), f"{drawn['path']}")
        none = await ed.call("visualize_navigation", path={"from": [-8, 0, -8], "to": [8, 0, 8], "agent_type": 0})
        ed.check(none["path"]["status"] != "complete", f"no navmesh for agent type 0: {none}")
        await ed.call("visualize_navigation", path={"from": [0, 0, 0], "to": [1, 0, 1], "agent_type": "Nobody"}, expect_error=True)
    finally:
        await ed.call("play", action="stop")
        await ed.call("undo", scope="project")


async def errors_since(ed: Editor) -> list[str]:
    return [e["message"] for e in await ed.console_since(types=["error"], limit=1000)]


async def test_bake_failures_reported(ed: Editor):
    """S53 AC5: (a) a refused start (Surface too large), (b) a tile failure (Runtime Carving with Tile
    Size 300) and (c) a save failure (the .navmesh path is a directory) each log exactly one Console
    error naming the surface, and get_navigation_info's last_bake says ok false with the reason. A
    good bake then reports ok true."""
    made = await ed.call("create_scene", name="NavFail", type="3D", path="scenes/NavFail")
    scene = made["scene"]["id"]
    ground = (await ed.call("create_entity", scene=scene, type="plane", name="Ground", scale=[2, 1, 2]))["entity_id"]
    await ed.call("add_component", scene=scene, entity=ground, component="navmeshsurface")

    async def surface() -> dict:
        return (await ed.call("get_navigation_info", scene=scene))["surfaces"][0]

    async def fails(case: str, reason: str) -> None:
        await ed.console_since(None, limit=1)
        message = await ed.call("bake_navigation", scene=scene, mode="full", expect_error=True)
        errors = await errors_since(ed)
        ed.check_eq(len(errors), 1, f"({case}) one Console error: {errors}")
        ed.check("NavMeshSurface 'Ground'" in errors[0] and reason in errors[0], f"({case}) it names the surface: {errors}")
        ed.check(reason in message, f"({case}) the tool says why: {message}")
        last = (await surface()).get("last_bake")
        ed.check(last is not None and last["ok"] is False and not last["cancelled"] and reason in last.get("error", ""),
                 f"({case}) last_bake: {last}")

    # a good bake first: the asset (c) blocks
    await ed.call("bake_navigation", scene=scene, mode="full")
    asset = (await surface())["asset"]
    ed.check(asset.endswith(".navmesh"), f"{asset}")
    file = ed.project / "assets" / asset
    if not file.exists():
        file = ed.project / asset
    ed.check(file.is_file(), f"the .navmesh on disk: {file}")

    # (a) 2 km at 5 cm voxels on 16-voxel tiles: more tiles than a polygon reference holds
    await ed.call("set_transform", scene=scene, entity=ground, scale=[200, 1, 200])
    await ed.call("set_properties", scene=scene, entity=ground, values={"navmeshsurface": {
        "overrideCellSize": True, "cellSize": 0.05, "overrideTileSize": True, "tileSize": 16}})
    await fails("a", "Surface too large")

    # (b) carving tiles hold at most 255 voxels a side: the tile build fails
    await ed.call("set_transform", scene=scene, entity=ground, scale=[2, 1, 2])
    await ed.call("set_properties", scene=scene, entity=ground, values={"navmeshsurface": {
        "overrideCellSize": False, "tileSize": 300, "runtimeCarving": True}})
    await fails("b", "runtime carving needs a tile size of at most 255 cells")

    # (c) the bake succeeds, its save does not
    await ed.call("set_properties", scene=scene, entity=ground, values={"navmeshsurface": {
        "overrideTileSize": False, "runtimeCarving": False}})
    file.unlink()
    file.mkdir()
    try:
        await fails("c", "baked but not saved")
    finally:
        file.rmdir()

    good = await ed.call("bake_navigation", scene=scene, mode="full")
    ed.check(good["polys"] > 0, f"{good}")
    last = (await surface()).get("last_bake")
    ed.check(last is not None and last["ok"] is True and "error" not in last, f"a good bake: {last}")


async def test_bake_warnings(ed: Editor):
    """S53 AC7: 70 stacked 3 x 3 m slabs, 2.5 m apart, are more spans than a compact heightfield
    column holds: the static bake succeeds and bake_navigation returns Recast's warning (and
    get_navigation_info's last_bake keeps it)."""
    made = await ed.call("create_scene", name="NavLayers", type="3D", path="scenes/NavLayers")
    scene = made["scene"]["id"]
    root = (await ed.call("create_entity", scene=scene, type="empty", name="Stack"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=root, component="navmeshsurface")
    await ed.batch([("create_entity", {"scene": scene, "type": "cube", "name": f"Slab{i}", "position": [0, 2.5 * i, 0],
                                       "scale": [3, 0.2, 3]}) for i in range(70)])
    baked = await ed.call("bake_navigation", scene=scene, mode="full")
    warnings = baked["surfaces"][0]["warnings"]
    ed.check(any("Heightfield has too many layers" in w for w in warnings), f"Recast's warning: {warnings}")
    last = (await ed.call("get_navigation_info", scene=scene))["surfaces"][0]["last_bake"]
    ed.check(last["ok"] and last["warnings"] == warnings, f"last_bake keeps them: {last}")


async def test_carving_settings_undo(ed: Editor):
    """S53 AC26: Max Carving Obstacles (a surface property) and Carve Tiles Per Tick (a project
    setting) are set, shown by get_navigation_info and undone."""
    s = await setup(ed)
    info = await ed.call("get_navigation_info", scene=s["scene"])
    ed.check_eq(info["surfaces"][0]["max_carving_obstacles"], 128, f"the default: {info['surfaces'][0]}")
    ed.check_eq(info["settings"]["carve_tiles_per_tick"], 0, f"Auto: {info['settings']}")
    await ed.call("set_properties", scene=s["scene"], entity=s["floor"], values={"navmeshsurface": {"maxCarvingObstacles": 16}})
    changed = await ed.call("set_navigation_settings", carve_tiles_per_tick=1)
    ed.check_eq(changed["carve_tiles_per_tick"], 1, f"{changed}")
    await ed.call("set_navigation_settings", carve_tiles_per_tick=65, expect_error=True)
    info = await ed.call("get_navigation_info", scene=s["scene"])
    ed.check_eq((info["surfaces"][0]["max_carving_obstacles"], info["settings"]["carve_tiles_per_tick"]), (16, 1), f"{info}")
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="scene", scene=s["scene"])
    info = await ed.call("get_navigation_info", scene=s["scene"])
    ed.check_eq((info["surfaces"][0]["max_carving_obstacles"], info["settings"]["carve_tiles_per_tick"]), (128, 0),
                f"both undone: {info}")
