"""The meshes group's tools on one new empty project.

Detail levels (docs/specs/S54-lod-crossfade.md R14, R15, AC7): capture_model_lods on a model file
(levels built on the CPU, a contact sheet) and on a GameObject (the renderer, a forced level per
tile), its group hidden until tool_groups enables it, and the LOD numbers of get_render_stats.

Vertex colors (docs/specs/V1-vertex-colors.md R120-R124, AC6, AC7): edit_mesh_colors' result and
next, dry runs and failed batches that write nothing, undo and redo by hash, sample_mesh_colors'
element cap and the editor selection, from_texture on a 4-colour checker with hard edges, the
Vertex Color draw mode, get_model_info's facts, bake_mesh_colors' model and the CLI's schema.

.mesh assets (docs/specs/S98-editor-tooling-gaps.md F1, ledger E1: scene files embedded mesh buffers):
- test_scene_save_moves_large_meshes_into_mesh_assets: a scene save writes a sphere's geometry to
  "<scene folder>/<scene name>/<GameObject>.mesh" and the scene names it instead of holding its buffers
  (a box stays inline); get_asset_info reads the file; the reopened scene shows the geometry again.
- test_save_as_is_one_undo_step: set_mesh_geometry save_as writes the new primitive to the asset and
  shows it, one undo step; save_as alone saves the current geometry; a model's mesh is refused.
- test_a_script_assigns_a_mesh_asset: Mesh.meshAsset in Play reads the file at once.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import base64
import json
import math
import struct
import subprocess
from pathlib import Path

import harness
import pngdiff
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


MESH_COLOR_TOOLS = {"edit_mesh_colors", "sample_mesh_colors", "bake_mesh_colors"}


async def test_meshes_group_is_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_model_lods" not in default, "capture_model_lods is hidden until the meshes group is enabled")
    ed.check(not (MESH_COLOR_TOOLS & default), f"the vertex color tools are hidden too: {MESH_COLOR_TOOLS & default}")
    await ed.call("tool_groups", enable=["meshes"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_model_lods" in listed, "listed once meshes is enabled")
    ed.check(MESH_COLOR_TOOLS <= listed, f"and the vertex color tools: {MESH_COLOR_TOOLS - listed}")


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


# ---------------------------------------------------------------- vertex colors (V1)

CHECKER = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255), "yellow": (255, 255, 0)}
QUADS = 16


def write_checker_png(path: Path, size: int = 256) -> None:
    """2x2 quadrants, rows top to bottom: red | green over blue | yellow (exact in linear too)."""
    half = size // 2
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            name = ("red" if x < half else "green") if y < half else ("blue" if x < half else "yellow")
            rgba += bytes(CHECKER[name]) + b"\xff"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(size, size, rgba))


def write_checker_quad(path: Path, quads: int = QUADS, primitives: int = 1, albedo: str | None = None) -> None:
    """A 2x2 quad on XZ facing up, quads x quads cells sharing their vertices, uv = ((x+1)/2, (z+1)/2)."""
    positions, normals, uvs, indices = [], [], [], []
    for j in range(quads + 1):
        for i in range(quads + 1):
            positions += [-1.0 + 2.0 * i / quads, 0.0, -1.0 + 2.0 * j / quads]
            normals += [0.0, 1.0, 0.0]
            uvs += [i / quads, j / quads]
    for j in range(quads):
        for i in range(quads):
            a = j * (quads + 1) + i
            indices += [a, a + quads + 1, a + 1, a + 1, a + quads + 1, a + quads + 2]
    pos = struct.pack(f"<{len(positions)}f", *positions)
    nor = struct.pack(f"<{len(normals)}f", *normals)
    uv = struct.pack(f"<{len(uvs)}f", *uvs)
    # several primitives share the vertices and split the cells between them (rows of cells)
    per = len(indices) // primitives
    idx = struct.pack(f"<{len(indices)}H", *indices)
    blob = pos + nor + uv + idx
    count = len(positions) // 3
    views, offset = [], 0
    for part in (pos, nor, uv, idx):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(part)})
        offset += len(part)
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": "Checker", "mesh": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3 + k, "material": 0}
                                   for k in range(primitives)]}],
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": views,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3", "min": [-1.0, 0.0, -1.0], "max": [1.0, 0.0, 1.0]},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": count, "type": "VEC2"},
        ] + [{"bufferView": 3, "byteOffset": k * per * 2, "componentType": 5123, "count": per, "type": "SCALAR"} for k in range(primitives)],
    }
    if albedo:
        doc["images"] = [{"uri": albedo}]
        doc["samplers"] = [{"magFilter": 9729, "minFilter": 9729}]
        doc["textures"] = [{"source": 0, "sampler": 0}]
        doc["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc))


async def colors_setup(ed: Editor) -> dict:
    s = await setup(ed)
    if "quad" not in s:
        write_checker_quad(ed.project / "models" / "checker.gltf")
        write_checker_png(ed.project / "textures" / "checker.png")
        made = await ed.call("set_model", scene=s["scene"], create={"name": "Checker"}, path="models/checker.gltf")
        s["quad"] = made["model"]["id"]
    return s


async def mesh_hash(ed: Editor, entity) -> str:
    r = await ed.call("sample_mesh_colors", scene=S["scene"], entity=entity, query="hash")
    return r["targets"][0]["hash"]


async def undo_ids(ed: Editor) -> list:
    return [e["id"] for e in (await ed.call("get_undo_history", scene=S["scene"], scope="scene"))["entries"]]


def has_colors_component(entity: dict) -> bool:
    return any("vertex" in name.lower() and "color" in name.lower() for name in entity.get("components", {}))


async def test_mesh_colors_dry_run_writes_nothing(ed: Editor):
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    before = await mesh_hash(ed, s["sphere"])
    ids = await undo_ids(ed)
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["sphere"], dry_run=True,
                      ops=[{"op": "fill", "target": {"kind": "all"}, "value": "#0000FF"}])
    ed.check(r["dry_run"] is True and r["changed"]["vertices"] > 0, f"a dry run reports the change it would make: {r.get('changed')}")
    ed.check("Nothing changed" in r["next"], f"and says it made none: {r['next']}")
    ed.check_eq(await mesh_hash(ed, s["sphere"]), before, "the values are unchanged")
    ed.check_eq(await undo_ids(ed), ids, "no undo step")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["sphere"], detail="summary")
    ed.check(not has_colors_component(got), f"no Vertex Colors component was added: {list(got['components'])}")


async def test_mesh_colors_edit_result_and_next(ed: Editor):
    ed.require_tools("edit_mesh_colors")
    s = await colors_setup(ed)
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["sphere"],
                      ops=[{"op": "fill", "target": {"kind": "all"}, "value": "#FF0000"}])
    ed.check(r["created"] == {"component": True, "file": True}, f"the first edit adds the component and file: {r['created']}")
    ed.check(any(w["code"] == "created" for w in r["warnings"]), f"and says so: {r['warnings']}")
    target = r["targets"][0]
    ed.check(target["target"]["index"] == 0 and target["changed"]["vertices"] > 0, f"changed per target: {target}")
    op = target["ops"][0]
    for key in ("changed_vertices", "changed_corners", "split_added", "filtered_out", "in_target"):
        ed.check(key in op, f"ops[0].{key}: {op}")
    after = target["stats"]["after"]
    ed.check(after["min"][:3] == [1.0, 0.0, 0.0] and after["max"][:3] == [1.0, 0.0, 0.0], f"stats after: all red ({after})")
    ed.check("sample_mesh_colors" in r["next"] and "vertex_color" in r["next"], f"next: check, then the vertex color view ({r['next']})")
    ed.check(r["undo"]["name"].endswith("Edit Vertex Colors (Ball)"), f"one named undo step: {r.get('undo')}")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["sphere"], detail="summary")
    ed.check(has_colors_component(got), f"the component is there: {list(got['components'])}")
    again = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["sphere"],
                          ops=[{"op": "fill", "target": {"kind": "all"}, "value": "#FF0000"}])
    ed.check(again["created"] == {"component": False, "file": False}, f"later edits add nothing: {again['created']}")
    ed.check_eq(again["changed"]["vertices"], 0, "the same fill again changes nothing")


async def test_mesh_colors_failing_op_changes_nothing(ed: Editor):
    ed.require_tools("edit_mesh_colors")
    s = await colors_setup(ed)
    before = await mesh_hash(ed, s["sphere"])
    ids = await undo_ids(ed)
    e = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["sphere"], expect_error=True, ops=[
        {"op": "fill", "target": {"kind": "all"}, "value": "#00FF00"},
        {"op": "set", "target": {"kind": "faces", "ids": [0, 999999]}, "value": "#0000FF"},
    ])
    ed.check("ops[1].target.ids" in e, f"the error names the field: {e}")
    ed.check("Nothing was changed" in e, e)
    ed.check_eq(await mesh_hash(ed, s["sphere"]), before, "the first op was rolled back")
    ed.check_eq(await undo_ids(ed), ids, "no undo step")
    unknown = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["sphere"], expect_error=True,
                            ops=[{"op": "fill", "target": {"kind": "all"}, "colour": "#00FF00"}])
    ed.check("ops[0].colour" in unknown and "value" in unknown, f"an unknown field lists the valid ones: {unknown}")


async def test_mesh_colors_undo_redo_by_hash(ed: Editor):
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    before = await mesh_hash(ed, s["sphere"])
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["sphere"], hash=True,
                      ops=[{"op": "set", "target": {"kind": "sphere", "center": [0, 0.5, 0], "radius": 0.5}, "value": [0.0, 0.0, 1.0]}])
    edited = r["targets"][0]["hash"]
    ed.check(edited != before, "the edit changed the hash")
    ed.check_eq(await mesh_hash(ed, s["sphere"]), edited, "sample_mesh_colors reads the hash the edit returned")
    await ed.call("undo", scene=s["scene"])
    ed.check_eq(await mesh_hash(ed, s["sphere"]), before, "undo restores the values")
    await ed.call("redo", scene=s["scene"])
    ed.check_eq(await mesh_hash(ed, s["sphere"]), edited, "redo brings the edit back")


async def test_mesh_colors_elements_cap_and_selection(ed: Editor):
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    e = await ed.call("sample_mesh_colors", scene=s["scene"], entity=s["quad"], query="elements", max_elements=50, expect_error=True)
    ed.check("region_too_large" in e and "sphere" in e, f"too many elements asks for a narrower region: {e}")
    region = {"kind": "sphere", "center": [-0.5, 0, -0.5], "radius": 0.3}
    r = await ed.call("sample_mesh_colors", scene=s["scene"], entity=s["quad"], query="elements", region=region, max_elements=50, select=True)
    found = r["targets"][0]
    ed.check(0 < r["count"] <= 50 and found["kind"] == "vertices" and len(found["ids"]) == len(found["values"]) == r["count"], f"{r}")
    ed.check(r.get("selected") is True and "selection" in r["next"], f"select: true says the editor selects them: {r}")
    sel = await ed.call("sample_mesh_colors", scene=s["scene"], entity=s["quad"], query="elements", region={"kind": "selection"})
    ed.check_eq(sel["targets"][0]["ids"], found["ids"], "the editor's element selection holds exactly those vertices")
    painted = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["quad"],
                            ops=[{"op": "set", "target": {"kind": "selection"}, "value": "#00FF00"}])
    ed.check_eq(painted["targets"][0]["ops"][0]["in_target"], r["count"], "a selection target acts on the selected vertices")
    await ed.call("undo", scene=s["scene"])


async def test_mesh_colors_from_checker_texture(ed: Editor):
    """AC7: from_texture with keep_seams matches a 4-colour checker and gives its edges hard corners"""
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=s["quad"],
                      ops=[{"op": "from_texture", "texture": "textures/checker.png", "keep_seams": True}])
    op = r["targets"][0]["ops"][0]
    ed.check(op["mean_delta_e"] < 3.0, f"mean_delta_e under 3: {op}")
    ed.check(op["split_added"] > 0 and r["targets"][0]["split_vertices"] > 0, f"corner overrides on the checker edges: {op}, {r['targets'][0]}")
    near = 1.0 / QUADS / 4
    points = [[-0.5, 0, -0.5], [0.5, 0, -0.5], [-0.5, 0, 0.5], [0.5, 0, 0.5], [-near, 0, -0.5], [near, 0, -0.5]]
    got = await ed.call("sample_mesh_colors", scene=s["scene"], entity=s["quad"], query="points", points=points)
    want = ["red", "green", "blue", "yellow", "red", "green"]
    for sample, name in zip(got["points"], want):
        rgb = [c / 255 for c in CHECKER[name]]
        # a blend across the border would be halfway (0.5); bilinear taps near a texel edge stay far below 0.1
        ed.check(all(abs(a - b) < 0.1 for a, b in zip(sample["value"][:3], rgb)),
                 f"{sample['point']} is {name} (a hard edge on the checker's border, not a blend): {sample}")
    entries = (await ed.call("sample_mesh_colors", scene=s["scene"], entity=s["quad"], query="palette"))["targets"][0]["palette"]
    total = sum(e["corners"] for e in entries)
    pure = sum(e["corners"] for e in entries if any(all(abs(a - c / 255) < 0.1 for a, c in zip(e["value"][:3], rgb)) for rgb in CHECKER.values()))
    ed.check(total == QUADS * QUADS * 6 and pure >= 0.95 * total, f"the corners hold the four checker colours, none blended: {entries}")


async def test_mesh_colors_from_material_slot(ed: Editor):
    """R41: material_slot reads the target's own material (the shared loader the Vertex Paint window uses)"""
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    write_checker_quad(ed.project / "models" / "textured.gltf", albedo="../textures/checker.png")
    made = await ed.call("set_model", scene=s["scene"], create={"name": "Textured", "position": [0, 0, -10]}, path="models/textured.gltf")
    textured = made["model"]["id"]
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=textured, ops=[{"op": "from_texture", "material_slot": "albedo"}])
    op = r["targets"][0]["ops"][0]
    ed.check(op["mean_delta_e"] < 3.0 and op["split_added"] > 0, f"the albedo map, hard edges kept: {op}")
    got = await ed.call("sample_mesh_colors", scene=s["scene"], entity=textured, query="points", points=[[-0.5, 0, -10.5], [0.5, 0, -9.5]])
    for sample, name in zip(got["points"], ["red", "yellow"]):
        rgb = [c / 255 for c in CHECKER[name]]
        ed.check(all(abs(a - b) < 0.1 for a, b in zip(sample["value"][:3], rgb)), f"{sample['point']} is {name}: {sample}")
    flat = await ed.call("edit_mesh_colors", scene=s["scene"], entity=textured, hash=True,
                         ops=[{"op": "from_texture", "material_slot": "metallic_roughness"}])
    after = flat["targets"][0]["stats"]["after"]
    ed.check(after["min"][:3] == [1.0, 1.0, 1.0] == after["max"][:3], f"no map: the slot's factor alone: {after}")
    e = await ed.call("edit_mesh_colors", scene=s["scene"], entity=textured, expect_error=True,
                      ops=[{"op": "from_texture", "material_slot": "detail"}])
    ed.check("ops[0].material_slot" in e and "metallic_roughness" in e, f"an unknown slot lists the slots: {e}")


