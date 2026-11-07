"""A model whose glTF has several mesh nodes keeps its geometry on child "parts" (scripting
catch-up, Doriax cc550567a): set_properties on the root's empty mesh warns and names the parts,
and the Model/Mesh Lua API reaches the parts (before, setColor on the model changed nothing drawn)."""

import json
import struct

from harness import Editor

EDITOR = {"create": True, "name": "Model Parts"}
CI = True


def two_part_gltf(folder) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    normals = struct.pack("<9f", 0, 0, 1, 0, 0, 1, 0, 0, 1)
    indices = struct.pack("<4H", 0, 1, 2, 0)
    (folder / "parts.bin").write_bytes(positions + normals + indices)
    primitive = {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}
    gltf = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 1]}],
        "nodes": [{"name": "Body", "mesh": 0}, {"name": "Eyes", "mesh": 1, "translation": [2, 0, 0]}],
        "meshes": [{"name": "Body", "primitives": [primitive]}, {"name": "Eyes", "primitives": [primitive]}],
        "buffers": [{"uri": "parts.bin", "byteLength": 80}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
                        {"buffer": 0, "byteOffset": 72, "byteLength": 6, "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}],
    }
    (folder / "parts.gltf").write_text(json.dumps(gltf))


async def test_model_parts(ed: Editor):
    two_part_gltf(ed.project / "assets" / "models")
    await ed.call("sync_files", paths=["assets/models/parts.gltf"])
    loaded = await ed.call("set_model", path="assets/models/parts.gltf", create={"name": "Robot"})
    model = loaded["model"]
    root = model.get("id", model.get("entity_id")) if isinstance(model, dict) else model
    ed.check(root is not None and not loaded["loading"], loaded)

    out = await ed.call("set_properties", entity=root, values={"mesh": {"castShadows": False}})
    warnings = out.get("warnings", [])
    ed.check(any("Body" in w and "Eyes" in w for w in warnings), f"the warning names the parts: {warnings}")

    await ed.call("play", action="start")
    result = await ed.call("eval_lua", code="""
        local model = object('Robot', 'Model')
        model:setColor(1, 0, 0, 1)
        model.castShadows = false
        local b = object(model:findChild('Body').entity, 'Mesh')
        local e = object(model:findChild('Eyes').entity, 'Mesh')
        return { model:getNumSubmeshes(), b.color.x, e.color.x, b.castShadows, e.castShadows }
    """)
    value = result["values"][0]
    ed.check_eq(value[0], 2, "submeshes counted across the parts")
    ed.check(value[1] > 0.99 and value[2] > 0.99, f"both parts tinted: {value}")
    ed.check_eq([value[3], value[4]], [False, False], "a mesh-wide setter reaches every part")
    await ed.call("play", action="stop")
