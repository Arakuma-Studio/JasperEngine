"""The `meshes` group over MCP (docs/specs/S53-model-import-cook.md AC10): the tools are hidden
until the group is on; inspect_models reads every model from its header and cook record; a dry run
writes nothing and predicts exactly what the applied settings cook; set_model_import_settings is one
undo step and a cook job (wait false returns a job id); bad settings name the valid keys and write
nothing; setting is refused while playing; check_render_budgets fails (isError, so `call` exits 1)
with the offenders when a budget is exceeded, passes within it, and refuses without a budget.

The project is a copy of tools/fixtures/textures with a generated grid model added (a pure
function of this file).
"""

from __future__ import annotations

import asyncio
import json
import math
import struct
import time
from pathlib import Path

import yaml

from harness import Editor, tool_meta

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools" / "fixtures" / "textures"
EDITOR = {"project": FIXTURE}
TIMEOUT = 900
CI = True

TOOLS = ("inspect_models", "set_model_import_settings", "reimport_models", "check_render_budgets")
GRID = "assets/models/grid.glb"


def grid_glb(quads: int = 48) -> bytes:
    """A bumpy grid of unwelded triangles in a shuffled order: duplicates to weld, a cache to fix."""
    positions, normals, indices = [], [], []
    triangles = []
    for j in range(quads):
        for i in range(quads):
            triangles.append([(i, j), (i, j + 1), (i + 1, j)])
            triangles.append([(i + 1, j), (i, j + 1), (i + 1, j + 1)])
    # a fixed shuffle (a linear congruential order, no random module state)
    order = sorted(range(len(triangles)), key=lambda k: (k * 7919) % len(triangles))
    for k in order:
        for (i, j) in triangles[k]:
            x, z = i / quads, j / quads
            positions += [x, 0.1 * math.sin(x * 6) * math.cos(z * 5), z]
            normals += [0.0, 1.0, 0.0]
            indices.append(len(indices))
    pos = struct.pack(f"<{len(positions)}f", *positions)
    nor = struct.pack(f"<{len(normals)}f", *normals)
    idx = struct.pack(f"<{len(indices)}H", *indices)
    bin_chunk = pos + nor + idx
    while len(bin_chunk) % 4:
        bin_chunk += b"\0"
    count = len(positions) // 3
    xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
    gltf = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(bin_chunk)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nor), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos) + len(nor), "byteLength": len(idx), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "meshes": [{"name": "Grid", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
        "nodes": [{"name": "Grid", "mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    text = json.dumps(gltf, separators=(",", ":")).encode()
    while len(text) % 4:
        text += b" "
    total = 12 + 8 + len(text) + 8 + len(bin_chunk)
    return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(text), 0x4E4F534A) + text +
            struct.pack("<II", len(bin_chunk), 0x004E4942) + bin_chunk)


def stored(ed: Editor, rel: str) -> dict | None:
    meta = ed.project / (rel + ".meta")
    if not meta.exists():
        return None
    data = yaml.safe_load(meta.read_text()) or {}
    return (data.get("import") or {}).get("modelImporter")


async def add_grid(ed: Editor):
    path = ed.project / GRID
    if not path.exists():
        path.write_bytes(grid_glb())
        await ed.call("sync_files", paths=[GRID])


async def cooked(ed: Editor, rel: str, platform: str = "desktop") -> dict:
    result = await ed.call("reimport_models", paths=[rel], platforms=[platform])
    model = result["models"][0]
    ed.check_eq(model["status"], "cooked", f"{rel} cooked: {model}")
    return model


async def test_tools_hidden_until_enabled(ed: Editor):
    ed.require_tools(*TOOLS)
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"meshes tools listed by default: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["meshes"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(TOOLS) <= listed, "meshes tools listed once the group is on")
    for name in TOOLS:
        ed.check_eq(tool_meta(ed.tool(name)).get("jasper/group"), "meshes", f"{name} group")


async def test_inspect_reads_every_model(ed: Editor):
    ed.require_tools(*TOOLS)
    await add_grid(ed)
    model = await cooked(ed, GRID)
    ed.check_eq(model["triangles"], 48 * 48 * 2, "LOD 0 keeps every triangle")
    record = model["cook"]["record"]
    ed.check(record["cooked"]["vertices"] < record["source"]["vertices"], f"duplicates welded: {record['source']} -> {record['cooked']}")
    ed.check(record["download_bytes"] < record["source_bytes"], "the cook is smaller")
    ed.check_eq(record["visual_change"], "None", "lossless: the same values")
    ed.check(len(record["lods"]) >= 1, f"Automatic baked levels: {record['lods']}")
    everything = await ed.call("inspect_models", sort_by="triangles")
    paths = [m["path"] for m in everything["models"]]
    ed.check(GRID in paths and "assets/models/panel.gltf" in paths, f"every model: {paths}")
    ed.check_eq(paths[0], GRID, "sorted by triangles, the grid first")
    ed.check(everything["totals"]["triangles"] >= 48 * 48 * 2, "totals")
    # the scene scope: the open scene's models only
    scene = await ed.call("inspect_models", scene="Main")
    ed.check_eq([m["path"] for m in scene["models"]], ["assets/models/panel.gltf"], "the scene's models")


async def test_dry_run_predicts_the_applied_cook_and_undo_restores(ed: Editor):
    ed.require_tools(*TOOLS)
    await add_grid(ed)
    await cooked(ed, GRID)
    before_meta = stored(ed, GRID)
    dry = await ed.call("inspect_models", paths=[GRID], settings={"mesh_compression": "high", "generate_lods": "off"})
    ed.check(dry["dry_run"] and not dry["written"], "a dry run")
    predicted = dry["models"][0]["predicted"]
    ed.check_eq(stored(ed, GRID), before_meta, "the dry run wrote no sidecar")
    ed.check(predicted["download_bytes"] > 0 and predicted["visual_change"] != "None", f"a lossy prediction: {predicted}")
    applied = await ed.call("set_model_import_settings", paths=[GRID], settings={"mesh_compression": "high", "generate_lods": "off"})
    ed.check_eq(applied["updated"], [GRID])
    ed.check_eq(stored(ed, GRID).get("meshCompression"), "high", "the sidecar holds the setting")
    record = applied["models"][0]["cook"]["record"]
    ed.check_eq(record["download_bytes"], predicted["download_bytes"], "the dry run's numbers are the applied cook's")
    ed.check_eq(record["cooked"]["memory_bytes"], predicted["cooked"]["memory_bytes"], "and its memory")
    ed.check_eq(applied["models"][0]["cook"]["cache_hit"], True, "applying the dry run's settings is a cache hit")
    # the same call again changes nothing
    again = await ed.call("set_model_import_settings", paths=[GRID], settings={"mesh_compression": "high", "generate_lods": "off"})
    ed.check_eq(again["updated"], [], "idempotent")
    # one undo step back to no section at all
    await ed.call("undo", scope="project")
    ed.check_eq(stored(ed, GRID), before_meta, "undo restores the sidecar")
    restored = await cooked(ed, GRID)
    ed.check_eq(restored["cook"]["record"]["visual_change"], "None", "and the lossless cook")


async def test_bad_settings_write_nothing(ed: Editor):
    ed.require_tools(*TOOLS)
    await add_grid(ed)
    message = await ed.call("set_model_import_settings", paths=[GRID], settings={"lod_count": 3},
                            reset=["lod_counts"], expect_error=True)
    ed.check("not a model import setting" in message, message)
    message = await ed.call("set_model_import_settings", paths=["assets/textures/ao.png"], settings={"weld_vertices": False}, expect_error=True)
    ed.check("is not a model" in message, message)
    message = await ed.call("inspect_models", settings={"mesh_compression": "low"}, expect_error=True)
    ed.check("needs paths" in message, message)
    ed.check(stored(ed, "assets/models/panel.gltf") is None, "nothing written")


async def test_job_and_refused_while_playing(ed: Editor):
    ed.require_tools(*TOOLS)
    await add_grid(ed)
    started = await ed.call("set_model_import_settings", paths=[GRID], settings={"optimize_mesh": "vertex_order"}, wait=False)
    ed.check("job_id" in started, f"wait false returns a job: {started}")
    deadline = time.monotonic() + 300
    while True:
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        if time.monotonic() > deadline:
            ed.fail("the cook job did not finish")
        await asyncio.sleep(0.5)
    ed.check_eq(job["status"], "succeeded", f"the cook job: {job}")
    await ed.call("undo", scope="project")
    await ed.call("play", action="start", scene="Main")
    try:
        message = await ed.call("set_model_import_settings", paths=[GRID], settings={"lod_count": 2}, expect_error=True)
        ed.check("play" in message.lower(), message)
        # reading stays allowed
        await ed.call("inspect_models", paths=[GRID])
    finally:
        await ed.call("play", action="stop")


async def test_render_budgets(ed: Editor):
    ed.require_tools(*TOOLS)
    message = await ed.call("check_render_budgets", scene="Main", platform="mobile", expect_error=True)
    ed.check("No render budget for mobile" in message and "set_project_settings" in message, message)
    await ed.call("set_project_settings", values={"quality.render_budgets.mobile.triangles": 1, "quality.render_budgets.mobile.draw_calls": 1000})
    r = await ed.call_raw("check_render_budgets", {"scene": "Main", "platform": "mobile"})
    ed.check(r.is_error, f"over budget is an error: {r.text}")
    data = r.structured
    ed.check_eq(data["ok"], False)
    ed.check_eq([v["metric"] for v in data["violations"]], ["triangles"], "only the triangles are over")
    ed.check(data["offenders"] and data["offenders"][0]["model"] == "assets/models/panel.gltf", f"the offender: {data['offenders']}")
    ed.check(data["values"]["triangles"] >= 2, data["values"])
    await ed.call("set_project_settings", values={"quality.render_budgets.mobile.triangles": 1000000})
    ok = await ed.call("check_render_budgets", scene="Main", platform="mobile")
    ed.check_eq(ok["ok"], True, f"within the budget: {ok}")
