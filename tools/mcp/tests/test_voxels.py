"""T6 Voxel Volumes through MCP (docs/specs/T6-voxel-volumes.md R66-R72, AC18, AC24, AC28, AC30, AC40,
AC46): the voxels group; edit_voxels with create; every sample_voxels mode, hash stability and
connectivity on a known tunnel and a sealed cavity; visualize_voxels' slice bytes and overlays;
generate_voxels as a job, its undo and cancel; an edit in Play that leaves edit mode alone; the guide;
the presets; a scene whose bytes do not change; and the budgets on the reference cave world.

Runs in one new project; tests run in order and share the editor.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import re
import sys
import time
from pathlib import Path

from harness import Editor, find_key, tool_meta

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import tool_budget  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Voxels CI"}
TIMEOUT = 1800

TOOLS = ("edit_voxels", "sample_voxels", "visualize_voxels", "generate_voxels")
S: dict = {}

# T2's format: the distance to a height, which is ground as a Noise Graph writes it
def height_graph(height: float) -> str:
    return json.dumps({
        "formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 8,
        "nodes": [{"id": "n1", "type": "position", "position": [0, 0], "params": {}},
                  {"id": "n2", "type": "scale_offset", "position": [240, 0], "params": {"scale": 1.0, "offset": -height}},
                  {"id": "n3", "type": "output_value", "position": [480, 0], "params": {"name": "sdf"}}],
        "edges": [{"id": "e4", "from": {"node": "n1", "port": "y"}, "to": {"node": "n2", "port": "in"}},
                  {"id": "e5", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "in"}}],
        "variables": [], "groups": [], "viewport": {"offset": [0, 0], "zoom": 1.0},
        "metadata": {"noise": {"space": "world", "dimensions": "3d"}}})


# R57: per view, on the reference content
GPU_BUDGETS = {"High": (300, 600_000), "Medium": (180, 300_000), "Low": (120, 150_000)}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["voxels", "editor_ux", "navigation", "terrain"])
    made = await ed.call("create_scene", name="Caves", type="3D", path="scenes/Caves")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    return S


def code_of(result) -> str:
    return result.value.get("error", "") if isinstance(result.value, dict) else ""


async def refused(ed: Editor, tool: str, code: str, **args) -> dict:
    result = await ed.call_raw(tool, args)
    ed.check(result.is_error, f"{tool} {args} should fail with {code}: {result.text[:300]}")
    ed.check_eq(code_of(result), code, f"{tool} {args}: {result.text[:300]}")
    return result.value


async def finished(ed: Editor, started: dict, timeout: float = 900.0) -> dict:
    """A job's result, or the reply itself when the call did not need one."""
    if "job_id" not in started:
        return started
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            return job
        await asyncio.sleep(0.2)
    ed.fail(f"job {started['job_id']} still running after {timeout} s")


async def volume(ed: Editor, name: str, size=(64, 64, 64), height=32.0, **create) -> int:
    made = await ed.call("edit_voxels", create={"name": name, "size": list(size), "base": "solid_ground", "height": height, **create})
    return made["entity"]["id"]


async def content_hash(ed: Editor, entity: int) -> str:
    reply = await finished(ed, await ed.call("sample_voxels", entity=entity, mode="hash"))
    return reply.get("result", reply)["content_hash"]


async def history_size(ed: Editor, scene: int) -> int:
    """The steps done: an undone step waits in the redo tail and does not count."""
    return len((await ed.call("get_undo_history", scene=scene, limit=500, include_redo=False))["entries"])


async def test_group_hidden_until_enabled(ed: Editor):
    """R66: hidden until tool_groups enables it, and within R66's budget without an exception."""
    await setup(ed)
    ed.require_tools("tool_groups", *TOOLS)
    await ed.call("tool_groups", disable=["voxels"])
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"hidden by default: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["voxels"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(TOOLS) <= listed, f"listed once enabled: {sorted(set(TOOLS) - listed)}")
    # a group's listing carries the core tools too
    core = {t.name for t in await ed.tools(groups=[])}
    tools = {t.name: t for t in await ed.tools(groups=["voxels"]) if t.name not in core}
    ed.check_eq(set(tools), set(TOOLS), "four tools and only four (R82)")
    sizes = {name: tool_budget.measure(tools[name])["total"] for name in TOOLS}
    print(f"     [budget] voxels tools: {sizes}, {sum(sizes.values())} chars")
    ed.check(sum(sizes.values()) <= 4500, f"{sizes}: over 4,500")
    for name, size in sizes.items():
        ed.check(size <= 1500, f"{name}: {size} chars, over tool_default")
        ed.check_eq(tool_meta(tools[name]).get("jasper/while_playing"), name != "generate_voxels", f"{name} while playing")


