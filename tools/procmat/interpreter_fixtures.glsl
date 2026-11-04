// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The fragment shaders of editor/tests/procmat/SpirvInterpreterTest.cpp. A line "//# name" starts a
// shader; tools/procmat/gen_interpreter_fixtures.py compiles them with procmat-sweep. GLSL is the
// source of truth for the test: the SPIR-V is glslang's, as a real pass would give the interpreter.

//# arithmetic
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform u_fs_procmat { vec4 v[2]; } pm;
void main() {
    vec3 a = vec3(v_uv, pm.v[0].x);
    vec3 b = a.zxy * 2.0 + vec3(1.0, -1.0, 0.5);
    float m = mod(-b.x, 0.75) + fract(b.y) + step(0.5, a.x) + smoothstep(0.0, 2.0, b.z);
    vec2 n = normalize(a.xy + vec2(1.0));
    float l = length(a) + dot(a, b) + distance(a, b) + cross(a, b).z;
    float c = clamp(b.x, 0.25, 0.5) + mix(a.x, a.y, 0.25) + min(a.x, b.x) + max(a.y, b.y) + abs(-a.x) + sign(-b.y) + floor(b.z) + ceil(b.z);
    float t = sin(a.x) + cos(a.y) + pow(a.x + 1.0, 1.5) + exp(a.x) + log(a.y + 1.0) + sqrt(a.x) + atan(a.y, a.x + 1.0) + radians(a.x) + round(b.x * 3.0);
    g_finalColor = vec4(m, n.x * n.y, l + c, t);
}

//# integers
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
void main() {
    int i = int(v_uv.x * 1000.0) - 300;
    uint u = uint(v_uv.y * 1000.0);
    int d = i / 7 + i % 7 + (i << 3) + (i >> 2) + (i & 255) + (i | 8) + (i ^ 77) + abs(i) + min(i, 5) + max(i, -5) + clamp(i, -2, 2) + sign(i);
    uint e = u * 2654435761u;
    e ^= e >> 16u;
    e = (e << 7u) | (e >> 25u);
    e += min(u, 5u) + max(u, 9u) + u / 3u + u % 3u;
    ivec2 pair = ivec2(i, i + 1) * ivec2(3, -2);
    int zero = int(v_uv.x * 0.0);
    int safe = i / zero + i % zero;
    g_finalColor = vec4(float(d), float(e >> 8u), float(pair.x + pair.y), float(safe) * 0.0 + float(uint(pair.x) >> 28u));
}

//# control_flow
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
const float table[4] = float[4](0.125, 0.25, 0.5, 1.0);
struct Item { vec2 p; float w; };
float weight(Item item, inout float total, out int visits) {
    total += item.w;
    visits = 3;
    return item.p.x * item.w;
}
float steps(float x, int count) {
    float sum = 0.0;
    for (int i = 0; i < count; ++i) {
        if (i == 2)
            continue;
        if (float(i) > x * 8.0)
            break;
        sum += table[i % 4];
    }
    return sum;
}
float choose(int k) {
    if (k == 0)
        return 1.5;
    else if (k == 1 || k == 2)
        return 2.5;
    return -1.0;
}
void main() {
    float total = 1.0;
    int visits = 0;
    Item item = Item(v_uv, 0.5);
    float w = weight(item, total, visits);
    bool both = v_uv.x > 0.25 && v_uv.y > 0.25;
    bool either = v_uv.x > 0.75 || v_uv.y > 0.75;
    float picked = both ? 1.0 : (either ? 2.0 : 3.0);
    int index = int(v_uv.x * 4.0);
    float vectorPart = vec4(1.0, 2.0, 3.0, 4.0)[index];
    float a[3];
    a[0] = v_uv.x;
    a[1] = v_uv.y;
    a[2] = a[index % 2] + 1.0;
    vec4 inserted = vec4(0.0);
    inserted[index] = 2.0;
    mat2 m = mat2(1.0, 2.0, 3.0, v_uv.x);
    vec2 mv = m * v_uv + v_uv * m;
    mat2 mm = m * m;
    g_finalColor = vec4(w + total + float(visits), steps(v_uv.x, 6) + choose(index) + picked, vectorPart + a[2] + inserted[index] + inserted[(index + 1) % 4], mv.x + mv.y + mm[1][0]);
}

