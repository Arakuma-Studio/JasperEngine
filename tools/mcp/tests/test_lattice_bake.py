"""Lattice bake (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md §9.4, §12 WP-BAKE).

Public edit_lattice bake operations on glTF models the test writes itself:

- "Ball": a UV sphere (.gltf + external .bin + external PNG texture) with NORMAL, TANGENT (w = -1
  on half of it), TEXCOORD_0, COLOR_0 and a sparse morph target, two primitives sharing every
  vertex accessor; bound to a lattice for all submeshes and to a second one for submesh 1 only
  with the red mask. The written .glb is parsed here: positions, normals, tangents (w kept) and
  the morph displacement of chosen vertices against values computed in this file from the spec
  formulas (§3), the texture still referenced next to the new file, one embedded buffer, the
  shared accessors split per primitive; the baked model (bindings removed) renders as the live
  GPU deformation did (pngdiff, and the images are kept); undo restores the model file and the
  bindings and keeps the file; a new bake never overwrites (`_2`).
- "Column": a skinned glTF: the Before Skinning binding bakes into the bind pose (in place, no
  new accessors), the After Skinning one is kept (and still deforms after the bake).
- "Pair": two nodes sharing one mesh, only one of them bound: its node gets a copy of the mesh,
  the other node's data stays; the mesh part entities are kept with their ids.
- A box primitive is refused by the MCP bake command without writing a file.

Runs in a new empty project; the tests share one editor and run in order (state in S).
"""

from __future__ import annotations

import base64
import json
import math
import shutil
import struct
import sys
import zlib
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

EDITOR = {"create": True, "name": "Lattice Bake"}
CI = True
TIMEOUT = 900

S: dict = {}

MODELS = "assets/models"

# -- glTF reading -------------------------------------------------------------------------------

COMPONENTS = {5120: ("b", 1, 127.0), 5121: ("B", 1, 255.0), 5122: ("h", 2, 32767.0), 5123: ("H", 2, 65535.0),
              5125: ("I", 4, None), 5126: ("f", 4, None)}
TYPES = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path: Path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2 and length == len(data), f"{path} is not a glTF binary"
    offset, doc, binary = 12, None, b""
    while offset < length:
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8:offset + 8 + chunk_length]
        if chunk_type == 0x4E4F534A:
            doc = json.loads(chunk.decode("utf-8"))
        elif chunk_type == 0x004E4942:
            binary = chunk
        offset += 8 + chunk_length
    return doc, [binary]


def read_gltf(path: Path):
    doc = json.loads(path.read_text())
    buffers = []
    for b in doc["buffers"]:
        uri = b["uri"]
        if uri.startswith("data:"):
            buffers.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            buffers.append((path.parent / uri).read_bytes())
    return doc, buffers


def accessor(doc, buffers, index):
    """The elements of an accessor as tuples of floats (normalized, strided, sparse)."""
    a = doc["accessors"][index]
    fmt, size, norm = COMPONENTS[a["componentType"]]
    n = TYPES[a["type"]]
    normalized = a.get("normalized", False)

    def convert(v):
        if normalized and norm:
            return max(v / norm, -1.0)
        return float(v)

    out = [tuple(0.0 for _ in range(n)) for _ in range(a["count"])]
    if "bufferView" in a:
        view = doc["bufferViews"][a["bufferView"]]
        data = buffers[view["buffer"]]
        stride = view.get("byteStride") or size * n
        base = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        for i in range(a["count"]):
            out[i] = tuple(convert(v) for v in struct.unpack_from("<" + fmt * n, data, base + i * stride))
    sparse = a.get("sparse")
    if sparse:
        iv = doc["bufferViews"][sparse["indices"]["bufferView"]]
        vv = doc["bufferViews"][sparse["values"]["bufferView"]]
        ifmt, isize, _ = COMPONENTS[sparse["indices"]["componentType"]]
        ibase = iv.get("byteOffset", 0) + sparse["indices"].get("byteOffset", 0)
        vbase = vv.get("byteOffset", 0) + sparse["values"].get("byteOffset", 0)
        for s in range(sparse["count"]):
            index_value = struct.unpack_from("<" + ifmt, buffers[iv["buffer"]], ibase + s * isize)[0]
            out[index_value] = tuple(convert(v) for v in struct.unpack_from("<" + fmt * n, buffers[vv["buffer"]], vbase + s * size * n))
    return out


# -- glTF writing (the test models) ---------------------------------------------------------------


