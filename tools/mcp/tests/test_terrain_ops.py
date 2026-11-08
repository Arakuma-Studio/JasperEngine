"""edit_terrain, sample_terrain's numbers and the terrain guide (T1 R68-R70, R73, R95; AC54, AC55, AC58, AC66, AC75).

One terrain per test, made with edit_terrain's smallest preset so each call stays a few milliseconds.
"""

import asyncio
import re

from harness import Editor

EDITOR = {"create": True, "name": "Terrain Ops"}
CI = True

CIRCLE = {"type": "circle", "center": [0, 0], "radius": 8}


async def new_terrain(ed: Editor, name: str):
    sid = (await ed.call("create_scene", name=name, type="3d"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=sid, name=name, type="terrain"))["entity_id"]
    await ed.call("edit_terrain", scene=sid, entity=terrain, create="test_patch", map_resolution=65)
    return sid, terrain


async def hashes(ed: Editor, sid, terrain) -> dict:
    return (await ed.call("sample_terrain", scene=sid, entity=terrain, hash=True))["hashes"]


async def test_create_preset(ed: Editor):
    sid, terrain = await new_terrain(ed, "Preset")
    fields = await ed.call("get_entity", scene=sid, entity=terrain, components=["terrain"], properties=["terrainSize", "maxHeight"])
    values = fields["components"]["terrain"]
    ed.check_eq(values["terrainSize"], 64.0, "test_patch is 64 m")
    ed.check_eq(values["maxHeight"], 16.0, "test_patch is 16 m high")
    sampled = await ed.call("sample_terrain", scene=sid, entity=terrain, stats=["height"])
    ed.check_eq(sampled["stats"]["height"]["max"], 0.0, "a new terrain is flat at 0")
    await ed.check_error("edit_terrain", scene=sid, entity=terrain, create="small", contains="already has a height map")


async def test_result_shape(ed: Editor):
    sid, terrain = await new_terrain(ed, "Result")
    out = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "delta": 4, "shape": CIRCLE}])
    ed.check_eq(out["dry_run"], False)
    ed.check_eq([c["map"] for c in out["changed"]], ["height"])
    change = out["changed"][0]
    ed.check(change["texels"] > 0 and len(change["rect"]) == 4, f"a texel rectangle and count: {change}")
    ed.check(-10 <= change["local"]["min"][0] < -6 and 6 < change["local"]["max"][0] <= 10, f"the circle's box: {change['local']}")
    op = out["ops"][0]
    for key in ("op", "changed_texels", "clipped_low", "clipped_high", "filtered_out", "outside"):
        ed.check(key in op, f"ops[0] has {key}: {op}")
    ed.check_eq(op["changed_texels"], change["texels"])
    ed.check(abs(out["stats"]["max_delta"] - 4.0) < 0.01, f"a 4 m raise: {out['stats']}")
    ed.check("next" in out, "the result says what to call next")


async def test_dry_run_changes_nothing(ed: Editor):
    sid, terrain = await new_terrain(ed, "Dry")
    ops = [{"op": "add_noise", "amplitude": 3, "noise_size": 6, "seed": 11}, {"op": "smooth", "shape": CIRCLE}]
    before = await hashes(ed, sid, terrain)
    async def step_ids():
        return [entry["id"] for entry in (await ed.call("get_undo_history", scene=sid, scope="scene"))["entries"]]
    history = await step_ids()
    dry = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=ops, dry_run=True)
    ed.check_eq(await hashes(ed, sid, terrain), before, "a dry run writes nothing")
    ed.check_eq(await step_ids(), history, "and adds no undo step")
    real = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=ops)
    ed.check_eq(dry["changed"], real["changed"], "the same changed maps")
    ed.check_eq(dry["ops"], real["ops"], "the same per-op numbers")
    ed.check(await hashes(ed, sid, terrain) != before, "the real run wrote")


async def test_all_or_nothing(ed: Editor):
    sid, terrain = await new_terrain(ed, "Atomic")
    before = await hashes(ed, sid, terrain)
    message = await ed.check_error("edit_terrain", scene=sid, entity=terrain,
                                   ops=[{"op": "raise", "delta": 2}, {"op": "lower", "delta": 1, "shape": {"type": "circle", "radius": 2, "centre": [0, 0]}}])
    ed.check("ops[1].shape.centre" in message, f"the error names the field: {message}")
    ed.check_eq(await hashes(ed, sid, terrain), before, "the first op did not run either")


