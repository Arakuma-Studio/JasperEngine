"""render-correctness (docs/superpowers/specs/2026-11-04-render-correctness-design.md): the
agent surfaces of the new controls.

- Project Settings > Quality > Foliage Distance per platform group (set/get_project_settings,
  range 0-10), the runtime Engine.foliageDistanceScale in Play (the desktop default, a script's
  override, reset when Play stops);
- Instanced Mesh Near Fade Start/End and a foliage layer's Min Draw Distance through
  set_properties, with undo;
- get_render_stats' gpu.robust_buffer_access (null off Vulkan);
- an Instanced Mesh on a textured model with a blend shape, past the 16 vertex inputs, says so
  once in the Console, naming the GameObject and the fix.
"""

import base64
import json
import struct
import time
import zlib

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Render Correctness Test"}


async def foliage_scale(ed: Editor) -> float:
    return (await ed.call("eval_lua", code="return Engine.foliageDistanceScale"))["values"][0]


async def test_foliage_distance_settings(ed: Editor):
    keys = ["quality.foliage_distance_desktop", "quality.foliage_distance_mobile", "quality.foliage_distance_web"]
    values = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq([values[k] for k in keys], [1, 1, 1], "Foliage Distance defaults to 1 on every platform group")

    await ed.call("set_project_settings", values={"quality.foliage_distance_desktop": 0.75, "quality.foliage_distance_mobile": 0.5})
    values = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq([values[k] for k in keys], [0.75, 0.5, 1], "the scales set")
    refused = await ed.call_raw("set_project_settings", {"values": {"quality.foliage_distance_web": 11}})
    ed.check(refused.is_error, f"a scale above 10 is refused: {refused.text[:200]}")

    await ed.call("create_entity", type="cube")
    await ed.call("play", action="start")
    try:
        ed.check_eq(await foliage_scale(ed), 0.75, "Play runs the desktop scale")
        await ed.call("eval_lua", code="Engine.foliageDistanceScale = 0.25")
        ed.check_eq(await foliage_scale(ed), 0.25, "a script overrides it")
    finally:
        await ed.call("play", action="stop")
    await ed.call("play", action="start")
    try:
        ed.check_eq(await foliage_scale(ed), 0.75, "the override ends with Play")
    finally:
        await ed.call("play", action="stop")


