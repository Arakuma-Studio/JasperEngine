#!/usr/bin/env python3
# A pure-Python port of the reference math of the shaders.com kit (packages/core/src/gpu/kit,
# MIT, Copyright (c) 2026 Shader Effects Inc.; see engine/shaderlib/effects/LICENSE.shaders-com.txt)
# that the UI effects' include library (engine/shaderlib/effects/lib/) ports to GLSL:
#
#   hash22        noise.ts hash22 (the bit-cast "Fi" hash: f32 multiply, floatBitsToUint, u32 wrap)
#   perlin12      noise.ts perlin12 (gradient noise from hash22, x 0.7 + 0.5)
#   mxNoiseFloat3 noise.ts mxNoiseFloat3 (MaterialX perlin: Bob Jenkins lookup3 hash)
#   linearToSrgb  tonemap.ts linearToSrgb
#   rgbToOklab    colorMixing.ts rgbToOklab
#   fbmPerlin     noise.ts fbmPerlin
#
# EffectLibTest (engine/tests/htmlui/effects/EffectLibTest.cpp) renders
# engine/tests/htmlui/fixtures/effects/fx/lib-test.frag, which evaluates the GLSL library at the
# same fixed inputs (one case per pixel), and compares the 8-bit result with the values this
# script writes to engine/tests/htmlui/effects/kit_reference.json (tolerance 1/255).
#
#   python3 tools/htmlui/kit_reference.py [--check] [--print]
#
# --check exits 1 when the JSON is stale; --print shows the values. The f32 steps that change
# bits (the hash inputs, u32 arithmetic) are emulated exactly; the rest runs in double, well
# inside the tolerance.

import json
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUTPUT = os.path.join(ROOT, "engine", "tests", "htmlui", "effects", "kit_reference.json")
CASES = 32
U32 = 0xFFFFFFFF


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def float_bits(x):
    return struct.unpack("<I", struct.pack("<f", f32(x)))[0]


def u32(x):
    return x & U32


# float(~0u) in f32 (4294967295.0 rounds to 4294967296.0)
U32_MAX_F = f32(4294967295.0)


def u32_to_float01(u):
    return f32(f32(float(u)) / U32_MAX_F)


# ---- noise.ts: the Fi hashes

def hash22(p):
    ux = float_bits(f32(f32(p[0]) * f32(141421356.0)))
    uy = float_bits(f32(f32(p[1]) * f32(2718281828.0)))
    h = ux ^ uy
    return (u32_to_float01(u32(h * 3141592653)), u32_to_float01(u32(h * 1618033988)))


def perlin_grad2(i, f, o):
    h = hash22((i[0] + o[0], i[1] + o[1]))
    g = (h[0] - 0.5, h[1] - 0.5)
    length = math.sqrt(g[0] * g[0] + g[1] * g[1])
    g = (g[0] / length, g[1] / length)
    return g[0] * (f[0] - o[0]) + g[1] * (f[1] - o[1])


def mix(a, b, t):
    return a * (1.0 - t) + b * t


def perlin12(p):
    i = (math.floor(p[0]), math.floor(p[1]))
    f = (p[0] - i[0], p[1] - i[1])
    u = tuple(c * c * c * (c * (c * 6.0 - 15.0) + 10.0) for c in f)
    a = perlin_grad2(i, f, (0.0, 0.0))
    b = perlin_grad2(i, f, (1.0, 0.0))
    c = perlin_grad2(i, f, (0.0, 1.0))
    d = perlin_grad2(i, f, (1.0, 1.0))
    return mix(mix(a, b, u[0]), mix(c, d, u[0]), u[1]) * 0.7 + 0.5


def fbm_perlin(p, octaves):
    s = 0.0
    m = 0.0
    a = 1.0
    pp = p
    for _ in range(octaves):
        s += a * perlin12(pp)
        m += a
        a *= 0.5
        pp = (pp[0] * 2.0, pp[1] * 2.0)
    return s / m


# ---- noise.ts: MaterialX (lookup3)

def rotl32(x, k):
    return u32((x << k) | (x >> (32 - k)))


def bjfinal(a, b, c):
    c = u32(c ^ b); c = u32(c - rotl32(b, 14))
    a = u32(a ^ c); a = u32(a - rotl32(c, 11))
    b = u32(b ^ a); b = u32(b - rotl32(a, 25))
    c = u32(c ^ b); c = u32(c - rotl32(b, 16))
    a = u32(a ^ c); a = u32(a - rotl32(c, 4))
    b = u32(b ^ a); b = u32(b - rotl32(a, 14))
    c = u32(c ^ b); c = u32(c - rotl32(b, 24))
    return c


def mx_hash3(x, y, z):
    seed = u32(0xDEADBEEF + (3 << 2) + 13)
    return bjfinal(u32(seed + u32(x)), u32(seed + u32(y)), u32(seed + u32(z)))