async def test_create(ed: Editor):
    """R67: everything validated first, a dry run writes nothing, one undo step."""
    s = await setup(ed)
    scene = s["scene"]
    await refused(ed, "edit_voxels", "invalid_arguments", create={"name": "Bad"})
    await refused(ed, "edit_voxels", "invalid_arguments", create={"size": [17, 64, 64]})
    await refused(ed, "edit_voxels", "invalid_arguments", create={"size": [8, 64, 64]})
    await refused(ed, "edit_voxels", "invalid_arguments", create={"size": [64, 64, 64], "voxel_size": 0})
    await refused(ed, "edit_voxels", "invalid_arguments", create={"size": [64, 64, 64], "colour": 1})
    # AC1: a non-unit scale is refused
    await refused(ed, "edit_voxels", "invalid_arguments", create={"size": [64, 64, 64], "scale": [2, 2, 2]})
    before = await history_size(ed, scene)
    files = sorted(p.name for p in ed.project.rglob("*.voxelvolume"))
    # a bad op in the call leaves nothing behind: no file, no GameObject
    await refused(ed, "edit_voxels", "out_of_bounds", create={"name": "Nope", "size": [32, 32, 32]},
                  ops=[{"op": "dig", "shape": {"type": "sphere", "center": [500, 0, 0], "radius": 1}}])
    await refused(ed, "edit_voxels", "invalid_shape", create={"name": "Nope", "size": [32, 32, 32]},
                  ops=[{"op": "dig", "shape": {"type": "sphere", "center": [5, 5, 5]}}])
    dry = await ed.call("edit_voxels", create={"name": "Dry", "size": [1024, 128, 1024]}, dry_run=True)
    ed.check(dry["dry_run"] and dry["estimated_memory"] > 0, f"{dry}")
    targets = set(dry["over_budget_targets"])
    ed.check({"android", "ios", "web"} <= targets and "linux" not in targets, f"1024 is over mobile and web: {dry['over_budget_targets']}")
    ed.check_eq(sorted(p.name for p in ed.project.rglob("*.voxelvolume")), files, "nothing written by refusals and dry runs")
    ed.check_eq(await history_size(ed, scene), before, "no undo step from refusals and dry runs")

    made = await ed.call("edit_voxels", create={"name": "First", "size": [64, 32, 48], "voxel_size": 0.5, "position": [10, 0, 0],
                                                "base": "solid_ground", "materials": [{"name": "Rock"}, {"name": "Dirt", "color": [0.4, 0.3, 0.2]}]})
    entity = made["entity"]["id"]
    ed.check_eq(made["dimensions"], [64, 32, 48], "dimensions")
    slots = json.dumps(await ed.call("get_entity", entity=entity, components=["voxelvolume"], properties=["materialSlots"]))
    ed.check(re.search(r'pbr\W+true', slots) is not None, f"a colour is a PBR slot's tint, or it would draw untinted: {slots}")
    ed.check(made["asset"].endswith("First.voxelvolume"), f"{made}")
    ed.check((ed.project / made["asset"]).exists() or any(p.name == "First.voxelvolume" for p in ed.project.rglob("*.voxelvolume")), "the file exists")
    ed.check_eq(await history_size(ed, scene), before + 1, "one undo step")
    first_hash = await content_hash(ed, entity)
    # metres in world space: the surface is at half of 32 voxels of 0.5 m, the volume at x = 10
    probe = await ed.call("sample_voxels", entity=entity, mode="values", points=[[20, 2, 10], [20, 12, 10]])
    ed.check(probe["values"][0]["inside"] and not probe["values"][1]["inside"], f"{probe}")
    await ed.call("undo", scope="scene", scene=scene)
    ed.check_eq(await history_size(ed, scene), before, "undone")
    gone = await ed.call_raw("sample_voxels", {"entity": entity, "mode": "stats"})
    ed.check(gone.is_error, "the GameObject went with the step")
    await ed.call("redo", scope="scene", scene=scene)
    ed.check_eq(await content_hash(ed, entity), first_hash, "redo brings the same content back")