class Builder:
    """Packs float/uint accessors into one buffer."""

    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def _view(self, payload: bytes, target=None) -> int:
        while len(self.data) % 4:
            self.data.append(0)
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(payload)}
        if target:
            view["target"] = target
        self.data += payload
        self.views.append(view)
        return len(self.views) - 1

    def floats(self, rows, type_, minmax=False) -> int:
        n = TYPES[type_]
        payload = b"".join(struct.pack("<" + "f" * n, *row) for row in rows)
        acc = {"bufferView": self._view(payload, 34962), "componentType": 5126, "count": len(rows), "type": type_}
        if minmax:
            acc["min"] = [min(r[c] for r in rows) for c in range(n)]
            acc["max"] = [max(r[c] for r in rows) for c in range(n)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def ushorts(self, values, type_="SCALAR", target=34963) -> int:
        n = TYPES[type_]
        rows = values if n > 1 else [(v,) for v in values]
        payload = b"".join(struct.pack("<" + "H" * n, *row) for row in rows)
        while len(payload) % 4:
            payload += b"\0"
        self.accessors.append({"bufferView": self._view(payload, target), "componentType": 5123,
                               "count": len(rows), "type": type_})
        return len(self.accessors) - 1

    def sparse_vec3(self, count, indices, values) -> int:
        """A VEC3 accessor of zeros but at `indices` (no bufferView)."""
        iview = self._view(b"".join(struct.pack("<I", i) for i in indices))
        vview = self._view(b"".join(struct.pack("<fff", *v) for v in values))
        dense = [(0.0, 0.0, 0.0)] * count
        for i, v in zip(indices, values):
            dense[i] = v
        self.accessors.append({"componentType": 5126, "count": count, "type": "VEC3",
                               "min": [min(r[c] for r in dense) for c in range(3)],
                               "max": [max(r[c] for r in dense) for c in range(3)],
                               "sparse": {"count": len(indices), "indices": {"bufferView": iview, "componentType": 5125},
                                          "values": {"bufferView": vview}}})
        return len(self.accessors) - 1


def write_png(path: Path, size=8):
    rows = b""
    for y in range(size):
        rows += b"\0" + b"".join(bytes((230, 230, 230, 255) if (x + y) % 2 else (60, 90, 200, 255)) for x in range(size))
    chunk = lambda kind, payload: struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


BALL_SEGMENTS = 24
BALL_RINGS = 16


def ball_vertices():
    """(position, normal, tangent, uv, color) of the UV sphere, radius 0.5."""
    out = []
    for r in range(BALL_RINGS + 1):
        theta = math.pi * r / BALL_RINGS
        for s in range(BALL_SEGMENTS + 1):
            phi = 2 * math.pi * s / BALL_SEGMENTS
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
            p = tuple(0.5 * c for c in n)
            t = (-math.sin(phi), 0.0, math.cos(phi))
            w = -1.0 if s >= BALL_SEGMENTS // 2 else 1.0
            red = min(max(p[1] * 2.0 + 0.5, 0.0), 1.0)
            out.append((p, n, t + (w,), (s / BALL_SEGMENTS, r / BALL_RINGS), (red, 0.8, 0.8, 1.0)))
    return out


def write_ball(directory: Path):
    verts = ball_vertices()
    b = Builder()
    pos = b.floats([v[0] for v in verts], "VEC3", minmax=True)
    nor = b.floats([v[1] for v in verts], "VEC3")
    tan = b.floats([v[2] for v in verts], "VEC4")
    uv = b.floats([v[3] for v in verts], "VEC2")
    col = b.floats([v[4] for v in verts], "VEC4")
    upper, lower = [], []
    for r in range(BALL_RINGS):
        for s in range(BALL_SEGMENTS):
            a = r * (BALL_SEGMENTS + 1) + s
            c = a + BALL_SEGMENTS + 1
            tris = [(a, c, a + 1), (a + 1, c, c + 1)]
            (upper if r < BALL_RINGS // 2 else lower).extend(tris)
    i0 = b.ushorts([i for t in upper for i in t])
    i1 = b.ushorts([i for t in lower for i in t])
    # a bump at the top ring (sparse, no bufferView), weight 0
    top = [1 * (BALL_SEGMENTS + 1) + s for s in range(0, BALL_SEGMENTS, 3)]
    morph = b.sparse_vec3(len(verts), top, [(0.0, 0.1, 0.0)] * len(top))
    attributes = {"POSITION": pos, "NORMAL": nor, "TANGENT": tan, "TEXCOORD_0": uv, "COLOR_0": col}
    doc = {
        "asset": {"version": "2.0", "generator": "test_lattice_bake.py"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Ball", "mesh": 0}],
        "meshes": [{"name": "Ball", "weights": [0.0], "extras": {"targetNames": ["Bump"]}, "primitives": [
            {"attributes": attributes, "indices": i0, "material": 0, "targets": [{"POSITION": morph}]},
            {"attributes": attributes, "indices": i1, "material": 1, "targets": [{"POSITION": morph}]}]}],
        "materials": [
            {"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.8}},
            {"name": "Orange", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.55, 0.2, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.6}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9728, "minFilter": 9728}],
        "images": [{"uri": "checker.png"}],
        "accessors": b.accessors, "bufferViews": b.views,
        "buffers": [{"uri": "ball.bin", "byteLength": len(b.data)}],
    }
    (directory / "ball.bin").write_bytes(bytes(b.data))
    (directory / "ball.gltf").write_text(json.dumps(doc, indent=1))
    write_png(directory / "checker.png")


def cube_vertices(half=0.3):
    out = []
    for axis in range(3):
        for sign in (-1.0, 1.0):
            n = [0.0, 0.0, 0.0]
            n[axis] = sign
            u_axis, v_axis = (axis + 1) % 3, (axis + 2) % 3
            for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                p = [0.0, 0.0, 0.0]
                p[axis] = sign * half
                p[u_axis] = du * half
                p[v_axis] = dv * half
                out.append((tuple(p), tuple(n)))
    return out


def write_pair(directory: Path):
    """Two nodes, A and B, sharing one subdivided-free cube mesh."""
    verts = cube_vertices()
    b = Builder()
    pos = b.floats([v[0] for v in verts], "VEC3", minmax=True)
    nor = b.floats([v[1] for v in verts], "VEC3")
    indices = []
    for f in range(6):
        a = f * 4
        flip = f % 2 == 0
        tris = [(a, a + 2, a + 1), (a, a + 3, a + 2)] if flip else [(a, a + 1, a + 2), (a, a + 2, a + 3)]
        indices.extend(i for t in tris for i in t)
    idx = b.ushorts(indices)
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0, 1]}],
        "nodes": [{"name": "A", "mesh": 0, "translation": [-0.6, 0.0, 0.0]}, {"name": "B", "mesh": 0, "translation": [0.6, 0.0, 0.0]}],
        "meshes": [{"name": "Cube", "primitives": [{"attributes": {"POSITION": pos, "NORMAL": nor}, "indices": idx, "material": 0}]}],
        "materials": [{"name": "Grey", "pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.7, 0.75, 1.0], "metallicFactor": 0.0}}],
        "accessors": b.accessors, "bufferViews": b.views,
        "buffers": [{"uri": "pair.bin", "byteLength": len(b.data)}],
    }
    (directory / "pair.bin").write_bytes(bytes(b.data))
    (directory / "pair.gltf").write_text(json.dumps(doc, indent=1))


# -- the spec's math (§3), independent of the engine ------------------------------------------------


def padded_grid(res, offsets):
    rx, ry, rz = res
    P = {}
    for k in range(rz):
        for j in range(ry):
            for i in range(rx):
                P[(i, j, k)] = offsets[i + rx * (j + ry * k)]

    def lin(a, b):
        return tuple(2 * x - y for x, y in zip(a, b))
    for k in range(rz):
        for j in range(ry):
            P[(-1, j, k)] = lin(P[(0, j, k)], P[(1, j, k)])
            P[(rx, j, k)] = lin(P[(rx - 1, j, k)], P[(rx - 2, j, k)])
    for k in range(rz):
        for a in range(-1, rx + 1):
            P[(a, -1, k)] = lin(P[(a, 0, k)], P[(a, 1, k)])
            P[(a, ry, k)] = lin(P[(a, ry - 1, k)], P[(a, ry - 2, k)])
    for a in range(-1, rx + 1):
        for b in range(-1, ry + 1):
            P[(a, b, -1)] = lin(P[(a, b, 0)], P[(a, b, 1)])
            P[(a, b, rz)] = lin(P[(a, b, rz - 1)], P[(a, b, rz - 2)])
    return P


def axis_weights(x, r, mode):
    xc = min(max(x, 0.0), 1.0)
    g = xc * (r - 1)
    c = min(math.floor(g), r - 2)
    t = g - c
    if mode == "cubic":
        return [(c - 1, (1 - t) ** 3 / 6), (c, (3 * t ** 3 - 6 * t ** 2 + 4) / 6),
                (c + 1, (-3 * t ** 3 + 3 * t ** 2 + 3 * t + 1) / 6), (c + 2, t ** 3 / 6)]
    if mode == "smooth":
        s = t * t * (3 - 2 * t)
        return [(c, 1 - s), (c + 1, s)]
    return [(c, 1 - t), (c + 1, t)]


def offset_field(P, res, mode, u):
    out = [0.0, 0.0, 0.0]
    for a, wa in axis_weights(u[0], res[0], mode):
        for b, wb in axis_weights(u[1], res[1], mode):
            for c, wc in axis_weights(u[2], res[2], mode):
                w = wa * wb * wc
                for d in range(3):
                    out[d] += w * P[(a, b, c)][d]
    return out


def fade_factor(u, fade):
    d = math.sqrt(sum((x - min(max(x, 0.0), 1.0)) ** 2 for x in u))
    if fade <= 0:
        return 0.0 if d > 0 else 1.0
    s = min(max(d / fade, 0.0), 1.0)
    return 1.0 - s * s * (3 - 2 * s)


class Cage:
    """A lattice placed by position and scale (no rotation), as the scene holds it."""

    def __init__(self, position, scale, res=(3, 3, 3), mode="cubic", fade=0.25, offsets=None):
        self.position, self.scale, self.res, self.mode, self.fade = position, scale, res, mode, fade
        n = res[0] * res[1] * res[2]
        self.offsets = [tuple(o) for o in (offsets or [(0.0, 0.0, 0.0)] * n)]
        self.P = padded_grid(res, self.offsets)

    def deform(self, p, mesh_position, mesh_scale, weight=1.0, mask=1.0):
        """Object-space point of a mesh at mesh_position/mesh_scale -> deformed object-space point.
        M = translate(0.5) * inverse(lattice) * mesh; p' = p + mat3(M^-1) * delta."""
        world = [mesh_position[i] + mesh_scale[i] * p[i] for i in range(3)]
        u = [(world[i] - self.position[i]) / self.scale[i] + 0.5 for i in range(3)]
        f = fade_factor(u, self.fade)
        D = offset_field(self.P, self.res, self.mode, u)
        delta = [weight * mask * f * x for x in D]
        # mat3(M^-1): normalized -> object, the lattice scale over the mesh scale
        return tuple(p[i] + delta[i] * self.scale[i] / mesh_scale[i] for i in range(3))

    def jacobian(self, p, mesh_position, mesh_scale, weight=1.0, mask=1.0, h=1e-4):
        """d p'/d p by central differences (object space)."""
        cols = []
        for k in range(3):
            a = list(p)
            b = list(p)
            a[k] += h
            b[k] -= h
            pa = self.deform(a, mesh_position, mesh_scale, weight, mask)
            pb = self.deform(b, mesh_position, mesh_scale, weight, mask)
            cols.append([(pa[i] - pb[i]) / (2 * h) for i in range(3)])
        return cols  # cols[k] = d p' / d p_k


def mat_vec(cols, v):
    return [sum(cols[k][i] * v[k] for k in range(3)) for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def normalize(v):
    length = math.sqrt(sum(x * x for x in v))
    return [x / length for x in v]


def deformed_normal(cols, n):
    """cof(J) n: the columns j2 x j3, j3 x j1, j1 x j2 (spec §3.4)."""
    c = [cross(cols[1], cols[2]), cross(cols[2], cols[0]), cross(cols[0], cols[1])]
    return normalize(mat_vec(c, n))


def near(a, b, tol):
    return len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


# -- editor helpers ---------------------------------------------------------------------------------


async def scene(ed) -> int:
    if "scene" not in S:
        made = await ed.call("create_scene", name="Bake", type="3D", path="scenes/Bake")
        S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    return S["scene"]


async def create(ed, type_="empty", **args) -> int:
    made = await ed.call("create_entity", type=type_, scene=S["scene"], **args)
    return made["entity_id"]


async def props(ed, entity, component, *names) -> dict:
    args = {"scene": S["scene"], "entity": entity, "components": [component]}
    if names:
        args["properties"] = list(names)
    got = await ed.call("get_entity", **args)
    return got["components"][component]


async def set_props(ed, entity, values: dict):
    return await ed.call("set_properties", scene=S["scene"], entity=entity, values=values)


async def make_lattice(ed, name, position, scale, offsets: dict, interpolation="cubic") -> int:
    cage = await create(ed, "empty", name=name)
    await ed.call("set_transform", scene=S["scene"], entity=cage, position=list(position), scale=list(scale))
    await ed.call("add_component", scene=S["scene"], entity=cage, component="lattice")
    values = {f"offsets[{i}]": list(v) for i, v in offsets.items()}
    values["interpolation"] = interpolation
    await set_props(ed, cage, {"lattice": values})
    return cage


def offsets_list(res, offsets: dict):
    n = res[0] * res[1] * res[2]
    return [tuple(offsets.get(i, (0.0, 0.0, 0.0))) for i in range(n)]


def ref_id(value):
    return value.get("id") if isinstance(value, dict) else value


async def bindings_of(ed, entity) -> list:
    got = await props(ed, entity, "latticemodifier", "bindings")
    return [(ref_id(b.get("lattice")), b.get("stage"), b.get("submesh")) for b in got["bindings"]]


async def model_file(ed, entity) -> str:
    return (await props(ed, entity, "model", "filename"))["filename"]


async def frames(ed, n=20):
    await ed.call("wait", frames=n)


# -- tests ------------------------------------------------------------------------------------------

BALL_AT = (-0.7, 0.0, 0.0)
SQUASH = {"position": (-0.7, 0.0, 0.0), "scale": (1.3, 1.3, 1.3),
          "offsets": {16: (0.0, -0.25, 0.0), 14: (0.2, 0.0, 0.0), 4: (0.0, 0.0, 0.12)}}
# the top layer (j = 2) of a sharp 3x3x3 lattice sheared along +z
SHEAR = {"position": (-0.7, 0.25, 0.0), "scale": (1.2, 0.6, 1.2),
         "offsets": {i + 3 * (2 + 3 * k): (0.0, 0.0, 0.15) for i in range(3) for k in range(3)}}


async def setup_scene(ed: Editor):
    sid = await scene(ed)
    models = ed.project / MODELS
    models.mkdir(parents=True, exist_ok=True)
    write_ball(models)
    write_pair(models)
    fixture = Path(__file__).resolve().parents[2] / "fixtures" / "shader-library" / "assets" / "models" / "skinned_column.gltf"
    shutil.copy(fixture, models / "column.gltf")
    await ed.call("sync_files", paths=[f"{MODELS}/ball.gltf", f"{MODELS}/ball.bin", f"{MODELS}/checker.png",
                                       f"{MODELS}/pair.gltf", f"{MODELS}/pair.bin", f"{MODELS}/column.gltf"])
    camera = await create(ed, "camera", name="BakeCamera", position=[0.0, 0.1, 3.6])
    await ed.call("set_main_camera", scene=sid, entity=camera)
    return sid


async def test_ball_bake(ed: Editor):
    sid = await setup_scene(ed)
    ball = await create(ed, "model", name="Ball", model=f"{MODELS}/ball.gltf", position=list(BALL_AT))
    await frames(ed, 30)
    squash = await make_lattice(ed, "Squash", SQUASH["position"], SQUASH["scale"], SQUASH["offsets"])
    shear = await make_lattice(ed, "Shear", SHEAR["position"], SHEAR["scale"], SHEAR["offsets"], interpolation="sharp")
    await ed.call("add_component", scene=sid, entity=ball, component="latticemodifier")
    # submesh 1 is the first primitive (the loader lists primitives in reverse)
    await set_props(ed, ball, {"latticemodifier": {"bindings": [{"lattice": squash},
                                                               {"lattice": shear, "submesh": 1, "mask": "red"}]}})
    S.update(ball=ball, squash=squash, shear=shear)
    await frames(ed, 30)

    live = await ed.screenshot("game", path=str(ed.scratch.root / "ball-live.png"), max_width=0)
    ed.check(not pngdiff.flat(pngdiff.decode(live)), "the live view shows something")
    # the comparison below means something: the same view without the lattices differs
    await set_props(ed, ball, {"latticemodifier": {"enabled": False}})
    await frames(ed, 20)
    rest = await ed.screenshot("game", path=str(ed.scratch.root / "ball-rest.png"), max_width=0)
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(live), pngdiff.decode(rest), channel_tolerance=16, max_fraction=0.003)
    ed.check(not ok, f"the lattices visibly deform the ball ({fraction:.4%} of the pixels change)")
    await ed.call("undo")
    await frames(ed, 20)

    await ed.call("edit_lattice", scene=sid, lattice=squash, ops=[{"op": "bake", "target": ball}])
    filename = await model_file(ed, ball)
    ed.check_eq(filename, f"{MODELS}/ball_lattice.glb", "the Model switched to the baked file")
    baked_path = ed.project / filename
    ed.check(baked_path.is_file(), "the baked file exists")
    ed.check_eq(await bindings_of(ed, ball), [], "both bindings were baked and removed")
    S["ball_bytes"] = baked_path.read_bytes()

    # ---- the file
    doc, bufs = read_glb(baked_path)
    src, src_bufs = read_gltf(ed.project / MODELS / "ball.gltf")
    ed.check_eq(len(doc["buffers"]), 1, "one buffer")
    ed.check("uri" not in doc["buffers"][0], "embedded")
    ed.check_eq(doc["images"], [{"uri": "checker.png"}], "the texture stays referenced next to the file")
    ed.check_eq([m["name"] for m in doc["materials"]], ["Checker", "Orange"], "materials kept")
    ed.check_eq(doc["meshes"][0]["extras"], {"targetNames": ["Bump"]}, "mesh extras kept")
    prims = doc["meshes"][0]["primitives"]
    pos0, pos1 = prims[0]["attributes"]["POSITION"], prims[1]["attributes"]["POSITION"]
    ed.check(pos0 != pos1, "the primitives deformed differently got their own POSITION accessors")
    ed.check_eq(prims[0]["attributes"]["TEXCOORD_0"], src["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"], "UVs untouched")

    verts = ball_vertices()
    squash_cage = Cage(SQUASH["position"], SQUASH["scale"], offsets=offsets_list((3, 3, 3), SQUASH["offsets"]))
    shear_cage = Cage(SHEAR["position"], SHEAR["scale"], mode="sharp", offsets=offsets_list((3, 3, 3), SHEAR["offsets"]))
    one = (1.0, 1.0, 1.0)

    def chain(p, upper, color):
        q = squash_cage.deform(p, BALL_AT, one)
        if upper:
            q = shear_cage.deform(q, BALL_AT, one, mask=color[0])
        return q

    def chain_jacobian(p, upper, color, h=1e-4):
        cols = []
        for k in range(3):
            a, b = list(p), list(p)
            a[k] += h
            b[k] -= h
            pa, pb = chain(a, upper, color), chain(b, upper, color)
            cols.append([(pa[i] - pb[i]) / (2 * h) for i in range(3)])
        return cols

    data = {}
    for prim_index, prim in enumerate(prims):
        data[prim_index] = {sem: accessor(doc, bufs, prim["attributes"][sem]) for sem in ("POSITION", "NORMAL", "TANGENT")}
    moved = 0
    for v in (1 * 25 + 0, 4 * 25 + 6, 6 * 25 + 3, 8 * 25 + 12, 8 * 25 + 2, 12 * 25 + 3):
        p, n, t, _, color = verts[v]
        for prim_index in (0, 1):
            upper = prim_index == 0
            expected = chain(p, upper, color)
            got = data[prim_index]["POSITION"][v]
            ed.check(near(got, expected, 2e-4), f"vertex {v} of primitive {prim_index}: baked {got}, spec {expected}")
            moved += max(abs(a - b) for a, b in zip(expected, p)) > 1e-3
            J = chain_jacobian(p, upper, color)
            n_expected = deformed_normal(J, n)
            ed.check(near(data[prim_index]["NORMAL"][v], n_expected, 3e-3),
                     f"normal {v}/{prim_index}: baked {data[prim_index]['NORMAL'][v]}, spec {n_expected}")
            t_expected = normalize(mat_vec(J, t[:3]))
            got_t = data[prim_index]["TANGENT"][v]
            ed.check(near(got_t[:3], t_expected, 3e-3), f"tangent {v}/{prim_index}: baked {got_t}, spec {t_expected}")
            ed.check_eq(got_t[3], t[3], f"tangent w {v}/{prim_index} kept")
    ed.check(moved >= 6, f"the checked vertices move ({moved})")

    # the morph target: still sparse, its displacement re-based on the baked shape
    for prim_index, prim in enumerate(prims):
        target = doc["accessors"][prim["targets"][0]["POSITION"]]
        ed.check("sparse" in target and "bufferView" not in target and target["sparse"]["count"] == 8,
                 f"primitive {prim_index}'s morph target stays sparse: {target}")
        deltas = accessor(doc, bufs, prim["targets"][0]["POSITION"])
        v = 1 * 25 + 0
        p, _, _, _, color = verts[v]
        bumped = (p[0], p[1] + 0.1, p[2])
        expected = [a - b for a, b in zip(chain(bumped, prim_index == 0, color), chain(p, prim_index == 0, color))]
        ed.check(near(deltas[v], expected, 3e-4), f"morph displacement {v}/{prim_index}: {deltas[v]} vs {expected}")
        ed.check_eq(deltas[2], (0.0, 0.0, 0.0), "a vertex the target does not move stays at 0")

    # ---- the render: the baked model without bindings draws what the live deformation drew
    await frames(ed, 30)
    baked = await ed.screenshot("game", path=str(ed.scratch.root / "ball-baked.png"), max_width=0)
    ok, fraction, diff = pngdiff.compare(pngdiff.decode(live), pngdiff.decode(baked), channel_tolerance=16, max_fraction=0.003)
    (ed.scratch.root / "ball-diff.png").write_bytes(diff)
    ed.check(ok, f"baked render differs from the live one on {fraction:.4%} of the pixels (ball-diff.png)")

    # ---- undo: model and bindings back, the file stays; redo bakes again
    await ed.call("undo")
    await frames(ed, 20)
    ed.check_eq(await model_file(ed, ball), f"{MODELS}/ball.gltf", "undo restores the model file")
    ed.check_eq(await bindings_of(ed, ball), [(squash, "after_skinning", -1), (shear, "after_skinning", 1)], "undo restores the bindings")
    ed.check(baked_path.is_file(), "the baked file stays after undo")
    undone = await ed.screenshot("game", path=str(ed.scratch.root / "ball-undone.png"), max_width=0)
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(live), pngdiff.decode(undone), channel_tolerance=4, max_fraction=0.0005)
    ed.check(ok, f"after undo the live deformation draws again ({fraction:.4%} differ)")
    await ed.call("redo")
    await frames(ed, 10)
    ed.check_eq(await model_file(ed, ball), f"{MODELS}/ball_lattice.glb", "redo")
    ed.check_eq(await bindings_of(ed, ball), [], "redo removes the bindings")
    await ed.call("undo")
    await frames(ed, 10)

    # ---- a new bake never overwrites
    await ed.call("edit_lattice", scene=sid, lattice=squash, ops=[{"op": "bake", "target": ball}])
    ed.check_eq(await model_file(ed, ball), f"{MODELS}/ball_lattice_2.glb", "the second bake writes _2")
    ed.check_eq(baked_path.read_bytes(), S["ball_bytes"], "the first file is untouched")
    second = (ed.project / MODELS / "ball_lattice_2.glb").read_bytes()
    ed.check_eq(second, S["ball_bytes"], "the same deformation writes the same file")

    # saved and reopened: the baked model loads with no bindings, nothing deforms twice
    await ed.call("save_scene", scene=sid)
    text = (ed.project / "scenes" / "Bake.scene").read_text()
    ed.check_in("ball_lattice_2.glb", text, "the scene names the baked file")
    await ed.check_alive()


