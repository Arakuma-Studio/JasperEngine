# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

# Writes the source models the S41 loader fixtures are made from (run with python3 -I).
# usage: make_sources.py <out dir>
import base64, json, math, os, struct, sys, zlib

def png(width, height, pixel):
    rows = b""
    for y in range(height):
        rows += b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))

def checker(x, y):
    on = ((x // 2) + (y // 2)) % 2 == 0
    return (230, 120, 40, 255) if on else (40, 90, 200, 255)

class Builder:
    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []
    def align(self):
        while len(self.bin) % 4:
            self.bin.append(0)
    def view(self, data, target=None):
        self.align()
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.bin += data
        self.views.append(v)
        return len(self.views) - 1
    def floats(self, values, kind, components, target=34962, minmax=False):
        data = struct.pack("<%df" % len(values), *values)
        acc = {"bufferView": self.view(data, target), "componentType": 5126, "count": len(values) // components, "type": kind}
        if minmax:
            acc["min"] = [min(values[i::components]) for i in range(components)]
            acc["max"] = [max(values[i::components]) for i in range(components)]
        self.accessors.append(acc)
        return len(self.accessors) - 1
    def indices(self, values):
        data = struct.pack("<%dH" % len(values), *values)
        self.accessors.append({"bufferView": self.view(data, 34963), "componentType": 5123, "count": len(values), "type": "SCALAR"})
        return len(self.accessors) - 1

def sphere(rings, segments, radius):
    pos, nrm, uv, tan, idx = [], [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        phi = v * math.pi
        for s in range(segments + 1):
            u = s / segments
            theta = u * 2 * math.pi
            n = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
            pos += [radius * c for c in n]
            nrm += list(n)
            uv += [u * 2.0, v]  # repeats twice around: UVs past 1, as real models have
            t = (-math.sin(theta), 0.0, math.cos(theta))
            tan += [t[0], t[1], t[2], 1.0]
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            idx += [a, b, a + 1, a + 1, b, b + 1]
    return pos, nrm, uv, tan, idx

def box(size):
    h = size / 2
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    pos, nrm, uv, col, idx = [], [], [], [], []
    for f, (n, u, v) in enumerate(faces):
        base = len(pos) // 3
        for (a, b) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [h * (n[i] + a * u[i] + b * v[i]) for i in range(3)]
            pos += p
            nrm += list(n)
            uv += [(a + 1) / 2, (1 - b) / 2]
            col += [0.5 + 0.5 * n[0], 0.5 + 0.5 * n[1], 0.5 + 0.5 * n[2], 1.0]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, col, idx

def write_glb(path, gltf, binary):
    js = json.dumps(gltf, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    binary = bytes(binary)
    while len(binary) % 4:
        binary += b"\x00"
    total = 12 + 8 + len(js) + 8 + len(binary)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(binary), 0x004E4942) + binary)

def scene_model():
    b = Builder()
    spos, snrm, suv, stan, sidx = sphere(10, 16, 0.75)
    ball = {"attributes": {"POSITION": b.floats(spos, "VEC3", 3, minmax=True), "NORMAL": b.floats(snrm, "VEC3", 3),
                           "TANGENT": b.floats(stan, "VEC4", 4), "TEXCOORD_0": b.floats(suv, "VEC2", 2)},
            "indices": b.indices(sidx), "material": 0}
    bpos, bnrm, buv, bcol, bidx = box(1.0)
    cube = {"attributes": {"POSITION": b.floats(bpos, "VEC3", 3, minmax=True), "NORMAL": b.floats(bnrm, "VEC3", 3),
                           "TEXCOORD_0": b.floats(buv, "VEC2", 2), "COLOR_0": b.floats(bcol, "VEC4", 4)},
            "indices": b.indices(bidx), "material": 1}
    # a turn of the box around Y and a bob: rotation, translation keys
    times = [i / 4 for i in range(9)]
    rot, tr = [], []
    for t in times:
        a = t * math.pi
        rot += [0.0, math.sin(a / 2), 0.0, math.cos(a / 2)]
        tr += [1.2, 0.25 * math.sin(t * math.pi), 0.0]
    t_acc = b.floats(times, "SCALAR", 1, target=None, minmax=True)
    r_acc = b.floats(rot, "VEC4", 4, target=None)
    p_acc = b.floats(tr, "VEC3", 3, target=None)
    image = b.view(png(16, 16, checker))
    gltf = {
        "asset": {"version": "2.0", "generator": "JasperEngine S41 fixture source"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "Root", "children": [1, 2]},
            {"name": "Ball", "mesh": 0, "translation": [-1.2, 0.0, 0.0], "scale": [0.8, 0.8, 0.8]},
            {"name": "Box", "mesh": 1, "translation": [1.2, 0.0, 0.0]},
        ],
        "meshes": [{"name": "Ball", "primitives": [ball]}, {"name": "Box", "primitives": [cube]}],
        "materials": [
            {"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.6}},
            {"name": "Painted", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.4}},
        ],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9728, "minFilter": 9728, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": image, "mimeType": "image/png"}],
        "animations": [{"name": "Spin", "samplers": [{"input": t_acc, "output": r_acc, "interpolation": "LINEAR"},
                                                     {"input": t_acc, "output": p_acc, "interpolation": "LINEAR"}],
                        "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}},
                                     {"sampler": 1, "target": {"node": 2, "path": "translation"}}]}],
        "accessors": b.accessors,
        "bufferViews": b.views,
        "buffers": [{"byteLength": 0}],
    }
    b.align()
    gltf["buffers"][0]["byteLength"] = len(b.bin)
    return gltf, b.bin

def instanced_model():
    # the same box mesh under six nodes: what gltfpack -mi turns into one instanced node
    b = Builder()
    bpos, bnrm, buv, bcol, bidx = box(0.5)
    cube = {"attributes": {"POSITION": b.floats(bpos, "VEC3", 3, minmax=True), "NORMAL": b.floats(bnrm, "VEC3", 3),
                           "TEXCOORD_0": b.floats(buv, "VEC2", 2)}, "indices": b.indices(bidx), "material": 0}
    nodes = []
    for i in range(6):
        a = i * math.pi / 3
        nodes.append({"mesh": 0, "translation": [1.5 * math.cos(a), 0.2 * i, 1.5 * math.sin(a)],
                      "rotation": [0.0, math.sin(a / 2), 0.0, math.cos(a / 2)], "scale": [1.0 + 0.1 * i] * 3})
    gltf = {"asset": {"version": "2.0", "generator": "JasperEngine S41 fixture source"}, "scene": 0,
            "scenes": [{"nodes": list(range(6))}], "nodes": nodes,
            "meshes": [{"primitives": [cube]}],
            "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.5, 0.2, 1.0], "metallicFactor": 0.0}}],
            "accessors": b.accessors, "bufferViews": b.views, "buffers": [{"byteLength": 0}]}
    b.align()
    gltf["buffers"][0]["byteLength"] = len(b.bin)
    return gltf, b.bin

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, make in (("source", scene_model), ("instances_source", instanced_model)):
        gltf, binary = make()
        write_glb(os.path.join(out, name + ".glb"), gltf, binary)
        print(name, os.path.getsize(os.path.join(out, name + ".glb")))