async def test_edit(ed: Editor):
    """R27, R68: every code, a failed call changes nothing, dry_run reports what apply does, a batch
    is one step (AC28), and an edit over a second becomes a job that refuses others (volume_busy)."""
    s = await setup(ed)
    scene = s["scene"]
    entity = await volume(ed, "Edits", materials=[{"name": "Rock"}, {"name": "Dirt"}])
    s["edits"] = entity
    start = await content_hash(ed, entity)

    await refused(ed, "edit_voxels", "out_of_bounds", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [900, 0, 0], "radius": 2}}])
    await refused(ed, "edit_voxels", "invalid_shape", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": -1}}])
    await refused(ed, "edit_voxels", "invalid_shape", entity=entity, ops=[{"op": "dig", "shape": {"type": "cone", "center": [32, 32, 32]}}])
    typo = await refused(ed, "edit_voxels", "invalid_shape", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "centre": [1, 1, 1], "radius": 1}}])
    ed.check("center" in typo["message"], f"names the field: {typo}")
    await refused(ed, "edit_voxels", "invalid_shape", entity=entity, ops=[{"op": "blast", "shape": {"type": "sphere", "center": [1, 1, 1], "radius": 1}}])
    await refused(ed, "edit_voxels", "invalid_material", entity=entity, ops=[{"op": "paint", "material": 9, "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 2}}])
    await refused(ed, "edit_voxels", "invalid_material", entity=entity, ops=[{"op": "paint", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 2}}])
    await refused(ed, "edit_voxels", "missing_asset", entity=entity, ops=[{"op": "add", "shape": {"type": "stamp", "stamp": "voxels/none.voxelvolume", "center": [32, 32, 32]}}])
    # all or nothing: the second op fails, the first does not stay
    mixed = await refused(ed, "edit_voxels", "out_of_bounds", entity=entity,
                          ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 3}},
                               {"op": "dig", "shape": {"type": "sphere", "center": [900, 0, 0], "radius": 3}}])
    ed.check_eq(await content_hash(ed, entity), start, f"a refused call changes nothing: {mixed}")

    ops = [{"op": "dig", "shape": {"type": "sphere", "center": [12 + 10 * i, 32, 32], "radius": 3}, "blend": 0.5} for i in range(3)]
    ops.append({"op": "add", "material": 1, "shape": {"type": "box", "center": [32, 40, 16], "half_extents": [3, 2, 3], "rotation": {"euler": [0, 30, 0]}}})
    ops.append({"op": "smooth", "amount": 0.5, "iterations": 2, "shape": {"type": "capsule", "from": [10, 32, 32], "to": [34, 32, 32], "radius": 5}})

    dry = await ed.call("edit_voxels", entity=entity, ops=ops, dry_run=True, hash=True)
    ed.check(dry["dry_run"] and dry["voxels_changed"] > 0, f"{dry}")
    ed.check_eq(await content_hash(ed, entity), start, "dry_run changes nothing")
    steps = await history_size(ed, scene)
    applied = await ed.call("edit_voxels", entity=entity, ops=ops, hash=True)
    for key in ("voxels_changed", "surface_voxels_changed", "blocks_touched", "removed_volume", "added_volume", "clamped_voxels", "content_hash_after", "per_op"):
        ed.check_eq(applied[key], dry[key], f"dry_run and apply agree on {key}")
    ed.check_eq(len(applied["per_op"]), 5, "per op")
    ed.check(applied["removed_volume"] > 0 and applied["added_volume"] > 0, f"{applied}")
    ed.check(applied["changed_bounds"]["world"] and applied["changed_bounds"]["voxels"], f"{applied}")
    ed.check_eq(len(applied["removed_by_material"]), 16, "per slot")
    edited = await content_hash(ed, entity)
    ed.check(edited != start, "the edit changed the content")
    ed.check_eq(applied["content_hash_after"], edited, "the result's hash is sample_voxels'")
    # AC28: five ops, one step
    ed.check_eq(await history_size(ed, scene), steps + 1, "one call with 5 ops is one undo step")
    top = (await ed.call("get_undo_history", scene=scene, limit=1))["entries"][0]
    ed.check("Edits" in top["name"], f"named with the GameObject: {top}")
    await ed.call("undo", scope="scene", scene=scene)
    ed.check_eq(await content_hash(ed, entity), start, "undo is byte-exact")
    await ed.call("redo", scope="scene", scene=scene)
    ed.check_eq(await content_hash(ed, entity), edited, "redo is byte-exact")

    # Field shapes: a mesh GameObject and a stamp asset (R26)
    box = await ed.call("create_entity", name="Boulder", type="box", position=[32, 45, 48])
    await finished(ed, await ed.call("edit_voxels", entity=entity, ops=[{"op": "add", "shape": {"type": "mesh", "mesh": box["entity_id"], "scale": 3}}]))
    after_mesh = await content_hash(ed, entity)
    ed.check(after_mesh != edited, "a mesh shape added solid")
    made = await ed.call("edit_voxels", create={"name": "StampSource", "size": [16, 16, 16], "base": "solid_ground", "height": 6})
    await finished(ed, await ed.call("edit_voxels", entity=entity, ops=[{"op": "add", "shape": {"type": "stamp", "stamp": made["asset"], "center": [20, 50, 20]}}]))
    ed.check(await content_hash(ed, entity) != after_mesh, "a stamp added solid")

    # not_loaded names why
    await ed.call("set_properties", entity=entity, values={"voxelvolume": {"enabled": False}})
    await refused(ed, "edit_voxels", "not_loaded", entity=entity, ops=ops)
    await ed.call("set_properties", entity=entity, values={"voxelvolume": {"enabled": True}})

    # R68: over a second is a job, and R66: a job on the volume refuses edits
    big = await volume(ed, "Big", size=(512, 64, 512))
    heavy = [{"op": "smooth", "amount": 1, "iterations": 8, "shape": {"type": "box", "center": [256, 32, 256], "half_extents": [250, 20, 250]}}]
    started = await ed.call("edit_voxels", entity=big, ops=heavy)
    ed.check("job_id" in started and started["status"] == "running", f"a job: {started}")
    ed.check("get_job" in started["next"], f"next says get_job: {started}")
    busy = await refused(ed, "edit_voxels", "volume_busy", entity=big, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 3}}])
    ed.check("get_job" in busy["next"] and busy["job_id"] == started["job_id"], f"{busy}")
    done = await finished(ed, started)
    ed.check_eq(done["status"], "succeeded", f"{done}")
    ed.check("voxels_changed" in done["result"], f"{done}")
    await ed.call("undo", scope="scene", scene=scene)
    await ed.call("redo", scope="scene", scene=scene)
    again = await ed.call("edit_voxels", entity=big, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 3}}])
    ed.check("job_id" not in again, "free again, and a small edit is not a job")


