"""T5: foliage through the terrain group's tools: scatter, sculpt, pick, Play and the docs.

AC18, AC20-AC22, AC24, AC27, AC36-AC38, AC39, AC42, AC43. The meshes are small OBJ files this test
writes into its own project.
"""

from __future__ import annotations

import asyncio
import hashlib
from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Foliage CI"}

CROSS = """v -1 0 0
v 1 0 0
v 1 6 0
v -1 6 0
v 0 0 -1
v 0 0 1
v 0 6 1
v 0 6 -1
vn 0 0 1
vn 1 0 0
f 1//1 2//1 3//1
f 1//1 3//1 4//1
f 3//1 2//1 1//1
f 4//1 3//1 1//1
f 5//2 6//2 7//2
f 5//2 7//2 8//2
f 7//2 6//2 5//2
f 8//2 7//2 5//2
"""

SHAPE = {"type": "rect", "min": [-40, -40], "max": [40, 40]}


async def terrain_scene(ed: Editor, name: str) -> tuple[int, int]:
    await ed.call("tool_groups", enable=["terrain"])
    (ed.project / "assets").mkdir(exist_ok=True)
    (ed.project / "assets" / "tree.obj").write_text(CROSS)
    (ed.project / "assets" / "blade.obj").write_text(CROSS)
    sid = (await ed.call("create_scene", name=name, type="3d", path=f"scenes/{name}"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=sid, name="Land", type="terrain"))["entity_id"]
    await ed.call("set_terrain_maps", scene=sid, entity=terrain,
                  heightmap={"generate": "fractal_noise", "resolution": 129, "seed": 3, "amplitude": 0.2,
                             "base_height": 0.3, "frequency": 3.0, "octaves": 3})
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {
        "treePrototypes": [{"id": 1, "meshPath": "assets/tree.obj"}, {"id": 2, "meshPath": "assets/tree.obj"}]}})
    return sid, terrain


def scatter(seed: int = 42, density: float = 0.02, **extra) -> dict:
    return {"op": "scatter_by_rules", "shape": SHAPE, "seed": seed, "density": density,
            "target": {"trees": [{"prototype": 0, "weight": 1}]}, **extra}


async def tree_count(ed: Editor, sid: int, terrain: int) -> int:
    got = await ed.call("sample_terrain", scene=sid, entity=terrain, foliage=True, grid=0)
    return sum(item["count"] for item in got["foliage"]["trees"])


async def tree_data_path(ed: Editor, sid: int, terrain: int) -> Path:
    fields = await ed.call("get_entity", scene=sid, entity=terrain, components=["terrain"], properties=["treeData"])
    relative = fields["components"]["terrain"]["treeData"]
    ed.check(relative, f"{fields}")
    return ed.project / relative


async def test_group_hidden_until_enabled(ed: Editor):
    default = {tool.name for tool in await ed.tools(groups=None)}
    ed.check("edit_terrain" not in default and "visualize_terrain" not in default, "the terrain group is hidden")
    chosen = {tool.name for tool in await ed.tools(groups=["terrain"])}
    ed.check("edit_terrain" in chosen and "visualize_terrain" in chosen, "tool_groups lists it")


async def test_scatter_by_rules_is_deterministic(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Deterministic")
    dry = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()], dry_run=True)
    ed.check_eq(await tree_count(ed, sid, terrain), 0, "dry_run writes nothing")
    real = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    ed.check_eq(real["ops"][0]["instances_added"], dry["ops"][0]["instances_added"], f"{dry} {real}")
    ed.check_eq(real["ops"][0]["hash"], dry["ops"][0]["hash"], "a dry run reports what a real run does")
    ed.check(real["ops"][0]["instances_added"] > 0, f"{real}")
    await ed.call("undo", scope="scene")
    again = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    ed.check_eq(again["ops"][0]["hash"], real["ops"][0]["hash"], "the same input, the same trees")
    other = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter(seed=43, mode="replace")], dry_run=True)
    ed.check(other["ops"][0]["hash"] != real["ops"][0]["hash"], "another seed, other trees")