async def test_mesh_colors_model_info_facts(ed: Editor):
    ed.require_tools("get_model_info")
    s = await colors_setup(ed)
    # split_vertices is what the render path bound: drawn frames bind the split the edit made
    for _ in range(20):
        info = await ed.call("get_model_info", scene=s["scene"], entity=s["quad"])
        if info["vertex_colors"][0].get("split_vertices"):
            break
        await ed.screenshot("scene", scene=s["scene"])
    colors = info["vertex_colors"]
    ed.check_eq(len(colors), 1, f"one target: {colors}")
    target = colors[0]
    ed.check(target["node"] == 0 and target["primitive"] == 0 and target["vertex_count"] == (QUADS + 1) ** 2, f"{target}")
    ed.check(target["has_vertex_colors"] and target["painted"] and target["stale"] is False, f"painted, fresh: {target}")
    ed.check(target["split_vertices"] > 0, f"the checker's hard edges split vertices: {target}")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["quad"])
    ed.check_eq(got.get("vertex_colors"), colors, "get_entity reports the same facts")


async def test_mesh_colors_selections_of_two_targets_one_step(ed: Editor):
    """Each target's own selection, one call, one undo step (EditRequest::perTargetOps)"""
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    write_checker_quad(ed.project / "models" / "halves.gltf", primitives=2)
    made = await ed.call("set_model", scene=s["scene"], create={"name": "Halves", "position": [0, 0, 10]}, path="models/halves.gltf")
    halves = made["model"]["id"]
    targets = (await ed.call("get_model_info", scene=s["scene"], entity=halves))["vertex_colors"]
    ed.check_eq(len(targets), 2, f"two targets: {targets}")
    picked = await ed.call("sample_mesh_colors", scene=s["scene"], entity=halves, query="elements", select=True,
                           region={"kind": "box", "center": [0, 0, 9.6], "half_extents": [0.3, 0.1, 0.6], "element": "face"})
    counts = [len(t["ids"]) for t in picked["targets"]]
    ed.check(all(n > 0 for n in counts) and counts[0] != counts[1], f"both halves have selected faces, not as many: {counts}")
    before = [t["hash"] for t in (await ed.call("sample_mesh_colors", scene=s["scene"], entity=halves, query="hash"))["targets"]]
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=halves, ops=[{"op": "set", "target": {"kind": "selection"}, "value": "#00FFFF"}])
    ed.check_eq([t["ops"][0]["in_target"] for t in r["targets"]], counts, "each target sets its own selection")
    after = [t["hash"] for t in (await ed.call("sample_mesh_colors", scene=s["scene"], entity=halves, query="hash"))["targets"]]
    ed.check(all(a != b for a, b in zip(after, before)), "both targets changed")
    await ed.call("undo", scene=s["scene"])
    undone = [t["hash"] for t in (await ed.call("sample_mesh_colors", scene=s["scene"], entity=halves, query="hash"))["targets"]]
    ed.check_eq(undone, before, "one undo restores both targets")