async def test_sample(ed: Editor):
    """R69: every mode; hash stable; connectivity on a known tunnel and a sealed cavity; a large read is a job."""
    s = await setup(ed)
    entity = await volume(ed, "Samples", materials=[{"name": "Rock"}, {"name": "Dirt"}])
    await ed.call("edit_voxels", entity=entity, ops=[{"op": "paint", "material": 1, "weight": 1, "shape": {"type": "sphere", "center": [10, 31, 10], "radius": 4}}])

    got = (await ed.call("sample_voxels", entity=entity, mode="values", points=[[32, 20, 32], [32, 45, 32], [10, 31.5, 10]]))["values"]
    ed.check(got[0]["inside"] and got[0]["distance"] < 0 and got[0]["source"] == "base", f"{got[0]}")
    ed.check(not got[1]["inside"] and got[1]["distance"] > 0, f"{got[1]}")
    ed.check_eq(got[2]["slot_a"], 1, f"painted: {got[2]}")
    ed.check_eq(got[2]["source"], "modified", f"{got[2]}")
    await refused_points(ed, entity)

    # (-300, -300) is under no volume of this scene: every test's volumes start at the origin
    ground = (await ed.call("sample_voxels", entity=entity, mode="values", ground=True, points=[[32, 60, 32], [-300, 60, -300]]))["ground"]
    ed.check_eq(ground[0]["source"], "voxel", f"{ground[0]}")
    ed.check(abs(ground[0]["height"] - 32) < 0.1, f"the ground of a volume: {ground[0]}")
    ed.check_eq(ground[1]["source"], "none", f"{ground[1]}")

    stats = await ed.call("sample_voxels", entity=entity, mode="box", min=[16, 16, 16], max=[47, 47, 47])
    ed.check(0.3 < stats["solid_fraction"] < 0.7 and abs(stats["solid_fraction"] + stats["air_fraction"] - 1) < 1e-6, f"{stats}")
    ed.check(stats["surface_samples"] > 0 and stats["min_distance"] < 0 < stats["max_distance"], f"{stats}")
    ed.check_eq(stats["slots"], [0], f"{stats}")
    grid = await ed.call("sample_voxels", entity=entity, mode="box", min=[0, 0, 0], max=[32, 40, 32], stride=8)
    ed.check_eq(len(grid["values"]), grid["counts"][0] * grid["counts"][1] * grid["counts"][2], f"{grid['counts']}")
    too_many = await ed.call_raw("sample_voxels", {"entity": entity, "mode": "box", "min": [0, 0, 0], "max": [63, 63, 63], "stride": 1})
    ed.check(not too_many.is_error or "stride" in too_many.text, too_many.text[:200])
    await refused(ed, "sample_voxels", "out_of_bounds", entity=entity, mode="box", min=[900, 0, 0], max=[901, 1, 1])

    bounds = await ed.call("sample_voxels", entity=entity, mode="bounds")
    ed.check_eq(bounds["volume"]["voxels"]["max"], [63, 63, 63], f"{bounds}")
    ed.check(abs(bounds["solid"]["world"]["max"][1] - 32) <= 1, f"the solid ends at the height: {bounds['solid']}")
    ed.check(bounds["surface"]["voxels"], f"{bounds}")

    numbers = await ed.call("sample_voxels", entity=entity, mode="stats")
    ed.check(numbers["blocks"]["total"] > 0 and numbers["blocks"]["modified"] > 0, f"{numbers['blocks']}")
    ed.check(numbers["memory"]["total"] > 0, f"{numbers['memory']}")
    ed.check_eq(set(numbers["budgets"]), {"desktop", "mobile", "web"}, "per target")
    ed.check_eq(numbers["base_stale"], False, f"{numbers}")

    first = await ed.call("sample_voxels", entity=entity, mode="hash")
    ed.check_eq((await ed.call("sample_voxels", entity=entity, mode="hash"))["content_hash"], first["content_hash"], "stable")
    per = await ed.call("sample_voxels", entity=entity, mode="hash", per_block=True)
    ed.check_eq(per["content_hash"], first["content_hash"], "the same hash per block")
    ed.check(per["blocks"] and per["uniform_blocks"] > 0, f"{len(per['blocks'])} blocks")

    # a tunnel from the sky to a chamber; a sealed cavity beside it
    await ed.call("edit_voxels", entity=entity, ops=[
        {"op": "dig", "shape": {"type": "sphere", "center": [40, 14, 20], "radius": 5}},
        {"op": "dig", "shape": {"type": "capsule", "from": [40, 40, 20], "to": [40, 14, 20], "radius": 2.5}},
        {"op": "dig", "shape": {"type": "sphere", "center": [16, 14, 46], "radius": 4}},
    ])
    sky = [40, 50, 20]
    tunnel = await ed.call("sample_voxels", entity=entity, mode="connectivity", to=[40, 14, 20], **{"from": sky})
    ed.check(tunnel["connected"] and tunnel["from_in_air"] and tunnel["to_in_air"], f"{tunnel}")
    ed.check(tunnel["open_to_sky"], f"{tunnel}")
    sealed = await ed.call("sample_voxels", entity=entity, mode="connectivity", to=[16, 14, 46], **{"from": sky})
    ed.check(not sealed["connected"] and sealed["to_in_air"], f"the cavity is sealed: {sealed}")
    ed.check_eq(sealed["component_count"], 2, f"{sealed}")
    cavity = next(c for c in sealed["air_components"] if c["index"] == sealed["to_component"])
    ed.check(not cavity["open_to_sky"] and cavity["voxels"] > 100 and cavity["bounds"], f"{cavity}")
    in_solid = await ed.call("sample_voxels", entity=entity, mode="connectivity", to=[32, 5, 5], **{"from": sky})
    ed.check(not in_solid["connected"] and not in_solid["to_in_air"], f"{in_solid}")

    # R69: a large hash is a job and gives the same answer however it runs
    large = await volume(ed, "LargeRead", size=(512, 128, 512))
    started = await ed.call("sample_voxels", entity=large, mode="hash")
    ed.check("job_id" in started, f"unresolved blocks make it a job: {started}")
    one = await finished(ed, started)
    ed.check_eq(one["status"], "succeeded", f"{one}")
    two = await finished(ed, await ed.call("sample_voxels", entity=large, mode="hash"))
    ed.check_eq(two["result"]["content_hash"], one["result"]["content_hash"], "stable through jobs")
    joined = await finished(ed, await ed.call("sample_voxels", entity=large, mode="connectivity"))
    ed.check_eq(joined["status"], "succeeded", f"{joined}")
    ed.check_eq(joined["result"]["component_count"], 1, f"{joined}")


