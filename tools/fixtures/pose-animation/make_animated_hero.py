#!/usr/bin/env python3
"""Generates assets/models/hero_animated.gltf: the pose-animation fixture's hero (make_humanoids.py,
same skeleton, mesh and skin) with five glTF animations, the animation clips tests' model sub-clips:

- "Walk"  LINEAR, 1 s: Hips translation (bob), legs, arm swing and a spine twist
- "Idle"  CUBICSPLINE, 2 s: Hips translation and Spine1 rotation with tangents; Head scale LINEAR
- (unnamed, sub-clip "Animation 2")  STEP, 1.5 s: LeftHand rotation and the scene root node "Hero"
- "Wave"  LINEAR, 0.9 s: RightArm rotation
- "Wave"  (a second animation of the same name, sub-clip "Wave 2") LINEAR, 0.8 s: RightForeArm

Rest bone rotations are identity, so each rotation key is the bone's local rotation. Times and
values are rounded to 6 decimals before they are packed as float32.

    python3 -I tools/fixtures/pose-animation/make_animated_hero.py           # write the model
    python3 -I tools/fixtures/pose-animation/make_animated_hero.py --check   # validate the file

Deterministic: rerunning rewrites the same bytes. --check runs make_humanoids.py's validation on the
model (buffers, skin, inverse binds, ground contact), then validates the animations (names,
samplers, accessor types and counts, increasing times with exact min/max, unit rotations, target
nodes) and that the file matches a fresh generation byte for byte.
"""

import argparse
import base64
import importlib.util
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = "hero_animated.gltf"


def load_humanoids():
    # python -I does not put the script's directory on sys.path
    spec = importlib.util.spec_from_file_location("make_humanoids", os.path.join(HERE, "make_humanoids.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mh = load_humanoids()
M = "mixamorig:"

# The sub-clip names the engine gives the animations, in file order (the tests' contract)
EXPECTED_CLIPS = ["Walk", "Idle", "Animation 2", "Wave", "Wave 2"]


def quat(axis, degrees):
    """Unit quaternion [x, y, z, w] of a rotation about a unit axis."""
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return [mh.r6(axis[0] * s), mh.r6(axis[1] * s), mh.r6(axis[2] * s), mh.r6(math.cos(half))]


X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)


def animations(rest):
    """[(name or None, [(node name, path, interpolation, times, values)])]. Cubic values are
    (in-tangent, value, out-tangent) triples per key. `rest` maps a node name to its translation."""
    hips = rest[M + "Hips"]
    walk_t = [0.0, 0.25, 0.5, 0.75, 1.0]
    swing = [25.0, 0.0, -25.0, 0.0, 25.0]
    walk = [
        (M + "Hips", "translation", "LINEAR", walk_t,
         [[hips[0], mh.r6(hips[1] + b), hips[2]] for b in (0.0, -0.03, 0.0, -0.03, 0.0)]),
        (M + "LeftUpLeg", "rotation", "LINEAR", walk_t, [quat(X, -a) for a in swing]),
        (M + "RightUpLeg", "rotation", "LINEAR", walk_t, [quat(X, a) for a in swing]),
        (M + "LeftLeg", "rotation", "LINEAR", walk_t, [quat(X, a) for a in (10.0, 40.0, 10.0, 5.0, 10.0)]),
        (M + "RightLeg", "rotation", "LINEAR", walk_t, [quat(X, a) for a in (10.0, 5.0, 10.0, 40.0, 10.0)]),
        (M + "LeftArm", "rotation", "LINEAR", walk_t, [quat(Y, -a) for a in swing]),
        (M + "RightArm", "rotation", "LINEAR", walk_t, [quat(Y, -a) for a in swing]),
        (M + "Spine", "rotation", "LINEAR", walk_t, [quat(Y, a / 5.0) for a in swing]),
    ]
    zero3, zero4 = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0]
    idle_t = [0.0, 1.0, 2.0]
    idle = [
        (M + "Hips", "translation", "CUBICSPLINE", idle_t,
         [[zero3, [hips[0], mh.r6(hips[1] + b), hips[2]], zero3] for b in (0.0, -0.01, 0.0)]),
        (M + "Spine1", "rotation", "CUBICSPLINE", idle_t,
         [[zero4, quat(X, a), zero4] for a in (0.0, 3.0, 0.0)]),
        (M + "Head", "scale", "LINEAR", idle_t, [[s, s, s] for s in (1.0, 1.02, 1.0)]),
    ]
    unnamed = [
        (M + "LeftHand", "rotation", "STEP", [0.0, 0.5, 1.0, 1.5], [quat(Z, a) for a in (0.0, 20.0, 0.0, -20.0)]),
        ("Hero", "translation", "STEP", [0.0, 0.75, 1.5], [[0.0, 0.0, 0.0], [0.0, 0.0, 0.1], [0.0, 0.0, 0.0]]),
    ]
    wave = [(M + "RightArm", "rotation", "LINEAR", [0.0, 0.3, 0.6, 0.9],
             [quat(Z, a) for a in (0.0, 60.0, 40.0, 60.0)])]
    wave2 = [(M + "RightForeArm", "rotation", "LINEAR", [0.0, 0.4, 0.8], [quat(Z, a) for a in (0.0, 30.0, 0.0)])]
    return [("Walk", walk), ("Idle", idle), (None, unnamed), ("Wave", wave), ("Wave", wave2)]