async def test_mesh_colors_transfer_keeps_hard_edges(ed: Editor):
    """transfer from a GameObject with face colors carries its hard edges as corner overrides (R60)"""
    ed.require_tools("edit_mesh_colors", "sample_mesh_colors")
    s = await colors_setup(ed)
    write_checker_quad(ed.project / "models" / "edges.gltf", quads=4)
    made = []
    for name in ("Edged", "Copy"):
        made.append((await ed.call("set_model", scene=s["scene"], create={"name": name, "position": [20, 0, 0]}, path="models/edges.gltf"))["model"]["id"])
    source, copy = made
    # the faces of x < 20 red over white: the vertices on x = 20 hold both
    await ed.call("edit_mesh_colors", scene=s["scene"], entity=source, ops=[
        {"op": "fill", "target": {"kind": "all"}, "value": "#FFFFFF"},
        {"op": "set", "target": {"kind": "box", "center": [19.5, 0, 0], "half_extents": [0.501, 0.5, 1.5], "element": "face"}, "value": "#FF0000"}])
    r = await ed.call("edit_mesh_colors", scene=s["scene"], entity=copy, ops=[{"op": "transfer", "source_entity": source, "max_distance": 0.01}])
    ed.check(r["changed"]["split_added"] > 0, f"the edge's corners split: {r['changed']}")

    async def palette(entity) -> dict:
        got = await ed.call("sample_mesh_colors", scene=s["scene"], entity=entity, query="palette")
        return {e["hex"]: e["corners"] for e in got["targets"][0]["palette"]}

    want = await palette(source)
    ed.check(len(want) == 2, f"the source has red and white corners: {want}")
    ed.check_eq(await palette(copy), want, "every corner took its own side's color")


