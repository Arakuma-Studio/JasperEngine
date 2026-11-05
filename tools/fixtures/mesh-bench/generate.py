"""Builds the mesh benchmark project of docs/specs/S55-draw-submission.md (R8) into a directory:

    python3 tools/fixtures/mesh-bench/generate.py <dir>

then runs it (the counters are the gate of tools/perf/bench-thresholds.json, "mesh-bench"):

    python3 tools/perf/mesh_bench.py --editor <jasperengine-editor> --project <dir>

The project builds its scene at runtime (scripts/MeshBench.lua), from assets this script writes,
all of them deterministic:
  - 2,000 Models of 10 meshes (meshes/m0..m9.gltf, 85 to 1,014 vertices: more than the 64 that
    dynamic batching takes) on a grid, in 3 materials: what GPU Instancing batches;
  - 200 unique meshes (unique.gltf, one node and mesh each): what it cannot;
  - one terrain (textures/height.png) under a sun of 4 shadow cascades;
  - a skinned crowd of 50 (crowd.gltf, the shadows fixture's skinned strip);
  - an interleaved glTF (one buffer view with a byte stride) and a shared-accessor glTF (two
    primitives, two materials, over one POSITION accessor), 20 copies each.
The camera holds still: draws, triangles and uniform bytes are the same every frame.
"""
import base64
import json
import math
import shutil
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
SKINNED = REPO / "tools" / "fixtures" / "shadows" / "assets" / "skinned_morph.gltf"


# ---------------------------------------------------------------- geometry