async def refused_points(ed: Editor, entity: int):
    result = await ed.call_raw("sample_voxels", {"entity": entity, "mode": "values", "points": [[0, 0, 0]] * 1025})
    ed.check(result.is_error and "1024" in result.text, result.text[:200])
    result = await ed.call_raw("sample_voxels", {"entity": entity, "mode": "values"})
    ed.check(result.is_error, "values needs points")


async def test_visualize(ed: Editor):
    """R70: a slice's bytes repeat and its numbers are real; every overlay returns a non-empty image."""
    s = await setup(ed)
    entity = await volume(ed, "Views")
    await ed.call("edit_voxels", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 8}}])
    one = await ed.call_raw("visualize_voxels", {"entity": entity, "mode": "slice", "axis": "z", "position": 32, "max_width": 256})
    two = await ed.call_raw("visualize_voxels", {"entity": entity, "mode": "slice", "axis": "z", "position": 32, "max_width": 256})
    ed.check(one.images and two.images, f"images: {one.text}")
    ed.check_eq(hashlib.sha256(one.images[0][0]).hexdigest(), hashlib.sha256(two.images[0][0]).hexdigest(), "the slice repeats byte for byte")
    data = one.value
    ed.check_eq(data["position_voxels"], 32, f"{data}")
    ed.check(0.2 < data["solid_fraction"] < 0.6 and data["iso_line_length"] > 10, f"{data}")
    other = await ed.call_raw("visualize_voxels", {"entity": entity, "mode": "slice", "axis": "y", "position": 10, "max_width": 256})
    ed.check(hashlib.sha256(other.images[0][0]).digest() != hashlib.sha256(one.images[0][0]).digest(), "another plane differs")
    # under the ground all is solid but a Closed volume's outer layer of air (R8): 62 of 64 voxels each way
    ed.check(abs(other.value["solid_fraction"] - (62 / 64) ** 2) < 1e-6, f"under the ground: {other.value}")

    for mode in ("wireframe", "lod", "materials", "blocks", "collision", "holes"):
        shot = await ed.call_raw("visualize_voxels", {"entity": entity, "mode": mode, "frame": True, "max_width": 256})
        ed.check(not shot.is_error and shot.images and len(shot.images[0][0]) > 100, f"{mode}: {shot.text[:300]}")
    off = await ed.call("visualize_voxels", entity=entity, mode="off")
    ed.check_eq(off["overlay"], "none", f"{off}")
    bad = await ed.call_raw("visualize_voxels", {"entity": entity, "mode": "xray"})
    ed.check(bad.is_error, "closed in the handler")


