"""Noise Graphs through the generic graph tools and preview_noise_graph (T2 R15, R64; AC10, AC24)."""

import json

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Noise Graph Test"}
TIMEOUT = 600

KIND_CODES = {
    "no-output": {"nodes": [{"id": "n1", "type": "constant", "params": {}}]},
    "duplicate-output": {"nodes": [{"id": "n1", "type": "constant", "params": {}},
                                   {"id": "n2", "type": "output_height", "params": {}},
                                   {"id": "n3", "type": "output_height", "params": {}}]},
    "octave-limit": {"nodes": [{"id": "n1", "type": "fbm", "params": {"octaves": 20}},
                               {"id": "n2", "type": "output_value", "params": {"name": "v"}}]},
    "unknown-variable": {"nodes": [{"id": "n1", "type": "variable", "params": {"name": "missing"}},
                                   {"id": "n2", "type": "output_value", "params": {"name": "v"}}]},
    "curve-keys": {"nodes": [{"id": "n1", "type": "curve", "params": {"keys": "not keys"}},
                             {"id": "n2", "type": "output_value", "params": {"name": "v"}}]},
    "bake-only-node": {"nodes": [{"id": "n1", "type": "thermal_erosion", "params": {}},
                                 {"id": "n2", "type": "output_value", "params": {"name": "v"}}],
                       "metadata": {"noise": {"usage": "runtime"}}},
    "grid-only-node": {"nodes": [{"id": "n1", "type": "slope_mask", "params": {}},
                                 {"id": "n2", "type": "output_value", "params": {"name": "v"}}],
                       "metadata": {"noise": {"usage": "points"}}},
    "erosion-budget": {"nodes": [{"id": "n1", "type": "hydraulic_erosion", "params": {"droplets_per_texel": 4.0, "lifetime": 128}},
                                 {"id": "n2", "type": "output_value", "params": {"name": "v"}}],
                       "metadata": {"noise": {"resolution": 4096}}},
}


def graph_file(fields: dict) -> str:
    base = {"formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 50, "edges": [], "variables": [], "groups": [],
            "viewport": {"offset": [0.0, 0.0], "zoom": 1.0}, "metadata": {}}
    base.update(fields)
    return json.dumps(base, indent=2)


async def test_kind_with_existing_tools(ed: Editor):
    kinds = await ed.call("list_graph_kinds")
    ed.check("noise" in [k["id"] for k in kinds["kinds"]], f"noise is listed: {kinds}")
    types = (await ed.call("list_graph_node_types", kind="noise"))["node_types"]
    ed.check_eq(len(types), 29, "29 node types")
    fbm = next(t for t in types if t["id"] == "fbm")
    octaves = next(p for p in fbm["params"] if p["name"] == "octaves")
    ed.check_eq((octaves["min"], octaves["max"]), (1.0, 12.0), "parameter ranges come from ParamDesc (R65)")

    (ed.project / "graphs").mkdir(exist_ok=True)
    for code, fields in KIND_CODES.items():
        path = f"graphs/{code}.noisegraph"
        (ed.project / path).write_text(graph_file(fields))
        issues = (await ed.call("validate_graph", path=path))["issues"]
        ed.check(any(i["code"] == code for i in issues), f"{code}: {issues}")

    # A file edited by hand: nodes and edges out of id order work in every tool
    shuffled = "graphs/Shuffled.noisegraph"
    (ed.project / shuffled).write_text(graph_file({
        "nodes": [{"id": "n10", "type": "output_height", "params": {}}, {"id": "n2", "type": "perlin", "params": {}}],
        "edges": [{"id": "e11", "from": {"node": "n2", "port": "out"}, "to": {"node": "n10", "port": "in"}}]}))
    issues = (await ed.call("validate_graph", path=shuffled))["issues"]
    ed.check(not [i for i in issues if i["severity"] == "error"], f"an out-of-order file is valid: {issues}")
    await ed.call("edit_graph", path=shuffled, ops=[{"op": "setParam", "node": "n2", "name": "frequency", "value": 3.0}])
    previewed = await ed.call("preview_noise_graph", path=shuffled, size=16, images="none")
    ed.check("height" in previewed["outputs"], f"it previews: {previewed['outputs']}")

    # connect + setParam, one undo step; unknown keys kept
    path = "graphs/Edit.noisegraph"
    (ed.project / path).write_text(graph_file({
        "nodes": [{"id": "n1", "type": "perlin", "params": {}, "note": "kept"},
                  {"id": "n2", "type": "output_value", "params": {"name": "v"}}],
        "future": {"a": 1}}))
    before = (ed.project / path).read_text()
    edited = await ed.call("edit_graph", path=path, ops=[
        {"op": "connect", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}},
        {"op": "setParam", "node": "n1", "name": "frequency", "value": 9.0}])
    ed.check_eq(edited["applied"], 2, "both ops")
    got = await ed.call("get_graph", path=path)
    ed.check_eq(got["graph"]["future"], {"a": 1}, "the file's unknown key")
    ed.check_eq(got["graph"]["nodes"][0]["note"], "kept", "the node's unknown key")
    # Graph files are project history: undo's default scene scope says where the step is, and the timeline,
    # what Ctrl+Z runs, takes it
    scene_only = await ed.call_raw("undo", {})
    ed.check(scene_only.is_error and "scope timeline" in scene_only.text, f"the empty scene history names the fix: {scene_only.text}")
    await ed.call("undo", scope="timeline")
    ed.check_eq((ed.project / path).read_text(), before, "one undo restores the file byte for byte")