async def test_unknown_op_and_key_name_the_valid_ones(ed: Editor):
    sid, terrain = await new_terrain(ed, "Names")
    message = await ed.check_error("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "flatten"}])
    ed.check("set_height" in message and "raise" in message, f"the valid ops are listed: {message}")
    message = await ed.check_error("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "amount": 2}])
    ed.check("ops[0].amount" in message and "jasper://docs/terrain" in message, f"the field and where the fields are: {message}")
    message = await ed.check_error("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "place_objects", "asset": "a.obj"}])
    ed.check("sculpt_terrain" in message, f"placing GameObjects points to sculpt_terrain: {message}")


async def test_outside_shape_fails_with_bounds(ed: Editor):
    sid, terrain = await new_terrain(ed, "Outside")
    message = await ed.check_error("edit_terrain", scene=sid, entity=terrain,
                                   ops=[{"op": "raise", "delta": 1, "shape": {"type": "circle", "center": [500, 500], "radius": 4}}])
    ed.check("ops[0]" in message, f"the op is named: {message}")


async def test_one_undo_per_call(ed: Editor):
    sid, terrain = await new_terrain(ed, "Undo")
    before = await hashes(ed, sid, terrain)
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[
        {"op": "raise", "delta": 3, "shape": CIRCLE},
        {"op": "paint_layer", "layer": 1, "shape": CIRCLE},
        {"op": "cut_holes", "shape": {"type": "circle", "center": [0, 0], "radius": 2}}])
    after = await hashes(ed, sid, terrain)
    ed.check(after["height"] != before["height"] and "hole" in after and "blend:0" in after, f"three maps changed: {after}")
    await ed.call("undo", scene=sid)
    ed.check_eq(await hashes(ed, sid, terrain), before, "one undo takes the whole call back, made maps included")
    await ed.call("redo", scene=sid)
    ed.check_eq(await hashes(ed, sid, terrain), after, "and redo gives it again")


async def test_next_names_the_fix(ed: Editor):
    sid, terrain = await new_terrain(ed, "Clipped")
    out = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "set_height", "height": 40}])
    ed.check(any(w["code"] == "clipped_high" for w in out["warnings"]), f"40 m on a 16 m terrain clips: {out['warnings']}")
    ed.check("maxHeight" in out["next"], f"next names the fix: {out['next']}")


async def test_sample_stats_and_issues(ed: Editor):
    sid, terrain = await new_terrain(ed, "Stats")
    # the left half at 4 m, the right half at 12 m
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[
        {"op": "set_height", "height": 4},
        {"op": "set_height", "height": 12, "shape": {"type": "rect", "min": [0, -32], "max": [32, 32]}, "strength": 1}])
    stats = (await ed.call("sample_terrain", scene=sid, entity=terrain, stats=["height", "slope", "layers"]))["stats"]
    height = stats["height"]
    ed.check(abs(height["min"] - 4.0) < 0.01 and abs(height["max"] - 12.0) < 0.01, f"the two levels: {height}")
    ed.check(abs(height["p05"] - 4.0) < 0.01 and abs(height["p95"] - 12.0) < 0.01, f"percentiles: {height}")
    shares = stats["slope"]["shares"]
    ed.check(abs(sum(shares) - 1.0) < 1e-6, f"the slope histogram sums to 1: {shares}")
    ed.check(shares[0] > 0.9, f"most of it is flat: {shares}")
    region = (await ed.call("sample_terrain", scene=sid, entity=terrain, stats=["height"], region={"min": [-30, -30], "max": [-2, 30]}))["stats"]
    ed.check(abs(region["height"]["max"] - 4.0) < 0.01, f"the region sees the left half only: {region['height']}")
    issues = (await ed.call("sample_terrain", scene=sid, entity=terrain, issues=True))["issues"]
    ed.check(isinstance(issues, list), f"issues is a list: {issues}")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "set_height", "height": 30}])
    issues = (await ed.call("sample_terrain", scene=sid, entity=terrain, issues=True))["issues"]
    ed.check(any(i["code"] == "height_clipped_high" and i.get("fix") for i in issues), f"a clipped map is an issue with a fix: {issues}")


async def test_hash_tracks_content(ed: Editor):
    sid, terrain = await new_terrain(ed, "Hash")
    first = await hashes(ed, sid, terrain)
    ed.check_eq(await hashes(ed, sid, terrain), first, "a read does not change it")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "delta": 1, "shape": CIRCLE}])
    raised = await hashes(ed, sid, terrain)
    ed.check(raised["height"] != first["height"], "an edit does")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "lower", "delta": 1, "shape": CIRCLE}])
    ed.check_eq((await hashes(ed, sid, terrain))["height"], first["height"], "the same bytes give the same hash")


async def test_pain_points(ed: Editor):
    sid, terrain = await new_terrain(ed, "Pain")
    # outside the terrain: a query clamps to the edge and says so, never NaN
    sample = (await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[1000, 1000]]))["samples"][0]
    ed.check(sample["height"] == sample["height"], f"no NaN outside: {sample}")
    # a paint stroke leaves the height map's bytes alone
    before = (await hashes(ed, sid, terrain))["height"]
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "paint_layer", "layer": 1, "shape": CIRCLE}])
    ed.check_eq((await hashes(ed, sid, terrain))["height"], before, "painting does not touch the height map")


