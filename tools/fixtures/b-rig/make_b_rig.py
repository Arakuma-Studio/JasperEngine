#!/usr/bin/env python3
"""Generates assets/models/b_rig_proxy.gltf: a small skinned humanoid named like Kevin Iglesias'
rigs (B-hips, B-upperArm.L ...) with the spine proxy bone of Unity's Spine Proxy, for the Mesh Space
Rotation tests (docs/specs/mesh-space-rotation.md R1 to R3).

Layout: scene root node "Rig" -> [B-root, RigMesh]; B-root -> [B-hips, B-spineProxy]. B-root and the
bones under B-hips are the skin's joints; B-spineProxy is a plain node, no joint and no weights, with
the spine's rest rotation relative to B-root. Rest bone rotations as Blender exports them (each bone's
local +Y points along the bone), T-pose, 1.75 m, facing +Z with the left side on +X. The figure is one
rigid box per bone (make_humanoids.py's MeshBuilder), so the file stays small.

Two LINEAR animations of 1 s, with five keys at 0, 0.25, 0.5, 0.75 and 1 s:

- "Run": B-hips turned 90 degrees about +Y at every key, the thighs swinging 30 degrees each way,
  the shins bending
- "Attack": B-hips turned -20 degrees about +Y at every key, B-spine leaning forward to 30 degrees and
  back, B-upperArm.R swinging forward to 80 degrees and back

Both also key B-spineProxy with the spine's rotation relative to B-root in that clip, as Spine Proxy
clips do. Rotations are about the model's axes, applied to the rest pose.

    python3 -I tools/fixtures/b-rig/make_b_rig.py           # write the model
    python3 -I tools/fixtures/b-rig/make_b_rig.py --check   # validate the file

Deterministic: rerunning rewrites the same bytes. --check runs make_humanoids.py's validation on the
model and make_animated_hero.py's on the animations (both in tools/fixtures/pose-animation), checks the
proxy node (not a joint, a child of B-root beside B-hips, keyed in every animation) and that the file
matches a fresh generation byte for byte.

Its own folder, not the pose-animation project: every model in a project the editor opens is cooked
before the scene's models reload, so an extra one there delays the tests that drive that project with
real input. The tests copy it into their scratch project.
"""

import argparse
import base64
import importlib.util
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# the box helpers and the animation packer of the pose-animation fixture
POSE = os.path.join(os.path.dirname(HERE), "pose-animation")
MODELS = os.path.join(HERE, "assets", "models")
OUTPUT = "b_rig_proxy.gltf"


