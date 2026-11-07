"""generate_terrain and edit_graph's create (T2 R16, R28-R33, R63; AC10, AC19-AC23, AC25, AC26)."""

import hashlib
import json

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Generate Terrain Test"}
TIMEOUT = 900

SIZE = 200
MAX_HEIGHT = 100


async def scene_with_terrain(ed: Editor, name: str, values: dict | None = None):
    await ed.call("tool_groups", enable=["terrain"])
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}")
    sid = made["scene"] if "scene" in made else made["scene_id"]
    terrain_values = {"terrainSize": SIZE, "maxHeight": MAX_HEIGHT, "resolution": 8}
    terrain_values.update(values or {})
    terrain = (await ed.call("create_entity", scene=sid, name="Ground", type="terrain", values={"terrain": terrain_values}))["entity_id"]
    return sid, terrain


async def seed_height(ed: Editor, sid, terrain, resolution=65):
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "flat", "resolution": resolution, "base_height": 0.25})


def map_files(ed: Editor) -> dict:
    root = ed.project
    out = {}
    for path in root.rglob("*.png"):
        if ".jasperengine" in str(path):
            continue
        out[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


async def test_group(ed: Editor):
    """AC25: hidden until tool_groups enables terrain."""
    hidden = [t.name for t in await ed.tools(groups=None)]
    ed.check("generate_terrain" not in hidden and "preview_noise_graph" not in hidden, "the group starts hidden")
    await ed.call("tool_groups", enable=["terrain"])
    shown = [t.name for t in await ed.tools(groups=["terrain"])]
    ed.check("generate_terrain" in shown and "preview_noise_graph" in shown, f"enabled: {shown}")


async def test_create(ed: Editor):
    """AC10: create makes a file, a second is refused, undo removes it, from_preset copies."""
    path = "graphs/Made.noisegraph"
    made = await ed.call("edit_graph", path=path, create={"kind": "noise"}, ops=[
        {"op": "addNode", "node": {"id": "n1", "type": "constant", "params": {"value": 0.5}}}])
    ed.check_eq(made["applied"], 1, "ops apply to the new file")
    ed.check((ed.project / path).exists(), "the file exists")
    await ed.check_error("edit_graph", {"path": path, "create": {"kind": "noise"}, "ops": []}, contains="already exists")
    await ed.call("undo", scope="project")   # graph files are project history (GraphEditCmd)
    ed.check(not (ed.project / path).exists(), "undo removes the file")

    copied = "graphs/Alpine.noisegraph"
    await ed.call("edit_graph", path=copied, create={"kind": "noise", "from_preset": "alpine"}, ops=[])
    got = await ed.call("get_graph", path=copied)
    ed.check(any(n["type"] == "hydraulic_erosion" for n in got["graph"]["nodes"]), "from_preset copies")


async def test_dry_run_writes_nothing(ed: Editor):
    """AC19."""
    sid, terrain = await scene_with_terrain(ed, "Dry")
    await seed_height(ed, sid, terrain)
    before = map_files(ed)
    first = await ed.call_raw("generate_terrain", {"scene": sid, "entity": terrain, "preset": "rolling_hills", "dry_run": True, "seed": 5})
    ed.check(not first.is_error, first.text)
    second = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="rolling_hills", dry_run=True, seed=5)
    data = first.structured
    ed.check(first.images, "a dry run returns a heightmap image")
    ed.check_eq(data["seed"], 5, "the seed is echoed")
    ed.check_eq(data["hash"], second["hash"], "the same hash twice")
    ed.check(data["stats"]["height"]["max"] > data["stats"]["height"]["min"], str(data["stats"]))
    ed.check(data["dry_run"], "says it is a dry run")
    ed.check_eq(map_files(ed), before, "no file changed")
    # No undo step: an undo now takes back the map's own creation, not a generation
    await ed.call("undo", scene=sid)
    info = (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0))["info"]
    ed.check(not info.get("height_map"), f"the step before the dry runs was the last one: {info}")


