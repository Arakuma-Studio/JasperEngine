"""The meshes group's detail level tools (docs/specs/S54-lod-crossfade.md R14, R15, AC7):
capture_model_lods on a model file (levels built on the CPU, a contact sheet) and on a GameObject
(the renderer, a forced level per tile), its group hidden until tool_groups enables it, and the
LOD numbers of get_render_stats.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import base64
import json
import math
import struct
from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Meshes CI"}
TIMEOUT = 900

S: dict = {}


def write_sphere_gltf(path: Path, slices: int = 48, stacks: int = 24) -> None:
    """A UV sphere of radius 1 with normals, one primitive, an embedded buffer."""
    positions: list[float] = []
    normals: list[float] = []
    for y in range(stacks + 1):
        phi = math.pi * y / stacks
        for x in range(slices + 1):
            theta = 2.0 * math.pi * x / slices
            p = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
            positions.extend(p)
            normals.extend(p)
    indices: list[int] = []
    for y in range(stacks):
        for x in range(slices):
            a = y * (slices + 1) + x
            b = a + slices + 1
            if y > 0:
                indices.extend((a, b, a + 1))
            if y + 1 < stacks:
                indices.extend((a + 1, b, b + 1))
    vertex_count = len(positions) // 3
    pos_bytes = struct.pack(f"<{len(positions)}f", *positions)
    nor_bytes = struct.pack(f"<{len(normals)}f", *normals)
    idx_bytes = struct.pack(f"<{len(indices)}H", *indices)
    blob = pos_bytes + nor_bytes + idx_bytes
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.5, 0.2, 1.0]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes)},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nor_bytes)},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nor_bytes), "byteLength": len(idx_bytes)},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": vertex_count, "type": "VEC3",
             "min": [-1.0, -1.0, -1.0], "max": [1.0, 1.0, 1.0]},
            {"bufferView": 1, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc))


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Lods", type="3D", path="scenes/Lods")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    sphere = await ed.call("create_entity", scene=S["scene"], type="sphere", name="Ball")
    S["sphere"] = sphere["entity_id"]
    await ed.call("create_entity", scene=S["scene"], type="directional_light", name="Sun")
    write_sphere_gltf(ed.project / "models" / "ball.gltf")
    return S


def check_levels(ed: Editor, data: dict, what: str) -> None:
    levels = data["levels"]
    ed.check(len(levels) >= 2, f"{what}: more than LOD 0 ({levels})")
    ed.check_eq(levels[0]["level"], 0, f"{what}: LOD 0 first")
    ed.check(levels[0]["psnr_db"] is None and levels[0]["switches_below_px"] is None, f"{what}: LOD 0 has no PSNR or switch height")
    for before, after in zip(levels, levels[1:]):
        ed.check(after["triangles"] < before["triangles"], f"{what}: each level has fewer triangles ({levels})")
        ed.check(after["error"] >= before["error"], f"{what}: errors grow with the level ({levels})")
    for entry in levels[1:]:
        ed.check(entry["error"] > 0.0, f"{what}: a coarser level deviates ({entry})")
        ed.check(entry["switches_below_px"] is not None and entry["switches_below_px"] > 0, f"{what}: a switch height ({entry})")
        ed.check(entry["psnr_db"] is None or entry["psnr_db"] > 10.0, f"{what}: a level looks like LOD 0 ({entry})")


async def test_meshes_group_is_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_model_lods" not in default, "capture_model_lods is hidden until the meshes group is enabled")
    await ed.call("tool_groups", enable=["meshes"])
    ed.check("capture_model_lods" in {t.name for t in await ed.tools(groups=None)}, "listed once meshes is enabled")


async def test_capture_model_lods_of_a_model_file(ed: Editor):
    ed.require_tools("capture_model_lods")
    await setup(ed)
    r = await ed.call_raw("capture_model_lods", {"path": "models/ball.gltf", "size": 128, "save_path": "captures/ball_lods.png"})
    ed.check(not r.is_error, r.text)
    data = r.value
    ed.check_eq(data["source"], "built", "a source model's levels are built as the runtime builds them")
    check_levels(ed, data, "model")
    ed.check(r.images and r.images[0][0].startswith(b"\x89PNG"), "the contact sheet comes back as a PNG")
    ed.check_eq(data["width"], 128 * len(data["levels"]), "one tile per level, side by side")
    ed.check((ed.project / "captures" / "ball_lods.png").is_file(), "save_path wrote the sheet")
    # only the levels asked for (and LOD 0 first)
    two = await ed.call("capture_model_lods", path="models/ball.gltf", levels=[0, 1], size=64)
    ed.check_eq([e["level"] for e in two["levels"]], [0, 1])


async def test_capture_model_lods_refuses_bad_arguments(ed: Editor):
    ed.require_tools("capture_model_lods")
    await setup(ed)
    both = await ed.call("capture_model_lods", path="models/ball.gltf", entity=S["sphere"], expect_error=True)
    ed.check("not both" in both, both)
    missing = await ed.call("capture_model_lods", path="models/none.gltf", expect_error=True)
    ed.check("path" in missing, missing)


async def test_capture_model_lods_of_a_gameobject(ed: Editor):
    ed.require_tools("capture_model_lods", "frame_entity")
    s = await setup(ed)
    await ed.call("frame_entity", scene=s["scene"], entity=s["sphere"])
    r = await ed.call_raw("capture_model_lods", {"scene": s["scene"], "entity": s["sphere"], "size": 160})
    ed.check(not r.is_error, r.text)
    data = r.value
    ed.check_eq(data["source"], "renderer")
    check_levels(ed, data, "entity")
    ed.check(r.images and r.images[0][0].startswith(b"\x89PNG"), "the tiles come back as one PNG")
    # the forced levels are given back: the mesh selects its level again
    got = await ed.call("get_entity", scene=s["scene"], entity=s["sphere"], components=["mesh"])
    ed.check("mesh" in got["components"], got)


async def test_render_stats_count_levels(ed: Editor):
    ed.require_tools("get_render_stats")
    await setup(ed)
    await ed.screenshot("scene", scene=S["scene"])
    stats = await ed.call("get_render_stats", view="scene")
    ed.check("vertices" in stats and stats["vertices"] >= stats["triangles"], f"vertices are reported: {stats.get('vertices')}")
    lod = stats["lod"]
    for key in ("lod_histogram", "fading", "culled_by_size", "fade_draws"):
        ed.check(key in lod, f"lod.{key} is reported: {lod}")
    ed.check_eq(len(lod["lod_histogram"]), 4, "one count per level")
    ed.check(sum(lod["lod_histogram"]) >= 1, f"the sphere is counted at its level: {lod}")