async def test_invalid_op_changes_nothing(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Invalid")
    await ed.check_error("edit_terrain", {"scene": sid, "entity": terrain,
                         "ops": [scatter(), {"op": "scatter_by_rules", "target": {"trees": [{"prototype": 99}]}, "density": 1}]},
                         contains="prototype")
    ed.check_eq(await tree_count(ed, sid, terrain), 0, "all or nothing")
    await ed.check_error("edit_terrain", {"scene": sid, "entity": terrain, "ops": [{"op": "no_such_op"}]}, contains="no_such_op")


async def test_one_undo_step(ed: Editor):
    sid, terrain = await terrain_scene(ed, "OneUndo")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    count = await tree_count(ed, sid, terrain)
    ed.check(count > 0, "planted")
    await ed.call("undo", scope="scene")
    ed.check_eq(await tree_count(ed, sid, terrain), 0, "one undo takes the whole call")
    await ed.call("redo", scope="scene")
    ed.check_eq(await tree_count(ed, sid, terrain), count, "redo brings it back")


async def test_sculpt_resnaps_trees(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Resnap")
    placed = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[
        {"op": "place_trees", "prototype": 0, "points": [[0, 0], [10, 5], [-8, 12]]}], space="local")
    ed.check_eq(len(placed["ops"][0]["ids"]), 3, f"{placed}")
    await ed.call("sculpt_terrain", scene=sid, entity=terrain, space="local", strokes=[
        {"mode": "raise", "points": [[0, 0], [10, 5], [-8, 12]], "size": 12.0, "strength": 1.0, "seconds_per_point": 0.2}])
    await ed.call("wait", seconds=30, frames=30)
    got = await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local terrain = Terrain(scene, entity('Land')) "
        "local list = terrain:getFoliageInstancesInRadius(Vector3(0, 0, 0), 500) "
        "local out = {} "
        "for _, tree in ipairs(list) do out[#out + 1] = tree.position.x; out[#out + 1] = tree.position.y; out[#out + 1] = tree.position.z end "
        "return #list, table.concat(out, ',')"))
    ed.check_eq(got["values"][0], 3, f"{got}")
    numbers = [float(value) for value in got["values"][1].split(",")]
    trees = [numbers[i:i + 3] for i in range(0, len(numbers), 3)]
    surface = await ed.call("sample_terrain", scene=sid, entity=terrain, space="world", points=[[tree[0], tree[2]] for tree in trees])
    for tree, sample in zip(trees, surface["samples"]):
        ed.check(abs(sample["height"] - tree[1]) <= 1e-3, f"the tree follows the surface: {tree} {sample}")


async def test_mass_place_does_not_stall(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Mass")
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter(density=2.0)], wait=False)
    ed.check("job_id" in started, f"a big run is a job: {started}")
    for _ in range(600):
        await asyncio.sleep(0.05)
        status = await ed.call("editor_status")
        ed.check(status is not None, "the editor answers while it runs")
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check(await tree_count(ed, sid, terrain) > 0, "planted")


async def test_job_lands_in_an_idle_editor(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Idle")
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()], wait=False)
    # Nothing calls the editor meanwhile, so nothing makes it draw: idle is the condition under test.
    # The job ends when its result lands, so it must end during the quiet, not at the next call.
    quiet = 5.0
    await asyncio.sleep(quiet)
    for _ in range(600):
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        await asyncio.sleep(0.05)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check(job["elapsed_seconds"] < quiet - 1.0, f"the result landed while the editor was idle: {job}")


async def test_stats_match_a_count(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Stats")
    real = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    got = await ed.call("sample_terrain", scene=sid, entity=terrain, foliage=True, grid=0)
    ed.check_eq(sum(item["count"] for item in got["foliage"]["trees"]), real["ops"][0]["instances_added"], f"{got['foliage']}")
    totals = got["foliage"]["totals"]
    stored = real["ops"][0]["instances_added"] * 40
    ed.check(totals["cpu_bytes"] >= stored, f"at least the stored trees' bytes: {totals}")
    ed.check(totals["cpu_bytes"] <= (stored + totals["resident"] * 32) * 1.1, f"{totals}")


async def test_drawn_reads_foliage_stats(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Drawn")
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {
        "foliageLayers": [{"density": 0.5, "meshPath": "assets/blade.obj"}]}})
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[
        {"op": "scatter_by_rules", "target": {"detail_layer": 0}, "density": 0.5, "seed": 1}])
    await ed.call("wait", seconds=60, frames=30)
    got = await ed.call("sample_terrain", scene=sid, entity=terrain, foliage=True, grid=0)
    details = got["foliage"]["details"][0]
    ed.check(details["instances"] > 0, f"{details}")
    ed.check("drawn" in details, f"{details}")


async def test_density_overlay_changes_the_capture(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Overlay")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    before = (await ed.call_raw("visualize_terrain", {"scene": sid, "entity": terrain, "overlay": "none"})).images
    shown = await ed.call_raw("visualize_terrain", {"scene": sid, "entity": terrain, "overlay": "tree_density"})
    ed.check(shown.images and before and shown.images[0][0] != before[0][0], "the heat map appears")
    ed.check("legend" in shown.value, f"{shown.value}")
    hidden = await ed.call("visualize_terrain", scene=sid, entity=terrain, overlay="none")
    passes = await ed.call("get_gpu_passes")
    ed.check(not any("overlay" in str(item.get("name", "")).lower() and "terrain" in str(item.get("name", "")).lower()
                     for item in passes.get("passes", [])), "off leaves no pass")
    ed.check(hidden is not None, "none hides it")


async def test_pick_at_reports_tree(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Pick")
    placed = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[
        {"op": "place_trees", "prototype": 0, "points": [[0, 0]]}], space="local")
    surface = await ed.call("sample_terrain", scene=sid, entity=terrain, space="local", points=[[0, 0]])
    y = surface["samples"][0]["height"]
    await ed.call("wait", seconds=60, frames=30)
    await ed.call("set_scene_camera", scene=sid, position=[0, y + 3, 20], target=[0, y + 3, 0])
    await ed.call("wait", seconds=30, frames=5)
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    trees = [hit for hit in got["hits"] if hit["kind"] == "tree"]
    ed.check(trees, f"{got['hits']}")
    ed.check_eq(trees[0]["tree_id"], placed["ops"][0]["ids"][0], f"{trees[0]}")
    ed.check_eq(trees[0]["prototype"], 0, f"{trees[0]}")
    ed.check_eq(trees[0]["entity"], terrain, "the GameObject is the Terrain")


async def test_set_properties_reaches_prototypes(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Props")
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {
        "treeDistance": 321, "billboardStart": 44, "maxMeshTrees": 77,
        "treePrototypes": [{"id": 1, "meshPath": "assets/tree.obj", "collision": "capsule", "collisionRadius": 0.8, "navObstacle": True}]}})
    got = await ed.call("get_entity", scene=sid, entity=terrain, components=["terrain"],
                        properties=["treeDistance", "billboardStart", "maxMeshTrees", "treePrototypes"])
    fields = got["components"]["terrain"]
    ed.check_eq(fields["treeDistance"], 321, f"{fields}")
    ed.check_eq(fields["maxMeshTrees"], 77, f"{fields}")
    proto = fields["treePrototypes"][0]
    ed.check_eq(proto["collisionRadius"], 0.8, f"{proto}")
    ed.check_eq(proto["navObstacle"], True, f"{proto}")
    await ed.call("undo", scope="scene")


