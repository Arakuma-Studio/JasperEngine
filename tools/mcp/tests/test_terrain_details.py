# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Terrain authoring for agents (agent-authoring): edit_terrain_details (Unity's Details:
validated detail layers in one undo step, density maps, the drawn instances in sample_terrain),
set_properties refusing invalid foliageLayers, and set_terrain_maps' blend map slots and Terrain
Layer factors."""

import shutil
from pathlib import Path

from harness import Editor

EDITOR = {"create": True, "name": "Terrain Details"}
CI = True

# one triangle: the smallest model a detail layer draws
GRASS_OBJ = "v 0 0 0\nv 0.2 0 0\nv 0.1 0.5 0\nf 1 2 3\n"


async def terrain(ed: Editor, name: str):
    sid = (await ed.call("create_scene", name=name, type="3d"))["scene"]["id"]
    eid = (await ed.call("create_entity", scene=sid, name="Ground", type="terrain",
                         values={"terrain": {"terrainSize": 32, "maxHeight": 4,
                                             "resolution": 8, "rootGridSize": 1, "levels": 1}}))["entity_id"]
    await ed.call("set_terrain_maps", scene=sid, entity=eid, heightmap={"generate": "flat", "base_height": 0.25, "resolution": 16})
    return sid, eid


def grass(ed: Editor) -> str:
    path = ed.project / "assets" / "models" / "grass.obj"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(GRASS_OBJ)
    return "assets/models/grass.obj"


async def details(ed: Editor, sid, eid) -> list:
    return (await ed.call("sample_terrain", scene=sid, entity=eid, grid=0))["info"]["details"]


async def test_group_lists_the_tool(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_terrain_details" not in default, "hidden until the terrain group is enabled")
    await ed.call("tool_groups", enable=["terrain"])
    ed.check("edit_terrain_details" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
    await ed.call("tool_groups", disable=["terrain"])


async def test_add_update_remove(ed: Editor):
    sid, eid = await terrain(ed, "Details")
    mesh = grass(ed)
    await ed.call("sync_files")
    listed = await ed.call("edit_terrain_details", scene=sid, entity=eid)
    ed.check_eq(listed["layers"], [], "a new terrain has no detail layers")

    added = await ed.call("edit_terrain_details", scene=sid, entity=eid, add=[{
        "mesh": mesh, "density": 2, "density_map": {"fill": 1, "resolution": 16}, "max_slope": 30,
        "cast_shadows": False, "seed": 7}])
    ed.check_eq(added["added"], [0], "the new layer's index")
    layer = added["layers"][0]
    ed.check(layer["mesh"] == mesh and layer["density"] == 2 and layer["max_slope"] == 30 and layer["cast_shadows"] is False,
             f"the fields: {layer}")
    ed.check(bool(layer["density_map"]) and (ed.project / layer["density_map"]).exists(),
             f"the density map was written: {layer['density_map']}")
    ed.check("undo" in added, "one undo step")

    # the editor scatters instances around its camera
    drawn = 0
    for _ in range(20):
        await ed.call("wait", frames=5)
        drawn = (await details(ed, sid, eid))[0]["drawn"]
        if drawn > 0:
            break
    ed.check(drawn > 0, f"the layer draws instances: {drawn}")

    updated = await ed.call("edit_terrain_details", scene=sid, entity=eid, update=[{"index": 0, "density": 4, "min_scale": 0.5}])
    ed.check(updated["layers"][0]["density"] == 4 and updated["layers"][0]["min_scale"] == 0.5, f"{updated['layers'][0]}")
    await ed.call("undo", scene=sid)
    ed.check_eq((await details(ed, sid, eid))[0]["density"], 2, "undo of the update")

    # remove then add in one call: indices before the call for remove
    both = await ed.call("edit_terrain_details", scene=sid, entity=eid, remove=[0], add=[{"mesh": mesh}])
    ed.check_eq(len(both["layers"]), 1, "removed one, added one")
    ed.check(any("density map" in n for n in both.get("notes", [])), f"a layer without a density map is noted: {both}")
    await ed.call("undo", scene=sid)
    await ed.call("undo", scene=sid)
    ed.check_eq(await details(ed, sid, eid), [], "every call was one undo step")


async def test_fbx_layer(ed: Editor):
    """an FBX is a detail mesh as in the Terrain window (it draws from its import); red before the fix:
    refused as not .obj, .gltf or .glb"""
    sid, eid = await terrain(ed, "Fbx")
    target = ed.project / "assets" / "models" / "rigged_cube.fbx"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(Path(__file__).resolve().parents[3] / "editor" / "tests" / "fixtures" / "rigged_cube.fbx", target)
    await ed.call("sync_files")
    added = await ed.call("edit_terrain_details", scene=sid, entity=eid,
                          add=[{"mesh": "assets/models/rigged_cube.fbx", "density": 1, "density_map": {"fill": 1, "resolution": 8}}])
    ed.check_eq(added["layers"][0]["mesh"], "assets/models/rigged_cube.fbx", "the FBX layer")
    drawn = 0
    for _ in range(20):
        await ed.call("wait", frames=5)
        drawn = (await details(ed, sid, eid))[0]["drawn"]
        if drawn > 0:
            break
    ed.check(drawn > 0, f"the FBX layer draws instances: {drawn}")
    # undo removes the layer and the density map the call wrote
    density = ed.project / added["layers"][0]["density_map"]
    ed.check(density.exists(), f"the density map: {density}")
    await ed.call("undo", scene=sid)
    ed.check(not density.exists(), "undo removes the density map the call wrote")


async def test_refusals(ed: Editor):
    sid, eid = await terrain(ed, "Refusals")
    mesh = grass(ed)
    (ed.project / "assets" / "notes.txt").write_text("not a model\n")
    cases = [
        ({"add": [{"mesh": mesh, "density": 500}]}, ["density", "128", "8192"]),
        ({"add": [{"mesh": mesh, "min_scale": 3, "max_scale": 1}]}, ["min_scale", "max_scale"]),
        ({"add": [{"mesh": "assets/notes.txt"}]}, [".obj", ".glb"]),
        ({"add": [{"density": 1}]}, ["mesh is required"]),
        ({"add": [{"mesh": mesh, "max_slope": 120}]}, ["max_slope", "90"]),
        ({"add": [{"mesh": mesh, "colour": 1}]}, ["unknown field", "density"]),
        ({"remove": [3]}, ["remove", "detail layer index"]),
        ({"update": [{"index": 0, "density": 1}]}, ["update[0].index"]),
    ]
    for args, needles in cases:
        err = await ed.call("edit_terrain_details", scene=sid, entity=eid, expect_error=True, **args)
        for needle in needles:
            ed.check_in(needle, err, f"error of {args}")
    ed.check_eq(await details(ed, sid, eid), [], "a refused call changes nothing")
    # every item is checked before anything is written: a density map of a refused call is not on disk
    before = {p.name for p in ed.project.rglob("*density*.png")}
    await ed.call("edit_terrain_details", scene=sid, entity=eid, expect_error=True,
                  add=[{"mesh": mesh, "density_map": {"fill": 1, "resolution": 8}}, {"mesh": mesh, "density": 500}])
    ed.check_eq({p.name for p in ed.project.rglob("*density*.png")}, before, "no density map written by a refused call")


async def test_set_properties_refuses_invalid_layers(ed: Editor):
    """red before the fix: set_properties wrote foliageLayers with any values"""
    sid, eid = await terrain(ed, "Guard")
    err = await ed.call("set_properties", scene=sid, entity=eid, expect_error=True,
                        values={"terrain": {"foliageLayers": [{"density": -5, "minScale": 1, "maxScale": 2}]}})
    ed.check_in("density", err, "the bad field is named")
    ed.check_in("edit_terrain_details", err, "the tool that edits detail layers is named")
    ed.check_eq(await details(ed, sid, eid), [], "nothing was written")


async def test_blend_map_slots_and_layer_factors(ed: Editor):
    """red before the fix: blendmap had no index (always slot 0) and layers no factors"""
    sid, eid = await terrain(ed, "Layers")
    await ed.call("set_terrain_maps", scene=sid, entity=eid, blendmap={"color": [1, 0, 0], "resolution": 4})
    out = await ed.call("set_terrain_maps", scene=sid, entity=eid, blendmap={"color": [0, 1, 0], "index": 1, "resolution": 4})
    ed.check_eq(out["blendmap"]["index"], 1, "the slot written")
    maps = (await ed.call("sample_terrain", scene=sid, entity=eid, grid=0))["info"]["blend_maps"]
    ed.check(len(maps) == 2 and maps[0] != maps[1], f"slot 1 set, slot 0 kept: {maps}")
    err = await ed.call("set_terrain_maps", scene=sid, entity=eid, expect_error=True, blendmap={"color": [0, 0, 1], "index": 3})
    ed.check_in("index", err, "slot out of range")

    sid2, eid2 = await terrain(ed, "Gap")
    err = await ed.call("set_terrain_maps", scene=sid2, entity=eid2, expect_error=True, blendmap={"color": [0, 0, 1], "index": 2})
    ed.check_in("needs slot 1", err, "no gaps")

    await ed.call("set_terrain_maps", scene=sid, entity=eid, layers=[{
        "index": 3, "uv_scale": [4, 2], "uv_offset": [0.5, 0], "tint": [1, 0.9, 0.8], "normal_strength": 1.5,
        "roughness_factor": 0.6, "metallic_factor": 0.1, "occlusion_strength": 0.7}])
    got = await ed.call("get_entity", scene=sid, entity=eid, components=["terrain"], detail="values",
                        properties=["surfaceLayers[3].uvScale", "surfaceLayers[3].roughnessFactor",
                                    "surfaceLayers[3].normalStrength", "surfaceLayers[3].colorFactor"])
    values = next(iter(got["components"].values()))
    ed.check(values["surfaceLayers[3].uvScale"] == [4, 2], f"uv_scale: {values}")
    ed.check(abs(values["surfaceLayers[3].roughnessFactor"] - 0.6) < 1e-5, f"roughness: {values}")
    ed.check(abs(values["surfaceLayers[3].normalStrength"] - 1.5) < 1e-5, f"normal strength: {values}")
    err = await ed.call("set_terrain_maps", scene=sid, entity=eid, expect_error=True, layers=[{"index": 0, "uv_scale": [0, 1]}])
    ed.check_in("uv_scale", err, "uv_scale range")
    err = await ed.call("set_terrain_maps", scene=sid, entity=eid, expect_error=True, layers=[{"index": 0, "tint": [2, 0, 0]}])
    ed.check_in("tint", err, "tint range")
