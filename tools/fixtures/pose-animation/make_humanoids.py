#!/usr/bin/env python3
"""Generates the two skinned humanoids of the pose-animation fixture:

- assets/models/hero_tpose.gltf: 1.75 m tall, Mixamo bone names (mixamorig:Hips ...), T-pose,
  three fingers per hand (thumb, index, middle, 3 joints each), head-top and toe end bones. Rest
  bone rotations are identity (translations only), as a Unity-style T-pose export.
- assets/models/robot_apose.gltf: Blender/Rigify-like names (pelvis, spine, spine.001, neck, head,
  shoulder.L, upper_arm.L, upper_arm_twist.L, forearm.L, hand.L, thigh.L, shin.L, foot.L, toe.L and
  the .R side), A-pose (arms 40 degrees below horizontal), arms 1.2x and legs 0.85x the hero's
  lengths, no upper chest (two spine bones), a twist helper between the upper arm and the forearm.
  Rest bone rotations as Blender exports them: each bone's local +Y points along the bone.

Both face +Z with their left side on +X, stand with the feet's lowest vertices at y = 0 and are a
readable figure of tubes and boxes skinned to the skeleton (rigid segments, smooth two-bone blends
at the joints, up to 4 weights summing to 1), one material per side (left warm, right cool, centre
neutral) so screenshots read left/right. Layout: scene root node -> [root bone, mesh node].

    python3 -I tools/fixtures/pose-animation/make_humanoids.py           # write the models
    python3 -I tools/fixtures/pose-animation/make_humanoids.py --check   # validate the files

Deterministic: rerunning rewrites the same bytes. --check validates the committed files (buffers,
accessors, bounds, joints/weights, inverse bind matrices against the node rest transforms, the
hierarchy, the ground contact) and that they match a fresh generation byte for byte.
"""

import argparse
import base64
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, "assets", "models")

# ---------------------------------------------------------------------------
# Vector maths (tuples of 3 floats; a rotation matrix is a tuple of its 3 column vectors)


def vadd(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vmul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def normalize(a):
    n = length(a)
    return (a[0] / n, a[1] / n, a[2] / n)


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def mirror_x(p):
    return (-p[0], p[1], p[2])


IDENTITY3 = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def m3_apply(m, v):
    return vadd(vadd(vmul(m[0], v[0]), vmul(m[1], v[1])), vmul(m[2], v[2]))


def m3_transpose(m):
    return tuple((m[0][i], m[1][i], m[2][i]) for i in range(3))


def m3_mul(a, b):
    return tuple(m3_apply(a, col) for col in b)


def quat_from_m3(m):
    # m[c][r] is row r of column c
    r00, r01, r02 = m[0][0], m[1][0], m[2][0]
    r10, r11, r12 = m[0][1], m[1][1], m[2][1]
    r20, r21, r22 = m[0][2], m[1][2], m[2][2]
    trace = r00 + r11 + r22
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w, x, y, z = 0.25 * s, (r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s
    elif r00 > r11 and r00 > r22:
        s = math.sqrt(1.0 + r00 - r11 - r22) * 2.0
        w, x, y, z = (r21 - r12) / s, 0.25 * s, (r01 + r10) / s, (r02 + r20) / s
    elif r11 > r22:
        s = math.sqrt(1.0 + r11 - r00 - r22) * 2.0
        w, x, y, z = (r02 - r20) / s, (r01 + r10) / s, 0.25 * s, (r12 + r21) / s
    else:
        s = math.sqrt(1.0 + r22 - r00 - r11) * 2.0
        w, x, y, z = (r10 - r01) / s, (r02 + r20) / s, (r12 + r21) / s, 0.25 * s
    if w < 0.0:
        x, y, z, w = -x, -y, -z, -w
    return (x, y, z, w)


def m3_from_quat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    return ((1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)),
            (2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)),
            (2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)))


# 4x4 matrices: flat lists of 16 floats, column-major (glTF order)

def m4_identity():
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def m4_mul(a, b):
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return out


def m4_from_trs(t, q, s=(1.0, 1.0, 1.0)):
    m = m3_from_quat(q)
    return [m[0][0] * s[0], m[0][1] * s[0], m[0][2] * s[0], 0.0,
            m[1][0] * s[1], m[1][1] * s[1], m[1][2] * s[1], 0.0,
            m[2][0] * s[2], m[2][1] * s[2], m[2][2] * s[2], 0.0,
            t[0], t[1], t[2], 1.0]


def m4_apply_point(m, p):
    return (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14])


