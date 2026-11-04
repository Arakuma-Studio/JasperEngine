"""Models the MCP tests write into their project copies: pure functions of their arguments
(docs/specs/S53-model-import-cook.md)."""

from __future__ import annotations

import json
import math
import struct


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