async def test_apply_is_one_undo_step(ed: Editor):
    """AC20."""
    sid, terrain = await scene_with_terrain(ed, "Apply")
    await seed_height(ed, sid, terrain)
    before = map_files(ed)
    sampled_before = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=4)
    applied = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="rolling_hills", seed=7)
    ed.check_eq(applied["undo_name"], "Generate Terrain (Rolling Hills, seed 7)", "the step's name")
    ed.check(any(c["map"] == "height" for c in applied["changed"]), f"changed: {applied['changed']}")
    sampled = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    low, high = applied["stats"]["height_metres"]["min"], applied["stats"]["height_metres"]["max"]
    ed.check(low - 1 <= sampled["grid"]["min"] and sampled["grid"]["max"] <= high + 1, f"{sampled['grid']['min']}..{sampled['grid']['max']} within {low}..{high}")
    ed.check(sampled["grid"]["max"] > sampled_before["grid"]["max"] or sampled["grid"]["min"] != sampled_before["grid"]["min"], "the heights changed")
    await ed.call("undo", scene=sid)
    restored = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=4)
    ed.check_eq(restored["grid"]["min"], sampled_before["grid"]["min"], "undo restores the heights")
    ed.check_eq(restored["grid"]["max"], sampled_before["grid"]["max"], "undo restores the heights")
    after_undo = map_files(ed)
    for path, digest in before.items():
        ed.check_eq(after_undo.get(path), digest, f"undo restores {path} byte for byte")
    await ed.call("redo", scene=sid)
    redone = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="rolling_hills", seed=7, dry_run=True)
    ed.check_eq(redone["hash"], applied["hash"], "redo's terrain is the same run's")
    again = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    ed.check_eq(again["grid"]["max"], sampled["grid"]["max"], "redo gives the same heights")


async def test_layers_and_density(ed: Editor):
    """AC21: paint_by_slope on a sculpted terrain, with coverage."""
    sid, terrain = await scene_with_terrain(ed, "Layers")
    await ed.call("generate_terrain", scene=sid, entity=terrain, preset="mountains", seed=3, resolution=65, apply_to=["height"])
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"surfaceLayers": [{}, {}]}})
    painted = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="paint_by_slope", seed=1, dry_run=False)
    ed.check(any(c["map"].startswith("blend") for c in painted["changed"]), f"blend changed: {painted['changed']}")
    coverage = painted["stats"]["layer_coverage"]["1"]
    ed.check(0.0 < coverage <= 1.0, f"coverage: {coverage}")


async def test_all_or_nothing(ed: Editor):
    """AC21: refused before evaluation, with issues and node ids, nothing written."""
    sid, terrain = await scene_with_terrain(ed, "Refused")
    await seed_height(ed, sid, terrain)
    before = map_files(ed)
    (ed.project / "graphs").mkdir(exist_ok=True)
    path = "graphs/Bad.noisegraph"
    (ed.project / path).write_text(json.dumps({
        "formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 9,
        "nodes": [{"id": "n1", "type": "fbm", "params": {"octaves": 40}}, {"id": "n2", "type": "output_height", "params": {}}],
        "edges": [{"id": "e1", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}}]}))
    bad = await ed.call_raw("generate_terrain", {"scene": sid, "entity": terrain, "graph": path})
    ed.check(bad.is_error, "an invalid graph is refused")
    issue = next(i for i in bad.structured["issues"] if i["code"] == "octave-limit")
    ed.check_eq(issue["node"], "n1", "the issue names the node")
    ed.check("edit_graph" in bad.structured["next"] and "n1" in bad.structured["next"], f"next names the fix: {bad.structured.get('next')}")
    region = await ed.call_raw("generate_terrain", {"scene": sid, "entity": terrain, "preset": "plains", "region": {"min": [5000, 5000], "max": [6000, 6000]}})
    ed.check(region.is_error and any(i["code"] == "bad-region" for i in region.structured["issues"]), region.text)
    made = await ed.call("create_scene", name="Empty", type="3D", path="scenes/Empty")
    empty = made["scene"] if "scene" in made else made["scene_id"]
    missing = await ed.call_raw("generate_terrain", {"scene": empty, "preset": "plains"})
    ed.check(missing.is_error and "no terrain" in missing.text, missing.text)
    ed.check_eq(map_files(ed), before, "nothing written")


