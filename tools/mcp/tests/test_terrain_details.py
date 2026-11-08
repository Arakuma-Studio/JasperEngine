# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Terrain authoring for agents (agent-authoring): edit_terrain_details (Unity's Details:
validated detail layers in one undo step, density maps, the drawn instances in sample_terrain),
set_properties refusing invalid foliageLayers, and set_terrain_maps' blend map slots and Terrain
Layer factors."""

import shutil
import struct
import time
import zlib
from pathlib import Path

from harness import Editor, over_budget

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


def write_png(path: Path, size: int = 4, shade: int | None = None) -> None:
    """a small grey checker (or one shade), written without any image library"""
    def texel(x: int, y: int) -> bytes:
        return bytes([shade if shade is not None else (0xc0 if (x + y) % 2 else 0x40)] * 3)
    rows = b"".join(b"\x00" + b"".join(texel(x, y) for x in range(size)) for y in range(size))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


async def drawn_instances(ed: Editor, sid, eid, what: str) -> int:
    """the first layer's drawn instances once there are any: the chunks wait for their mesh and
    its shader variant, which a cold cache on a loaded machine builds for longer than any frame
    budget (the deadline is only a hang guard; the time is reported)"""
    start = time.monotonic()
    while True:
        await ed.call("wait", frames=5)
        drawn = (await details(ed, sid, eid))[0]["drawn"]
        if drawn > 0 or time.monotonic() - start > 60:
            over_budget(f"{what} first drawn", {"seconds": time.monotonic() - start}, 10)
            return drawn


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
    drawn = await drawn_instances(ed, sid, eid, "an OBJ detail layer")
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
    refused as not .obj, .gltf or .glb. This one is textured, skinned and has a blend shape: a chunk
    draws it at rest (red before the fix: its instanced variant needed 17 vertex inputs, a textured
    mesh's blend shapes alone too, so 0 instances drew)"""
    sid, eid = await terrain(ed, "Fbx")
    target = ed.project / "assets" / "models" / "rigged_cube.fbx"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(Path(__file__).resolve().parents[3] / "editor" / "tests" / "fixtures" / "rigged_cube.fbx", target)
    write_png(target.parent / "textures" / "checker.png")
    await ed.call("sync_files")
    added = await ed.call("edit_terrain_details", scene=sid, entity=eid,
                          add=[{"mesh": "assets/models/rigged_cube.fbx", "density": 1, "density_map": {"fill": 1, "resolution": 8}}])
    ed.check_eq(added["layers"][0]["mesh"], "assets/models/rigged_cube.fbx", "the FBX layer")
    drawn = await drawn_instances(ed, sid, eid, "a textured, skinned, blend-shaped FBX detail layer")
    ed.check(drawn > 0, f"the FBX layer draws instances: {drawn}\n"
             f"{await ed.call('read_console', levels=['error'], include_shader_builds=True, limit=10)}")
    # undo removes the layer and the density map the call wrote
    density = ed.project / added["layers"][0]["density_map"]
    ed.check(density.exists(), f"the density map: {density}")
    await ed.call("undo", scene=sid)
    ed.check(not density.exists(), "undo removes the density map the call wrote")


async def cooked(ed: Editor, path: str) -> dict:
    """the desktop cook of `path` once it is cooked (not stale, queued or cooking)"""
    start = time.monotonic()
    while True:
        report = (await ed.call("get_texture_cook_report", paths=[path], platforms=["desktop"]))["textures"][0]["platforms"]["desktop"]
        if report.get("status") == "cooked" or time.monotonic() - start > 60:
            return report
        await ed.call("wait", frames=5)


async def test_density_map_cooked_before_its_layer(ed: Editor):
    """an image cooked as a colour texture, then made a detail layer's density map, is cooked again
    CPU-readable in memory (its .meta untouched). Red before the fix: the layer sampled the
    GPU-only BC cook, which has no CPU pixels, and drew 0 instances"""
    sid, eid = await terrain(ed, "CookedFirst")
    mesh = grass(ed)
    image = ed.project / "assets" / "terrain" / "painted_density.png"
    write_png(image, 16, shade=255)
    await ed.call("sync_files")
    path = "assets/terrain/painted_density.png"
    start = time.monotonic()
    while True:
        first = await ed.call_raw("reimport_textures", {"paths": [path], "platforms": ["desktop"]})
        if not first.is_error or time.monotonic() - start > 60:
            break
        await ed.call("wait", frames=5)
    ed.check(not first.is_error, f"the image is cooked first: {first.text[:300]}")
    gpu = first.structured["textures"][0]["platforms"]["desktop"]
    ed.check(gpu.get("cpu_readable") is False, f"first a colour texture, GPU-only: {gpu}")
    meta = (image.parent / (image.name + ".meta")).read_text()
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]

    await ed.call("edit_terrain_details", scene=sid, entity=eid, add=[{"mesh": mesh, "density_map": path}])
    drawn = await drawn_instances(ed, sid, eid, "a detail layer on an image cooked before it")
    ed.check(drawn > 0, f"the layer draws instances: {drawn}")
    again = await cooked(ed, path)
    ed.check(again.get("cpu_readable") is True, f"cooked again CPU-readable: {again}")
    said = (await ed.call("read_console", since=cursor, contains="again CPU-readable", limit=10))["entries"]
    ed.check(len(said) == 1 and path in said[0]["message"], f"one line says so: {said}")
    ed.check_eq((image.parent / (image.name + ".meta")).read_text(), meta, "its .meta is not written")