async def test_generate_job(ed: Editor):
    """R71: a job for every source, real numbers, one undo step, a cancel that leaves everything."""
    s = await setup(ed)
    scene = s["scene"]
    entity = await volume(ed, "Generated", materials=[{"name": "Rock"}])
    start = await content_hash(ed, entity)
    steps = await history_size(ed, scene)

    started = await ed.call("generate_voxels", entity=entity, source="preset", preset="empty")
    ed.check("job_id" in started and "get_job" in started["next"], f"a job: {started}")
    done = await finished(ed, started)
    ed.check_eq(done["status"], "succeeded", f"{done}")
    for key in ("blocks_non_uniform", "memory", "triangles_estimate", "content_hash", "base_changed", "non_watertight"):
        ed.check(key in done["result"], f"{key} in {done['result']}")
    ed.check(done["result"]["base_changed"], f"{done['result']}")
    ed.check_eq(await history_size(ed, scene), steps + 1, "one undo step")
    ed.check(await content_hash(ed, entity) != start, "the base changed")
    await ed.call("undo", scope="scene", scene=scene)
    ed.check_eq(await content_hash(ed, entity), start, "undo restores the old base")
    await ed.call("redo", scope="scene", scene=scene)
    ed.check_eq(await content_hash(ed, entity), done["result"]["content_hash"], "redo gives the result's hash")
    await ed.call("undo", scope="scene", scene=scene)

    steps = await history_size(ed, scene)
    estimate = await finished(ed, await ed.call("generate_voxels", entity=entity, source="preset", preset="solid_ground", dry_run=True))
    ed.check(estimate["result"]["estimate"], f"says it is an estimate: {estimate}")
    ed.check_eq(await content_hash(ed, entity), start, "a dry run changes nothing")
    ed.check_eq(await history_size(ed, scene), steps, "a dry run makes no undo step")

    # keep_edits keeps the block an edit made
    await ed.call("edit_voxels", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [16, 32, 16], "radius": 5}}])
    await finished(ed, await ed.call("generate_voxels", entity=entity, source="preset", preset="solid_ground", keep_edits=True))
    kept = (await ed.call("sample_voxels", entity=entity, mode="values", points=[[16, 32, 16]]))["values"][0]
    ed.check(not kept["inside"] and kept["source"] == "modified", f"the dig stays: {kept}")

    # From a terrain, a mesh
    terrain = await ed.call("create_entity", name="Ground", type="terrain")
    await ed.call("set_properties", entity=terrain["entity_id"], values={"terrain": {"terrainSize": 128, "maxHeight": 20}})
    from_terrain = await finished(ed, await ed.call("generate_voxels", entity=entity, source="terrain", terrain=terrain["entity_id"], keep_edits=False))
    ed.check_eq(from_terrain["status"], "succeeded", f"{from_terrain}")
    box = await ed.call("create_entity", name="MeshSource", type="box", position=[32, 32, 32], scale=[20, 20, 20])
    from_mesh = await finished(ed, await ed.call("generate_voxels", entity=entity, source="mesh", mesh=box["entity_id"], keep_edits=False))
    ed.check_eq(from_mesh["status"], "succeeded", f"{from_mesh}")
    await refused(ed, "generate_voxels", "missing_asset", entity=entity, source="noise_graph", graph="graphs/none.noisegraph")

    # From a Noise Graph, whose later change is reported and applies nothing by itself (R33, V10)
    graphs = ed.project / "graphs"
    graphs.mkdir(exist_ok=True)
    graph = graphs / "Ground.noisegraph"
    graph.write_text(height_graph(20.0))
    await ed.call("sync_files")
    from_graph = await finished(ed, await ed.call("generate_voxels", entity=entity, source="noise_graph", graph="graphs/Ground.noisegraph", keep_edits=False))
    ed.check_eq(from_graph["status"], "succeeded", f"{from_graph}")
    surface = (await ed.call("sample_voxels", entity=entity, mode="values", points=[[32, 10, 32], [32, 30, 32]]))["values"]
    ed.check(surface[0]["inside"] and not surface[1]["inside"], f"the graph's height: {surface}")
    ed.check_eq((await ed.call("sample_voxels", entity=entity, mode="stats"))["base_stale"], False, "just generated")
    held = await content_hash(ed, entity)
    graph.write_text(height_graph(25.0))
    await ed.call("sync_files")
    ed.check_eq((await ed.call("sample_voxels", entity=entity, mode="stats"))["base_stale"], True, "the graph changed")
    ed.check_eq(await content_hash(ed, entity), held, "a stale base changes nothing by itself")
    await refused(ed, "generate_voxels", "invalid_source", entity=entity, source="magic")

    # Cancel: everything as it was, and the volume is free again
    big = await volume(ed, "BigGenerated", size=(512, 128, 512))
    before = await content_hash(ed, big)
    # overhangs: a Noise Graph over 512 x 128 x 512 takes seconds, solid ground is done before the next call
    long = await ed.call("generate_voxels", entity=big, source="preset", preset="overhangs", params={"size_scale": 0.5}, keep_edits=False)
    busy = await refused(ed, "edit_voxels", "volume_busy", entity=big, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 3}}])
    ed.check("get_job" in busy["next"], f"{busy}")
    await ed.call("cancel_job", job_id=long["job_id"])
    cancelled = await finished(ed, long)
    ed.check_eq(cancelled["status"], "cancelled", f"{cancelled}")
    ed.check_eq(await content_hash(ed, big), before, "a cancel leaves everything as it was")
    await ed.call("edit_voxels", entity=big, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 3}}])


async def test_edit_in_play_leaves_edit_mode(ed: Editor):
    """R66, R6: an edit in Play says play_only, reaches only the running copy and adds no undo step."""
    s = await setup(ed)
    scene = s["scene"]
    entity = await volume(ed, "Played", size=(256, 64, 256))
    await ed.call("save_scene", scene=scene)
    start = await content_hash(ed, entity)
    files = {p: p.read_bytes() for p in ed.project.rglob("*.voxelvolume")}
    steps = await history_size(ed, scene)
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=2)
        dug = await ed.call("edit_voxels", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 4}}])
        ed.check_eq(dug["play_only"], True, f"{dug}")
        ed.check(dug["voxels_changed"] > 0, f"{dug}")
        running = await content_hash(ed, entity)
        ed.check(running != start, "sample_voxels reads the running copy")
        ed.check_eq(await history_size(ed, scene), steps, "no undo step in Play")
        # R30: a Play edit has a budget per call
        await refused(ed, "edit_voxels", "budget_exceeded", entity=entity, ops=[{"op": "dig", "shape": {"type": "box", "center": [128, 32, 128], "half_extents": [120, 30, 120]}}])
        # R66: creating and generating wait for Stop
        refused_create = await ed.call_raw("edit_voxels", {"create": {"size": [16, 16, 16]}})
        ed.check(refused_create.is_error, refused_create.text[:200])
        refused_generate = await ed.call_raw("generate_voxels", {"entity": entity, "source": "preset", "preset": "empty"})
        ed.check(refused_generate.is_error, refused_generate.text[:200])
        slice_in_play = await ed.call_raw("visualize_voxels", {"entity": entity, "mode": "slice", "max_width": 64})
        ed.check(slice_in_play.images, "visualize_voxels works in Play")
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(await content_hash(ed, entity), start, "Stop leaves edit mode's content")
    await ed.call("save_scene", scene=scene)
    for path, bytes_before in files.items():
        ed.check(path.read_bytes() == bytes_before, f"{path.name} did not change on disk")