def sphere(rings, segments, radius=0.5):
    positions, normals, uvs, indices = [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        phi = v * math.pi
        for s in range(segments + 1):
            u = s / segments
            theta = u * 2.0 * math.pi
            n = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
            positions.append(tuple(radius * c for c in n))
            normals.append(n)
            uvs.append((u, v))
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            indices += [a, a + 1, b, b, a + 1, b + 1]
    return positions, normals, uvs, indices


def tiled_box(tiles, size=0.8):
    positions, normals, uvs, indices = [], [], [], []
    h = size * 0.5
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    for n, u_axis, v_axis in faces:
        base = len(positions)
        for j in range(tiles + 1):
            for i in range(tiles + 1):
                u = i / tiles * 2.0 - 1.0
                v = j / tiles * 2.0 - 1.0
                positions.append(tuple(h * (n[k] + u * u_axis[k] + v * v_axis[k]) for k in range(3)))
                normals.append(n)
                uvs.append((i / tiles, j / tiles))
        for j in range(tiles):
            for i in range(tiles):
                a = base + j * (tiles + 1) + i
                b = a + tiles + 1
                indices += [a, b, a + 1, a + 1, b, b + 1]
    return positions, normals, uvs, indices


def torus(rings, segments, major=0.4, minor=0.15):
    positions, normals, uvs, indices = [], [], [], []
    for r in range(rings + 1):
        a = r / rings * 2.0 * math.pi
        for s in range(segments + 1):
            b = s / segments * 2.0 * math.pi
            n = (math.cos(a) * math.cos(b), math.sin(b), math.sin(a) * math.cos(b))
            c = (math.cos(a) * major, 0.0, math.sin(a) * major)
            positions.append(tuple(c[k] + minor * n[k] for k in range(3)))
            normals.append(n)
            uvs.append((r / rings, s / segments))
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            indices += [a, b, a + 1, a + 1, b, b + 1]
    return positions, normals, uvs, indices


def cylinder(segments, stacks, radius=0.3, height=1.0):
    positions, normals, uvs, indices = [], [], [], []
    for j in range(stacks + 1):
        y = j / stacks * height - height * 0.5
        for s in range(segments + 1):
            t = s / segments * 2.0 * math.pi
            n = (math.cos(t), 0.0, math.sin(t))
            positions.append((radius * n[0], y, radius * n[2]))
            normals.append(n)
            uvs.append((s / segments, j / stacks))
    for j in range(stacks):
        for s in range(segments):
            a = j * (segments + 1) + s
            b = a + segments + 1
            indices += [a, b, a + 1, a + 1, b, b + 1]
    return positions, normals, uvs, indices


def meshes():
    """The ten instanced shapes (each over 64 vertices)."""
    return [sphere(8, 12), sphere(16, 24), sphere(24, 32), tiled_box(3), tiled_box(6), tiled_box(12),
            torus(12, 8), torus(32, 16), cylinder(16, 4), cylinder(32, 16)]


# ---------------------------------------------------------------- glTF

class Gltf:
    def __init__(self):
        self.blob = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "JasperEngine mesh-bench fixture"},
                    "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [],
                    "accessors": [], "bufferViews": [], "buffers": []}

    def view(self, data, target, stride=None):
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data), "target": target}
        if stride:
            view["byteStride"] = stride
        self.blob += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, view, component, count, kind, offset=0, minimum=None, maximum=None):
        accessor = {"bufferView": view, "byteOffset": offset, "componentType": component, "count": count, "type": kind}
        if minimum is not None:
            accessor["min"], accessor["max"] = minimum, maximum
        self.doc["accessors"].append(accessor)
        return len(self.doc["accessors"]) - 1

    def material(self, color):
        self.doc["materials"].append({"pbrMetallicRoughness": {"baseColorFactor": list(color), "metallicFactor": 0.0,
                                                               "roughnessFactor": 0.6}})
        return len(self.doc["materials"]) - 1

    def indices(self, indices):
        wide = max(indices) > 65535
        data = struct.pack(("<%dI" if wide else "<%dH") % len(indices), *indices)
        view = self.view(data, 34963)
        return self.accessor(view, 5125 if wide else 5123, len(indices), "SCALAR")

    def separate(self, geometry):
        positions, normals, uvs, _ = geometry
        flat = lambda rows: [c for row in rows for c in row]
        p = self.view(struct.pack("<%df" % (len(positions) * 3), *flat(positions)), 34962)
        n = self.view(struct.pack("<%df" % (len(normals) * 3), *flat(normals)), 34962)
        t = self.view(struct.pack("<%df" % (len(uvs) * 2), *flat(uvs)), 34962)
        lo = [min(v[k] for v in positions) for k in range(3)]
        hi = [max(v[k] for v in positions) for k in range(3)]
        return {"POSITION": self.accessor(p, 5126, len(positions), "VEC3", 0, lo, hi),
                "NORMAL": self.accessor(n, 5126, len(normals), "VEC3"),
                "TEXCOORD_0": self.accessor(t, 5126, len(uvs), "VEC2")}

    def interleaved(self, geometry):
        positions, normals, uvs, _ = geometry
        data = bytearray()
        for p, n, t in zip(positions, normals, uvs):
            data += struct.pack("<8f", *p, *n, *t)
        view = self.view(bytes(data), 34962, stride=32)
        lo = [min(v[k] for v in positions) for k in range(3)]
        hi = [max(v[k] for v in positions) for k in range(3)]
        return {"POSITION": self.accessor(view, 5126, len(positions), "VEC3", 0, lo, hi),
                "NORMAL": self.accessor(view, 5126, len(positions), "VEC3", 12),
                "TEXCOORD_0": self.accessor(view, 5126, len(positions), "VEC2", 24)}

    def mesh(self, primitives, translation=None):
        self.doc["meshes"].append({"primitives": primitives})
        node = {"mesh": len(self.doc["meshes"]) - 1}
        if translation:
            node["translation"] = list(translation)
        self.doc["nodes"].append(node)
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]) - 1)

    def write(self, path):
        self.doc["buffers"] = [{"byteLength": len(self.blob),
                                "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(self.blob)).decode()}]
        path.write_text(json.dumps(self.doc, indent=1) + "\n")


def single(geometry, color):
    gltf = Gltf()
    material = gltf.material(color)
    gltf.mesh([{"attributes": gltf.separate(geometry), "indices": gltf.indices(geometry[3]), "material": material}])
    return gltf


# ---------------------------------------------------------------- textures

def png_gray(path, width, height, pixel):
    rows = b"".join(b"\x00" + bytes(pixel(x, y) for x in range(width)) for y in range(height))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- project

def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1]).resolve()
    assets = out / "assets"
    (assets / "meshes").mkdir(parents=True, exist_ok=True)
    (assets / "textures").mkdir(parents=True, exist_ok=True)
    (out / "scripts").mkdir(parents=True, exist_ok=True)

    for i, geometry in enumerate(meshes()):
        single(geometry, (0.8, 0.8, 0.8, 1.0)).write(assets / "meshes" / ("m%d.gltf" % i))

    # 200 unique meshes: each node its own vertices (a sphere of its own ring count)
    unique = Gltf()
    material = unique.material((0.6, 0.7, 0.5, 1.0))
    for i in range(200):
        geometry = sphere(4 + i % 9, 8 + i // 9 % 12, 0.3 + 0.002 * i)
        unique.mesh([{"attributes": unique.separate(geometry), "indices": unique.indices(geometry[3]), "material": material}],
                    translation=(-20.0 + (i % 20) * 2.0, 0.4, -30.0 - (i // 20) * 2.0))
    unique.write(assets / "unique.gltf")

    # interleaved: one view, stride 32
    interleaved = Gltf()
    geometry = torus(24, 12)
    interleaved.mesh([{"attributes": interleaved.interleaved(geometry), "indices": interleaved.indices(geometry[3]),
                       "material": interleaved.material((0.9, 0.6, 0.3, 1.0))}])
    interleaved.write(assets / "interleaved.gltf")

    # shared accessor: two primitives (two materials) over one POSITION accessor, half the indices each
    shared = Gltf()
    geometry = sphere(16, 24)
    attributes = shared.separate(geometry)
    half = len(geometry[3]) // 6 * 3
    shared.mesh([{"attributes": attributes, "indices": shared.indices(geometry[3][:half]), "material": shared.material((0.3, 0.5, 0.9, 1.0))},
                 {"attributes": attributes, "indices": shared.indices(geometry[3][half:]), "material": shared.material((0.9, 0.3, 0.4, 1.0))}])
    shared.write(assets / "shared.gltf")

    shutil.copyfile(SKINNED, assets / "crowd.gltf")
    png_gray(assets / "textures" / "height.png", 129, 129,
             lambda x, y: int(127 + 60 * math.sin(x * 0.11) * math.cos(y * 0.07) + 40 * math.sin((x + y) * 0.05)))

    shutil.copyfile(HERE / "MeshBench.lua", out / "scripts" / "MeshBench.lua")
    (out / "project.yaml").write_text(
        "name: Mesh Bench\ncanvasWidth: 1280\ncanvasHeight: 720\nscalingMode: letterbox\nvsync: false\n"
        "windowMode: windowed\nwindowWidth: 1280\nwindowHeight: 720\nwindowResizable: true\n"
        "assetsDir: assets\nluaDir: scripts\nstartSceneId: 1\nscenes:\n  - filepath: Main Scene.scene\n")
    identity = ("        position: [0, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n")
    (out / "Main Scene.scene").write_text(
        "id: 1\nname: Main Scene\nscene:\n  backgroundColor: [0.42, 0.55, 0.72, 1]\n  gravity2D: [0, -9.81]\n"
        "  gravity3D: [0, -9.81, 0]\nsceneType: scene_3d\nmainCamera: 0\nentities:\n"
        "  - type: Entity\n    entity: 1001\n    name: Game\n    components:\n      transform:\n" + identity +
        "        visible: true\n      script:\n        scripts:\n          - type: lua\n            path: MeshBench.lua\n"
        "            className: MeshBench\n            enabled: true\n")
    print("mesh-bench written to %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