def write_hdr(path: Path, size: int, value: float) -> None:
    """a flat (not run-length) Radiance .hdr of one grey value"""
    exponent = 0
    mantissa = value
    while mantissa >= 1.0:
        mantissa /= 2.0
        exponent += 1
    byte = int(mantissa * 256.0)
    texel = bytes([byte, byte, byte, exponent + 128])
    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {size} +X {size}\n".encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + texel * (size * size))


async def test_float_density_map(ed: Editor):
    """an .hdr density map draws, from its source and from its CPU-readable float cook. Red before
    the fix: the float pixels were read as bytes (0) and the float cook had no CPU pixels"""
    sid, eid = await terrain(ed, "FloatDensity")
    mesh = grass(ed)
    write_hdr(ed.project / "assets" / "terrain" / "density.hdr", 16, 1.0)
    await ed.call("sync_files")
    path = "assets/terrain/density.hdr"
    await ed.call("edit_terrain_details", scene=sid, entity=eid, add=[{"mesh": mesh, "density_map": path}])
    drawn = await drawn_instances(ed, sid, eid, "an .hdr density map")
    ed.check(drawn > 0, f"the float map draws: {drawn}")
    report = await cooked(ed, path)
    ed.check(report.get("cpu_readable") is True and report.get("format") == "RGBA16F", f"its cook: {report}")
    await ed.call("wait", frames=10)
    drawn = await drawn_instances(ed, sid, eid, "an .hdr density map's float cook")
    ed.check(drawn > 0, f"its float cook draws: {drawn}\n{(await details(ed, sid, eid))[0]}")


def project_files(root: Path) -> dict:
    """every file of the project but the editor's own folder, by content"""
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and ".jasperengine" not in p.relative_to(root).parts}


async def cooks_settled(ed: Editor) -> None:
    """every texture cook of the project done (each first cook also writes its inferred settings)"""
    start = time.monotonic()
    while time.monotonic() - start < 120:
        report = await ed.call("get_texture_cook_report", platforms=["desktop"])
        busy = [t["path"] for t in report["textures"]
                if t["platforms"]["desktop"].get("status") in ("queued", "cooking", "stale")]
        if not busy:
            return
        await ed.call("wait", frames=10)
    ed.fail("the texture cooks never settled")


async def test_reopening_a_project_writes_no_file(ed: Editor):
    """a settled project whose terrain reads its maps on the CPU opens without writing a file. Red
    before the fix: the editor turned on the maps' Read/Write, rewriting their tracked .meta files"""
    sid, eid = await terrain(ed, "Reopened")
    mesh = grass(ed)
    await ed.call("edit_terrain_details", scene=sid, entity=eid,
                  add=[{"mesh": mesh, "density_map": {"fill": 1, "resolution": 16}}])
    await drawn_instances(ed, sid, eid, "a layer before the project is reopened")
    await ed.call("save_scene", scene=sid, path="scenes/Reopened.scene")
    # an older project has been cooked with its saved references: each texture's inferred
    # settings are written down once (R30), not by the opening under test
    await ed.call("reimport_textures", platforms=["desktop"], timeout_s=120)
    await cooks_settled(ed)
    # an older project's .meta files: no Read/Write on the maps
    for meta in (ed.project / "assets").rglob("*.meta"):
        text = meta.read_text()
        if "readable: true" in text:
            meta.write_text("".join(line for line in text.splitlines(True) if "readable: true" not in line))
    original = ed.project
    other = ed.scratch.root / "another-project"
    await ed.call("create_project", path=str(other), name="Another", unsaved="discard")
    before = project_files(original)
    await ed.call("open_project", path=str(original), unsaved="discard")
    await ed.call("open_scene", scene="scenes/Reopened.scene")
    drawn = await drawn_instances(ed, "scenes/Reopened.scene", "Ground", "the reopened project's layer")
    ed.check(drawn > 0, f"the reopened layer draws: {drawn}")
    await cooks_settled(ed)
    after = project_files(original)
    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    ed.check_eq(changed, [], "no project file was written by opening it")


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
