#!/usr/bin/env python3
"""Writes the reference points of the warp tests (engine/tests/core/titleart/geometry/WarpTest.cpp).

    python3 tools/titleart/gen_warp_refs.py           # rewrite the fixture
    python3 tools/titleart/gen_warp_refs.py --check   # exit 1 when the fixture differs

A second, independent transcription of Pachislop's src/text/warp.ts (by SafEight, used with the
author's permission; its mesh constructions are Patchy's, MIT, Copyright (c) 2026 Seth A. Robinson)
into Python, so the C++ port (engine/core/titleart/geometry/Warp.cpp) is checked against the
TypeScript's maths rather than against itself: every style at two bends, with and without
distortion, maps a grid of points (inside and outside the box) and reports the warped frame.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "engine/tests/core/titleart/fixtures/geometry/warp-reference.json"

STYLES = ["arc", "arcLower", "arcUpper", "arch", "bulge", "shellLower", "shellUpper", "flag", "wave", "fish",
          "rise", "fisheye", "inflate", "squeeze", "twist"]
# the document's names (snake_case) of Pachislop's styles
NAMES = {s: "".join("_" + c.lower() if c.isupper() else c for c in s) for s in STYLES}
BOX = {"x0": -150.0, "y0": -60.0, "x1": 150.0, "y1": 60.0}


def identity_mesh(w, h, nu, nv):
    xs, ys = [], []
    for r in range(nv):
        for c in range(nu):
            xs.append(w * c / (nu - 1))
            ys.append(h * r / (nv - 1))
    return {"nu": nu, "nv": nv, "xs": xs, "ys": ys}


def arc_row(m, ax, bx, y, theta, up):
    s, c = math.sin(theta), math.cos(theta)
    radius = (bx - ax) / (2 * s)
    handle = (4 / 3) * math.tan(theta / 2) * radius
    dy = -handle * s if up else handle * s
    m["xs"] += [ax, ax + handle * c, bx - handle * c, bx]
    m["ys"] += [y, y + dy, y + dy, y]


def flat_row(m, w, y):
    m["xs"] += [0, w / 3, (2 * w) / 3, w]
    m["ys"] += [y, y, y, y]


def mirror_rows(m, h):
    xs, ys = [], []
    for r in range(m["nv"] - 1, -1, -1):
        for c in range(m["nu"]):
            xs.append(m["xs"][r * m["nu"] + c])
            ys.append(h - m["ys"][r * m["nu"] + c])
    return {"nu": m["nu"], "nv": m["nv"], "xs": xs, "ys": ys}


def style_mesh(style, bend, w, h):
    b = min(100, max(-100, bend))
    if style == "none" or b == 0:
        return identity_mesh(w, h, 4, 2)
    theta = abs(b) * math.pi / 200
    d = 2 * h * b / 100
    m = {"nu": 4, "nv": 2, "xs": [], "ys": []}
    if style == "arc":
        s = math.sin(theta)
        arc_row(m, -h * s, w + h * s, h * (1 - math.cos(theta)), theta, True)
        arc_row(m, 0, w, h, theta, True)
        return mirror_rows(m, h) if b < 0 else m
    if style in ("arcLower", "arcUpper"):
        flat_row(m, w, 0)
        arc_row(m, 0, w, h, theta, b < 0)
        return mirror_rows(m, h) if style == "arcUpper" else m
    if style in ("arch", "bulge"):
        arc_row(m, 0, w, 0, theta, b > 0)
        arc_row(m, 0, w, h, theta, (b < 0) if style == "bulge" else (b > 0))
        return m
    if style in ("shellLower", "shellUpper"):
        s, c = math.sin(theta), math.cos(theta)
        m["nv"] = 4
        flat_row(m, w, 0)
        flat_row(m, w, h / 3)
        if b > 0:
            r = 2 * h / 3
            m["xs"] += [-r * s, w / 3, 2 * w / 3, w + r * s]
            m["ys"] += [r * c, 2 * h / 3, 2 * h / 3, r * c]
            arc_row(m, -h * s, w + h * s, h * c, theta, False)
        else:
            r = h / 3
            m["xs"] += [-r * s, w / 3, 2 * w / 3, w + r * s]
            m["ys"] += [h - r * c, 2 * h / 3, 2 * h / 3, h - r * c]
            arc_row(m, 0, w, h, theta, True)
        return mirror_rows(m, h) if style == "shellUpper" else m
    if style == "flag":
        flat_row(m, w, 0)
        flat_row(m, w, h)
        m["ys"][1] -= d
        m["ys"][2] += d
        m["ys"][5] -= d
        m["ys"][6] += d
        return m
    if style == "wave":
        m["nv"] = 3
        flat_row(m, w, 0)
        flat_row(m, w, h / 2)
        flat_row(m, w, h)
        m["ys"][5] += d
        m["ys"][6] -= d
        return m
    if style == "fish":
        flat_row(m, w, 0)
        flat_row(m, w, h)
        m["ys"][1] -= d
        m["ys"][2] += d
        m["ys"][5] += d
        m["ys"][6] -= d
        return m
    if style == "rise":
        flat_row(m, w, 0)
        flat_row(m, w, h)
        for i in (0, 1, 4, 5):
            m["ys"][i] += d
        return m
    if style == "fisheye":
        f = identity_mesh(w, h, 4, 4)
        t = b / 50
        for r in (1, 2):
            for c in (1, 2):
                i = r * 4 + c
                f["xs"][i] += t * ((0 if c == 1 else w) - f["xs"][i])
                f["ys"][i] += t * ((0 if r == 1 else h) - f["ys"][i])
        return f
    if style in ("inflate", "squeeze"):
        q = identity_mesh(w, h, 3, 3)
        dx, dy = w * b / 200, h * b / 200
        q["ys"][1] -= dy
        q["ys"][7] += dy
        q["xs"][3] += -dx if style == "inflate" else dx
        q["xs"][5] += dx if style == "inflate" else -dx
        return q
    if style == "twist":
        t = identity_mesh(w, h, 4, 4)
        mx, my = w * abs(b) / 100, h * abs(b) / 100
        if b > 0:
            t["xs"][5] += mx
            t["ys"][6] += my
            t["xs"][10] -= mx
            t["ys"][9] -= my
        else:
            t["ys"][5] += my
            t["xs"][6] -= mx
            t["ys"][10] -= my
            t["xs"][9] += mx
        return t
    raise ValueError(style)


def distort(m, horizontal, vertical):
    hz = min(100, max(-100, horizontal)) / 100
    vt = min(100, max(-100, vertical)) / 100

    def scale(idx, k):
        a, z = idx[0], idx[-1]
        mx = (m["xs"][a] + m["xs"][z]) / 2
        my = (m["ys"][a] + m["ys"][z]) / 2
        for i in idx:
            m["xs"][i] = mx + (m["xs"][i] - mx) * k
            m["ys"][i] = my + (m["ys"][i] - my) * k

    nu, nv = m["nu"], m["nv"]
    if vt != 0:
        for r in range(nv):
            scale([r * nu + c for c in range(nu)], 1 + (2 * (r / (nv - 1)) - 1) * vt)
    if hz != 0:
        for c in range(nu):
            scale([r * nu + c for r in range(nv)], 1 + (2 * (c / (nu - 1)) - 1) * hz)


def bernstein(n, t):
    s = 1 - t
    if n == 2:
        return [s, t]
    if n == 3:
        return [s * s, 2 * s * t, t * t]
    return [s * s * s, 3 * s * s * t, 3 * s * t * t, t * t * t]


def warper(box, style, bend, horizontal, vertical):
    w = box["x1"] - box["x0"]
    h = box["y1"] - box["y0"]
    m = style_mesh(style, bend, w, h)
    distort(m, horizontal, vertical)

    def eval_mesh(u, v):
        wu, wv = bernstein(m["nu"], u), bernstein(m["nv"], v)
        x = y = 0.0
        for r in range(m["nv"]):
            for c in range(m["nu"]):
                k = wu[c] * wv[r]
                x += k * m["xs"][r * m["nu"] + c]
                y += k * m["ys"][r * m["nu"] + c]
        return x, y

    fx0 = fy0 = math.inf
    fx1 = fy1 = -math.inf
    n = 64
    for i in range(n + 1):
        t = i / n
        for p in (eval_mesh(t, 0), eval_mesh(t, 1), eval_mesh(0, t), eval_mesh(1, t)):
            fx0, fx1 = min(fx0, p[0]), max(fx1, p[0])
            fy0, fy1 = min(fy0, p[1]), max(fy1, p[1])
    sx = w / 2 - (fx0 + fx1) / 2
    sy = h / 2 - (fy0 + fy1) / 2

    def mapping(x, y):
        qx, qy = eval_mesh((x - box["x0"]) / w, (box["y1"] - y) / h)
        return box["x0"] + qx + sx, box["y1"] - qy - sy

    frame = {"x0": box["x0"] + fx0 + sx, "y0": box["y1"] - fy1 - sy, "x1": box["x0"] + fx1 + sx, "y1": box["y1"] - fy0 - sy}
    return mapping, frame


def build() -> dict:
    points = []
    for i in range(7):
        for j in range(5):
            points.append([BOX["x0"] - 30 + i * (BOX["x1"] - BOX["x0"] + 60) / 6, BOX["y0"] - 20 + j * (BOX["y1"] - BOX["y0"] + 40) / 4])
    cases = []
    for style in STYLES:
        for bend in (-60, 35):
            for horizontal, vertical in ((0, 0), (25, -40)):
                mapping, frame = warper(BOX, style, bend, horizontal, vertical)
                cases.append({
                    "style": NAMES[style], "bend": bend, "horizontal": horizontal, "vertical": vertical,
                    "frame": [round(frame["x0"], 9), round(frame["y0"], 9), round(frame["x1"], 9), round(frame["y1"], 9)],
                    "mapped": [[round(v, 9) for v in mapping(x, y)] for x, y in points],
                })
    return {"_doc": "Generated by tools/titleart/gen_warp_refs.py (a Python transcription of Pachislop src/text/warp.ts); do not edit.",
            "box": [BOX["x0"], BOX["y0"], BOX["x1"], BOX["y1"]], "points": points, "cases": cases}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = json.dumps(build(), indent=1) + "\n"
    if args.check:
        if not FIXTURE.is_file() or FIXTURE.read_text(encoding="utf-8") != text:
            print(f"{FIXTURE.relative_to(REPO)} is stale: run tools/titleart/gen_warp_refs.py", file=sys.stderr)
            return 1
        return 0
    FIXTURE.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