def build():
    g = json.loads(mh.build_gltf("hero"))
    g["asset"]["generator"] = "pose-animation fixture make_animated_hero.py"
    head = "data:application/octet-stream;base64,"
    data = bytearray(base64.b64decode(g["buffers"][0]["uri"][len(head):]))
    node_index = {n["name"]: i for i, n in enumerate(g["nodes"])}
    rest = {n["name"]: n.get("translation", [0.0, 0.0, 0.0]) for n in g["nodes"]}

    def add(floats, kind, count, minmax=None):
        while len(data) % 4:
            data.append(0)
        blob = struct.pack("<%df" % len(floats), *floats)
        g["bufferViews"].append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob)})
        data.extend(blob)
        accessor = {"bufferView": len(g["bufferViews"]) - 1, "componentType": 5126, "count": count, "type": kind}
        if minmax:
            accessor["min"], accessor["max"] = minmax
        g["accessors"].append(accessor)
        return len(g["accessors"]) - 1

    out = []
    for name, channels in animations(rest):
        samplers, targets = [], []
        for node, path, interpolation, times, values in channels:
            times = [mh.r6(t) for t in times]
            inp = add(times, "SCALAR", len(times), ([times[0]], [times[-1]]))
            flat = []
            for v in values:
                for part in (v if interpolation == "CUBICSPLINE" else [v]):
                    flat.extend(mh.r6(c) for c in part)
            kind = "VEC4" if path == "rotation" else "VEC3"
            width = 4 if path == "rotation" else 3
            outp = add(flat, kind, len(flat) // width)
            samplers.append({"input": inp, "interpolation": interpolation, "output": outp})
            targets.append({"sampler": len(samplers) - 1, "target": {"node": node_index[node], "path": path}})
        animation = {"channels": targets, "samplers": samplers}
        if name is not None:
            animation = {"name": name, **animation}
        out.append(animation)
    g["animations"] = out
    g["buffers"][0] = {"byteLength": len(data), "uri": head + base64.b64encode(bytes(data)).decode()}
    # the same key order as make_humanoids.py's files, animations after the skins
    order = ["asset", "scene", "scenes", "nodes", "meshes", "skins", "animations", "materials", "accessors",
             "bufferViews", "buffers"]
    g = {k: g[k] for k in order}
    return (json.dumps(g, indent=1) + "\n").encode()


def validate_animations(raw):
    errors = []
    g = json.loads(raw)
    head = "data:application/octet-stream;base64,"
    data = base64.b64decode(g["buffers"][0]["uri"][len(head):])

    def read(ai, kind, width):
        a = g["accessors"][ai]
        if a["componentType"] != 5126 or a["type"] != kind or a.get("byteOffset", 0):
            errors.append(f"accessor {ai}: expected tightly packed float {kind}")
            return []
        v = g["bufferViews"][a["bufferView"]]
        if "byteStride" in v or a["count"] * width * 4 > v["byteLength"]:
            errors.append(f"accessor {ai}: its view is strided or too short")
            return []
        flat = struct.unpack("<%df" % (a["count"] * width), data[v["byteOffset"]:v["byteOffset"] + a["count"] * width * 4])
        return [flat[k * width:(k + 1) * width] for k in range(a["count"])]

    animations = g.get("animations", [])
    names = [a.get("name", "") for a in animations]
    clips = [n or f"Animation {i}" for i, n in enumerate(names)]
    for i, n in enumerate(clips):
        if n in clips[:i]:
            clips[i] = f"{n} {clips[:i].count(n) + 1}"
    if clips != EXPECTED_CLIPS:
        errors.append(f"sub-clip names {clips}, expected {EXPECTED_CLIPS}")
    for i, a in enumerate(animations):
        for j, c in enumerate(a["channels"]):
            where = f"animation {clips[i] if i < len(clips) else i} channel {j}"
            node = c["target"].get("node", -1)
            if not 0 <= node < len(g["nodes"]):
                errors.append(f"{where}: no target node")
            s = a["samplers"][c["sampler"]]
            times = [t[0] for t in read(s["input"], "SCALAR", 1)]
            if any(b <= a_ for a_, b in zip(times, times[1:])) or not times or times[0] < 0:
                errors.append(f"{where}: times are not increasing from >= 0")
            acc = g["accessors"][s["input"]]
            def f32(values):
                return [struct.unpack("<f", struct.pack("<f", v))[0] for v in values or []]
            if times and (f32(acc.get("min")) != [times[0]] or f32(acc.get("max")) != [times[-1]]):
                errors.append(f"{where}: input min/max differ from the data")
            path = c["target"]["path"]
            kind, width = ("VEC4", 4) if path == "rotation" else ("VEC3", 3)
            values = read(s["output"], kind, width)
            per_key = 3 if s["interpolation"] == "CUBICSPLINE" else 1
            if len(values) != len(times) * per_key:
                errors.append(f"{where}: {len(values)} outputs for {len(times)} keys ({s['interpolation']})")
            if path == "rotation":
                keyed = values[1::3] if per_key == 3 else values
                if any(abs(math.sqrt(sum(x * x for x in q)) - 1.0) > 1e-5 for q in keyed):
                    errors.append(f"{where}: a rotation key is not unit length")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="validate the file on disk (no writing)")
    args = ap.parse_args()
    fresh = build()
    path = os.path.join(mh.MODELS, OUTPUT)
    if not args.check:
        os.makedirs(mh.MODELS, exist_ok=True)
        with open(path, "wb") as f:
            f.write(fresh)
        print(f"wrote {os.path.relpath(path, HERE)} ({len(fresh)} bytes)")
        return 0
    with open(path, "rb") as f:
        raw = f.read()
    errors, info = mh.validate(raw, mh.EXPECTED_BONES["hero_tpose.gltf"])
    errors += validate_animations(raw)
    if raw != fresh:
        errors.append("the file differs from a fresh generation (rerun make_animated_hero.py)")
    g = json.loads(raw)
    print(f"{'FAIL' if errors else 'ok'} {OUTPUT}: {len(info.get('bones', []))} bones, "
          f"{len(g.get('animations', []))} animations ({', '.join(EXPECTED_CLIPS)}), "
          f"rest skin err {info.get('rest_skin_error', 0):.1e}")
    for e in errors:
        print("    " + e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
