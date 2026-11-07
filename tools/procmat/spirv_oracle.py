#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The independent oracle of S90 AC7: GLSL operations computed in Python, in 32-bit floats and
32-bit integers, from the GLSL 4.50 definitions alone. It knows nothing of editor/procmat.

    python3 -I tools/procmat/spirv_oracle.py            writes editor/tests/procmat/SpirvOracleCases.h
    python3 -I tools/procmat/spirv_oracle.py --check    fails when that file is not what it would write

SpirvInterpreterTest.MatchesThePythonOracle compiles each case's GLSL with the editor's glslang, runs it
on SpirvInterpreter for every input set and compares with what is computed here: bit for bit where
IEEE 754 or GLSL defines the result exactly, within the case's tolerance where GLSL leaves the last
bits to the implementation (transcendentals, and sums whose order it does not fix).

A case's inputs reach the shader as uniforms a, b and c (vec4 each), so glslang folds nothing.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "editor/tests/procmat/SpirvOracleCases.h"

NAN = float("nan")
INF = float("inf")


def f32(x: float) -> float:
    """Rounds to the nearest 32-bit float. +, -, *, / and sqrt of two such floats, done in a double
    and rounded once more, are correctly rounded (a double has more than 2 * 24 + 2 bits)."""
    if math.isnan(x) or math.isinf(x):
        return x
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(INF, x)


def bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def from_bits(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def i32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= (1 << 31) else v


def u32(v: int) -> int:
    return v & 0xFFFFFFFF


def add(x, y): return f32(x + y)
def sub(x, y): return f32(x - y)
def mul(x, y): return f32(x * y)
def div(x, y):
    if y == 0.0:
        return NAN if x == 0.0 or math.isnan(x) else math.copysign(INF, x) * math.copysign(1.0, y)
    return f32(x / y)


# GLSL 4.50, 8.3 and 8.5: the definitions as the specification writes them
def floor_(x): return float(math.floor(x)) if math.isfinite(x) else x
def ceil_(x): return float(math.ceil(x)) if math.isfinite(x) else x
def trunc_(x): return float(math.trunc(x)) if math.isfinite(x) else x
def fract(x): return sub(x, floor_(x))
def mod(x, y): return sub(x, mul(y, floor_(div(x, y))))
def sign(x): return 1.0 if x > 0 else (-1.0 if x < 0 else 0.0)
def step(edge, x): return 0.0 if x < edge else 1.0
def fmin(x, y): return y if y < x else x
def fmax(x, y): return y if x < y else x
def clamp(x, lo, hi): return fmin(fmax(x, lo), hi)
def mix(x, y, a): return add(mul(x, sub(1.0, a)), mul(y, a))
def smoothstep(e0, e1, x):
    t = clamp(div(sub(x, e0), sub(e1, e0)), 0.0, 1.0)
    return mul(mul(t, t), sub(3.0, mul(2.0, t)))
def round_(x):
    # GLSL lets a half go either way; the cases never hold one
    return float(math.floor(x + 0.5))


def dot(u, v):
    s = 0.0
    for p, q in zip(u, v):
        s = add(s, mul(p, q))
    return s


def length(u): return f32(math.sqrt(dot(u, u)))
def normalize(u):
    n = length(u)
    return [div(p, n) for p in u]
def cross(u, v):
    return [sub(mul(u[1], v[2]), mul(v[1], u[2])), sub(mul(u[2], v[0]), mul(v[2], u[0])), sub(mul(u[0], v[1]), mul(v[0], u[1]))]


def to_int(x: float) -> int:
    # GLSL: a float converts to int toward zero; the cases stay in range
    return int(math.trunc(x))


def sdiv(x, y): return i32(int(x / y)) if y else 0
def smod(x, y):
    # OpSMod: the sign of the divisor, as GLSL's % on the non-negative operands it defines
    return i32(x - y * math.floor(x / y))


def b(v) -> float: return 1.0 if v else 0.0


# Each case: name, GLSL that sets `vec4 r` from vec4 a, b, c (and helpers), the Python of the same,
# the tolerance (0: the same bits; otherwise relative to max(1, |expected|)), and its input sets.
TRANSCENDENTAL = 4e-6
ORDERED = 1e-6

POSITIVE = [
    ([0.25, 1.5, 2.75, 7.0], [0.5, 3.0, 0.125, 2.5], [1.0, 0.75, 0.3, 4.0]),
    ([3.0, 0.1, 9.5, 0.01], [1.25, 0.6, 0.2, 5.0], [2.0, 0.4, 0.9, 0.05]),
]
SIGNED = [
    ([-1.75, 2.5, -0.375, 0.0], [0.5, -3.25, 1.0, -0.125], [-2.0, 0.6, 1.5, -0.8]),
    ([5.5, -7.125, 0.875, -0.0], [-1.5, 2.0, -6.25, 3.0], [0.25, -0.5, 2.25, 1.0]),
    ([-0.4, 0.9, -12.3, 100.25], [0.7, -0.2, 3.3, -40.5], [-1.1, 1.9, -0.05, 7.7]),
]
UNIT = [
    ([0.2, -0.6, 0.95, -0.05], [0.3, 0.8, -0.4, 0.1], [0.5, 0.25, 0.75, 1.0]),
    ([-0.99, 0.0, 0.45, 0.7], [0.1, -0.9, 0.6, -0.3], [0.0, 1.0, 0.5, 0.1]),
]
COUNTS = [
    ([1.0, 2.0, 3.0, 4.0], [5.5, 0.0, 0.0, 0.0], [4.0, 1.0, 3.0, 0.0]),
    ([0.5, 4.0, 1.5, 2.0], [100.0, 0.0, 0.0, 0.0], [3.0, 2.0, 0.0, 1.0]),
    ([2.0, 2.0, 2.0, 2.0], [3.0, 0.0, 0.0, 0.0], [4.0, 3.0, 1.0, 2.0]),
]
INTEGERS = [
    ([7.9, -7.9, 123456.0, 3.0], [2.0, 3.0, 1000.0, 5.0], [3.0, 1.0, 4.0, 31.0]),
    ([255.5, 1.0, 65535.0, 17.0], [16.0, 7.0, 3.0, 4.0], [1.0, 8.0, 12.0, 2.0]),
    ([-100.25, 42.0, 2147483.0, 9.0], [7.0, 6.0, 9.0, 2.0], [5.0, 3.0, 20.0, 1.0]),
]
COMPARE = [
    ([1.0, 2.0, NAN, -0.0], [1.0, 3.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0]),
    ([5.0, -1.0, 2.0, NAN], [4.0, -1.0, NAN, 7.0], [1.0, -1.0, 0.0, 1.0]),
]


def per(f, *vs):
    return [f(*xs) for xs in zip(*vs)]


CASES = []


def case(name, glsl, tolerance, inputs, helpers=""):
    def wrap(fn):
        CASES.append((name, glsl, helpers, tolerance, inputs, fn))
        return fn
    return wrap


@case("arithmetic", "r = vec4(a.x + b.x, a.y - b.y, a.z * b.z, a.w / b.w) - vec4(0.0, c.y, 0.0, 0.0) * 0.5 + (-c);", 0, SIGNED + POSITIVE)
def _(a, b_, c):
    r = [add(a[0], b_[0]), sub(a[1], b_[1]), mul(a[2], b_[2]), div(a[3], b_[3])]
    r = [sub(r[0], 0.0), sub(r[1], mul(c[1], 0.5)), sub(r[2], 0.0), sub(r[3], 0.0)]
    return [add(p, -q) for p, q in zip(r, c)]


@case("rounding", "r = vec4(floor(a.x), ceil(a.y), fract(a.z), trunc(a.w)) + vec4(0.0, 0.0, 0.0, round(b.x * 4.0 + 0.1));", 0, SIGNED + POSITIVE)
def _(a, b_, c):
    return [floor_(a[0]), ceil_(a[1]), fract(a[2]), add(trunc_(a[3]), round_(add(mul(b_[0], 4.0), f32(0.1))))]


@case("mod_sign_step", "r = vec4(mod(a.x, b.x), sign(a.y), step(b.z, a.z), abs(a.w));", 0, SIGNED)
def _(a, b_, c):
    return [mod(a[0], b_[0]), sign(a[1]), step(b_[2], a[2]), abs(a[3])]


@case("min_max_clamp", "r = vec4(min(a.x, b.x), max(a.y, b.y), clamp(a.z, -1.0, 1.0), clamp(a.w, b.w, c.w + 10.0));", 0, SIGNED)
def _(a, b_, c):
    return [fmin(a[0], b_[0]), fmax(a[1], b_[1]), clamp(a[2], -1.0, 1.0), clamp(a[3], b_[3], add(c[3], 10.0))]


@case("mix_smoothstep", "r = vec4(mix(a.x, b.x, c.x), mix(a.yz, b.yz, c.yz), smoothstep(-1.0, 2.0, a.w));", ORDERED, SIGNED + UNIT)
def _(a, b_, c):
    return [mix(a[0], b_[0], c[0]), mix(a[1], b_[1], c[1]), mix(a[2], b_[2], c[2]), smoothstep(-1.0, 2.0, a[3])]


@case("exp_log_pow_sqrt", "r = vec4(exp(a.x), log(a.y), pow(a.z, b.z), sqrt(a.w)) + vec4(exp2(b.x), log2(b.y), 0.0, 0.0);", TRANSCENDENTAL, POSITIVE)
def _(a, b_, c):
    return [add(f32(math.exp(a[0])), f32(2.0 ** b_[0])), add(f32(math.log(a[1])), f32(math.log2(b_[1]))),
            f32(a[2] ** b_[2]), f32(math.sqrt(a[3]))]


@case("trigonometry", "r = vec4(sin(a.x), cos(a.y), tan(a.z), atan(a.w)) + vec4(asin(c.x), acos(c.y), atan(c.z, c.w), 0.0);", TRANSCENDENTAL, UNIT)
def _(a, b_, c):
    return [add(f32(math.sin(a[0])), f32(math.asin(c[0]))), add(f32(math.cos(a[1])), f32(math.acos(c[1]))),
            add(f32(math.tan(a[2])), f32(math.atan2(c[2], c[3]))), f32(math.atan(a[3]))]


@case("hyperbolic_angles", "r = vec4(sinh(a.x), cosh(a.y), tanh(a.z), radians(a.w * 90.0) + degrees(b.w));", TRANSCENDENTAL, UNIT)
def _(a, b_, c):
    return [f32(math.sinh(a[0])), f32(math.cosh(a[1])), f32(math.tanh(a[2])),
            add(mul(mul(a[3], 90.0), f32(math.pi / 180.0)), mul(b_[3], f32(180.0 / math.pi)))]


@case("geometry", "r = vec4(dot(a, b), length(a.xyz), distance(a.xy, b.xy), normalize(c.xyz).y) + vec4(cross(a.xyz, b.xyz), 0.0);", ORDERED, SIGNED + UNIT)
def _(a, b_, c):
    x = cross(a[:3], b_[:3])
    return [add(dot(a, b_), x[0]), add(length(a[:3]), x[1]), add(length([sub(a[0], b_[0]), sub(a[1], b_[1])]), x[2]), normalize(c[:3])[1]]


@case("compare", "r = vec4(lessThan(a, b)) + 2.0 * vec4(greaterThanEqual(a, b)) + 4.0 * vec4(equal(a, b)) + 8.0 * vec4(notEqual(a, b));", 0, COMPARE + SIGNED)
def _(a, b_, c):
    return [b(p < q) + 2 * b(p >= q) + 4 * b(p == q) + 8 * b(p != q) for p, q in zip(a, b_)]


@case("compare_more", "r = vec4(greaterThan(a, b)) + 2.0 * vec4(lessThanEqual(a, b)) + 4.0 * vec4(isnan(a)) + 8.0 * vec4(not(lessThan(a, b)));", 0, COMPARE + SIGNED)
def _(a, b_, c):
    return [b(p > q) + 2 * b(p <= q) + 4 * b(math.isnan(p)) + 8 * b(not p < q) for p, q in zip(a, b_)]


@case("logic_select", "bvec4 l = lessThan(a, b); r = vec4(any(l) ? 1.0 : 0.0, all(l) ? 1.0 : 0.0, (a.x < b.x) == (c.x > 0.0) ? 1.0 : 0.0, (a.y < b.y) != (c.y > 0.0) ? 1.0 : 0.0) + mix(vec4(10.0), vec4(20.0), l) + ((a.z > 0.0 && b.z > 0.0) || c.z > 1.0 ? 100.0 : 0.0);", 0, SIGNED + POSITIVE)
def _(a, b_, c):
    l = [p < q for p, q in zip(a, b_)]
    head = [b(any(l)), b(all(l)), b((a[0] < b_[0]) == (c[0] > 0)), b((a[1] < b_[1]) != (c[1] > 0))]
    tail = 100.0 if (a[2] > 0 and b_[2] > 0) or c[2] > 1.0 else 0.0
    return [add(add(h, 20.0 if x else 10.0), tail) for h, x in zip(head, l)]


@case("integers", "ivec4 i = ivec4(a); ivec4 j = ivec4(b); r = vec4(i.x + j.x * 3 - i.w, i.y / j.y, i.z % j.z, -i.w) + vec4(abs(i.y), min(i.x, j.x), max(i.z, j.z), clamp(i.y, -2, 2));", 0, INTEGERS)
def _(a, b_, c):
    i = [to_int(x) for x in a]
    j = [to_int(x) for x in b_]
    head = [i32(i[0] + j[0] * 3 - i[3]), sdiv(i[1], j[1]), smod(i[2], j[2]), i32(-i[3])]
    tail = [abs(i[1]), min(i[0], j[0]), max(i[2], j[2]), max(-2, min(2, i[1]))]
    return [float(h) + float(t) for h, t in zip(head, tail)]


@case("integer_bits", "ivec4 i = ivec4(a); ivec4 s = ivec4(c); r = vec4(i.x << s.x, i.y >> s.y, (i.z & 0xff) | (i.w ^ 5), ~i.w) + vec4(0.0, float(-i.y >> 1), 0.0, 0.0);", 0, INTEGERS)
def _(a, b_, c):
    i = [to_int(x) for x in a]
    s = [to_int(x) for x in c]
    return [float(i32(i[0] << s[0])), float(i[1] >> s[1]) + float((-i[1]) >> 1), float((i[2] & 0xFF) | (i[3] ^ 5)), float(i32(~i[3]))]


@case("unsigned_hash", "uvec4 u = uvec4(abs(a)); uvec4 v = uvec4(b); u = u * 0x9E3779B9u + v; u ^= u >> 16u; r = uintBitsToFloat((u & 0x007fffffu) | 0x3f800000u) + vec4(float(u.x / 3u % 1000u), float(min(u.y, v.y)), float(max(u.z, 7u)), float(u.w > v.w));", 0, INTEGERS)
def _(a, b_, c):
    u = [u32(int(abs(x))) for x in a]
    v = [u32(to_int(x)) for x in b_]
    u = [u32(p * 0x9E3779B9 + q) for p, q in zip(u, v)]
    u = [p ^ (p >> 16) for p in u]
    head = [from_bits((p & 0x007FFFFF) | 0x3F800000) for p in u]
    tail = [float((u[0] // 3) % 1000), float(min(u[1], v[1])), float(max(u[2], 7)), b(u[3] > v[3])]
    return [add(h, f32(t)) for h, t in zip(head, tail)]


@case("conversions", "r = vec4(float(int(a.x)), float(uint(abs(a.y))), float(int(a.z) * 2) * 0.5, floatBitsToUint(a.w) == 0x80000000u ? -1.0 : float(bool(a.w)));", 0, SIGNED + INTEGERS)
def _(a, b_, c):
    w = -1.0 if bits(a[3]) == 0x80000000 else b(a[3] != 0.0)
    return [float(to_int(a[0])), f32(float(u32(int(abs(a[1]))))), mul(f32(float(i32(to_int(a[2]) * 2))), 0.5), w]


@case("swizzle_insert_dynamic", "vec4 t = a.wzyx; t.y = b.x; int k = int(c.x) & 3; t[k] = t[k] + 100.0; float arr[4] = float[4](b.x, b.y, b.z, b.w); r = t + vec4(a[int(c.y) & 3], arr[int(c.z) & 3], arr[k], 0.0);", 0, COUNTS)
def _(a, b_, c):
    t = [a[3], a[2], a[1], a[0]]
    t[1] = b_[0]
    k = to_int(c[0]) & 3
    t[k] = add(t[k], 100.0)
    extra = [a[to_int(c[1]) & 3], b_[to_int(c[2]) & 3], b_[k], 0.0]
    return [add(p, q) for p, q in zip(t, extra)]


@case("matrices", "mat2 m = mat2(a.xy, a.zw); mat2 n = mat2(b.xy, b.zw); vec2 p = m * c.xy; vec2 q = c.zw * m; mat2 o = m * n * 0.5; r = vec4(p + q, o[0][1] + o[1][0], o[1][1]);", ORDERED, SIGNED + POSITIVE)
def _(a, b_, c):
    m = [[a[0], a[1]], [a[2], a[3]]]   # columns
    n = [[b_[0], b_[1]], [b_[2], b_[3]]]
    p = [add(mul(m[0][0], c[0]), mul(m[1][0], c[1])), add(mul(m[0][1], c[0]), mul(m[1][1], c[1]))]
    q = [add(mul(c[2], m[0][0]), mul(c[3], m[0][1])), add(mul(c[2], m[1][0]), mul(c[3], m[1][1]))]
    o = [[mul(add(mul(m[0][row], n[col][0]), mul(m[1][row], n[col][1])), 0.5) for row in range(2)] for col in range(2)]
    return [add(p[0], q[0]), add(p[1], q[1]), add(o[0][1], o[1][0]), o[1][1]]


@case("loops_and_calls", "float s = 0.0; int n = int(c.x); for (int i = 0; i < n; i++) { if (i == int(c.y)) continue; s += a[i & 3]; if (s > b.x) break; } vec2 w = vec2(s, 0.0); bump(w, c.z); int k = 0; while (k < int(c.w)) k += 2; r = vec4(s, w, float(k));", 0, COUNTS,
      helpers="void bump(inout vec2 v, float by) { v += vec2(by, by * 2.0); }\n")
def _(a, b_, c):
    s = 0.0
    for i in range(to_int(c[0])):
        if i == to_int(c[1]):
            continue
        s = add(s, a[i & 3])
        if s > b_[0]:
            break
    w = [add(s, c[2]), add(0.0, mul(c[2], 2.0))]
    k = 0
    while k < to_int(c[3]):
        k += 2
    return [s, w[0], w[1], float(k)]


@case("special_values", "r = vec4(a.x / b.x, b.y / a.y, min(a.z, 1.0), max(1.0, a.z)) * vec4(1.0, 1.0, 1.0, 1.0);", 0,
      [([1.0, 0.0, 2.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0] * 4), ([-1.0, -0.0, -3.0, 0.0], [0.0, 2.0, 0.0, 0.0], [0.0] * 4)])
def _(a, b_, c):
    return [div(a[0], b_[0]), div(b_[1], a[1]), fmin(a[2], 1.0), fmax(1.0, a[2])]


SHADER = """#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform u_fs_procmat {
    vec4 v[3];
} pm;
"""


def c_float(x: float) -> str:
    return f"0x{bits(x):08x}u"


def render() -> str:
    lines = [
        "// (c) Eduardo Doria and contributors",
        "// SPDX-License-Identifier: MIT",
        "",
        "// Generated by tools/procmat/spirv_oracle.py (S90 AC7). Do not edit.",
        "",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace jasperengine::editor::procmat::test {",
        "",
        "    // a, b, c then the expected r, each four floats as bits",
        "    struct SpirvOracleSet {",
        "        uint32_t inputs[12];",
        "        uint32_t expected[4];",
        "    };",
        "",
        "    struct SpirvOracleCase {",
        "        const char* name;",
        "        const char* fragment;",
        "        // 0: the same bits (any NaN matches a NaN); else relative to max(1, |expected|)",
        "        float tolerance;",
        "        const SpirvOracleSet* sets;",
        "        int setCount;",
        "    };",
        "",
    ]
    for index, (name, glsl, helpers, tolerance, inputs, fn) in enumerate(CASES):
        rows = []
        for a, b_, c in inputs:
            a, b_, c = [f32(x) for x in a], [f32(x) for x in b_], [f32(x) for x in c]
            expected = [f32(x) for x in fn(a, b_, c)]
            rows.append("        {{" + ", ".join(c_float(x) for x in a + b_ + c) + "}, {" + ", ".join(c_float(x) for x in expected) + "}},")
        lines.append(f"    inline const SpirvOracleSet oracleSets{index}[] = {{")
        lines.extend(rows)
        lines.append("    };")
    lines.append("")
    lines.append("    inline const SpirvOracleCase oracleCases[] = {")
    for index, (name, glsl, helpers, tolerance, inputs, fn) in enumerate(CASES):
        fragment = SHADER + helpers + "void main() {\n    vec4 a = pm.v[0];\n    vec4 b = pm.v[1];\n    vec4 c = pm.v[2];\n    vec4 r;\n    " + glsl + "\n    g_finalColor = r;\n}\n"
        literal = fragment.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n")
        lines.append(f"        {{\"{name}\", \"{literal}\", {float(tolerance)!r}f, oracleSets{index}, {len(inputs)}}},")
    lines.append("    };")
    lines.append("")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail when the committed header is stale")
    args = parser.parse_args(argv)
    text = render()
    if args.check:
        if OUT.read_text(encoding="utf-8") != text:
            print(f"{OUT} is stale: run tools/procmat/spirv_oracle.py", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT}: {len(CASES)} cases, {sum(len(c[4]) for c in CASES)} input sets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