//# rand
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
int jp_quantize(float x) {
    return int(floor(clamp(x, -32767.0, 32767.0) * 65536.0 + 0.5));
}
uint jp_hash_mix(uint h, int value) {
    uint k = uint(value) * 0xcc9e2d51u;
    k = ((k << 15u) | (k >> 17u)) * 0x1b873593u;
    h ^= k;
    h = (h << 13u) | (h >> 19u);
    return h * 5u + 0xe6546b64u;
}
float jp_hash_float(uint h, uint stream) {
    h ^= stream * 0x9e3779b9u;
    h ^= h >> 16u;
    h *= 0x85ebca6bu;
    h ^= h >> 13u;
    h *= 0xc2b2ae35u;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0);
}
void main() {
    uint h = jp_hash_mix(jp_hash_mix(0x2f8b15c7u, jp_quantize(v_uv.x)), jp_quantize(v_uv.y));
    g_finalColor = vec4(jp_hash_float(h, 1u), jp_hash_float(h, 2u), jp_hash_float(h, 3u), 1.0);
}

//# sampler
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform texture2D t_input;
uniform sampler s_input;
void main() {
    vec4 base = textureLod(sampler2D(t_input, s_input), v_uv, 0.0);
    vec4 wrapped = textureLod(sampler2D(t_input, s_input), v_uv + vec2(3.0, -2.0), 0.0);
    vec4 coarse = textureLod(sampler2D(t_input, s_input), v_uv, 1.0);
    g_finalColor = vec4(base.r, wrapped.r, coarse.r, textureLod(sampler2D(t_input, s_input), v_uv, 0.5).r);
}

//# nan_rules
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform u_fs_procmat { vec4 v[1]; } pm;
void main() {
    float nan = pm.v[0].x / pm.v[0].x;
    g_finalColor = vec4(clamp(nan, 0.25, 0.75), min(nan, 0.5), max(nan, 0.5), isnan(nan) ? 2.0 : 1.0);
}

//# endless
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
void main() {
    float x = v_uv.x;
    while (x < 2.0)
        x = x * 1.0;
    g_finalColor = vec4(x);
}

//# uses_derivative
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
void main() {
    g_finalColor = vec4(dFdx(v_uv.x));
}

//# uses_discard
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
void main() {
    if (v_uv.x < 0.5)
        discard;
    g_finalColor = vec4(1.0);
}

//# swizzle_alias
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
vec2 bump(inout vec3 p) {
    p += 1.0;
    return vec2(0.0);
}
void main() {
    vec3 v = vec3(v_uv, 0.0);
    vec2 r = v.xy + bump(v);
    g_finalColor = vec4(r, v.z, 1.0);
}

//# private_uninitialized
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
float g;
void main() {
    if (v_uv.x < 0.5)
        g = 1.0;
    g_finalColor = vec4(g);
}

//# output_partly_written
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
void main() {
    if (v_uv.x < 0.5)
        g_finalColor = vec4(1.0);
}

//# image_array
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform texture2DArray t_input;
uniform sampler s_input;
void main() {
    g_finalColor = textureLod(sampler2DArray(t_input, s_input), vec3(v_uv, 0.0), 0.0);
}

//# sign_of_zero
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
void main() {
    float y = v_uv.x - 1.0;
    g_finalColor = vec4(atan(0.0, ceil(y)), atan(0.0, trunc(y)), 1.0 / ceil(y), atan(0.0, floor(-(v_uv.x - v_uv.x))));
}

//# sample_nan
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform u_fs_procmat { vec4 v[1]; } pm;
uniform texture2D t_input;
uniform sampler s_input;
void main() {
    float nan = pm.v[0].x / pm.v[0].x;
    g_finalColor = textureLod(sampler2D(t_input, s_input), vec2(nan, 1.0 / pm.v[0].x), 0.0);
}

//# block_with_padding
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
uniform u_fs_padded { float a; vec4 b; } pm;
void main() {
    g_finalColor = pm.b;
}

//# coverage
#version 450
in vec2 v_uv;
out vec4 g_finalColor;
float both(bool c, float a) {
    if (c)
        return a;
    else
        return -a;
}
void main() {
    float a = v_uv.x, b = v_uv.y;
    int i = int(a * 100.0), j = int(b * 100.0);
    float k = 0.0;
    if (i != j) k += 1.0;
    if (i > j) k += 2.0;
    if (i >= j) k += 4.0;
    if (i <= j) k += 8.0;
    if (a != b) k += 16.0;
    if (a <= b) k += 32.0;
    if (a >= b) k += 64.0;
    bvec2 p = bvec2(a < b, a > 0.0);
    if (any(p)) k += 128.0;
    if (all(p)) k += 256.0;
    bool flag = true;
    if (a < 0.5)
        flag = false;
    float integers = float(-i) + float(~j);
    float t = trunc(a * 7.0) + degrees(a) + tan(a) + asin(a) + acos(a) + atan(a) + sinh(a) + cosh(a) + tanh(a) + exp2(a) + log2(a + 1.0);
    g_finalColor = vec4(k, integers, t, (a - b) + both(flag, a));
}