async def test_guide_served(ed: Editor):
    """R72: jasper://docs/voxels is served, and the agent guide's index leads to it."""
    await setup(ed)
    got = await ed.session.read_resource("jasper://docs/voxels")
    text = "".join(c.text for c in got.contents)
    for needle in ("edit_voxels", "sample_voxels", "visualize_voxels", "generate_voxels", "dry_run", "volume_busy", "entrance_narrow", "budget_exceeded", "connectivity"):
        ed.check(needle in text, f"the doc names {needle}")
    guide = await ed.session.read_resource("jasper://guide")
    guide_text = "".join(c.text for c in guide.contents)
    ed.check("voxels (edit_voxels)" in guide_text, "the Other areas index has the entry")
    ed.check("voxels" in guide_text.split("## Tool groups")[-1], "the group is named")


async def test_presets(ed: Editor):
    """AC30: each preset twice with one seed gives one hash; cave_entrance connects the sky and its
    chamber; caves lists components; overhangs has air under solid in some column."""
    s = await setup(ed)
    entity = await volume(ed, "Presets", size=(128, 64, 128))
    terrain = await ed.call("create_entity", name="PresetGround", type="terrain")
    await ed.call("set_properties", entity=terrain["entity_id"], values={"terrain": {"terrainSize": 256, "maxHeight": 20}})
    hashes = {}
    for preset in ("empty", "solid_ground", "caves", "overhangs", "cave_entrance"):
        args = {"entity": entity, "source": "preset", "preset": preset, "seed": 7, "keep_edits": False}
        if preset == "cave_entrance":
            args["terrain"] = terrain["entity_id"]
        one = await finished(ed, await ed.call("generate_voxels", **args))
        ed.check_eq(one["status"], "succeeded", f"{preset}: {one}")
        two = await finished(ed, await ed.call("generate_voxels", **args))
        ed.check_eq(two["result"]["content_hash"], one["result"]["content_hash"], f"{preset}: deterministic by seed")
        hashes[preset] = one["result"]["content_hash"]
        if preset == "caves":
            comps = await finished(ed, await ed.call("sample_voxels", entity=entity, mode="connectivity"))
            comps = comps.get("result", comps)
            ed.check(comps["component_count"] >= 1 and comps["air_components"], f"{comps}")
        if preset == "overhangs":
            # the band around the surface at stride 2: a result over 1 MB loses its structured content
            grid = await finished(ed, await ed.call("sample_voxels", entity=entity, mode="box", min=[0, 20, 0], max=[126, 44, 126], stride=2))
            grid = grid.get("result", grid)
            nx, ny, nz = grid["counts"]
            values = grid["values"]
            found = False
            for z in range(nz):
                for x in range(nx):
                    column = [values[x + nx * (y + ny * z)] for y in range(ny)]
                    # air with solid above it: an overhang
                    found = found or any(column[y] > 0 and any(v < 0 for v in column[y + 1:]) for y in range(1, ny - 1))
            ed.check(found, "overhangs has air under solid in a column")
        if preset == "cave_entrance":
            bounds = await finished(ed, await ed.call("sample_voxels", entity=entity, mode="bounds"))
            bounds = bounds.get("result", bounds)
            top = bounds["volume"]["world"]["max"]
            joined = await finished(ed, await ed.call("sample_voxels", entity=entity, mode="connectivity"))
            joined = joined.get("result", joined)
            chamber = next((c for c in joined["air_components"] if c["open_to_sky"]), None)
            ed.check(chamber is not None, f"cave_entrance is open to the sky: {joined}")
            # The chamber lies under the surface, and its component is the sky's
            low = chamber["bounds"]["min"]
            sky = [low[0] + 1, top[1] - 1, low[2] + 1]
            reach = await finished(ed, await ed.call("sample_voxels", entity=entity, mode="connectivity", to=[low[0] + 1, low[1] + 1, low[2] + 1], **{"from": sky}))
            reach = reach.get("result", reach)
            ed.check_eq(reach["connected"], True, f"from the sky to the chamber: {reach}")
    ed.check(len(set(hashes.values())) == len(hashes), f"each preset is its own: {hashes}")


