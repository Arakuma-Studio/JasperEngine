# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

# The S41 fixtures no exporter writes: MSFT_lod, KHR_texture_transform cases, an OBJ with one
# vertex per face corner (what loadOBJ makes of any OBJ). usage: make_handmade.py <out dir>
import json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_sources import Builder, sphere, png, checker, write_glb

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def lod_model():
    b = Builder()
    meshes = []
    for rings, segments in ((10, 16), (6, 10), (3, 5)):
        pos, nrm, uv, tan, idx = sphere(rings, segments, 0.75)
        meshes.append({"name": "Ball %dx%d" % (segments, rings), "primitives": [{
            "attributes": {"POSITION": b.floats(pos, "VEC3", 3, minmax=True), "NORMAL": b.floats(nrm, "VEC3", 3),
                           "TEXCOORD_0": b.floats(uv, "VEC2", 2)}, "indices": b.indices(idx), "material": 0}]})
    gltf = {"asset": {"version": "2.0", "generator": "JasperEngine S41 fixture"},
            "extensionsUsed": ["MSFT_lod"], "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"name": "Ball", "mesh": 0, "translation": [0.0, 0.5, 0.0],
                       "extensions": {"MSFT_lod": {"ids": [1, 2]}}, "extras": {"MSFT_screencoverage": [0.5, 0.2, 0.01]}},
                      {"name": "Ball LOD1", "mesh": 1, "translation": [0.0, 0.5, 0.0]},
                      {"name": "Ball LOD2", "mesh": 2, "translation": [0.0, 0.5, 0.0]}],
            "meshes": meshes,
            "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.3, 0.7, 0.4, 1.0], "metallicFactor": 0.0}}],
            "accessors": b.accessors, "bufferViews": b.views, "buffers": [{"byteLength": 0}]}
    b.align()
    gltf["buffers"][0]["byteLength"] = len(b.bin)
    return gltf, b.bin

def quad(b, uv1=False):
    pos = [-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1]
    nrm = [0, 1, 0] * 4
    uv = [0, 0, 1, 0, 1, 1, 0, 1]
    attributes = {"POSITION": b.floats([float(v) for v in pos], "VEC3", 3, minmax=True), "NORMAL": b.floats([float(v) for v in nrm], "VEC3", 3),
                  "TEXCOORD_0": b.floats([float(v) for v in uv], "VEC2", 2)}
    if uv1:
        attributes["TEXCOORD_1"] = b.floats([0.5 * v for v in uv], "VEC2", 2)
    return {"attributes": attributes, "indices": b.indices([0, 2, 1, 0, 3, 2])}

def transform_model():
    b = Builder()
    a = {"offset": [0.25, 0.5], "rotation": 0.3, "scale": [2.0, 3.0]}
    other = {"offset": [0.0, 0.0], "scale": [4.0, 4.0]}
    tt = lambda t: {"KHR_texture_transform": t}
    p0 = quad(b); p0["material"] = 0
    p1 = quad(b); p1["material"] = 1
    p2 = quad(b, uv1=True); p2["material"] = 2
    p3 = quad(b); p3["material"] = 3
    image = b.view(png(8, 8, checker))
    gltf = {"asset": {"version": "2.0", "generator": "JasperEngine S41 fixture"},
            "extensionsUsed": ["KHR_texture_transform"], "scene": 0, "scenes": [{"nodes": [0, 1, 2, 3]}],
            "nodes": [{"name": "One transform", "mesh": 0}, {"name": "Two transforms", "mesh": 1, "translation": [3, 0, 0]},
                      {"name": "No free set", "mesh": 2, "translation": [6, 0, 0]}, {"name": "Set override", "mesh": 3, "translation": [9, 0, 0]}],
            "meshes": [{"primitives": [p0]}, {"primitives": [p1]}, {"primitives": [p2]}, {"primitives": [p3]}],
            "materials": [
                # one transform on set 0, for the base colour and the normal map: baked into the UVs
                {"name": "One", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "extensions": tt(a)}},
                 "normalTexture": {"index": 0, "extensions": tt(a)}},
                # two on set 0, set 1 free: the emissive map's goes to a new set 1
                {"name": "Two", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "extensions": tt(a)}},
                 "emissiveTexture": {"index": 0, "extensions": tt(other)}, "emissiveFactor": [1, 1, 1]},
                # two on set 0 and set 1 taken by the occlusion map: the first transform wins, a warning
                {"name": "Taken", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "extensions": tt(a)}},
                 "emissiveTexture": {"index": 0, "extensions": tt(other)}, "emissiveFactor": [1, 1, 1],
                 "occlusionTexture": {"index": 0, "texCoord": 1}},
                # the transform's texCoord overrides the slot's
                {"name": "Override", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "texCoord": 1,
                 "extensions": tt(dict(a, texCoord=0))}}},
            ],
            "textures": [{"source": 0}], "images": [{"bufferView": image, "mimeType": "image/png"}],
            "accessors": b.accessors, "bufferViews": b.views, "buffers": [{"byteLength": 0}]}
    b.align()
    gltf["buffers"][0]["byteLength"] = len(b.bin)
    return gltf, b.bin

for name, make in (("msft_lod", lod_model), ("texture_transform", transform_model)):
    gltf, binary = make()
    write_glb(os.path.join(out, name + ".glb"), gltf, binary)
    print(name, os.path.getsize(os.path.join(out, name + ".glb")))

# an OBJ sphere: shared positions, normals and UVs through f v/vt/vn, as exporters write it
rings, segments = 8, 12
lines = ["# JasperEngine S41 fixture: a UV sphere with shared vertices (ObjDedupTest)", "mtllib dedup_sphere.mtl", "o Sphere"]
for r in range(rings + 1):
    phi = math.pi * r / rings
    for s in range(segments + 1):
        theta = 2 * math.pi * s / segments
        n = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
        lines.append("v %.6f %.6f %.6f" % n)
        lines.append("vn %.6f %.6f %.6f" % n)
        lines.append("vt %.6f %.6f" % (s / segments, 1 - r / rings))
lines.append("usemtl Clay")
lines.append("s 1")
for r in range(rings):
    for s in range(segments):
        a = r * (segments + 1) + s + 1
        c = a + segments + 1
        for tri in ((a, c, a + 1), (a + 1, c, c + 1)):
            lines.append("f " + " ".join("%d/%d/%d" % (i, i, i) for i in tri))
open(os.path.join(out, "dedup_sphere.obj"), "w").write("\n".join(lines) + "\n")
open(os.path.join(out, "dedup_sphere.mtl"), "w").write("newmtl Clay\nKd 0.8 0.45 0.3\n")
print("dedup_sphere.obj", os.path.getsize(os.path.join(out, "dedup_sphere.obj")))