def central_box(png: bytes) -> list:
    """The pixels around the center of the view: on a framed ball whatever the camera's angle"""
    w, h, rgba = pngdiff.decode(png)
    half = int(0.06 * h)
    out = []
    for y in range(h // 2 - half, h // 2 + half):
        for x in range(w // 2 - half, w // 2 + half):
            at = (y * w + x) * 4
            out.append(tuple(rgba[at:at + 3]))
    return out


async def test_mesh_colors_vertex_color_view(ed: Editor):
    ed.require_tools("set_scene_camera", "frame_entity", "select_entities")
    s = await colors_setup(ed)
    # a ball of its own, away from the others, unselected (no gizmo over it)
    ball = (await ed.call("create_entity", scene=s["scene"], type="sphere", name="ViewBall", position=[20, 0, 0]))["entity_id"]
    await ed.call("edit_mesh_colors", scene=s["scene"], entity=ball, ops=[{"op": "fill", "target": {"kind": "all"}, "value": "#FF0000"}])
    await ed.call("select_entities", scene=s["scene"], entities=[])
    await ed.call("frame_entity", scene=s["scene"], entity=ball)
    try:
        await ed.call("set_scene_camera", scene=s["scene"], display={"draw_mode": "vertex_color"})
        red = central_box(await ed.screenshot("scene", scene=s["scene"]))
        flat = sum(1 for p in red if p[0] > 240 and p[1] < 20 and p[2] < 20)
        ed.check(flat >= 0.95 * len(red), f"the ball draws its stored red, unlit and unshaded: {flat}/{len(red)} pure red ({red[:4]})")
        await ed.call("set_scene_camera", scene=s["scene"], display={"draw_mode": "vertex_color", "vertex_color_channel": "g"})
        green = central_box(await ed.screenshot("scene", scene=s["scene"]))
        black = sum(1 for p in green if max(p) < 20)
        ed.check(black >= 0.95 * len(green), f"channel g of red draws black: {black}/{len(green)} ({green[:4]})")
    finally:
        await ed.call("set_scene_camera", scene=s["scene"], display={"draw_mode": "shaded"})


async def test_bake_mesh_colors_model_has_color0(ed: Editor):
    ed.require_tools("bake_mesh_colors", "get_model_info")
    s = await colors_setup(ed)
    r = await ed.call("bake_mesh_colors", scene=s["scene"], entity=s["quad"], output="model", path="models/checker_painted.glb")
    ed.check(r["status"] == "succeeded" and "set_model" in r["next"], f"{r}")
    glb = (ed.project / "models" / "checker_painted.glb").read_bytes()
    ed.check(glb[:4] == b"glTF", "a binary glTF")
    length = struct.unpack("<I", glb[12:16])[0]
    doc = json.loads(glb[20:20 + length])
    prims = [p for m in doc["meshes"] for p in m["primitives"]]
    ed.check(prims and all("COLOR_0" in p["attributes"] for p in prims), f"every primitive has COLOR_0: {[p['attributes'] for p in prims]}")
    made = await ed.call("set_model", scene=s["scene"], create={"name": "Baked", "position": [3, 0, 0]}, path="models/checker_painted.glb")
    info = await ed.call("get_model_info", scene=s["scene"], entity=made["model"]["id"])
    ed.check(all(t["has_vertex_colors"] and not t["painted"] for t in info["vertex_colors"]), f"its own colors, nothing painted: {info['vertex_colors']}")


async def test_mesh_colors_cli_schema_offline(ed: Editor):
    work = ed.project.parent / "cli-colors"
    work.mkdir(exist_ok=True)
    (work / "settings.yaml").write_text("")
    shown = subprocess.run(harness.editor_command() + ["call", "edit_mesh_colors", "--help"], cwd=str(work), capture_output=True, text=True, timeout=60)
    ed.check(shown.returncode == 0 and "--ops" in shown.stdout and "--targets" in shown.stdout, f"--help: {shown.returncode} {shown.stdout} {shown.stderr}")
    bad = subprocess.run(harness.editor_command() + ["call", "--dry-run", "edit_mesh_colors", "--entity", "1", "--ops", json.dumps([{"op": "paint"}])],
                         cwd=str(work), capture_output=True, text=True, timeout=60)
    ed.check(bad.returncode == 2 and "must be one of" in bad.stderr, f"the op enum is checked before any editor: {bad.returncode} {bad.stderr}")


# S98 REQ-34/35: a box whose five materials each name a diffuse texture, three missing and two
# present with a tiling (editor/tests/util/FbxImporterWarningsTest.cpp holds the same model)
HALL_TEXTURES = ["missing_wall.psd", "missing_floor.png", "missing_roof.tga", "tile_a.png", "tile_b.png"]


def hall_fbx() -> str:
    objects, connections = [], ['\tC: "OO",200,0', '\tC: "OO",100,200']
    for i, name in enumerate(HALL_TEXTURES):
        tiled = i >= 3
        objects.append(f'\tMaterial: 30{i}, "Material::M{i}", "" {{\n\t\tVersion: 102\n\t\tShadingModel: "phong"\n\t\tMultiLayer: 0\n'
                       f'\t\tProperties70:  {{\n\t\t\tP: "DiffuseColor", "Color", "", "A",0.8,0.8,0.8\n\t\t}}\n\t}}')
        props = '\t\tProperties70:  {\n\t\t\tP: "Scaling", "Vector", "", "A",2,2,1\n\t\t}\n' if tiled else ""
        objects.append(f'\tTexture: 31{i}, "Texture::T{i}", "" {{\n\t\tType: "TextureVideoClip"\n\t\tVersion: 202\n'
                       f'\t\tTextureName: "Texture::T{i}"\n\t\tFileName: "textures/{name}"\n\t\tRelativeFilename: "textures/{name}"\n'
                       f'\t\tModelUVTranslation: 0,0\n\t\tModelUVScaling: {"2,2" if tiled else "1,1"}\n{props}\t}}')
        connections += [f'\tC: "OO",30{i},200', f'\tC: "OP",31{i},30{i}, "DiffuseColor"']
    return ('; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: "JasperEngine tests"\n}\n'
            'GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n\t\tP: "UpAxis", "int", "Integer", "",1\n'
            '\t\tP: "UpAxisSign", "int", "Integer", "",1\n\t\tP: "FrontAxis", "int", "Integer", "",2\n'
            '\t\tP: "FrontAxisSign", "int", "Integer", "",1\n\t\tP: "CoordAxis", "int", "Integer", "",0\n'
            '\t\tP: "CoordAxisSign", "int", "Integer", "",1\n\t\tP: "UnitScaleFactor", "double", "Number", "",100\n\t}\n}\n'
            'Objects:  {\n\tGeometry: 100, "Geometry::Box", "Mesh" {\n'
            '\t\tVertices: *24 {\n\t\t\ta: -1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1,-1,-1,1,1,-1,1,1,1,1,-1,1,1\n\t\t}\n'
            '\t\tPolygonVertexIndex: *24 {\n\t\t\ta: 0,3,2,-2,4,5,6,-8,0,1,5,-5,3,7,6,-3,0,4,7,-4,1,2,6,-6\n\t\t}\n'
            '\t\tGeometryVersion: 124\n\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: "UVMap"\n'
            '\t\t\tMappingInformationType: "ByPolygonVertex"\n\t\t\tReferenceInformationType: "IndexToDirect"\n'
            '\t\t\tUV: *8 {\n\t\t\t\ta: 0,0,1,0,1,1,0,1\n\t\t\t}\n'
            '\t\t\tUVIndex: *24 {\n\t\t\t\ta: 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3\n\t\t\t}\n\t\t}\n'
            '\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: ""\n\t\t\tMappingInformationType: "ByPolygon"\n'
            '\t\t\tReferenceInformationType: "IndexToDirect"\n\t\t\tMaterials: *6 {\n\t\t\t\ta: 0,1,2,3,4,4\n\t\t\t}\n\t\t}\n'
            '\t\tLayer: 0 {\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementUV"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n'
            '\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementMaterial"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n\t}\n'
            '\tModel: 200, "Model::Hall", "Mesh" {\n\t\tVersion: 232\n\t\tShading: T\n\t\tCulling: "CullingOff"\n\t}\n'
            + "\n".join(objects) + "\n}\nConnections:  {\n" + "\n".join(connections) + "\n}\n")


def glb_json(path: Path) -> dict:
    data = path.read_bytes()
    length = struct.unpack("<I", data[12:16])[0]
    return json.loads(data[20:20 + length])


async def test_material_creation_mode_none(ed: Editor):
    """S98 AC30 (MCP side) and AC31: an FBX import with warnings logs one Console line per model, the
    import report (inspect_models) lists each; Material Creation Mode None re-imports it with no
    material and no texture lookup, so nothing is warned, and its submeshes stay."""
    models = ed.project / "models"
    (models / "textures").mkdir(parents=True, exist_ok=True)
    for name in ("tile_a.png", "tile_b.png"):
        (models / "textures" / name).write_bytes(pngdiff.encode(4, 4, bytes([200, 200, 200, 255]) * 16))
    (models / "Hall.fbx").write_text(hall_fbx())
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("sync_files", paths=["models/Hall.fbx", "models/textures/tile_a.png", "models/textures/tile_b.png"])
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)

    lines = [e["message"] for e in (await ed.call("read_console", since=cursor, contains="Hall.fbx", limit=200))["entries"]
             if e["type"] == "warning"]
    ed.check_eq(len(lines), 1, f"one warning line for the model: {lines}")
    ed.check("3 textures not found" in lines[0] and "tiling/offset not imported on 2 textures" in lines[0], f"with the counts: {lines}")
    report = (await ed.call("inspect_models", paths=["models/Hall.fbx"]))["models"][0]
    warnings = [i["message"] for i in report["issues"] if i["code"] == "fbx-import-warning"]
    ed.check(sum("Texture not found" in w for w in warnings) == 3 and sum("tiling/offset" in w for w in warnings) == 2,
             f"the import report lists all five: {warnings}")
    ed.check_eq(report["settings"]["resolved"]["material_creation_mode"], "import", "Import by default")
    glb = ed.project / ".jasperengine" / "imported" / "models" / "Hall.fbx.glb"
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "the import made the five materials")

    await ed.check_error("set_model_import_settings", paths=["models/Hall.fbx"], settings={"material_creation_mode": "none"},
                         platform="mobile", contains="without platform")
    write_sphere_gltf(models / "ball.gltf")
    await ed.call("sync_files", paths=["models/ball.gltf"])
    await ed.check_error("set_model_import_settings", paths=["models/ball.gltf"], settings={"material_creation_mode": "none"},
                         contains="FBX")

    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("set_model_import_settings", paths=["models/Hall.fbx"], settings={"material_creation_mode": "none"}, timeout=600)
    meta = (models / "Hall.fbx.meta").read_text()
    ed.check("materialCreationMode: none" in meta, f"the sidecar holds it: {meta}")
    doc = glb_json(glb)
    ed.check(not doc.get("materials") and not doc.get("images"), "None: no material and no image in the import")
    ed.check(len(doc["meshes"][0]["primitives"]) >= 5 and all("material" not in p for p in doc["meshes"][0]["primitives"]),
             "its submeshes stay, with the default material")
    warned = [e for e in (await ed.call("read_console", since=cursor, contains="Hall.fbx", limit=200))["entries"] if e["type"] == "warning"]
    ed.check_eq(warned, [], "None warns about no texture")
    report = (await ed.call("inspect_models", paths=["models/Hall.fbx"]))["models"][0]
    ed.check_eq([i for i in report["issues"] if i["code"] == "fbx-import-warning"], [], "the report is empty")
    ed.check_eq(report["settings"]["resolved"]["material_creation_mode"], "none", "and says None")

    # one undo step: Import again, the materials come back
    await ed.call("undo", scope="project")
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "undo: the materials are imported again")