def mx_fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def mx_gradient3(h, x, y, z):
    h &= 15
    u = x if h < 8 else y
    v = y if h < 4 else (x if h in (12, 14) else z)
    return (-u if h & 1 else u) + (-v if h & 2 else v)


def mx_noise3(p):
    X, Y, Z = (int(math.floor(c)) for c in p)
    fx, fy, fz = p[0] - X, p[1] - Y, p[2] - Z
    u, v, w = mx_fade(fx), mx_fade(fy), mx_fade(fz)
    g = lambda dx, dy, dz: mx_gradient3(mx_hash3(X + dx, Y + dy, Z + dz), fx - dx, fy - dy, fz - dz)
    s1, t1, r1 = 1.0 - u, 1.0 - v, 1.0 - w
    result = (r1 * (t1 * (g(0, 0, 0) * s1 + g(1, 0, 0) * u) + v * (g(0, 1, 0) * s1 + g(1, 1, 0) * u)) +
              w * (t1 * (g(0, 0, 1) * s1 + g(1, 0, 1) * u) + v * (g(0, 1, 1) * s1 + g(1, 1, 1) * u)))
    return 0.982 * result


# ---- tonemap.ts, colorMixing.ts

def linear_to_srgb(c):
    if c <= 0.0031308:
        return c * 12.92
    return math.pow(c, 0.41666) * 1.055 - 0.055


def cbrt(x):
    return math.copysign(abs(x) ** (1.0 / 3.0), x)


def rgb_to_oklab(rgb):
    r, g, b = rgb
    l = r * 0.4122214708 + g * 0.5363325363 + b * 0.0514459929
    m = r * 0.2119034982 + (g * 0.6806995451 + b * 0.1073969566)
    s = r * 0.0883024619 + g * 0.2817188376 + b * 0.6299787005
    l_, m_, s_ = cbrt(l), cbrt(m), cbrt(s)
    return (l_ * 0.2104542553 + m_ * 0.7936177850 - s_ * 0.0040720468,
            l_ * 1.9779984951 - m_ * 2.4285922050 + s_ * 0.4505937099,
            l_ * 0.0259040371 + m_ * 0.7827717662 - s_ * 0.8086757660)


def fract(x):
    return x - math.floor(x)


# ---- the cases of lib-test.frag

def inputs(i):
    p = (i * 0.5 - 7.25, i * 0.25 + 0.375)
    q = (i * 0.5 - 7.25, i * 0.25 + 0.375, i * 0.125 - 1.0625)
    rgb = (fract(i * 0.375), fract(i * 0.625), fract(i * 0.8125))
    return p, q, rgb


def clamp01(x):
    return min(max(x, 0.0), 1.0)


def rows():
    out = [[], [], []]
    for i in range(CASES):
        p, q, rgb = inputs(i)
        h = hash22(p)
        out[0].append([h[0], h[1], perlin12(p)])
        out[1].append([mx_noise3(q) * 0.5 + 0.5, fbm_perlin(p, 4), linear_to_srgb(i / 32.0)])
        lab = rgb_to_oklab(rgb)
        out[2].append([lab[0], lab[1] + 0.5, lab[2] + 0.5])
    return [[[round(clamp01(v), 6) for v in case] for case in row] for row in out]


def document():
    return {
        "comment": "Generated by tools/htmlui/kit_reference.py: the shaders.com kit's reference math at the inputs of "
                   "engine/tests/htmlui/fixtures/effects/fx/lib-test.frag (rows: [hash22.x, hash22.y, perlin12], "
                   "[mxNoiseFloat3 * 0.5 + 0.5, fbmPerlin(4), linearToSrgb], [oklab L, a + 0.5, b + 0.5]). Do not edit.",
        "cases": CASES,
        "rows": rows(),
    }


def main():
    args = sys.argv[1:]
    text = json.dumps(document(), indent=1) + "\n"
    if "--print" in args:
        for r, row in enumerate(document()["rows"]):
            print("row", r)
            for i, case in enumerate(row):
                print("  %2d  %s" % (i, "  ".join("%.6f" % v for v in case)))
    current = open(OUTPUT, encoding="utf-8").read() if os.path.exists(OUTPUT) else None
    if "--check" in args:
        if current != text:
            print("%s is stale: run tools/htmlui/kit_reference.py" % os.path.relpath(OUTPUT, ROOT))
            sys.exit(1)
        print("kit_reference.json is current")
        return
    if current != text:
        with open(OUTPUT, "w", encoding="utf-8") as f:
            f.write(text)
        print("wrote %s" % os.path.relpath(OUTPUT, ROOT))
    else:
        print("kit_reference.json is current")


if __name__ == "__main__":
    main()
