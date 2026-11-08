"""Undo and redo of a mesh or model change free the GPU objects of the mesh they replace
(mesh-memory spec, Follow-ups 5): MeshChangeCmd (set_mesh_geometry) and ModelLoadCmd (set_model)
assign a decoded component over a loaded one, and before the fix its pipelines, buffers and
shaders stayed alive on every undo and redo. The device's alive counts (get_render_stats) come back
to what they were in the same state."""

import json
import struct

from harness import Editor

EDITOR = {"create": True, "name": "Mesh Undo Resources"}
CI = True

KINDS = ("pipelines", "buffers", "images", "shaders")


async def alive(ed: Editor) -> dict:
    """The device's alive objects once loads and frees have settled (two equal reads in a row)."""
    last = None
    for _ in range(30):
        await ed.call("wait", frames=5, timeout_seconds=300)
        counts = (await ed.call("get_render_stats"))["alive"]
        now = {kind: counts[kind] for kind in KINDS}
        if now == last:
            return now
        last = now
    return last


def one_mesh_gltf(path, vertices: int) -> None:
    """A glTF of one mesh node (its geometry on the Model's own mesh): a fan of `vertices` points."""
    path.parent.mkdir(parents=True, exist_ok=True)
    points = [(0.0, 0.0, 0.0)] + [(float(i % 2), float(i // 2), 0.0) for i in range(1, vertices)]
    indices = []
    for i in range(1, vertices - 1):
        indices += [0, i, i + 1]
    if len(indices) % 2:
        indices.append(0)
    positions = b"".join(struct.pack("<3f", *p) for p in points)
    index_bytes = struct.pack(f"<{len(indices)}H", *indices)
    path.with_suffix(".bin").write_bytes(positions + index_bytes)
    gltf = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": path.stem, "mesh": 0}],
        "meshes": [{"name": path.stem, "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"uri": path.with_suffix(".bin").name, "byteLength": len(positions) + len(index_bytes)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions), "target": 34962},
                        {"buffer": 0, "byteOffset": len(positions), "byteLength": len(index_bytes), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": vertices, "type": "VEC3",
                       "min": [0, 0, 0], "max": [1, float((vertices - 1) // 2), 0]},
                      {"bufferView": 1, "componentType": 5123, "count": (vertices - 2) * 3, "type": "SCALAR"}],
    }
    path.write_text(json.dumps(gltf))


async def cycle(ed: Editor, sid: int, before: dict, after: dict, what: str) -> None:
    """Three undo/redo rounds: each lands on the counts of its state."""
    seen = []
    for _ in range(3):
        await ed.call("undo", scene=sid)
        undone = await alive(ed)
        await ed.call("redo", scene=sid)
        seen.append((undone, await alive(ed)))
    rounds = f"before {before}, after {after}, (undo, redo) rounds {seen}"
    for round_, (undone, redone) in enumerate(seen, 1):
        ed.check_eq(undone, before, f"{what}: alive after undo {round_} (as before the change; {rounds})")
        ed.check_eq(redone, after, f"{what}: alive after redo {round_} (as after the change; {rounds})")


async def test_mesh_change_undo_frees_the_replaced_mesh(ed: Editor):
    sid = (await ed.call("create_scene", name="MeshUndo", type="3D", path="scenes/MeshUndo"))["scene"]["id"]
    shape = (await ed.call("create_entity", scene=sid, type="cube", name="Shape"))["entity_id"]
    before = await alive(ed)
    await ed.call("set_mesh_geometry", scene=sid, entity=shape, shape="sphere", radius=1)
    after = await alive(ed)
    ed.check(after["buffers"] > 0 and after["pipelines"] > 0, f"the sphere is drawn: {after}")
    await cycle(ed, sid, before, after, "set_mesh_geometry")


async def test_model_load_undo_frees_the_replaced_model_mesh(ed: Editor):
    models = ed.project / "assets" / "models"
    one_mesh_gltf(models / "Fan5.gltf", 5)
    one_mesh_gltf(models / "Fan9.gltf", 9)
    await ed.call("sync_files", paths=["assets/models/Fan5.gltf", "assets/models/Fan9.gltf"])
    sid = (await ed.call("create_scene", name="ModelUndo", type="3D", path="scenes/ModelUndo"))["scene"]["id"]
    loaded = await ed.call("set_model", scene=sid, path="assets/models/Fan5.gltf", create={"name": "Fan"})
    model = loaded["model"]
    entity = model.get("id", model.get("entity_id")) if isinstance(model, dict) else model
    ed.check(entity is not None and not loaded["loading"], loaded)
    changed = await ed.call("set_model", scene=sid, entity=entity, path="assets/models/Fan9.gltf")
    ed.check(not changed["loading"], changed)
    # the counts once both files have loaded: the first load of a file keeps an image of its own
    # (it does not grow with undo and redo), which is not the replaced mesh's
    await ed.call("undo", scene=sid)
    before = await alive(ed)
    await ed.call("redo", scene=sid)
    after = await alive(ed)
    await cycle(ed, sid, before, after, "set_model")