async def test_project_material_creation_mode(ed: Editor):
    """S98 REQ-44: Project Settings > Mesh > Material Creation Mode is the mode of the FBX models that
    set none (a model's own value wins), and settings search finds it."""
    models = ed.project / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "Hall.fbx").write_text(hall_fbx())
    await ed.call("sync_files", paths=["models/Hall.fbx"])
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    glb = ed.project / ".jasperengine" / "imported" / "models" / "Hall.fbx.glb"
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "Import by default")

    await ed.call("set_project_settings", values={"mesh.material_creation_mode": "none"})
    project_yaml = (ed.project / "project.yaml").read_text()
    ed.check("materialCreationMode: none" in project_yaml, f"project.yaml holds it: {project_yaml}")
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    ed.check(not glb_json(glb).get("materials"), "the project's None: no material in the import")
    report = (await ed.call("inspect_models", paths=["models/Hall.fbx"]))["models"][0]
    ed.check_eq(report["settings"]["resolved"]["material_creation_mode"], "none", "the model resolves the project's mode")

    await ed.call("set_model_import_settings", paths=["models/Hall.fbx"], settings={"material_creation_mode": "import"}, timeout=600)
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "the model's own Import wins")

    found = await ed.call("search_editor", query=":Material Creation Mode")
    keys = [r["key"] for r in found["results"]]
    ed.check("setting:mesh.material_creation_mode" in keys, f"settings search finds it: {keys}")