async def test_near_fade_properties(ed: Editor):
    scene = (await ed.call("create_scene", name="NearFade", type="3D", path="scenes/NearFade"))["scene"]["id"]
    batch = (await ed.call("create_entity", scene=scene, type="cube", name="Grass"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=batch, component="instancedmesh",
                  values={"instancedmesh": {"distanceFade": True, "fadeStart": 30, "fadeEnd": 40}})
    await ed.call("set_properties", scene=scene, entity=batch, values={"instancedmesh": {"nearFadeStart": 8, "nearFadeEnd": 10}})
    now = (await ed.call("get_entity", scene=scene, entity=batch, properties=["nearFadeStart", "nearFadeEnd"]))["components"]["instancedmesh"]
    ed.check_eq((now["nearFadeStart"], now["nearFadeEnd"]), (8, 10), "the near fade set")
    await ed.call("undo", scene=scene)
    now = (await ed.call("get_entity", scene=scene, entity=batch, properties=["nearFadeStart", "nearFadeEnd"]))["components"]["instancedmesh"]
    ed.check_eq((now["nearFadeStart"], now["nearFadeEnd"]), (0, 0), "one undo restores the near fade")
    refused = await ed.call_raw("set_properties", {"scene": scene, "entity": batch, "values": {"instancedmesh": {"nearFadeStart": -1}}})
    ed.check(refused.is_error, f"a negative near fade is refused: {refused.text[:200]}")


async def test_foliage_min_draw_distance(ed: Editor):
    scene = (await ed.call("create_scene", name="Foliage", type="3D", path="scenes/Foliage"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=scene, type="terrain"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=terrain,
                  values={"terrain": {"foliageLayers": [{"drawDistance": 300, "minDrawDistance": 40}]}})
    layers = (await ed.call("get_entity", scene=scene, entity=terrain, properties=["foliageLayers"]))["components"]["terrain"]["foliageLayers"]
    ed.check_eq((layers[0]["drawDistance"], layers[0]["minDrawDistance"]), (300, 40), f"the layer's draw range: {layers}")
    await ed.call("undo", scene=scene)
    layers = (await ed.call("get_entity", scene=scene, entity=terrain, properties=["foliageLayers"]))["components"]["terrain"].get("foliageLayers")
    ed.check_eq(layers or [], [], "one undo removes the layer")


async def test_render_stats_robustness_field(ed: Editor):
    stats = await ed.call("get_render_stats")
    ed.check_in("robust_buffer_access", stats["gpu"])
    if stats["gpu"].get("backend") != "Vulkan":
        ed.check_eq(stats["gpu"]["robust_buffer_access"], None, "robust buffer access is a Vulkan device's")
    else:
        ed.check(isinstance(stats["gpu"]["robust_buffer_access"], bool), f"the Vulkan decision: {stats['gpu']}")


def blend_shape_gltf() -> dict:
    """one textured triangle with a blend shape (texture and buffer embedded)"""
    positions = [0, 0, 0, 1, 0, 0, 0, 1, 0]
    normals = [0, 0, 1] * 3
    uvs = [0, 0, 1, 0, 0, 1]
    deltas = [0, 0.5, 0] * 3
    data = struct.pack("<9f", *positions) + struct.pack("<9f", *normals) + struct.pack("<6f", *uvs) + \
        struct.pack("<9f", *deltas) + struct.pack("<3H", 0, 1, 2) + b"\0\0"

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"\x00\x80\x80\x80")) + chunk(b"IEND", b"")
    views = [(0, 36), (36, 36), (72, 24), (96, 36), (132, 6)]
    return {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Blade", "mesh": 0}],
        "meshes": [{"weights": [0.0], "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "targets": [{"POSITION": 3}],
            "indices": 4, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "textures": [{"source": 0}],
        "images": [{"uri": "data:image/png;base64," + base64.b64encode(png).decode()}],
        "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode(),
                     "byteLength": len(data)}],
        "bufferViews": [{"buffer": 0, "byteOffset": o, "byteLength": n, "target": 34963 if i == 4 else 34962}
                        for i, (o, n) in enumerate(views)],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0.5, 0], "max": [0, 0.5, 0]},
            {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"}],
    }


async def test_instanced_blend_shapes_say_why_they_do_not_draw(ed: Editor):
    """red before the fix: only the shader compiler's lines (a vertex input past location 15)"""
    sid = (await ed.call("create_scene", name="Blend", type="3D", path="scenes/Blend"))["scene"]["id"]
    path = ed.project / "assets" / "models" / "blade_shape.gltf"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(blend_shape_gltf()))
    await ed.call("sync_files")
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    blade = (await ed.call("create_entity", scene=sid, type="model", name="ShapedBlade",
                           model="assets/models/blade_shape.gltf"))["entity_id"]
    await ed.call("add_component", scene=sid, entity=blade, component="instancedmesh")
    await ed.call("set_properties", scene=sid, entity=blade,
                  values={"instancedmesh": {"instances": [{"position": [x, 0, 0]} for x in range(3)]}})
    start = time.monotonic()
    said = []
    while time.monotonic() - start < 60:
        await ed.call("wait", frames=5)
        log = await ed.call("read_console", since=cursor, levels=["error"], contains="blend shapes", limit=10)
        said = [e["message"] for e in log["entries"]]
        if said:
            break
    ed.check(said, "the Console says why the instanced mesh does not draw")
    for needle in ("ShapedBlade", "16", "Instanced Mesh", "bake"):
        ed.check_in(needle, said[0], "the message names the GameObject and the fix")
    await ed.call("wait", frames=20)
    log = await ed.call("read_console", since=cursor, levels=["error"], contains="blend shapes", limit=10)
    ed.check_eq(len(log["entries"]), 1, "said once, not every frame")
