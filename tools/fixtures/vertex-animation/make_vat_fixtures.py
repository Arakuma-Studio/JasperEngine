#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Small glTF models for the Vertex Animation baker tests (editor/tests/VatBakerTest.cpp).

  assets/models/rigid_arm.gltf   two boxes in a hierarchy (Rig > Base > Arm), no skin:
                                 "Swing" (Base slides, Arm turns about Z: a rigid hierarchy)
                                 "Stretch" (Arm scales along X only: non-uniform bone scale)
  assets/models/morph_blob.gltf  a 3 x 3 grid plane with one morph target lifting its centre:
                                 "Pulse" (the weight 0 -> 1 -> 0)

Run from anywhere: python3 make_vat_fixtures.py (writes next to itself). Deterministic output.
"""

import base64
import json
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


class Builder:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def _view(self, payload, target=None):
        while len(self.data) % 4:
            self.data.append(0)
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(payload)}
        if target:
            view["target"] = target
        self.data += payload
        self.views.append(view)
        return len(self.views) - 1

    def floats(self, values, kind, bounds=False, target=34962):
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        flat = [float(x) for v in values for x in (v if isinstance(v, (list, tuple)) else [v])]
        view = self._view(struct.pack("<%df" % len(flat), *flat), target)
        accessor = {"bufferView": view, "componentType": 5126, "count": len(flat) // width, "type": kind}
        if bounds:
            accessor["min"] = [min(flat[i::width]) for i in range(width)]
            accessor["max"] = [max(flat[i::width]) for i in range(width)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def indices(self, values):
        view = self._view(struct.pack("<%dH" % len(values), *values), 34963)
        self.accessors.append({"bufferView": view, "componentType": 5123, "count": len(values), "type": "SCALAR"})
        return len(self.accessors) - 1

    def buffer(self):
        return [{"byteLength": len(self.data),
                 "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(self.data)).decode("ascii")}]


def box(sx, sy, sz, oy):
    """24 vertices (4 per face, outward normals), 36 indices; centred on x/z, from y=oy"""
    positions, normals, indices = [], [], []
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 1, 0), (0, 0, -1)),
             ((0, 1, 0), (0, 0, 1), (1, 0, 0)), ((0, -1, 0), (0, 0, -1), (1, 0, 0)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    for n, u, v in faces:
        base = len(positions)
        for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [n[i] + a * u[i] + b * v[i] for i in range(3)]
            positions.append([p[0] * sx / 2, (p[1] + 1) * sy / 2 + oy, p[2] * sz / 2])
            normals.append(list(n))
        # counter-clockwise seen from outside: u x v = n
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


def quat_z(degrees):
    h = math.radians(degrees) / 2
    return [0.0, 0.0, math.sin(h), math.cos(h)]


def write(name, gltf):
    path = os.path.join(HERE, "assets", "models", name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        json.dump(gltf, f, indent=1, sort_keys=False)
        f.write("\n")


def rigid_arm():
    b = Builder()
    meshes = []
    for i, (sx, sy, sz) in enumerate(((0.6, 0.4, 0.6), (0.2, 1.0, 0.2))):
        p, n, idx = box(sx, sy, sz, 0.0)
        meshes.append({"name": ["BaseBox", "ArmBox"][i], "primitives": [{
            "attributes": {"POSITION": b.floats(p, "VEC3", True), "NORMAL": b.floats(n, "VEC3")},
            "indices": b.indices(idx), "material": i}]})
    times = b.floats([0.0, 0.5, 1.0], "SCALAR", True, None)
    slide = b.floats([[0, 0, 0], [0.5, 0, 0], [0, 0, 0]], "VEC3", False, None)
    turn = b.floats([quat_z(0), quat_z(90), quat_z(0)], "VEC4", False, None)
    stretch = b.floats([[1, 1, 1], [2.5, 1, 1], [1, 1, 1]], "VEC3", False, None)
    gltf = {
        "asset": {"version": "2.0", "generator": "vertex-animation make_vat_fixtures.py"},
        "scene": 0,
        "scenes": [{"name": "Rig", "nodes": [0]}],
        "nodes": [
            {"name": "Rig", "children": [1]},
            {"name": "Base", "mesh": 0, "children": [2]},
            {"name": "Arm", "mesh": 1, "translation": [0.0, 0.4, 0.0]},
        ],
        "meshes": meshes,
        "materials": [
            {"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.1, 0.1, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.7}},
            {"name": "Blue", "pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.2, 0.8, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.5}},
        ],
        "animations": [
            {"name": "Swing",
             "samplers": [{"input": times, "output": slide, "interpolation": "LINEAR"},
                          {"input": times, "output": turn, "interpolation": "LINEAR"}],
             "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}},
                          {"sampler": 1, "target": {"node": 2, "path": "rotation"}}]},
            {"name": "Stretch",
             "samplers": [{"input": times, "output": stretch, "interpolation": "LINEAR"}],
             "channels": [{"sampler": 0, "target": {"node": 2, "path": "scale"}}]},
        ],
    }
    gltf["accessors"] = b.accessors
    gltf["bufferViews"] = b.views
    gltf["buffers"] = b.buffer()
    write("rigid_arm.gltf", gltf)


def morph_blob():
    b = Builder()
    positions, normals, deltas, indices = [], [], [], []
    for z in range(3):
        for x in range(3):
            positions.append([x * 0.5 - 0.5, 0.0, z * 0.5 - 0.5])
            normals.append([0.0, 1.0, 0.0])
            deltas.append([0.0, 0.4 if (x, z) == (1, 1) else 0.0, 0.0])
    for z in range(2):
        for x in range(2):
            a = z * 3 + x
            indices += [a, a + 3, a + 1, a + 1, a + 3, a + 4]
    times = b.floats([0.0, 0.5, 1.0], "SCALAR", True, None)
    weights = b.floats([0.0, 1.0, 0.0], "SCALAR", False, None)
    gltf = {
        "asset": {"version": "2.0", "generator": "vertex-animation make_vat_fixtures.py"},
        "scene": 0,
        "scenes": [{"name": "Blob", "nodes": [0]}],
        "nodes": [{"name": "Blob", "children": [1]}, {"name": "Surface", "mesh": 0}],
        "meshes": [{"name": "Surface", "weights": [0.0], "primitives": [{
            "attributes": {"POSITION": b.floats(positions, "VEC3", True), "NORMAL": b.floats(normals, "VEC3")},
            "targets": [{"POSITION": b.floats(deltas, "VEC3", True)}],
            "indices": b.indices(indices), "material": 0}]}],
        "materials": [{"name": "Green", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.7, 0.3, 1.0], "metallicFactor": 0.0}}],
        "animations": [{"name": "Pulse",
                        "samplers": [{"input": times, "output": weights, "interpolation": "LINEAR"}],
                        "channels": [{"sampler": 0, "target": {"node": 1, "path": "weights"}}]}],
    }
    gltf["accessors"] = b.accessors
    gltf["bufferViews"] = b.views
    gltf["buffers"] = b.buffer()
    write("morph_blob.gltf", gltf)


if __name__ == "__main__":
    rigid_arm()
    morph_blob()
