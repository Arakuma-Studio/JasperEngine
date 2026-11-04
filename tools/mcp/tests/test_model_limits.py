"""What a model is over is seen when it is imported, not only in the game that loads it (S65 R4).

A real limit (a mesh's blend shapes: the shader's vertex inputs) is named in the Console, by the model,
the count and the limit, when import_files brings the model in and when a file appears in the project.
"""

from __future__ import annotations

import json
import struct

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Model Limits"}

MAX_MORPHTARGETS = 8


def write_model(path, targets=0):
    """A triangle with `targets` morph targets."""
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    indices = struct.pack("<3H", 0, 1, 2)
    binary = path.with_suffix(".bin")
    binary.write_bytes(positions + indices)
    primitive = {"attributes": {"POSITION": 0}, "indices": 1}
    if targets:
        primitive["targets"] = [{"POSITION": 0}] * targets
    path.write_text(json.dumps({
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"name": "Face", "primitives": [primitive]}],
        "buffers": [{"uri": binary.name, "byteLength": len(positions) + len(indices)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions), "target": 34962},
                        {"buffer": 0, "byteOffset": len(positions), "byteLength": len(indices), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
    }))
    return path


EXPECTED = f"mesh 'Face' has {MAX_MORPHTARGETS + 1} blend shapes, more than the maximum allowed ({MAX_MORPHTARGETS})"


async def test_an_import_shows_the_limit_a_model_is_over(ed: Editor):
    outside = ed.scratch.root / "to-import"
    outside.mkdir(parents=True, exist_ok=True)
    face = write_model(outside / "face.gltf", targets=MAX_MORPHTARGETS + 1)
    fine = write_model(outside / "fine.gltf", targets=MAX_MORPHTARGETS)

    await ed.console_since(None)
    await ed.call("import_files", sources=[str(face), str(face.with_suffix(".bin")), str(fine), str(fine.with_suffix(".bin"))],
                  to_dir="assets/models")
    errors = [entry["message"] for entry in await ed.console_since() if entry.get("type") == "error"]
    ed.check(any("face.gltf" in message and EXPECTED in message for message in errors), f"the limit in the Console: {errors}")
    ed.check(not any("fine.gltf" in message for message in errors), f"a model at the limit reports nothing: {errors}")


async def test_a_file_that_appears_in_the_project_shows_it_too(ed: Editor):
    folder = ed.project / "assets" / "dropped"
    folder.mkdir(parents=True, exist_ok=True)
    await ed.console_since(None)
    write_model(folder / "dropped.gltf", targets=MAX_MORPHTARGETS + 1)
    await ed.call("sync_files", paths=["assets/dropped/dropped.gltf"])
    found = [entry["message"] for entry in await ed.console_since() if entry.get("type") == "error"]
    ed.check(any("dropped.gltf" in message and EXPECTED in message for message in found), f"{found}")