async def test_docs_exist(ed: Editor):
    await ed.call("tool_groups", enable=["terrain"])
    root = Path(__file__).resolve().parents[3]
    text = (root / "docs" / "mcp" / "guide" / "terrain.md").read_text()
    for word in ("scatter_by_rules", "place_trees", "bake_impostors", "sample_terrain"):
        ed.check_in(word, text, "the guide names it")
    ed.check((root / "docs" / "scripting" / "foliage.md").exists(), "the Lua page")


async def test_chop_in_play_leaves_the_file_alone(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Chop")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    await ed.call("save_scene", scene=sid)
    tree_file = await tree_data_path(ed, sid, terrain)
    before = hashlib.sha256(tree_file.read_bytes()).hexdigest()

    await ed.call("play", action="start", scene=sid, fixed_fps=60)
    try:
        got = await ed.call("eval_lua", code=(
            "local terrain = Terrain(scene, entity('Land')) "
            "local list = terrain:getFoliageInstancesInRadius(Vector3(0, 0, 0), 500) "
            "local removed = terrain:removeTreeInstance(list[1].id) "
            "return #list, removed ~= nil, #terrain:getFoliageInstancesInRadius(Vector3(0, 0, 0), 500)"))
        ed.check(got["values"][0] > 0 and got["values"][1], f"{got}")
        ed.check_eq(got["values"][2], got["values"][0] - 1, f"{got}")
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(hashlib.sha256(tree_file.read_bytes()).hexdigest(), before, "a chop never touches the file")


async def test_force_field_overrides_wind(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Wind")
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"windDirection": [1, 0, 0], "windSpeed": 2}})
    await ed.call("play", action="start", scene=sid, fixed_fps=60)
    try:
        got = await ed.call("eval_lua", code=(
            "local terrain = Terrain(scene, entity('Land')) "
            "return terrain.windSpeed, terrain.windDirection.x"))
        ed.check_eq(got["values"], [2, 1], f"{got}")
    finally:
        await ed.call("play", action="stop")


async def test_distance_scale_from_lua(ed: Editor):
    sid, _ = await terrain_scene(ed, "Scale")
    await ed.call("play", action="start", scene=sid, fixed_fps=60)
    try:
        got = await ed.call("eval_lua", code=(
            "Engine.foliageDistanceScale = 0.5 "
            "local half = Engine.foliageDistanceScale "
            "Engine.foliageDistanceScale = -3 "
            "local clamped = Engine.foliageDistanceScale "
            "Engine.resetFoliageDistanceScale() "
            "return half, clamped"))
        ed.check_eq(got["values"], [0.5, 0], f"{got}")
    finally:
        await ed.call("play", action="stop")


async def test_duplicate_terrain_copies_on_first_write(ed: Editor):
    sid, terrain = await terrain_scene(ed, "Duplicate")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[scatter()])
    await ed.call("save_scene", scene=sid)
    count = await tree_count(ed, sid, terrain)
    made = await ed.call("duplicate_entities", scene=sid, entities=[terrain])
    copy = made["created"][0]["id"]
    original_file = await tree_data_path(ed, sid, terrain)
    before = original_file.read_bytes()
    await ed.call("edit_terrain", scene=sid, entity=copy, ops=[{"op": "clear_foliage", "target": "trees"}])
    ed.check_eq(await tree_count(ed, sid, copy), 0, "the copy is cleared")
    ed.check_eq(await tree_count(ed, sid, terrain), count, "the original keeps its trees")
    await ed.call("save_scene", scene=sid)
    ed.check_eq(original_file.read_bytes(), before, "the copy's write leaves the original's file alone")
    ed.check((await tree_data_path(ed, sid, copy)) != original_file, "the copy has a file of its own")