async def test_guide_is_served(ed: Editor):
    ed.check("jasper://docs/terrain" in ed.tool("edit_terrain").description, "the description points to the guide")
    got = await ed.session.read_resource("jasper://docs/terrain")
    guide = "".join(getattr(c, "text", "") or "" for c in got.contents)
    # a guide past the split size serves its intro and an index of sections to read one by one
    for uri in re.findall(r"`(jasper://docs/terrain/[^`]+)`", guide):
        part = await ed.session.read_resource(uri)
        guide += "".join(getattr(c, "text", "") or "" for c in part.contents)
    ed.check("import_heightmap" in guide and "paint_by_rules" in guide, f"the guide has the ops: {guide[:400]}")


async def test_refused_while_playing(ed: Editor):
    sid, terrain = await new_terrain(ed, "Playing")
    path = "scenes/Playing.scene"
    await ed.call("save_scene", scene=sid, path=path)
    await ed.call("play", action="start", scene=sid, auto_pause=True)
    try:
        await ed.check_error("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "delta": 1}], contains="Stop Play")
    finally:
        await ed.call("play", action="stop")


async def test_navmesh_goes_stale(ed: Editor):
    """AC67 (R80): an edit leaves the NavMesh as baked; sample_terrain says so until the next bake."""
    sid, terrain = await new_terrain(ed, "Nav")
    await ed.call("add_component", scene=sid, entity=terrain, component="navmeshsurface")
    await ed.call("bake_navigation", scene=sid, mode="full")
    issues = (await ed.call("sample_terrain", scene=sid, entity=terrain, issues=True))["issues"]
    ed.check(not any(i["code"] == "navmesh_stale" for i in issues), f"fresh after the bake: {issues}")
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "delta": 4, "shape": CIRCLE}])
    issues = (await ed.call("sample_terrain", scene=sid, entity=terrain, issues=True))["issues"]
    ed.check(any(i["code"] == "navmesh_stale" for i in issues), f"the edit made it stale: {issues}")
    await ed.call("bake_navigation", scene=sid, mode="dirty")
    issues = (await ed.call("sample_terrain", scene=sid, entity=terrain, issues=True))["issues"]
    ed.check(not any(i["code"] == "navmesh_stale" for i in issues), f"a bake clears it: {issues}")


async def big_terrain(ed: Editor, name: str):
    sid = (await ed.call("create_scene", name=name, type="3d"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=sid, name=name, type="terrain"))["entity_id"]
    # 2049 squared is past the 4 M texels of R69
    await ed.call("edit_terrain", scene=sid, entity=terrain, create="test_patch", map_resolution=2049)
    return sid, terrain


async def wait_job(ed: Editor, job_id: str) -> dict:
    # polled from outside with no frame asked for: the result must reach an idle editor (a hand-back
    # through jobs::postToMain waits for a frame that draws a scene, which an idle editor never has)
    for _ in range(600):
        job = await ed.call("get_job", job_id=job_id)
        if job["status"] != "running":
            return job
        await asyncio.sleep(0.1)
    raise AssertionError(f"job {job_id} never finished")


async def test_large_edit_is_a_job(ed: Editor):
    """AC54 (R69): past 4 M texels the edit runs as a job on a copy and lands as one undo step."""
    sid, terrain = await big_terrain(ed, "Big")
    before = await hashes(ed, sid, terrain)
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "delta": 2}], wait=False)
    ed.check("job_id" in started and started["status"] in ("running", "succeeded"), f"a job: {started}")
    job = await wait_job(ed, started["job_id"])
    ed.check_eq(job["status"], "succeeded", f"the job: {job}")
    ed.check(job["result"]["changed"][0]["texels"] > 4_000_000, f"the whole map changed: {job['result']['changed']}")
    after = await hashes(ed, sid, terrain)
    ed.check(after["height"] != before["height"], "the job's edit reached the map")
    await ed.call("undo", scene=sid)
    ed.check_eq((await hashes(ed, sid, terrain))["height"], before["height"], "one undo takes it back")


async def test_job_conflict_fails(ed: Editor):
    """AC54 (R69): a second call on a terrain a job is editing fails, naming get_job."""
    sid, terrain = await big_terrain(ed, "Busy")
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "smooth", "iterations": 4}], wait=False)
    ed.check_eq(started["status"], "running", "four smoothing passes over 4 M texels outlast the call")
    await ed.check_error("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "raise", "delta": 1}], contains="get_job")
    await wait_job(ed, started["job_id"])