# widens the top of the column and lifts it
HEAD = {16: (0.0, 0.3, 0.0), 15: (-0.45, 0.0, 0.0), 17: (0.45, 0.0, 0.0), 24: (-0.3, 0.0, 0.0), 26: (0.3, 0.0, 0.0)}


async def test_skinned_bake(ed: Editor):
    sid = await scene(ed)
    column = await create(ed, "model", name="Column", model=f"{MODELS}/column.gltf", position=[0.9, -1.0, 0.0])
    await frames(ed, 30)
    head = await make_lattice(ed, "Head", (0.9, 0.5, 0.0), (0.8, 1.2, 0.8), HEAD)
    feet = await make_lattice(ed, "Feet", (0.9, -0.6, 0.0), (0.8, 0.6, 0.8), {10: (0.3, 0.0, 0.0)})
    await ed.call("add_component", scene=sid, entity=column, component="latticemodifier")
    await set_props(ed, column, {"latticemodifier": {"bindings": [{"lattice": head, "stage": "before_skinning"},
                                                                 {"lattice": feet, "stage": "after_skinning"}]}})
    await frames(ed, 30)
    live = await ed.screenshot("game", path=str(ed.scratch.root / "column-live.png"), max_width=0)
    await set_props(ed, column, {"latticemodifier": {"enabled": False}})
    await frames(ed, 20)
    rest = await ed.screenshot("game", path=str(ed.scratch.root / "column-rest.png"), max_width=0)
    ok, fraction, _ = pngdiff.compare(pngdiff.decode(live), pngdiff.decode(rest), channel_tolerance=16, max_fraction=0.003)
    ed.check(not ok, f"the lattices visibly deform the column ({fraction:.4%} of the pixels change)")
    await ed.call("undo")
    await frames(ed, 20)

    await ed.call("edit_lattice", scene=sid, lattice=head, ops=[{"op": "bake", "target": column}])
    filename = await model_file(ed, column)
    ed.check_eq(filename, f"{MODELS}/column_lattice.glb", "baked")
    ed.check_eq(await bindings_of(ed, column), [(feet, "after_skinning", -1)], "the After Skinning binding is kept")

    doc, bufs = read_glb(ed.project / filename)
    src, src_bufs = read_gltf(ed.project / MODELS / "column.gltf")
    ed.check_eq(len(doc["accessors"]), len(src["accessors"]), "single-user FLOAT accessors are written in place")
    ed.check_eq(doc["skins"], src["skins"], "the skin is kept")
    ed.check_eq(accessor(doc, bufs, 6), accessor(src, src_bufs, 6), "inverse bind matrices untouched")
    ed.check_eq(accessor(doc, bufs, 2), accessor(src, src_bufs, 2), "joints untouched")
    cage = Cage((0.9, 0.5, 0.0), (0.8, 1.2, 0.8), offsets=offsets_list((3, 3, 3), HEAD))
    before = accessor(src, src_bufs, 0)
    after = accessor(doc, bufs, doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"])
    moved = 0
    for v in range(len(before)):
        expected = cage.deform(before[v], (0.9, -1.0, 0.0), (1.0, 1.0, 1.0))
        ed.check(near(after[v], expected, 2e-4), f"bind pose vertex {v}: baked {after[v]}, spec {expected}")
        moved += max(abs(a - b) for a, b in zip(expected, before[v])) > 1e-3
    ed.check(moved > 8, f"the head moved ({moved} vertices)")
    pos = doc["accessors"][0]
    ed.check(near(pos["max"], [max(p[i] for p in after) for i in range(3)], 1e-6), "POSITION max updated")

    await frames(ed, 30)
    baked = await ed.screenshot("game", path=str(ed.scratch.root / "column-baked.png"), max_width=0)
    ok, fraction, diff = pngdiff.compare(pngdiff.decode(live), pngdiff.decode(baked), channel_tolerance=16, max_fraction=0.003)
    (ed.scratch.root / "column-diff.png").write_bytes(diff)
    ed.check(ok, f"the baked column renders as the live one ({fraction:.4%} differ, column-diff.png)")

    await ed.call("undo")
    await frames(ed, 10)
    ed.check_eq(await model_file(ed, column), f"{MODELS}/column.gltf", "undo")
    ed.check_eq(await bindings_of(ed, column), [(head, "before_skinning", -1), (feet, "after_skinning", -1)], "undo restores both")
    await ed.call("redo")
    await frames(ed, 10)
    ed.check_eq(await model_file(ed, column), f"{MODELS}/column_lattice.glb", "redo")
    await ed.check_alive()


async def test_shared_mesh_nodes(ed: Editor):
    sid = await scene(ed)
    pair = await create(ed, "model", name="Pair", model=f"{MODELS}/pair.gltf", position=[0.0, 1.1, 0.0])
    await frames(ed, 30)
    tree = await ed.call("get_scene_tree", scene=sid, root=pair, format="json")
    parts = {}

    def walk(node):
        if isinstance(node, dict):
            if node.get("name") in ("A", "B") and isinstance(node.get("id"), int):
                parts[node["name"]] = node["id"]
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(tree)
    ed.check(set(parts) == {"A", "B"}, f"the model's two mesh parts: {parts}")
    cage = await make_lattice(ed, "Dent", (-0.6, 1.1, 0.0), (0.9, 0.9, 0.9), {26: (0.3, 0.3, 0.3)})
    await ed.call("add_component", scene=sid, entity=parts["A"], component="latticemodifier")
    await set_props(ed, parts["A"], {"latticemodifier": {"bindings": [{"lattice": cage}]}})
    await frames(ed, 20)

    await ed.call("edit_lattice", scene=sid, lattice=cage, ops=[{"op": "bake", "target": parts["A"]}])
    ed.check_eq(await model_file(ed, pair), f"{MODELS}/pair_lattice.glb", "the Model switched")
    ed.check_eq(await bindings_of(ed, parts["A"]), [], "A's binding baked")
    tree = await ed.call("get_scene_tree", scene=sid, root=pair, format="json")
    again = {}
    parts_before = dict(parts)
    parts.clear()
    walk(tree)
    again, parts = parts, parts_before
    ed.check_eq(again, parts, "the mesh part entities are kept with their ids")

    doc, bufs = read_glb(ed.project / MODELS / "pair_lattice.glb")
    nodes = {n["name"]: n for n in doc["nodes"]}
    ed.check_eq(nodes["B"]["mesh"], 0, "B keeps the mesh")
    ed.check(nodes["A"]["mesh"] == 1 and len(doc["meshes"]) == 2, f"A gets a copy: {[n.get('mesh') for n in doc['nodes']]}")
    src, src_bufs = read_gltf(ed.project / MODELS / "pair.gltf")
    original = accessor(src, src_bufs, 0)
    ed.check_eq(accessor(doc, bufs, doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]), original, "B's data unchanged")
    dent = Cage((-0.6, 1.1, 0.0), (0.9, 0.9, 0.9), offsets=offsets_list((3, 3, 3), {26: (0.3, 0.3, 0.3)}))
    a_pos = accessor(doc, bufs, doc["meshes"][1]["primitives"][0]["attributes"]["POSITION"])
    corner = max(range(len(original)), key=lambda i: sum(original[i]))
    for v in (corner, 0, 5):
        expected = dent.deform(original[v], (-0.6, 1.1, 0.0), (1.0, 1.0, 1.0))
        ed.check(near(a_pos[v], expected, 2e-4), f"A vertex {v}: {a_pos[v]} vs {expected}")
    ed.check(max(abs(a - b) for a, b in zip(a_pos[corner], original[corner])) > 0.05, "A's corner moved")

    await ed.call("undo")
    await frames(ed, 10)
    ed.check_eq(await model_file(ed, pair), f"{MODELS}/pair.gltf", "undo")
    ed.check_eq(await bindings_of(ed, parts["A"]), [(cage, "after_skinning", -1)], "undo restores A's binding")
    await ed.check_alive()


