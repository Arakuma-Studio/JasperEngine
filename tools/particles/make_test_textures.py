#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Writes the textures of the particle GPU goldens (engine/tests/core/render/ParticlesGpuTest.cpp)
# into engine/tests/core/fixtures/particles/textures: a soft spark, a 4 x 4 sprite sheet of numbered
# colours (frame blending), and the two six-way smoke maps of a lit puff (RGBA layout:
# A = right, top, back, opacity; B = left, bottom, front, emission), and a lens distortion map
# (library:particles DISTORTION: red and green 0.5 shift nothing). Deterministic: run it again
# and the files are the same.
#
#   python3 tools/particles/make_test_textures.py

import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mcp"))
import pngdiff  # noqa: E402

OUT = os.path.join(ROOT, "engine", "tests", "core", "fixtures", "particles", "textures")


def clamp(v):
    return max(0, min(255, int(round(v))))


def spark(size=64):
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) / size * 2 - 1
            dy = (y + 0.5) / size * 2 - 1
            d = math.sqrt(dx * dx + dy * dy)
            a = max(0.0, 1.0 - d)
            a = a * a * (3 - 2 * a)
            core = max(0.0, 1.0 - d * 3)
            rgba += bytes((255, clamp(200 + 55 * core), clamp(120 + 135 * core), clamp(255 * a)))
    return size, size, rgba


def sheet(cell=32, tiles=4):
    size = cell * tiles
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            index = (y // cell) * tiles + (x // cell)
            hue = index / (tiles * tiles)
            r = 0.5 + 0.5 * math.cos(2 * math.pi * (hue + 0.0))
            g = 0.5 + 0.5 * math.cos(2 * math.pi * (hue + 0.33))
            b = 0.5 + 0.5 * math.cos(2 * math.pi * (hue + 0.67))
            cx = (x % cell + 0.5) / cell * 2 - 1
            cy = (y % cell + 0.5) / cell * 2 - 1
            d = math.sqrt(cx * cx + cy * cy)
            a = 255 if d < 0.9 else 0
            dot = d < 0.25
            rgba += bytes((255, 255, 255, a) if dot else (clamp(r * 255), clamp(g * 255), clamp(b * 255), a))
    return size, size, rgba


def sixway(size=64):
    a = bytearray()
    b = bytearray()
    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) / size * 2 - 1
            dy = 1 - (y + 0.5) / size * 2
            r2 = dx * dx + dy * dy
            if r2 >= 1.0:
                a += bytes((0, 0, 0, 0))
                b += bytes((0, 0, 0, 0))
                continue
            dz = math.sqrt(1 - r2)
            opacity = (1 - r2) ** 0.7
            # the light from each side on a sphere (+X, +Y, +Z back; -X, -Y, -Z front)
            right, left = max(0.0, dx), max(0.0, -dx)
            top, bottom = max(0.0, dy), max(0.0, -dy)
            back, front = 0.35 * dz, dz
            emission = 0.0
            a += bytes((clamp(right * 255), clamp(top * 255), clamp(back * 255), clamp(opacity * 255)))
            b += bytes((clamp(left * 255), clamp(bottom * 255), clamp(front * 255), clamp(emission * 255)))
    return (size, size, a), (size, size, b)


def distortion(size=64):
    # outward from the centre, strongest half-way out, nothing at the centre and from the rim
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) / size * 2 - 1
            dy = (y + 0.5) / size * 2 - 1
            r = math.sqrt(dx * dx + dy * dy)
            amount = math.sin(math.pi * r) if 0.0 < r < 1.0 else 0.0
            sx = dx / r * amount if r > 0.0 else 0.0
            sy = dy / r * amount if r > 0.0 else 0.0
            rgba += bytes((clamp(127.5 + 127.5 * sx), clamp(127.5 + 127.5 * sy), 128, 255))
    return size, size, rgba


def main():
    os.makedirs(OUT, exist_ok=True)
    images = {"spark.png": spark(), "sheet.png": sheet(), "distortion.png": distortion()}
    images["sixway_a.png"], images["sixway_b.png"] = sixway()
    for name, (w, h, rgba) in images.items():
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(pngdiff.encode(w, h, bytes(rgba)))
        print("wrote", os.path.relpath(os.path.join(OUT, name), ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