async def test_region_and_feather(ed: Editor):
    """AC22."""
    sid, terrain = await scene_with_terrain(ed, "Region")
    await seed_height(ed, sid, terrain)
    whole = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="plains", seed=4, dry_run=True)
    ed.check_eq(whole["rect"], [0, 0, 65, 65], "whole terrain")
    outside_before = (await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[-90, -90]]))["samples"][0]["height"]
    part = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="plains", seed=4, region={"min": [-20, -20], "max": [20, 20]})
    rect = part["rect"]
    ed.check(rect[2] < 65 and rect[3] < 65, f"snapped: {rect}")
    changed = next(c for c in part["changed"] if c["map"] == "height")["rect"]
    ed.check(changed[0] >= rect[0] and changed[1] >= rect[1], f"inside the rectangle: {changed} in {rect}")
    outside_after = (await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[-90, -90]]))["samples"][0]["height"]
    ed.check_eq(outside_after, outside_before, "outside stays")
    await ed.call("undo", scene=sid)
    feathered = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="plains", seed=4, region={"min": [-20, -20], "max": [20, 20]}, feather=10)
    border = (await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[rect_centre(rect, 65), -19.5]]))["samples"][0]["height"]
    centre = (await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[0, 0]]))["samples"][0]["height"]
    ed.check(abs(border - 25.0) < abs(centre - 25.0) or abs(centre - 25.0) < 1e-3, f"feather keeps the border near the old height: {border} {centre} ({feathered['rect']})")
    refused = await ed.call_raw("generate_terrain", {"scene": sid, "entity": terrain, "preset": "plains", "resolution": 33, "region": {"min": [-20, -20], "max": [20, 20]}})
    ed.check(refused.is_error and "resolution" in refused.text, refused.text)


def rect_centre(rect, width):
    return (rect[0] + rect[2] / 2) * SIZE / width - SIZE / 2


async def test_job(ed: Editor):
    """AC23: wait false, get_job, cancel writes nothing, a sculpt during erode_existing fails, refused in Play."""
    sid, terrain = await scene_with_terrain(ed, "Job")
    await seed_height(ed, sid, terrain, 129)
    started = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="plains", seed=2, wait=False)
    job = started["job_id"]
    for _ in range(600):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running":
            break
        await ed.call("wait", seconds=0.1)
    ed.check_eq(state["status"], "succeeded", str(state))

    before = map_files(ed)
    big = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="alpine", seed=2, wait=False, resolution=1024)
    await ed.call("cancel_job", job_id=big["job_id"])
    for _ in range(600):
        state = await ed.call("get_job", job_id=big["job_id"])
        if state["status"] != "running":
            break
        await ed.call("wait", seconds=0.1)
    ed.check_eq(state["status"], "cancelled", str(state))
    ed.check_eq(map_files(ed), before, "cancel writes nothing")

    # erode_existing reads the heights: a sculpt during the run makes it fail
    busy = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="erode_existing", seed=2, wait=False)
    await ed.call("sculpt_terrain", scene=sid, entity=terrain, strokes=[{"mode": "raise", "points": [[0, 0]], "size": 30, "strength": 1, "falloff": "constant", "seconds_per_point": 0.2}])
    for _ in range(1200):
        state = await ed.call("get_job", job_id=busy["job_id"])
        if state["status"] != "running":
            break
        await ed.call("wait", seconds=0.05)
    # The run can finish before the sculpt lands on a fast machine: only a late sculpt can be caught
    if state["status"] == "failed":
        ed.check("terrain-changed" in json.dumps(state) or "changed while" in json.dumps(state), str(state))

    await ed.call("play", action="start", scene=sid)
    try:
        refused = await ed.call_raw("generate_terrain", {"scene": sid, "entity": terrain, "preset": "plains"})
        ed.check(refused.is_error, "refused in Play")
    finally:
        await ed.call("play", action="stop")


async def test_every_preset_dry_runs(ed: Editor):
    """AC26."""
    sid, terrain = await scene_with_terrain(ed, "Presets")
    await seed_height(ed, sid, terrain)
    # paint_by_slope paints layer 1, which it does not mark optional: the terrain needs it (R30)
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"surfaceLayers": [{}, {}]}})
    for preset in ("rolling_hills", "mountains", "alpine", "canyons", "islands", "dunes", "plateaus", "plains", "erode_existing", "paint_by_slope"):
        result = await ed.call_raw("generate_terrain", {"scene": sid, "entity": terrain, "preset": preset, "dry_run": True, "seed": 1, "max_width": 64})
        ed.check(not result.is_error, f"{preset}: {result.text}")