async def test_scene_bytes_stable(ed: Editor):
    """AC18: a scene with a volume saved, reloaded and saved again is byte-identical, holds no chunk, and
    the hierarchy shows none."""
    await setup(ed)
    made = await ed.call("create_scene", name="Stable", type="3D", path="scenes/Stable")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    created = await ed.call("edit_voxels", create={"name": "StableVolume", "size": [64, 64, 64], "base": "solid_ground"},
                            ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 32, 32], "radius": 6}}])
    entity = created["entity"]["id"]
    hash_before = await content_hash(ed, entity)
    await ed.call("wait", frames=10)
    await ed.call("save_scene", scene=scene)
    path = ed.project / "scenes/Stable.scene"
    first = path.read_bytes()
    text = first.decode()
    ed.check("voxelvolume" in text.lower(), "the scene holds the component")
    ed.check(text.count("- type: Entity") <= 4 and "chunk" not in text.lower(), "no chunk in the scene")
    tree = await ed.call("get_scene_tree", scene=scene) if ed.has_tool("get_scene_tree") else None
    if tree is not None:
        ed.check("chunk" not in str(tree).lower(), "no chunk in the hierarchy")
    await ed.call("close_scene", scene=scene)
    opened = await ed.call("open_scene", scene="scenes/Stable.scene")
    scene = find_key(opened, "id")
    await ed.call("wait", frames=10)
    await ed.call("save_scene", scene=scene)
    ed.check(path.read_bytes() == first, "saved, reloaded and saved again: byte-identical")
    ed.check_eq(await content_hash(ed, "StableVolume"), hash_before, "the content came back")


async def test_navigation_dirty_after_edit(ed: Editor):
    """AC24: a dig under a baked NavMesh Surface dirties its tiles."""
    s = await setup(ed)
    made = await ed.call("create_scene", name="Nav", type="3D", path="scenes/NavVoxels")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    created = await ed.call("edit_voxels", create={"name": "NavGround", "size": [64, 32, 64], "base": "solid_ground", "height": 16})
    entity = created["entity"]["id"]
    await ed.call("add_component", entity=entity, component="navmeshsurface")
    await ed.call("wait", frames=5)
    baked = await ed.call("bake_navigation", scene=scene, mode="full", timeout_seconds=300)
    ed.check(baked["polys"] > 0, f"the ground is walkable: {baked}")
    clean = (await ed.call("get_navigation_info", scene=scene))["surfaces"][0]
    ed.check_eq(clean["dirty_tiles"], 0, f"{clean}")
    await ed.call("edit_voxels", entity=entity, ops=[{"op": "dig", "shape": {"type": "sphere", "center": [32, 16, 32], "radius": 6}}])
    # the editor counts dirty tiles at most once a second
    deadline = time.monotonic() + 30
    while True:
        dirty = (await ed.call("get_navigation_info", scene=scene))["surfaces"][0]
        if dirty["dirty_tiles"] > 0 or time.monotonic() > deadline:
            break
        await asyncio.sleep(0.25)
    ed.check(dirty["dirty_tiles"] > 0, f"the dig dirties tiles: {dirty}")
    await ed.call("select_scene", scene=s["scene"])


async def test_budgets_cave_world(ed: Editor):
    """AC40, R57: reference content (256 x 128 x 256 at 1 m, Cave Entrance over a terrain) from three
    cameras: chunks, triangles and memory per tier within budget. Counts, not milliseconds."""
    await setup(ed)
    made = await ed.call("create_scene", name="CaveWorld", type="3D", path="scenes/CaveWorld")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    terrain = await ed.call("create_entity", name="WorldGround", type="terrain")
    await ed.call("set_properties", entity=terrain["entity_id"], values={"terrain": {"terrainSize": 512, "maxHeight": 40}})
    await ed.call("set_terrain_maps", entity=terrain["entity_id"], heightmap={"generate": "fractal_noise", "resolution": 257, "seed": 3, "amplitude": 0.25})
    created = await ed.call("edit_voxels", create={"name": "World", "size": [256, 128, 256], "position": [-128, 0, -128], "terrain": terrain["entity_id"]})
    entity = created["entity"]["id"]
    # its From Terrain base is a job of its own
    ed.check("job_id" in created, f"a terrain base is generated: {created}")
    ed.check_eq((await finished(ed, created))["status"], "succeeded", f"{created}")
    done = await finished(ed, await ed.call("generate_voxels", entity=entity, source="preset", preset="cave_entrance", seed=1,
                                            terrain=terrain["entity_id"], keep_edits=False))
    ed.check_eq(done["status"], "succeeded", f"{done}")
    camera = await ed.call("create_entity", name="Probe", type="camera")
    cameras = [[0, 60, 0], [-120, 60, -120], [120, 30, 0]]
    for tier, (max_chunks, max_triangles) in GPU_BUDGETS.items():
        await ed.call("set_properties", entity=entity, values={"voxelvolume": {"quality": tier.lower()}})
        for position in cameras:
            await ed.call("set_transform", entity=camera["entity_id"], position=position)
            deadline = time.monotonic() + 300
            stats = None
            while time.monotonic() < deadline:
                await ed.call("wait", frames=5)
                stats = await ed.call("sample_voxels", entity=entity, mode="stats")
                if stats["jobs_pending"] == 0 and stats["chunks"] > 0:
                    break
            print(f"     [bench] voxels {tier} camera {position}: {stats['chunks']} chunks, {stats['triangles']} triangles, "
                  f"cpu {stats['memory']['total']} B, mesh {stats['memory']['mesh']} B")
            ed.check(stats["chunks"] <= max_chunks, f"{tier} {position}: {stats['chunks']} chunks over {max_chunks}")
            ed.check(stats["triangles"] <= max_triangles, f"{tier} {position}: {stats['triangles']} triangles over {max_triangles}")
            ed.check(stats["budgets"]["desktop"]["mesh_over"] is False and stats["budgets"]["desktop"]["cpu_over"] is False, f"{stats['budgets']}")
