#!/usr/bin/env python3
"""Generates the model of the extra-pass fixture: a hard-edged box column (every face has its
own vertices and flat normals, as an exported low-poly model does) skinned to two bones
(Root at the base, Tip half way up, which scripts/ExtraPass.lua bends) with one morph target
that widens its top.

    python3 tools/fixtures/extra-pass/make_assets.py

Deterministic: rerunning rewrites the same file.
"""

import base64
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, "assets", "models")

HALF = 0.25
RINGS = [0.0, 0.5, 1.0, 1.5, 2.0]


def weights_at(y):
    # Root below 0.5, Tip from 1.5, blended in between
    t = min(max((y - 0.5) / 1.0, 0.0), 1.0)
    return (1.0 - t, t)


def build():
    positions, normals, joints, weights, morph, indices = [], [], [], [], [], []

    def vertex(p, n):
        positions.append(p)
        normals.append(n)
        w = weights_at(p[1])
        joints.append((0, 1, 0, 0))
        weights.append((w[0], w[1], 0.0, 0.0))
        # the morph target widens the top ring
        grow = 0.6 * max(p[1] - 1.0, 0.0)
        morph.append((p[0] * grow, 0.0, p[2] * grow))
        return len(positions) - 1

    # the four sides: corners a, b going counter-clockwise seen from outside
    sides = [
        ((-HALF, HALF), (HALF, HALF), (0.0, 0.0, 1.0)),    # +Z
        ((HALF, HALF), (HALF, -HALF), (1.0, 0.0, 0.0)),    # +X
        ((HALF, -HALF), (-HALF, -HALF), (0.0, 0.0, -1.0)), # -Z
        ((-HALF, -HALF), (-HALF, HALF), (-1.0, 0.0, 0.0)), # -X
    ]
    for a, b, n in sides:
        column = []
        for y in RINGS:
            column.append((vertex((a[0], y, a[1]), n), vertex((b[0], y, b[1]), n)))
        for r in range(len(RINGS) - 1):
            a0, b0 = column[r]
            a1, b1 = column[r + 1]
            indices += [a0, b0, b1, a0, b1, a1]

    # caps
    top = RINGS[-1]
    t = [vertex((-HALF, top, HALF), (0.0, 1.0, 0.0)), vertex((HALF, top, HALF), (0.0, 1.0, 0.0)),
         vertex((HALF, top, -HALF), (0.0, 1.0, 0.0)), vertex((-HALF, top, -HALF), (0.0, 1.0, 0.0))]
    indices += [t[0], t[1], t[2], t[0], t[2], t[3]]
    b = [vertex((-HALF, 0.0, HALF), (0.0, -1.0, 0.0)), vertex((-HALF, 0.0, -HALF), (0.0, -1.0, 0.0)),
         vertex((HALF, 0.0, -HALF), (0.0, -1.0, 0.0)), vertex((HALF, 0.0, HALF), (0.0, -1.0, 0.0))]
    indices += [b[0], b[1], b[2], b[0], b[2], b[3]]
    return positions, normals, joints, weights, morph, indices


def main():
    positions, normals, joints, weights, morph, indices = build()
    count = len(positions)

    blobs = []
    views = []
    accessors = []

    def add(data, target, component, kind, count_, minmax=None):
        offset = sum(len(x) for x in blobs)
        while offset % 4:
            blobs.append(b"\0")
            offset += 1
        blobs.append(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count_, "type": kind}
        if minmax:
            accessor["min"], accessor["max"] = minmax
        accessors.append(accessor)
        return len(accessors) - 1

    def vec3(values):
        return b"".join(struct.pack("<3f", *v) for v in values)

    def bounds(values):
        return ([min(v[i] for v in values) for i in range(3)], [max(v[i] for v in values) for i in range(3)])

    position = add(vec3(positions), 34962, 5126, "VEC3", count, bounds(positions))
    normal = add(vec3(normals), 34962, 5126, "VEC3", count)
    joint = add(b"".join(struct.pack("<4H", *j) for j in joints), 34962, 5123, "VEC4", count)
    weight = add(b"".join(struct.pack("<4f", *w) for w in weights), 34962, 5126, "VEC4", count)
    target = add(vec3(morph), 34962, 5126, "VEC3", count, bounds(morph))
    index = add(b"".join(struct.pack("<H", i) for i in indices), 34963, 5123, "SCALAR", len(indices))
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    tip_inverse = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]
    inverse = add(struct.pack("<32f", *(identity + tip_inverse)), None, 5126, "MAT4", 2)

    data = b"".join(blobs)
    gltf = {
        "asset": {"version": "2.0", "generator": "extra-pass fixture make_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"name": "Column", "mesh": 0, "skin": 0},
            {"name": "Root", "children": [2]},
            {"name": "Tip", "translation": [0, 1, 0]},
        ],
        "meshes": [{
            "name": "Column",
            "primitives": [{
                "attributes": {"POSITION": position, "NORMAL": normal, "JOINTS_0": joint, "WEIGHTS_0": weight},
                "indices": index,
                "material": 0,
                "targets": [{"POSITION": target}],
            }],
            "weights": [0.0],
            "extras": {"targetNames": ["Widen"]},
        }],
        "skins": [{"joints": [1, 2], "inverseBindMatrices": inverse, "skeleton": 1}],
        "materials": [{"name": "Teal", "pbrMetallicRoughness": {"baseColorFactor": [0.15, 0.65, 0.6, 1.0],
                                                               "metallicFactor": 0.0, "roughnessFactor": 0.7}}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
    }
    os.makedirs(MODELS, exist_ok=True)
    with open(os.path.join(MODELS, "skinned_column.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