# ---- .mesh assets (S98 F1) ----
def mesh_of(text: str, name: str) -> str:
    """The `mesh:` block of the GameObject `name` in a scene file's text."""
    entity = text.split(f"name: {name}\n", 1)[1].split("  - type: Entity", 1)[0]
    return entity.split("mesh:", 1)[1] if "mesh:" in entity else ""


async def test_scene_save_moves_large_meshes_into_mesh_assets(ed: Editor):
    sid = (await ed.call("create_scene", name="Shapes", type="3D", path="scenes/Shapes"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="sphere", name="Ball")
    await ed.call("create_entity", scene=sid, type="sphere", name="Twin")
    await ed.call("create_entity", scene=sid, type="cube", name="Crate")
    await ed.call("save_scene", scene=sid)

    scene_file = ed.project / "scenes" / "Shapes.scene"
    text = scene_file.read_text()
    files = sorted(p.relative_to(ed.project).as_posix() for p in (ed.project / "scenes" / "Shapes").rglob("*.mesh"))
    ed.check(len(files) == 1 and files[0].endswith("Shapes/Ball.mesh"), f"one file for the two spheres, beside the scene: {files}")
    asset = files[0]
    ed.check((ed.project / (asset + ".meta")).exists(), "the .mesh has a GUID (its .meta)")
    ball = mesh_of(text, "Ball")
    ed.check("meshAsset:" in ball and "meshAssetGuid:" in ball and "buffer:" not in ball, f"the sphere names the file: {ball[:400]}")
    ed.check("meshAsset:" in mesh_of(text, "Twin"), "its twin names the same file")
    crate = mesh_of(text, "Crate")
    ed.check("buffer:" in crate and "meshAsset:" not in crate, "a box (under 4 KB) stays in the scene")
    ed.check(text.startswith("formatVersion: 11\n"), f"scene format 11: {text[:40]}")

    info = await ed.call("get_asset_info", path=asset)
    ed.check_eq(info["type"], "mesh", f"{info}")
    ed.check(info["vertex_count"] > 100 and info["index_count"] > 0, f"the sphere's geometry: {info}")
    ed.check_eq(sorted(u["entity"]["name"] for u in info["users"]), ["Ball", "Twin"], f"who shows it: {info['users']}")
    listed = await ed.call("list_assets", pattern="*.mesh", type="mesh")
    ed.check(any(a["path"] == asset for a in listed["entries"]), f"list_assets type mesh: {listed}")

    # saved again: not a byte changes
    await ed.call("save_scene", scene=sid)
    ed.check_eq(scene_file.read_text(), text, "a second save writes the same scene")

    # reopened: the geometry comes back from the file (save_as alone needs it to be there)
    await ed.call("close_scene", scene=sid, unsaved="discard")
    sid = (await ed.call("open_scene", scene="scenes/Shapes.scene"))["scene"]["id"]
    await ed.call("wait", frames=3)
    copy = await ed.call("set_mesh_geometry", scene=sid, entity="Ball", save_as="meshes/copy.mesh")
    size = copy["local_size"]
    ed.check(all(abs(v - 2.0) < 0.01 for v in size), f"the reopened sphere is the 2 m one it was: {size}")
    ed.check((ed.project / "meshes" / "copy.mesh").read_bytes() == (ed.project / asset).read_bytes(), "the same geometry, the same bytes")


async def test_save_as_is_one_undo_step(ed: Editor):
    sid = (await ed.call("create_scene", name="SaveAs", type="3D", path="scenes/SaveAs"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Pillar")
    r = await ed.call("set_mesh_geometry", scene=sid, entity="Pillar", shape="cylinder", radius=0.5, height=3, save_as="meshes/pillar.mesh")
    ed.check_eq(r["mesh_asset"], "meshes/pillar.mesh", f"{r}")
    ed.check(abs(r["local_size"][1] - 3.0) < 0.01, f"the cylinder read back from the file: {r}")
    ed.check((ed.project / "meshes" / "pillar.mesh").exists(), "the file is written")

    async def shown() -> str:
        entity = await ed.call("get_entity", scene=sid, entity="Pillar", components=["mesh"])
        return entity["components"]["mesh"].get("meshAsset") or ""

    ed.check_eq(await shown(), "meshes/pillar.mesh", "the Mesh field shows it")
    await ed.call("undo", scene=sid)
    ed.check_eq(await shown(), "", "undo: the cube again, its own geometry (one step)")
    ed.check((ed.project / "meshes" / "pillar.mesh").exists(), "the file stays")
    await ed.call("redo", scene=sid)
    ed.check_eq(await shown(), "meshes/pillar.mesh", "redo")

    # a primitive without save_as makes the geometry the mesh's own again
    await ed.call("set_mesh_geometry", scene=sid, entity="Pillar", shape="box")
    ed.check_eq(await shown(), "", "a new shape lets go of the file")

    # another GameObject shows the file through its Mesh field
    await ed.call("create_entity", scene=sid, type="cube", name="Copy")
    await ed.call("set_properties", scene=sid, entity="Copy", values={"mesh": {"meshAsset": "meshes/pillar.mesh"}})
    await ed.call("wait", frames=2)
    info = await ed.call("get_asset_info", path="meshes/pillar.mesh")
    ed.check_eq([u["entity"]["name"] for u in info["users"]], ["Copy"], f"set_properties shows it: {info['users']}")

    bad = await ed.call("set_mesh_geometry", scene=sid, entity="Pillar", save_as="meshes/pillar.glb", expect_error=True)
    ed.check(".mesh" in bad, f"another extension is refused with the fix: {bad}")


async def test_a_script_assigns_a_mesh_asset(ed: Editor):
    sid = (await ed.call("create_scene", name="Script", type="3D", path="scenes/Script"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="sphere", name="Source")
    await ed.call("set_mesh_geometry", scene=sid, entity="Source", shape="torus", save_as="meshes/ring.mesh")
    await ed.call("create_entity", scene=sid, type="cube", name="Target")
    await ed.call("play", action="start", scene=sid)
    try:
        r = await ed.call("eval_lua", code="local m = object('Target', 'Mesh'); m.meshAsset = 'meshes/ring.mesh'; "
                                            "return {asset = m.meshAsset, width = m:getAABB():getSize().x}")
    finally:
        await ed.call("play", action="stop")
    value = r["values"][0]
    ed.check_eq(value["asset"], "meshes/ring.mesh", f"{r}")
    ed.check(abs(value["width"] - 3.0) < 0.01, f"the torus (radius 1, ring 0.5) at once: {r}")