async def test_preview(ed: Editor):
    (ed.project / "graphs").mkdir(exist_ok=True)
    path = "graphs/Preview.noisegraph"
    (ed.project / path).write_text(graph_file({
        "nodes": [{"id": "n1", "type": "fbm", "params": {"octaves": 3}},
                  {"id": "n2", "type": "remap", "params": {"clamp": True}},
                  {"id": "n3", "type": "output_height", "params": {}},
                  {"id": "n4", "type": "simplex", "params": {}},
                  {"id": "n5", "type": "constant", "params": {"value": 2.0}},
                  {"id": "n6", "type": "math", "params": {"op": "negate"}},
                  {"id": "n7", "type": "output_value", "params": {"name": "folded"}}],
        "edges": [{"id": "e1", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}},
                  {"id": "e2", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "in"}},
                  {"id": "e3", "from": {"node": "n5", "port": "out"}, "to": {"node": "n6", "port": "a"}},
                  {"id": "e4", "from": {"node": "n6", "port": "out"}, "to": {"node": "n7", "port": "in"}}]}))
    before = (ed.project / path).read_text()
    result = await ed.call_raw("preview_noise_graph", {"path": path, "size": 32, "seed": 3})
    ed.check(not result.is_error, result.text)
    nodes = {n["id"]: n for n in result.structured["nodes"]}
    ed.check(nodes["n4"]["dead"], "an unconnected node is dead")
    ed.check(nodes["n6"]["folded"], "a constant's negation is folded")
    observed, analytic = nodes["n1"]["range_observed"], nodes["n1"]["range_analytic"]
    ed.check(analytic[0] <= observed[0] <= observed[1] <= analytic[1], f"{observed} within {analytic}")
    ed.check(result.images, "outputs have images by default")

    edited = await ed.call("preview_noise_graph", path=path, size=32, seed=3,
                           ops=[{"op": "setParam", "node": "n1", "name": "frequency", "value": 11.0}], images="none")
    ed.check(edited["nodes"][0]["hash"] != nodes["n1"]["hash"], "ops change the result")
    ed.check_eq((ed.project / path).read_text(), before, "ops touch no file")

    limited = await ed.call_raw("preview_noise_graph", {"preset": "alpine", "size": 32, "images": "all"})
    ed.check(not limited.is_error, limited.text)
    ed.check(len(limited.images) <= 12, f"at most 12 images: {len(limited.images)}")

    # A preset's output hash is generate_terrain's at the same size and seed
    made = await ed.call("create_scene", name="Hash", type="3D", path="scenes/Hash")
    sid = made["scene"] if "scene" in made else made["scene_id"]
    terrain = (await ed.call("create_entity", scene=sid, name="Island", type="terrain",
                             values={"terrain": {"terrainSize": 2000, "maxHeight": 600}}))["entity_id"]
    await ed.call("tool_groups", enable=["terrain"])
    dry = await ed.call("generate_terrain", scene=sid, entity=terrain, preset="mountains", seed=7, resolution=64, dry_run=True)
    preview = await ed.call("preview_noise_graph", preset="mountains", size=64, seed=7, images="none")
    ed.check_eq(preview["outputs"]["height"], dry["hash"]["height"], "the same bits through both tools")

    # A newer preview of the same graph supersedes the older one still running: an erosion preset at 512 takes far
    # longer than the next call needs to arrive, so the first is cancelled, and Normal holds no backlog (Deviations)
    first = await ed.call("preview_noise_graph", preset="alpine", size=512, images="none", wait=False)
    second = await ed.call("preview_noise_graph", preset="alpine", size=512, seed=2, images="none", wait=False)
    states = {}
    for _ in range(600):
        states = {name: (await ed.call("get_job", job_id=job["job_id"]))["status"] for name, job in (("first", first), ("second", second))}
        if all(state != "running" for state in states.values()):
            break
        await ed.call("wait", seconds=0.1)
    ed.check_eq(states, {"first": "cancelled", "second": "succeeded"}, "the newer preview supersedes the older")

    # Works in Play
    await ed.call("play", action="start", scene=sid)
    try:
        playing = await ed.call_raw("preview_noise_graph", {"preset": "plains", "size": 16, "images": "none"})
        ed.check(not playing.is_error, f"allowed while playing: {playing.text}")
    finally:
        await ed.call("play", action="stop")
