"""What a model is over is seen when it is imported, and what a mesh holds inline is no limit (S45).

import_files and a file that appears in the project report a real limit (a skin's bones, a mesh's
blend shapes) by the model, the count and the limit: in the reply, in get_asset_info and in the Console.
A model of more primitives than a mesh holds inline builds with the prebuilt player's method, and
the editor keeps all of it, in a scene saved and opened again.
"""

from __future__ import annotations

import json
import struct

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Model Limits"}

MAX_BONES = 128
MAX_MORPHTARGETS = 8
MAX_SUBMESHES = 16


def write_model(path, primitives=1, targets=0, joints=0):
    """`primitives` triangles of one mesh, `targets` morph targets on each and a skin of `joints`."""
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    indices = struct.pack("<3H", 0, 1, 2)
    binary = path.with_suffix(".bin")
    binary.write_bytes(positions + indices)
    primitive = {"attributes": {"POSITION": 0}, "indices": 1}
    if targets:
        primitive["targets"] = [{"POSITION": 0}] * targets
    nodes = [{"mesh": 0, **({"skin": 0} if joints else {})}] + [{"name": f"Joint {j}"} for j in range(joints)]
    gltf = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": nodes,
        "meshes": [{"name": "Face", "primitives": [primitive] * primitives}],
        "buffers": [{"uri": binary.name, "byteLength": len(positions) + len(indices)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions), "target": 34962},
                        {"buffer": 0, "byteOffset": len(positions), "byteLength": len(indices), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
    }
    if joints:
        gltf["skins"] = [{"name": "Armature", "joints": list(range(1, joints + 1))}]
    path.write_text(json.dumps(gltf))
    return path


async def test_an_import_reports_the_limits_a_model_is_over(ed: Editor):
    outside = ed.scratch.root / "to-import"
    outside.mkdir(parents=True, exist_ok=True)
    hero = write_model(outside / "hero.gltf", joints=MAX_BONES + 2)
    face = write_model(outside / "face.gltf", targets=MAX_MORPHTARGETS + 1)
    fine = write_model(outside / "fine.gltf", primitives=MAX_SUBMESHES + 4)

    await ed.console_since(None)
    data = await ed.call("import_files", sources=[str(hero), str(hero.with_suffix(".bin")), str(face), str(face.with_suffix(".bin")),
                                                  str(fine), str(fine.with_suffix(".bin"))], to_dir="assets/models")
    limits = {entry["model"]: entry for entry in data.get("model_limits", [])}
    ed.check_eq(sorted(limits), ["assets/models/face.gltf", "assets/models/hero.gltf"], f"{data}")

    bones = limits["assets/models/hero.gltf"]
    ed.check_eq((bones["kind"], bones["count"], bones["limit"]), ("bones", MAX_BONES + 2, MAX_BONES))
    ed.check_in("hero.gltf: skin 'Armature'", bones["message"])
    shapes = limits["assets/models/face.gltf"]
    ed.check_eq((shapes["kind"], shapes["count"], shapes["limit"]), ("blend shapes", MAX_MORPHTARGETS + 1, MAX_MORPHTARGETS))
    ed.check_in("mesh 'Face'", shapes["message"])

    # a person sees them in the Console
    errors = [entry["message"] for entry in await ed.console_since() if entry.get("type") == "error"]
    ed.check(any(bones["message"] in message for message in errors), f"the bones in the Console: {errors}")
    ed.check(any(shapes["message"] in message for message in errors), f"the blend shapes in the Console: {errors}")
    ed.check(not any("fine.gltf" in message for message in errors), f"more primitives than a mesh holds inline is no limit: {errors}")

    info = await ed.call("get_asset_info", path="assets/models/hero.gltf")
    ed.check_eq(info["model_limits"][0]["count"], MAX_BONES + 2, f"{info}")
    info = await ed.call("get_asset_info", path="assets/models/fine.gltf")
    ed.check("model_limits" not in info, f"{info}")


async def test_a_file_that_appears_in_the_project_reports_them_too(ed: Editor):
    folder = ed.project / "assets" / "dropped"
    folder.mkdir(parents=True, exist_ok=True)
    await ed.console_since(None)
    write_model(folder / "dropped.gltf", joints=MAX_BONES + 1)
    await ed.call("sync_files", paths=["assets/dropped"])
    found = [entry["message"] for entry in await ed.console_since() if "dropped.gltf" in entry["message"]]
    ed.check(any(f"has {MAX_BONES + 1} bones" in message and f"({MAX_BONES})" in message for message in found), f"{found}")


async def test_a_model_past_a_meshs_inline_capacity_needs_no_compile(ed: Editor):
    write_model(ed.project / "assets" / "many.gltf", primitives=MAX_SUBMESHES + 4)
    await ed.call("sync_files", paths=["assets/many.gltf"])
    loaded = await ed.call("set_model", create={"name": "Many"}, path="assets/many.gltf")
    ed.check(loaded, f"{loaded}")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])

    # the file keeps them all, and the build method does not take them for a reason to compile
    info = await ed.call("get_build_info")
    for platform, choice in info["platforms"].items():
        ed.check(not any("MAX_SUBMESHES" in reason or "MAX_EXTERNAL_BUFFERS" in reason for reason in choice["compile_reasons"]),
                 f"{platform}: a mesh's inline capacity is no reason to compile: {choice}")
    scene_file = ed.project / saved["scenes_in_build"][0]["filepath"] if "filepath" in saved["scenes_in_build"][0] else None
    if scene_file and scene_file.is_file():
        ed.check(f"maxSubmeshes: {MAX_SUBMESHES + 4}" in scene_file.read_text(), "the capacity table stays valid")