def m4_inverse(m):
    """General 4x4 inverse (Gauss-Jordan with partial pivoting); None when singular."""
    a = [[m[c * 4 + r] for c in range(4)] + [1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    for col in range(4):
        pivot = max(range(col, 4), key=lambda r: abs(a[r][col]))
        if abs(a[pivot][col]) < 1e-12:
            return None
        a[col], a[pivot] = a[pivot], a[col]
        p = a[col][col]
        a[col] = [v / p for v in a[col]]
        for r in range(4):
            if r != col:
                f = a[r][col]
                if f != 0.0:
                    a[r] = [v - f * w for v, w in zip(a[r], a[col])]
    return [a[r][4 + c] for c in range(4) for r in range(4)]


def r6(v):
    """Fixed precision for every float written to the JSON (and -0.0 written as 0.0)."""
    return round(v, 6) + 0.0


# ---------------------------------------------------------------------------
# Skeletons


class Bone:
    def __init__(self, name, parent, head, tail, side):
        self.name = name
        self.parent = parent      # Bone or None
        self.head = head          # rest world position of the joint
        self.tail = tail          # where the bone points (used for Blender-style rotations)
        self.side = side          # "C", "L" or "R" (the material of its geometry)
        self.children = []
        self.world_rot = IDENTITY3
        # the rest local TRS as written to the file (rounded)
        self.translation = None
        self.rotation = None
        self.index = -1


class Skeleton:
    def __init__(self):
        self.bones = []
        self.by_name = {}

    def add(self, name, parent, head, tail=None, side="C"):
        p = self.by_name[parent] if parent else None
        b = Bone(name, p, head, tail, side)
        if p:
            p.children.append(b)
        self.bones.append(b)
        self.by_name[name] = b
        return b

    def __getitem__(self, name):
        return self.by_name[name]

    def ordered(self):
        """Depth-first, parents first, children in insertion order (the node and joint order)."""
        out = []

        def visit(b):
            out.append(b)
            for c in b.children:
                visit(c)

        for b in self.bones:
            if b.parent is None:
                visit(b)
        return out

    def finish(self, blender_axes):
        """Rest local TRS of every bone. blender_axes: world rotation with local +Y from head to tail."""
        for b in self.bones:
            if blender_axes:
                y = normalize(vsub(b.tail, b.head))
                ref = (0.0, 0.0, 1.0) if abs(y[2]) < 0.7 else (0.0, 1.0, 0.0)
                z = normalize(vsub(ref, vmul(y, dot(ref, y))))
                x = cross(y, z)
                b.world_rot = (x, y, z)
            else:
                b.world_rot = IDENTITY3
        for b in self.bones:
            if b.parent:
                inv = m3_transpose(b.parent.world_rot)
                local_rot = m3_mul(inv, b.world_rot)
                local_pos = m3_apply(inv, vsub(b.head, b.parent.head))
            else:
                local_rot = b.world_rot
                local_pos = b.head
            b.translation = [r6(v) for v in local_pos]
            b.rotation = [r6(v) for v in quat_from_m3(local_rot)]
        for i, b in enumerate(self.ordered()):
            b.index = i

    def world_matrices(self):
        """Rest world matrices from the rounded local TRS (what a loader computes from the file)."""
        out = {}
        for b in self.ordered():
            local = m4_from_trs(b.translation, b.rotation)
            out[b.name] = m4_mul(out[b.parent.name], local) if b.parent else local
        return out


# Hero: proportions of a 1.75 m adult, joint positions of the left side (the right one mirrors x)
HERO_HIPS_Y = 0.95
HERO_LEFT = {
    "shoulder": (0.06, 1.42, 0.0),
    "arm": (0.18, 1.42, 0.0),
    "forearm": (0.46, 1.42, 0.0),
    "hand": (0.72, 1.42, 0.0),
    "upleg": (0.09, 0.90, 0.0),
    "knee": (0.09, 0.50, 0.01),
    "ankle": (0.09, 0.08, -0.02),
    "toe": (0.09, 0.02, 0.11),
    "toe_end": (0.09, 0.02, 0.19),
}
HERO_FINGERS = {  # left hand: three joints and a tip per finger
    "Thumb": [(0.745, 1.41, 0.03), (0.77, 1.405, 0.055), (0.79, 1.40, 0.075), (0.805, 1.398, 0.09)],
    "Index": [(0.81, 1.42, 0.025), (0.85, 1.42, 0.025), (0.88, 1.42, 0.025), (0.905, 1.42, 0.025)],
    "Middle": [(0.815, 1.42, -0.005), (0.86, 1.42, -0.005), (0.893, 1.42, -0.005), (0.918, 1.42, -0.005)],
}
HERO_SPINE = [("Spine", 1.05), ("Spine1", 1.17), ("Spine2", 1.30)]
HERO_NECK_Y, HERO_HEAD_Y, HERO_TOP_Y = 1.48, 1.56, 1.75

ROBOT_ARM_SCALE = 1.2
ROBOT_LEG_SCALE = 0.85
ROBOT_ARM_DROP = 40.0  # degrees below horizontal


def hero_skeleton():
    s = Skeleton()
    m = "mixamorig:"
    s.add(m + "Hips", None, (0.0, HERO_HIPS_Y, 0.0))
    parent = m + "Hips"
    for name, y in HERO_SPINE:
        s.add(m + name, parent, (0.0, y, 0.0))
        parent = m + name
    s.add(m + "Neck", m + "Spine2", (0.0, HERO_NECK_Y, 0.0))
    s.add(m + "Head", m + "Neck", (0.0, HERO_HEAD_Y, 0.0))
    s.add(m + "HeadTop_End", m + "Head", (0.0, HERO_TOP_Y, 0.0))
    for side, flip in (("Left", lambda p: p), ("Right", mirror_x)):
        c = side[0]
        j = {k: flip(v) for k, v in HERO_LEFT.items()}
        s.add(m + side + "Shoulder", m + "Spine2", j["shoulder"], side=c)
        s.add(m + side + "Arm", m + side + "Shoulder", j["arm"], side=c)
        s.add(m + side + "ForeArm", m + side + "Arm", j["forearm"], side=c)
        s.add(m + side + "Hand", m + side + "ForeArm", j["hand"], side=c)
        for finger, points in HERO_FINGERS.items():
            parent = m + side + "Hand"
            for k in range(3):
                name = m + side + "Hand" + finger + str(k + 1)
                s.add(name, parent, flip(points[k]), side=c)
                parent = name
    for side, flip in (("Left", lambda p: p), ("Right", mirror_x)):
        c = side[0]
        j = {k: flip(v) for k, v in HERO_LEFT.items()}
        s.add(m + side + "UpLeg", m + "Hips", j["upleg"], side=c)
        s.add(m + side + "Leg", m + side + "UpLeg", j["knee"], side=c)
        s.add(m + side + "Foot", m + side + "Leg", j["ankle"], side=c)
        s.add(m + side + "ToeBase", m + side + "Foot", j["toe"], side=c)
        s.add(m + side + "Toe_End", m + side + "ToeBase", j["toe_end"], side=c)
    s.finish(blender_axes=False)
    return s


def robot_joints():
    """Joint positions of the robot (left side), derived from the hero's proportions."""
    h = HERO_LEFT
    thigh = vmul(vsub(h["knee"], h["upleg"]), ROBOT_LEG_SCALE)
    shin = vmul(vsub(h["ankle"], h["knee"]), ROBOT_LEG_SCALE)
    ankle_y = h["ankle"][1]
    upleg = (h["upleg"][0], ankle_y - thigh[1] - shin[1], h["upleg"][2])
    drop = h["upleg"][1] - upleg[1]       # everything above the legs moves down by this
    knee = vadd(upleg, thigh)
    ankle = vadd(knee, shin)
    toe = vadd(ankle, vsub(h["toe"], h["ankle"]))
    toe_end = vadd(toe, vsub(h["toe_end"], h["toe"]))
    down = lambda p: (p[0], p[1] - drop, p[2])
    a = math.radians(ROBOT_ARM_DROP)
    arm_dir = (math.cos(a), -math.sin(a), 0.0)
    shoulder = down(h["shoulder"])
    arm = down(h["arm"])
    upper_len = (h["forearm"][0] - h["arm"][0]) * ROBOT_ARM_SCALE
    fore_len = (h["hand"][0] - h["forearm"][0]) * ROBOT_ARM_SCALE
    j = {
        "drop": drop,
        "pelvis": (0.0, HERO_HIPS_Y - drop, 0.0),
        "spine": (0.0, HERO_SPINE[0][1] - drop, 0.0),
        "spine.001": (0.0, 1.24 - drop, 0.0),
        "neck": (0.0, HERO_NECK_Y - drop, 0.0),
        "head": (0.0, HERO_HEAD_Y - drop, 0.0),
        "head_top": (0.0, HERO_TOP_Y - drop, 0.0),
        "shoulder": shoulder,
        "arm": arm,
        "arm_dir": arm_dir,
        "twist": vadd(arm, vmul(arm_dir, upper_len * 0.5)),
        "forearm": vadd(arm, vmul(arm_dir, upper_len)),
        "hand": vadd(arm, vmul(arm_dir, upper_len + fore_len)),
        "hand_tip": vadd(arm, vmul(arm_dir, upper_len + fore_len + 0.11 * ROBOT_ARM_SCALE)),
        "upleg": upleg,
        "knee": knee,
        "ankle": ankle,
        "toe": toe,
        "toe_end": toe_end,
    }
    return j


def robot_skeleton():
    j = robot_joints()
    s = Skeleton()
    s.add("pelvis", None, j["pelvis"], tail=j["spine"])
    s.add("spine", "pelvis", j["spine"], tail=j["spine.001"])
    s.add("spine.001", "spine", j["spine.001"], tail=j["neck"])
    s.add("neck", "spine.001", j["neck"], tail=j["head"])
    s.add("head", "neck", j["head"], tail=j["head_top"])
    for c, flip in (("L", lambda p: p), ("R", mirror_x)):
        f = lambda k: flip(j[k])
        s.add("shoulder." + c, "spine.001", f("shoulder"), tail=f("arm"), side=c)
        s.add("upper_arm." + c, "shoulder." + c, f("arm"), tail=f("twist"), side=c)
        s.add("upper_arm_twist." + c, "upper_arm." + c, f("twist"), tail=f("forearm"), side=c)
        s.add("forearm." + c, "upper_arm_twist." + c, f("forearm"), tail=f("hand"), side=c)
        s.add("hand." + c, "forearm." + c, f("hand"), tail=f("hand_tip"), side=c)
    for c, flip in (("L", lambda p: p), ("R", mirror_x)):
        f = lambda k: flip(j[k])
        s.add("thigh." + c, "pelvis", f("upleg"), tail=f("knee"), side=c)
        s.add("shin." + c, "thigh." + c, f("knee"), tail=f("ankle"), side=c)
        s.add("foot." + c, "shin." + c, f("ankle"), tail=f("toe"), side=c)
        s.add("toe." + c, "foot." + c, f("toe"), tail=f("toe_end"), side=c)
    s.finish(blender_axes=True)
    return s


# ---------------------------------------------------------------------------
# Geometry


class MeshBuilder:
    PARTS = ("C", "L", "R")

    def __init__(self, skeleton):
        self.skeleton = skeleton
        self.positions, self.normals, self.joints, self.weights = [], [], [], []
        self.indices = {p: [] for p in self.PARTS}

    def vertex(self, p, n, influences):
        total = {}
        for bone, w in influences:
            if w > 1e-6:
                total[bone] = total.get(bone, 0.0) + w
        items = sorted(total.items(), key=lambda kv: (-kv[1], self.skeleton[kv[0]].index))[:4]
        s = sum(w for _, w in items)
        items = [(self.skeleton[b].index, w / s) for b, w in items]
        while len(items) < 4:
            items.append((0, 0.0))
        self.positions.append(tuple(round(v, 6) + 0.0 for v in p))
        self.normals.append(tuple(round(v, 6) + 0.0 for v in normalize(n)))
        self.joints.append(tuple(i for i, _ in items))
        self.weights.append(tuple(w for _, w in items))
        return len(self.positions) - 1

    def triangle(self, part, a, b, c):
        # counter-clockwise seen from outside: the geometric normal agrees with the vertex normals
        pa, pb, pc = self.positions[a], self.positions[b], self.positions[c]
        face = cross(vsub(pb, pa), vsub(pc, pa))
        avg = vadd(vadd(self.normals[a], self.normals[b]), self.normals[c])
        if dot(face, avg) < 0.0:
            b, c = c, b
        self.indices[part] += [a, b, c]

    def quad(self, part, a, b, c, d):
        self.triangle(part, a, b, c)
        self.triangle(part, a, c, d)

    # -- shapes ---------------------------------------------------------------

    def box(self, part, center, axes, half, bone):
        """A rigid box: axes = 3 unit vectors, half = half extents along them; 24 vertices."""
        infl = [(bone, 1.0)]
        for k in range(3):
            for sign in (1.0, -1.0):
                n = vmul(axes[k], sign)
                u, v = axes[(k + 1) % 3], axes[(k + 2) % 3]
                hu, hv = half[(k + 1) % 3], half[(k + 2) % 3]
                c = vadd(center, vmul(n, half[k]))
                ids = [self.vertex(vadd(c, vadd(vmul(u, su * hu), vmul(v, sv * hv))), n, infl)
                       for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
                self.quad(part, *ids)

    def ellipsoid(self, part, center, radii, bone, segments=16, rings=10):
        infl = [(bone, 1.0)]
        grid = []
        for i in range(rings + 1):
            phi = math.pi * i / rings
            row = []
            for k in range(segments + 1):
                th = 2.0 * math.pi * k / segments
                d = (math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th))
                p = vadd(center, (d[0] * radii[0], d[1] * radii[1], d[2] * radii[2]))
                n = (d[0] / radii[0], d[1] / radii[1], d[2] / radii[2])
                row.append(self.vertex(p, n, infl))
            grid.append(row)
        for i in range(rings):
            for k in range(segments):
                a, b, c, d = grid[i][k], grid[i][k + 1], grid[i + 1][k + 1], grid[i + 1][k]
                if i == 0:
                    self.triangle(part, a, c, d)
                elif i == rings - 1:
                    self.triangle(part, a, b, c)
                else:
                    self.quad(part, a, b, c, d)

    def tube(self, part, points, bones, radii, parent=None, child=None, sides=10, flat=False,
             phase=0.0, blend=0.04, step=0.03):
        """A tube along the polyline `points` (joint positions), segment i skinned to bones[i].

        radii: one (width, depth) per point, the half extents of the cross-section (width across
        the body, depth front to back). Near each joint the two bones blend linearly over `blend`
        metres each way (50/50 at the joint); `parent` blends in at the first point and `child` at
        the last one. Capped at both ends. flat: one normal per side (a boxy section).
        """
        seg_len = [length(vsub(points[i + 1], points[i])) for i in range(len(points) - 1)]
        starts = [0.0]
        for L in seg_len:
            starts.append(starts[-1] + L)
        total = starts[-1]
        # ring stations: every joint, plus evenly spaced ones in between
        stations = []
        for i, L in enumerate(seg_len):
            n = max(1, int(math.ceil(L / step)))
            for k in range(n):
                stations.append((i, k / n))
        stations.append((len(seg_len) - 1, 1.0))
        # extra rings at the blend limits keep the deformation smooth at the joints
        extra = []
        for i, L in enumerate(seg_len):
            b = min(blend, 0.45 * L)
            for t in (b / L, 1.0 - b / L):
                extra.append((i, t))
        stations = sorted(set(stations + extra), key=lambda st: starts[st[0]] + st[1] * seg_len[st[0]])

        def influences(i, t):
            L = seg_len[i]
            b = min(blend, 0.45 * L)
            d0, d1 = t * L, (1.0 - t) * L
            prev = bones[i - 1] if i > 0 else parent
            nxt = bones[i + 1] if i + 1 < len(bones) else child
            out = []
            rest = 1.0
            if prev and d0 < b:
                w = 0.5 * (1.0 - d0 / b)
                out.append((prev, w))
                rest -= w
            if nxt and d1 < b:
                w = 0.5 * (1.0 - d1 / b)
                out.append((nxt, w))
                rest -= w
            out.append((bones[i], rest))
            return out

        def frame(i, t):
            d = normalize(vsub(points[i + 1], points[i]))
            if t == 0.0 and i > 0:
                d = normalize(vadd(d, normalize(vsub(points[i], points[i - 1]))))
            elif t == 1.0 and i + 1 < len(seg_len):
                d = normalize(vadd(d, normalize(vsub(points[i + 2], points[i + 1]))))
            ref = (0.0, 0.0, 1.0) if abs(d[2]) < 0.7 else (0.0, 1.0, 0.0)
            v = normalize(vsub(ref, vmul(d, dot(ref, d))))
            u = cross(d, v)
            return d, u, v

        angles = [phase + 2.0 * math.pi * k / sides for k in range(sides)]
        rings = []
        for i, t in stations:
            center = lerp(points[i], points[i + 1], t)
            ra = (radii[i][0] + (radii[i + 1][0] - radii[i][0]) * t, radii[i][1] + (radii[i + 1][1] - radii[i][1]) * t)
            d, u, v = frame(i, t)
            infl = influences(i, t)
            offs = [vadd(vmul(u, ra[0] * math.cos(a)), vmul(v, ra[1] * math.sin(a))) for a in angles]
            ring = {"center": center, "d": d, "infl": infl, "pos": [vadd(center, o) for o in offs], "ids": []}
            if flat:
                for k in range(sides):
                    p0, p1 = ring["pos"][k], ring["pos"][(k + 1) % sides]
                    n = cross(d, vsub(p1, p0))
                    ring["ids"].append((self.vertex(p0, n, infl), self.vertex(p1, n, infl)))
            else:
                for k, a in enumerate(angles):
                    n = vadd(vmul(u, math.cos(a) / ra[0]), vmul(v, math.sin(a) / ra[1]))
                    ring["ids"].append(self.vertex(ring["pos"][k], n, infl))
            rings.append(ring)
        for r0, r1 in zip(rings, rings[1:]):
            for k in range(sides):
                if flat:
                    a, b = r0["ids"][k]
                    d_, c = r1["ids"][k]
                    self.quad(part, a, b, c, d_)
                else:
                    k1 = (k + 1) % sides
                    self.quad(part, r0["ids"][k], r0["ids"][k1], r1["ids"][k1], r1["ids"][k])
        for ring, sign in ((rings[0], -1.0), (rings[-1], 1.0)):
            n = vmul(ring["d"], sign)
            c = self.vertex(ring["center"], n, ring["infl"])
            ids = [self.vertex(p, n, ring["infl"]) for p in ring["pos"]]
            for k in range(sides):
                self.triangle(part, c, ids[k], ids[(k + 1) % sides])


def foot_and_toe(mb, part, ankle, toe, toe_end, foot_bone, toe_bone, half_width, boxy):
    """Rigid foot box from the heel to the toe joint and a toe box to the toe end, soles at y = 0."""
    x = ankle[0]
    heel_z = ankle[2] - 0.05
    axes = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    top = ankle[1] + (0.012 if boxy else 0.01)
    mb.box(part, (x, top / 2.0, (heel_z + toe[2]) / 2.0), axes, (half_width, top / 2.0, (toe[2] - heel_z) / 2.0), foot_bone)
    toe_top = 0.045
    tip = toe_end[2] + 0.01
    mb.box(part, (x, toe_top / 2.0, (toe[2] + tip) / 2.0), axes, (half_width * 0.95, toe_top / 2.0, (tip - toe[2]) / 2.0), toe_bone)


def hero_mesh(s):
    mb = MeshBuilder(s)
    m = "mixamorig:"
    J = lambda name: s[m + name].head
    # torso: pelvis to the base of the neck, blending across the spine joints
    bottom = (0.0, 0.84, 0.0)
    top = (0.0, 1.50, 0.0)
    mb.tube("C", [bottom, J("Spine"), J("Spine1"), J("Spine2"), top],
            [m + "Hips", m + "Spine", m + "Spine1", m + "Spine2"],
            [(0.15, 0.10), (0.135, 0.095), (0.145, 0.10), (0.165, 0.105), (0.14, 0.09)], sides=12)
    mb.tube("C", [(0.0, 1.45, 0.0), J("Head")], [m + "Neck"], [(0.045, 0.045), (0.042, 0.042)],
            parent=m + "Spine2", child=m + "Head", sides=10)
    mb.ellipsoid("C", (0.0, HERO_TOP_Y - 0.095, 0.01), (0.085, 0.095, 0.10), m + "Head")
    mb.box("C", (0.0, 1.645, 0.11), IDENTITY3, (0.014, 0.02, 0.02), m + "Head")   # the nose: faces +Z
    for side in ("Left", "Right"):
        c = side[0]
        sx = 1.0 if c == "L" else -1.0
        b = lambda name: m + side + name
        mb.tube(c, [J(side + "Shoulder"), J(side + "Arm"), J(side + "ForeArm"), J(side + "Hand")],
                [b("Shoulder"), b("Arm"), b("ForeArm")], [(0.05, 0.05), (0.052, 0.052), (0.042, 0.042), (0.032, 0.03)],
                parent=m + "Spine2", child=b("Hand"))
        hand = J(side + "Hand")
        mb.box(c, (hand[0] + sx * 0.05, hand[1], hand[2] + 0.01), IDENTITY3, (0.05, 0.016, 0.042), b("Hand"))
        for finger, points in HERO_FINGERS.items():
            pts = [J(side + "Hand" + finger + str(k + 1)) for k in range(3)]
            pts.append(points[3] if c == "L" else mirror_x(points[3]))
            r = 0.012 if finger == "Thumb" else 0.0105
            mb.tube(c, pts, [b("Hand" + finger + str(k + 1)) for k in range(3)],
                    [(r, r), (r * 0.92, r * 0.92), (r * 0.85, r * 0.85), (r * 0.75, r * 0.75)],
                    parent=b("Hand"), sides=6, blend=0.008, step=0.02)
        mb.tube(c, [J(side + "UpLeg"), J(side + "Leg"), J(side + "Foot")], [b("UpLeg"), b("Leg")],
                [(0.075, 0.075), (0.052, 0.052), (0.04, 0.04)], parent=m + "Hips", child=b("Foot"))
        foot_and_toe(mb, c, J(side + "Foot"), J(side + "ToeBase"), J(side + "Toe_End"), b("Foot"), b("ToeBase"),
                     0.045, False)
    return mb


def robot_mesh(s):
    mb = MeshBuilder(s)
    j = robot_joints()
    J = lambda name: s[name].head
    box = dict(sides=4, flat=True, phase=math.pi / 4.0)
    r2 = math.sqrt(2.0)
    sq = lambda w, d: (w * r2, d * r2)   # half extents of a square section from its corner radius
    drop = j["drop"]
    bottom = (0.0, 0.84 - drop, 0.0)
    top = (0.0, 1.50 - drop, 0.0)
    mb.tube("C", [bottom, J("spine"), J("spine.001"), top], ["pelvis", "spine", "spine.001"],
            [sq(0.15, 0.095), sq(0.13, 0.09), sq(0.16, 0.10), sq(0.15, 0.09)], **box)
    mb.tube("C", [(0.0, 1.45 - drop, 0.0), J("head")], ["neck"], [sq(0.04, 0.04), sq(0.04, 0.04)],
            parent="spine.001", child="head", **box)
    head_c = (0.0, j["head_top"][1] - 0.095, 0.0)
    mb.box("C", head_c, IDENTITY3, (0.085, 0.095, 0.09), "head")
    mb.box("C", (0.0, head_c[1] + 0.015, 0.095), IDENTITY3, (0.065, 0.022, 0.012), "head")  # the visor: faces +Z
    for c in ("L", "R"):
        b = lambda name: name + "." + c
        mb.tube(c, [J(b("shoulder")), J(b("upper_arm")), J(b("upper_arm_twist")), J(b("forearm")), J(b("hand"))],
                [b("shoulder"), b("upper_arm"), b("upper_arm_twist"), b("forearm")],
                [sq(0.05, 0.05), sq(0.05, 0.05), sq(0.046, 0.046), sq(0.042, 0.042), sq(0.034, 0.032)],
                parent="spine.001", child=b("hand"), **box)
        arm_dir = j["arm_dir"] if c == "L" else mirror_x(j["arm_dir"])
        across = cross((0.0, 0.0, 1.0), arm_dir)   # in the arm's vertical plane, perpendicular to it
        hand = J(b("hand"))
        hand_len = 0.055 * ROBOT_ARM_SCALE
        mb.box(c, vadd(hand, vmul(arm_dir, hand_len)), (arm_dir, across, (0.0, 0.0, 1.0)),
               (hand_len, 0.02, 0.05), b("hand"))
        mb.tube(c, [J(b("thigh")), J(b("shin")), J(b("foot"))], [b("thigh"), b("shin")],
                [sq(0.072, 0.072), sq(0.05, 0.05), sq(0.04, 0.04)], parent="pelvis", child=b("foot"), **box)
        foot_and_toe(mb, c, J(b("foot")), J(b("toe")), j["toe_end"] if c == "L" else mirror_x(j["toe_end"]),
                     b("foot"), b("toe"), 0.048, True)
    return mb


# ---------------------------------------------------------------------------
# glTF writer


MATERIALS = {
    "hero": [("HeroBody", [0.80, 0.72, 0.62, 1.0]), ("HeroLeftWarm", [0.92, 0.52, 0.28, 1.0]),
             ("HeroRightCool", [0.28, 0.55, 0.92, 1.0])],
    "robot": [("RobotBody", [0.62, 0.64, 0.68, 1.0]), ("RobotLeftWarm", [0.88, 0.42, 0.22, 1.0]),
              ("RobotRightCool", [0.22, 0.45, 0.88, 1.0])],
}


def build_gltf(kind):
    s = hero_skeleton() if kind == "hero" else robot_skeleton()
    mb = hero_mesh(s) if kind == "hero" else robot_mesh(s)
    bones = s.ordered()
    worlds = s.world_matrices()
    count = len(mb.positions)

    blobs, views, accessors = [], [], []

    def add(data, target, component, kind_, count_, minmax=None):
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
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count_, "type": kind_}
        if minmax:
            accessor["min"], accessor["max"] = minmax
        accessors.append(accessor)
        return len(accessors) - 1

    def f32(v):
        return struct.unpack("<f", struct.pack("<f", v))[0]

    pos_min = [r6(f32(min(p[i] for p in mb.positions))) for i in range(3)]
    pos_max = [r6(f32(max(p[i] for p in mb.positions))) for i in range(3)]
    position = add(b"".join(struct.pack("<3f", *p) for p in mb.positions), 34962, 5126, "VEC3", count, (pos_min, pos_max))
    normal = add(b"".join(struct.pack("<3f", *n) for n in mb.normals), 34962, 5126, "VEC3", count)
    joint = add(b"".join(struct.pack("<4H", *j) for j in mb.joints), 34962, 5123, "VEC4", count)
    weight = add(b"".join(struct.pack("<4f", *w) for w in mb.weights), 34962, 5126, "VEC4", count)
    primitives = []
    for material, part in enumerate(MeshBuilder.PARTS):
        idx = mb.indices[part]
        index = add(b"".join(struct.pack("<H", i) for i in idx), 34963, 5123, "SCALAR", len(idx))
        primitives.append({"attributes": {"POSITION": position, "NORMAL": normal, "JOINTS_0": joint, "WEIGHTS_0": weight},
                           "indices": index, "material": material})
    ibm = []
    for b in bones:
        inv = m4_inverse(worlds[b.name])
        ibm.extend(r6(v) for v in inv)
    inverse = add(b"".join(struct.pack("<16f", *ibm[i * 16:(i + 1) * 16]) for i in range(len(bones))),
                  None, 5126, "MAT4", len(bones))

    # nodes: 0 the scene root, 1.. the bones (depth first), last the mesh
    root_name = "Hero" if kind == "hero" else "Robot"
    nodes = [{"name": root_name, "children": [1, len(bones) + 1]}]
    for b in bones:
        node = {"name": b.name}
        if b.children:
            node["children"] = [c.index + 1 for c in b.children]
        node["rotation"] = b.rotation
        node["translation"] = b.translation
        nodes.append(node)
    nodes.append({"name": root_name + "Mesh", "mesh": 0, "skin": 0})

    def material(name, colour):
        return {"name": name, "pbrMetallicRoughness": {"baseColorFactor": colour, "metallicFactor": 0.0,
                                                         "roughnessFactor": 0.65}}

    data = b"".join(blobs)
    gltf = {
        "asset": {"version": "2.0", "generator": "pose-animation fixture make_humanoids.py"},
        "scene": 0,
        "scenes": [{"name": root_name, "nodes": [0]}],
        "nodes": nodes,
        "meshes": [{"name": root_name + "Mesh", "primitives": primitives}],
        "skins": [{"name": root_name + "Skin", "joints": [b.index + 1 for b in bones], "inverseBindMatrices": inverse,
                   "skeleton": 1}],
        "materials": [material(n, c) for n, c in MATERIALS[kind]],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
    }
    return (json.dumps(gltf, indent=1) + "\n").encode()


OUTPUTS = (("hero", "hero_tpose.gltf"), ("robot", "robot_apose.gltf"))

# The bones each file must have (the fixture's contract with the tests)
EXPECTED_BONES = {
    "hero_tpose.gltf": ["mixamorig:" + n for n in
                        ["Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "HeadTop_End"] +
                        [s + p for s in ("Left", "Right") for p in
                         ["Shoulder", "Arm", "ForeArm", "Hand"] +
                         ["Hand" + f + str(k) for f in ("Thumb", "Index", "Middle") for k in (1, 2, 3)] +
                         ["UpLeg", "Leg", "Foot", "ToeBase", "Toe_End"]]],
    "robot_apose.gltf": ["pelvis", "spine", "spine.001", "neck", "head"] +
                        [b + "." + s for s in ("L", "R") for b in
                         ("shoulder", "upper_arm", "upper_arm_twist", "forearm", "hand",
                          "thigh", "shin", "foot", "toe")],
}


# ---------------------------------------------------------------------------
# Validation (--check)


COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
TYPE_SIZE = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def validate(raw, expected_bones):
    """Return (errors, info) for one glTF file's bytes."""
    errors = []
    info = {}
    g = json.loads(raw)
    if g.get("asset", {}).get("version") != "2.0":
        errors.append("asset.version is not 2.0")
    buffers = []
    for i, b in enumerate(g["buffers"]):
        uri = b["uri"]
        head = "data:application/octet-stream;base64,"
        if not uri.startswith(head):
            errors.append(f"buffer {i} is not an embedded base64 buffer")
            buffers.append(b"")
            continue
        data = base64.b64decode(uri[len(head):], validate=True)
        if len(data) != b["byteLength"]:
            errors.append(f"buffer {i} byteLength {b['byteLength']} != decoded {len(data)}")
        buffers.append(data)
    for i, v in enumerate(g["bufferViews"]):
        if v["byteOffset"] % 4:
            errors.append(f"bufferView {i} offset not 4-aligned")
        if v["byteOffset"] + v["byteLength"] > len(buffers[v["buffer"]]):
            errors.append(f"bufferView {i} runs past its buffer")

    def read(ai):
        a = g["accessors"][ai]
        fmt, size = COMPONENT[a["componentType"]]
        n = TYPE_SIZE[a["type"]]
        v = g["bufferViews"][a["bufferView"]]
        need = a["count"] * n * size
        if need > v["byteLength"]:
            errors.append(f"accessor {ai} needs {need} bytes, its view has {v['byteLength']}")
            return []
        if a.get("byteOffset", 0) != 0 or "byteStride" in v:
            errors.append(f"accessor {ai}: offsets/strides not expected here")
        data = buffers[v["buffer"]][v["byteOffset"]:v["byteOffset"] + need]
        flat = struct.unpack("<" + fmt * (a["count"] * n), data)
        return [flat[k * n:(k + 1) * n] for k in range(a["count"])]

    nodes = g["nodes"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            if c in parent:
                errors.append(f"node {c} has two parents")
            parent[c] = i
    for i in range(len(nodes)):
        seen, k = set(), i
        while k in parent:
            if k in seen:
                errors.append(f"node {i} is in a cycle")
                break
            seen.add(k)
            k = parent[k]
    roots = g["scenes"][g.get("scene", 0)]["nodes"]
    for i in range(len(nodes)):
        k = i
        while k in parent:
            k = parent[k]
        if k not in roots:
            errors.append(f"node {i} is not under a scene root")

    def local(i):
        n = nodes[i]
        if "matrix" in n:
            return list(n["matrix"])
        return m4_from_trs(n.get("translation", [0, 0, 0]), n.get("rotation", [0, 0, 0, 1]), n.get("scale", [1, 1, 1]))

    def world(i):
        m = local(i)
        k = i
        while k in parent:
            k = parent[k]
            m = m4_mul(local(k), m)
        return m

    for i, n in enumerate(nodes):
        if "rotation" in n:
            q = n["rotation"]
            if abs(math.sqrt(sum(v * v for v in q)) - 1.0) > 1e-5:
                errors.append(f"node {n['name']}: rotation not unit length")

    if len(g.get("skins", [])) != 1:
        errors.append("expected exactly one skin")
        return errors, info
    skin = g["skins"][0]
    joints = skin["joints"]
    names = [nodes[j]["name"] for j in joints]
    info["bones"] = names
    if len(set(names)) != len(names):
        errors.append("duplicate bone names")
    if sorted(names) != sorted(expected_bones):
        errors.append(f"bones differ from the expected list: missing {sorted(set(expected_bones) - set(names))}, "
                      f"extra {sorted(set(names) - set(expected_bones))}")
    root = skin["skeleton"]
    if root != joints[0] or parent.get(root) is None:
        errors.append("skin.skeleton must be the first joint and have a parent (the scene root node)")
    for j in joints:
        k = j
        while k != root and k in parent:
            k = parent[k]
        if k != root:
            errors.append(f"joint {nodes[j]['name']} is not under the skeleton root")
    for j in joints:
        if j != root and parent.get(j) not in joints:
            errors.append(f"joint {nodes[j]['name']}'s parent is not a joint")
    mesh_nodes = [i for i, n in enumerate(nodes) if "mesh" in n]
    if len(mesh_nodes) != 1:
        errors.append("expected one mesh node")
    else:
        mn = mesh_nodes[0]
        if nodes[mn].get("skin") != 0:
            errors.append("the mesh node does not use the skin")
        if parent.get(mn) is None or parent.get(mn) != parent.get(root):
            errors.append("the mesh node is not a sibling of the root bone")
        if any(k in nodes[mn] for k in ("translation", "rotation", "scale", "matrix")):
            errors.append("the skinned mesh node has a transform")
    # the scene root must be the identity (bones are relative to the model)
    for r in roots:
        if local(r) != m4_identity():
            errors.append("scene root node is not the identity")

    ibm = read(skin["inverseBindMatrices"])
    if len(ibm) != len(joints):
        errors.append("inverseBindMatrices count != joint count")
    worlds = [world(j) for j in joints]
    worst = 0.0
    for k, (m, w) in enumerate(zip(ibm, worlds)):
        m = list(m)
        if m4_inverse(m) is None:
            errors.append(f"inverse bind matrix of {names[k]} is singular")
            continue
        if abs(m[3]) + abs(m[7]) + abs(m[11]) > 1e-6 or abs(m[15] - 1.0) > 1e-6:
            errors.append(f"inverse bind matrix of {names[k]} has a projective row")
        prod = m4_mul(m, w)
        err = max(abs(a - b) for a, b in zip(prod, m4_identity()))
        worst = max(worst, err)
        if err > 1e-4:
            errors.append(f"inverse bind matrix of {names[k]} does not invert its rest world matrix (err {err:.2e})")
    info["ibm_error"] = worst

    mesh = g["meshes"][0]
    attrs = mesh["primitives"][0]["attributes"]
    for p in mesh["primitives"]:
        if p["attributes"] != attrs:
            errors.append("primitives do not share the vertex attributes")
    pa = g["accessors"][attrs["POSITION"]]
    if "min" not in pa or "max" not in pa:
        errors.append("POSITION has no min/max")
    pos = read(attrs["POSITION"])
    nor = read(attrs["NORMAL"])
    jnt = read(attrs["JOINTS_0"])
    wgt = read(attrs["WEIGHTS_0"])
    count = len(pos)
    for name, values in (("NORMAL", nor), ("JOINTS_0", jnt), ("WEIGHTS_0", wgt)):
        if len(values) != count:
            errors.append(f"{name} count {len(values)} != POSITION count {count}")
    if pos and "min" in pa:
        lo = [min(p[i] for p in pos) for i in range(3)]
        hi = [max(p[i] for p in pos) for i in range(3)]
        if any(abs(a - b) > 1e-6 for a, b in zip(lo + hi, pa["min"] + pa["max"])):
            errors.append(f"POSITION min/max {pa['min']} {pa['max']} != data {lo} {hi}")
        info["bounds"] = (lo, hi)
    bad_n = sum(1 for n in nor if abs(math.sqrt(sum(v * v for v in n)) - 1.0) > 1e-3)
    if bad_n:
        errors.append(f"{bad_n} normals are not unit length")
    worst_w = 0.0
    for k, (j4, w4) in enumerate(zip(jnt, wgt)):
        if any(w < 0.0 for w in w4):
            errors.append(f"vertex {k} has a negative weight")
            break
        worst_w = max(worst_w, abs(sum(w4) - 1.0))
        used = [j for j, w in zip(j4, w4) if w > 0.0]
        if any(j >= len(joints) for j in j4):
            errors.append(f"vertex {k} references joint index out of range")
            break
        if len(set(used)) != len(used):
            errors.append(f"vertex {k} uses a joint twice")
            break
    if worst_w > 2e-6:
        errors.append(f"weights not normalised (worst |sum-1| = {worst_w:.2e})")
    info["weight_error"] = worst_w
    tris = 0
    for p in mesh["primitives"]:
        idx = [i[0] for i in read(p["indices"])]
        if len(idx) % 3:
            errors.append("index count not a multiple of 3")
        if idx and max(idx) >= count:
            errors.append("index out of range")
        tris += len(idx) // 3
        if p.get("material", -1) >= len(g.get("materials", [])):
            errors.append("primitive material out of range")
    info["vertices"] = count
    info["triangles"] = tris
    # rest-pose skinning: sum_j w_j * world_j * ibm_j * p must give p back; lowest point on the floor
    skin_mats = [m4_mul(w, list(m)) for w, m in zip(worlds, ibm)]
    worst_s, lowest, highest = 0.0, float("inf"), -float("inf")
    for p, j4, w4 in zip(pos, jnt, wgt):
        q = (0.0, 0.0, 0.0)
        for j, w in zip(j4, w4):
            if w > 0.0:
                q = vadd(q, vmul(m4_apply_point(skin_mats[j], p), w))
        worst_s = max(worst_s, length(vsub(q, p)))
        lowest = min(lowest, q[1])
        highest = max(highest, q[1])
    if worst_s > 1e-4:
        errors.append(f"rest-pose skinning moves vertices (worst {worst_s:.2e} m)")
    if abs(lowest) > 1e-6:
        errors.append(f"lowest rest vertex at y = {lowest}, not 0")
    info["rest_skin_error"] = worst_s
    info["height"] = highest
    info["joint_world"] = {names[k]: (w[12], w[13], w[14]) for k, w in enumerate(worlds)}
    return errors, info


def generate():
    return {name: build_gltf(kind) for kind, name in OUTPUTS}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="validate the files on disk (no writing)")
    ap.add_argument("--verbose", action="store_true", help="--check: also print the bones and key joints")
    args = ap.parse_args()
    fresh = generate()
    if not args.check:
        os.makedirs(MODELS, exist_ok=True)
        for name, data in fresh.items():
            with open(os.path.join(MODELS, name), "wb") as f:
                f.write(data)
            print(f"wrote {os.path.relpath(os.path.join(MODELS, name), HERE)} ({len(data)} bytes)")
        return 0
    failed = False
    for name, data in fresh.items():
        path = os.path.join(MODELS, name)
        with open(path, "rb") as f:
            raw = f.read()
        errors, info = validate(raw, EXPECTED_BONES[name])
        if raw != data:
            errors.append("the file differs from a fresh generation (rerun make_humanoids.py)")
        status = "FAIL" if errors else "ok"
        lo, hi = info.get("bounds", ([0] * 3, [0] * 3))
        print(f"{status} {name}: {len(info.get('bones', []))} bones, {info.get('vertices')} vertices, "
              f"{info.get('triangles')} triangles, height {info.get('height', 0):.4f} m, "
              f"bounds x {lo[0]:.3f}..{hi[0]:.3f} z {lo[2]:.3f}..{hi[2]:.3f}, "
              f"ibm err {info.get('ibm_error', 0):.1e}, weight err {info.get('weight_error', 0):.1e}, "
              f"rest skin err {info.get('rest_skin_error', 0):.1e}")
        if args.verbose:
            for b in info.get("bones", []):
                p = info["joint_world"][b]
                print(f"    {b:32s} ({p[0]: .4f}, {p[1]: .4f}, {p[2]: .4f})")
        for e in errors:
            print("    " + e)
        failed |= bool(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