async def test_mcp_bake(ed: Editor):
    """The same bake through MCP (edit_lattice op bake), no inspector click: the file, the switch,
    positions against a known world translation, undo, and a refused target."""
    if not (ed.project / MODELS / "ball.gltf").is_file():
        await setup_scene(ed)
    sid = await scene(ed)
    at = (0.6, 0.4, -0.5)
    ball = await create(ed, "model", name="McpBall", model=f"{MODELS}/ball.gltf", position=list(at))
    made = await ed.call("edit_lattice", scene=sid, create={"targets": [ball], "name": "McpCage"},
                         ops=[{"op": "move", "select": "all", "offset": [0.2, 0.1, 0.0], "space": "world"}])
    cage = made["lattice"]["id"]
    ed.check_eq([t["entity"]["id"] for t in made["targets"]], [ball], "bound by create")
    verts = [v[0] for v in ball_vertices()]
    probes = [0, 30, 120, 200, len(verts) - 1]
    await frames(ed, 10)

    before = sorted(p.name for p in (ed.project / MODELS).iterdir())
    got = await ed.call("edit_lattice", scene=sid, lattice=cage, ops=[{"op": "bake"}])
    baked = got["ops"][0]
    ed.check_eq(baked["target"]["id"], ball, "the only target is baked by default")
    filename = await model_file(ed, ball)
    ed.check(filename.endswith(".glb") and filename != f"{MODELS}/ball.gltf", f"the model switched: {filename}")
    ed.check(baked["path"].endswith(Path(filename).name), f"the op returns the file: {baked}")
    added = sorted(set(p.name for p in (ed.project / MODELS).iterdir()) - set(before))
    ed.check_eq(added, [Path(filename).name], "one new file, nothing overwritten")
    ed.check_eq(await bindings_of(ed, ball), [], "the baked binding is removed")
    ed.check_eq(got["targets"], [], "the lattice deforms nothing now")

    doc, bufs = read_glb(ed.project / filename)
    positions = accessor(doc, bufs, doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"])
    for v in probes:
        expected = [p + delta for p, delta in zip(verts[v], (0.2, 0.1, 0.0))]
        ed.check(near(positions[v], expected, 2e-4), f"baked vertex {v}: {positions[v]} vs translated {expected}")

    await ed.call("undo", scene=sid)
    await frames(ed, 10)
    ed.check_eq(await model_file(ed, ball), f"{MODELS}/ball.gltf", "undo restores the model")
    ed.check_eq(await bindings_of(ed, ball), [(cage, "after_skinning", -1)], "undo restores the binding")
    ed.check((ed.project / filename).is_file(), "the file stays after undo")

    # a generated mesh: the canBake reason, nothing written, nothing changed
    box = await create(ed, "cube", name="McpBox", position=[0.6, 0.4, -0.5])
    await ed.call("edit_lattice", scene=sid, lattice=cage, ops=[{"op": "bind", "target": box}])
    raw = await ed.call_raw("edit_lattice", {"scene": sid, "lattice": cage, "ops": [{"op": "bake", "target": box}]})
    ed.check(raw.is_error and "ops[0].target" in raw.text and "cannot be baked" in raw.text, raw.text)
    ed.check_eq(sorted(p.name for p in (ed.project / MODELS).iterdir()), sorted(before + added), "no file for the box")
    raw = await ed.call_raw("edit_lattice", {"scene": sid, "lattice": cage, "ops": [{"op": "bake"}]})
    ed.check(raw.is_error and "McpBall" in raw.text and "McpBox" in raw.text, f"two targets: name one ({raw.text})")
    await ed.call("delete_entities", scene=sid, entities=[ball, box, cage])
    await ed.check_alive()


async def test_bake_during_a_clip_preview(ed: Editor):
    """Animation clips 9.R6/R7 (final review M2): a clip preview animating the cage's offsets
    restores before a bake, so the file holds the scene's deformation. The clip goes from the
    cage's offsets to its rest: previewed at its end the cage is at rest (a bake of what is shown
    would find nothing to bake)."""
    if not (ed.project / MODELS / "ball.gltf").is_file():
        await setup_scene(ed)
    sid = await scene(ed)
    ball = await create(ed, "model", name="PreviewBall", model=f"{MODELS}/ball.gltf", position=[0.6, 0.4, -0.5])
    made = await ed.call("edit_lattice", scene=sid, create={"targets": [ball], "name": "PreviewCage"},
                         ops=[{"op": "move", "select": "all", "offset": [0.2, 0.1, 0.0], "space": "world"}])
    cage = made["lattice"]["id"]
    clip = "assets/anim/preview_cage.anim"
    r = await ed.call_raw("edit_animation", {"clip": clip, "create": {}, "root": cage, "tracks": [
        {"path": "", "channel": "lattice", "keys": [{"time": 0, "points": "current"}, {"time": 1, "points": "rest"}]}]})
    ed.check(not r.is_error, r.text[:800])
    await ed.call("add_component", scene=sid, entity=cage, component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": clip}]}})
    r = await ed.call_raw("preview_animation", {"animation": cage, "action": "seek", "time": 1.0})
    ed.check(not r.is_error and r.structured["previewing"], r.text[:600])
    await frames(ed, 5)

    before = sorted(p.name for p in (ed.project / MODELS).iterdir())
    r = await ed.call_raw("edit_lattice", {"scene": sid, "lattice": cage, "ops": [{"op": "bake", "target": ball}]})
    ed.check(not r.is_error, f"the bake reads the scene, not the previewed rest: {r.text[:600]}")
    state = await ed.call("preview_animation", animation=cage, action="pause")
    ed.check(not state["previewing"], f"the bake stopped the preview: {state}")
    filename = await model_file(ed, ball)
    added = sorted(set(p.name for p in (ed.project / MODELS).iterdir()) - set(before))
    ed.check_eq(added, [Path(filename).name], "one new file")
    doc, bufs = read_glb(ed.project / filename)
    positions = accessor(doc, bufs, doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"])
    verts = [v[0] for v in ball_vertices()]
    for v in [0, 30, 120, len(verts) - 1]:
        expected = [p + delta for p, delta in zip(verts[v], (0.2, 0.1, 0.0))]
        ed.check(near(positions[v], expected, 2e-4), f"baked vertex {v}: {positions[v]} vs the scene's deformation {expected}")
    await ed.call("undo", scene=sid)
    await frames(ed, 10)
    await ed.call("delete_entities", scene=sid, entities=[ball, cage])
    await ed.check_alive()