def load(name):
    # python -I does not put the script's directory on sys.path, and the helpers live in POSE
    spec = importlib.util.spec_from_file_location(name, os.path.join(POSE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mh = load("make_humanoids")
mah = load("make_animated_hero")

ROOT, MESH, PROXY = "Rig", "RigMesh", "B-spineProxy"
EXPECTED_CLIPS = ["Run", "Attack"]
EXPECTED_BONES = ["B-root", "B-hips", "B-spine", "B-chest", "B-neck", "B-head"] + \
                 ["B-" + b + "." + s for s in ("L", "R") for b in
                  ("shoulder", "upperArm", "forearm", "hand", "thigh", "shin", "foot", "toe")]

# joints of the left side (the right one mirrors x): the hero's proportions (make_humanoids.py)
LEFT = dict(mh.HERO_LEFT, hand_tip=(0.83, 1.42, 0.0))
SPINE_Y, CHEST_Y = 1.05, 1.25
X, Y = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)


def skeleton():
    s = mh.Skeleton()
    hips = (0.0, mh.HERO_HIPS_Y, 0.0)
    spine, chest = (0.0, SPINE_Y, 0.0), (0.0, CHEST_Y, 0.0)
    neck, head, top = (0.0, mh.HERO_NECK_Y, 0.0), (0.0, mh.HERO_HEAD_Y, 0.0), (0.0, mh.HERO_TOP_Y, 0.0)
    s.add("B-root", None, (0.0, 0.0, 0.0), tail=(0.0, 0.1, 0.0))
    s.add("B-hips", "B-root", hips, tail=spine)
    s.add("B-spine", "B-hips", spine, tail=chest)
    s.add("B-chest", "B-spine", chest, tail=neck)
    s.add("B-neck", "B-chest", neck, tail=head)
    s.add("B-head", "B-neck", head, tail=top)
    for c, flip in (("L", lambda p: p), ("R", mh.mirror_x)):
        j = {k: flip(v) for k, v in LEFT.items()}
        s.add("B-shoulder." + c, "B-chest", j["shoulder"], tail=j["arm"], side=c)
        s.add("B-upperArm." + c, "B-shoulder." + c, j["arm"], tail=j["forearm"], side=c)
        s.add("B-forearm." + c, "B-upperArm." + c, j["forearm"], tail=j["hand"], side=c)
        s.add("B-hand." + c, "B-forearm." + c, j["hand"], tail=j["hand_tip"], side=c)
    for c, flip in (("L", lambda p: p), ("R", mh.mirror_x)):
        j = {k: flip(v) for k, v in LEFT.items()}
        s.add("B-thigh." + c, "B-hips", j["upleg"], tail=j["knee"], side=c)
        s.add("B-shin." + c, "B-thigh." + c, j["knee"], tail=j["ankle"], side=c)
        s.add("B-foot." + c, "B-shin." + c, j["ankle"], tail=j["toe"], side=c)
        s.add("B-toe." + c, "B-foot." + c, j["toe"], tail=j["toe_end"], side=c)
    s.finish(blender_axes=True)
    return s


def mesh(s):
    """One rigid box along each bone (the feet and toes as make_humanoids.py's, soles at y = 0)."""
    mb = mh.MeshBuilder(s)
    thickness = {"B-hips": 0.13, "B-spine": 0.12, "B-chest": 0.14, "B-neck": 0.04, "B-head": 0.09,
                 "shoulder": 0.045, "upperArm": 0.05, "forearm": 0.04, "hand": 0.035, "thigh": 0.07, "shin": 0.05}
    for b in s.ordered():
        part = b.name[2:].split(".")[0]
        if b.name == "B-root" or part in ("foot", "toe"):
            continue
        r = thickness.get(b.name, thickness.get(part))
        half = mh.length(mh.vsub(b.tail, b.head)) / 2.0
        mb.box(b.side, mh.lerp(b.head, b.tail, 0.5), b.world_rot, (r, half, r * 0.8), b.name)
    for c, flip in (("L", lambda p: p), ("R", mh.mirror_x)):
        mh.foot_and_toe(mb, c, s["B-foot." + c].head, s["B-toe." + c].head, flip(LEFT["toe_end"]),
                        "B-foot." + c, "B-toe." + c, 0.045, True)
    return mb


def turn(axis, degrees):
    return mh.m3_from_quat(mah.quat(axis, degrees))


def rotated(s, name, world_turn):
    """The local rotation [x, y, z, w] of a bone turned by `world_turn` (a model-space rotation of
    its rest pose), relative to its parent's rest pose."""
    b = s[name]
    parent = b.parent.world_rot if b.parent else mh.IDENTITY3
    local = mh.m3_mul(mh.m3_transpose(parent), mh.m3_mul(world_turn, b.world_rot))
    return [mh.r6(v) for v in mh.quat_from_m3(local)]


def proxy_rotation(s, hips_turn, spine_turn):
    """B-spineProxy's key: the spine's rotation relative to B-root (identity at rest), with the hips
    and the spine turned."""
    world = mh.m3_mul(hips_turn, mh.m3_mul(spine_turn, s["B-spine"].world_rot))
    return [mh.r6(v) for v in mh.quat_from_m3(world)]


def animations(s):
    times = [0.0, 0.25, 0.5, 0.75, 1.0]
    none = mh.IDENTITY3
    swing = [30.0, 0.0, -30.0, 0.0, 30.0]
    hips_run = turn(Y, 90.0)
    run = [
        ("B-hips", "rotation", "LINEAR", times, [rotated(s, "B-hips", hips_run)] * len(times)),
        ("B-thigh.L", "rotation", "LINEAR", times, [rotated(s, "B-thigh.L", turn(X, -a)) for a in swing]),
        ("B-thigh.R", "rotation", "LINEAR", times, [rotated(s, "B-thigh.R", turn(X, a)) for a in swing]),
        ("B-shin.L", "rotation", "LINEAR", times, [rotated(s, "B-shin.L", turn(X, a)) for a in (10, 50, 10, 5, 10)]),
        ("B-shin.R", "rotation", "LINEAR", times, [rotated(s, "B-shin.R", turn(X, a)) for a in (10, 5, 10, 50, 10)]),
        (PROXY, "rotation", "LINEAR", times, [proxy_rotation(s, hips_run, none)] * len(times)),
    ]
    hips_attack = turn(Y, -20.0)
    lean = [0.0, 15.0, 30.0, 15.0, 0.0]
    attack = [
        ("B-hips", "rotation", "LINEAR", times, [rotated(s, "B-hips", hips_attack)] * len(times)),
        ("B-spine", "rotation", "LINEAR", times, [rotated(s, "B-spine", turn(X, a)) for a in lean]),
        ("B-upperArm.R", "rotation", "LINEAR", times,
         [rotated(s, "B-upperArm.R", turn(Y, a)) for a in (0.0, 40.0, 80.0, 40.0, 0.0)]),
        (PROXY, "rotation", "LINEAR", times, [proxy_rotation(s, hips_attack, turn(X, a)) for a in lean]),
    ]
    return [("Run", run), ("Attack", attack)]


def build():
    s = skeleton()
    mb = mesh(s)
    bones = s.ordered()
    worlds = s.world_matrices()
    count = len(mb.positions)
    blobs, views, accessors = [], [], []

    def add(data, target, component, kind, count_, minmax=None):
        offset = sum(len(x) for x in blobs)
        if offset % 4:
            pad = 4 - offset % 4
            blobs.append(b"\0" * pad)
            offset += pad
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

    def f32(v):
        return struct.unpack("<f", struct.pack("<f", v))[0]

    pos_min = [mh.r6(f32(min(p[i] for p in mb.positions))) for i in range(3)]
    pos_max = [mh.r6(f32(max(p[i] for p in mb.positions))) for i in range(3)]
    position = add(b"".join(struct.pack("<3f", *p) for p in mb.positions), 34962, 5126, "VEC3", count, (pos_min, pos_max))
    normal = add(b"".join(struct.pack("<3f", *n) for n in mb.normals), 34962, 5126, "VEC3", count)
    joint = add(b"".join(struct.pack("<4H", *j) for j in mb.joints), 34962, 5123, "VEC4", count)
    weight = add(b"".join(struct.pack("<4f", *w) for w in mb.weights), 34962, 5126, "VEC4", count)
    primitives = []
    for material, part in enumerate(mh.MeshBuilder.PARTS):
        idx = mb.indices[part]
        index = add(b"".join(struct.pack("<H", i) for i in idx), 34963, 5123, "SCALAR", len(idx))
        primitives.append({"attributes": {"POSITION": position, "NORMAL": normal, "JOINTS_0": joint, "WEIGHTS_0": weight},
                           "indices": index, "material": material})
    ibm = []
    for b in bones:
        ibm.extend(mh.r6(v) for v in mh.m4_inverse(worlds[b.name]))
    inverse = add(b"".join(struct.pack("<16f", *ibm[i * 16:(i + 1) * 16]) for i in range(len(bones))),
                  None, 5126, "MAT4", len(bones))

    # nodes: 0 the scene root, 1.. the bones (depth first), then the proxy and the mesh
    proxy_index, mesh_index = len(bones) + 1, len(bones) + 2
    nodes = [{"name": ROOT, "children": [1, mesh_index]}]
    for b in bones:
        node = {"name": b.name}
        children = [c.index + 1 for c in b.children] + ([proxy_index] if b.parent is None else [])
        if children:
            node["children"] = children
        node["rotation"] = b.rotation
        node["translation"] = b.translation
        nodes.append(node)
    spine = s["B-spine"]
    nodes.append({"name": PROXY, "rotation": [mh.r6(v) for v in mh.quat_from_m3(spine.world_rot)],
                  "translation": [mh.r6(v) for v in spine.head]})
    nodes.append({"name": MESH, "mesh": 0, "skin": 0})

    def material(name, colour):
        return {"name": name, "pbrMetallicRoughness": {"baseColorFactor": colour, "metallicFactor": 0.0,
                                                         "roughnessFactor": 0.65}}

    data = b"".join(blobs)
    g = {
        "asset": {"version": "2.0", "generator": "b-rig fixture make_b_rig.py"},
        "scene": 0,
        "scenes": [{"name": ROOT, "nodes": [0]}],
        "nodes": nodes,
        "meshes": [{"name": MESH, "primitives": primitives}],
        "skins": [{"name": "RigSkin", "joints": [b.index + 1 for b in bones], "inverseBindMatrices": inverse,
                   "skeleton": 1}],
        "animations": [],
        "materials": [material(n, c) for n, c in (("BRigBody", [0.70, 0.70, 0.66, 1.0]),
                                                   ("BRigLeftWarm", [0.90, 0.48, 0.25, 1.0]),
                                                   ("BRigRightCool", [0.25, 0.50, 0.90, 1.0]))],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
    }
    mah.append_animations(g, animations(s))
    return (json.dumps(g, indent=1) + "\n").encode()


def validate_proxy(raw):
    errors = []
    g = json.loads(raw)
    names = [n["name"] for n in g["nodes"]]
    if names.count(PROXY) != 1:
        return [f"expected one {PROXY} node"]
    proxy = names.index(PROXY)
    if proxy in g["skins"][0]["joints"]:
        errors.append(f"{PROXY} is a skin joint")
    root = g["nodes"][names.index("B-root")]
    if sorted(names[c] for c in root.get("children", [])) != ["B-hips", PROXY]:
        errors.append(f"B-root's children are not B-hips and {PROXY}")
    for a in g["animations"]:
        if proxy not in [c["target"]["node"] for c in a["channels"]]:
            errors.append(f"animation {a.get('name')} does not key {PROXY}")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="validate the file on disk (no writing)")
    args = ap.parse_args()
    fresh = build()
    path = os.path.join(MODELS, OUTPUT)
    if not args.check:
        os.makedirs(MODELS, exist_ok=True)
        with open(path, "wb") as f:
            f.write(fresh)
        print(f"wrote {os.path.relpath(path, HERE)} ({len(fresh)} bytes)")
        return 0
    with open(path, "rb") as f:
        raw = f.read()
    errors, info = mh.validate(raw, EXPECTED_BONES)
    errors += mah.validate_animations(raw, EXPECTED_CLIPS)
    errors += validate_proxy(raw)
    if raw != fresh:
        errors.append("the file differs from a fresh generation (rerun make_b_rig.py)")
    print(f"{'FAIL' if errors else 'ok'} {OUTPUT}: {len(info.get('bones', []))} bones and {PROXY}, "
          f"{info.get('vertices')} vertices, height {info.get('height', 0):.4f} m, "
          f"animations {', '.join(EXPECTED_CLIPS)}, rest skin err {info.get('rest_skin_error', 0):.1e}")
    for e in errors:
        print("    " + e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
