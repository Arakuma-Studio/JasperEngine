// HTML UI effects: noise. Ported from the shaders.com kit noise.ts (MIT, Copyright (c) 2026
// Shader Effects Inc.; see effects/LICENSE.shaders-com.txt): value12, perlin12, perlin12d,
// curl22, fbmPerlin, blue12 (the Fi family) and MaterialX's perlin and worley noise. The
// simplex noises, the cellular F1/F2 noise, the value fbm and the domain warp are the
// engine's own.
#ifndef JFX_NOISE_GLSL
#define JFX_NOISE_GLSL

#include "effects/lib/jfx_hash.glsl"

// smoothstep-interpolated value noise, [0, 1]
float jfx_value12(vec2 p){
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(jfx_hash12(i), jfx_hash12(i + vec2(1.0, 0.0)), f.x),
               mix(jfx_hash12(i + vec2(0.0, 1.0)), jfx_hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}

float jfx_perlin_grad2(vec2 i, vec2 f, vec2 o){
    return dot(normalize(jfx_hash22(i + o) - 0.5), f - o);
}

// gradient noise, roughly [0, 1] (x 0.7 + 0.5)
float jfx_perlin12(vec2 p){
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = jfx_perlin_grad2(i, f, vec2(0.0, 0.0));
    float b = jfx_perlin_grad2(i, f, vec2(1.0, 0.0));
    float c = jfx_perlin_grad2(i, f, vec2(0.0, 1.0));
    float d = jfx_perlin_grad2(i, f, vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.7 + 0.5;
}

// gradient noise with its analytic gradient: (value, d/dx, d/dy)
vec3 jfx_perlin12d(vec2 p){
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = jfx_hash22(i) * 2.0 - 1.0;
    vec2 gb = jfx_hash22(i + vec2(1.0, 0.0)) * 2.0 - 1.0;
    vec2 gc = jfx_hash22(i + vec2(0.0, 1.0)) * 2.0 - 1.0;
    vec2 gd = jfx_hash22(i + vec2(1.0, 1.0)) * 2.0 - 1.0;
    float va = dot(ga, f);
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));
    float k = va - vb - vc + vd;
    float value = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * k;
    vec2 deriv = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                 du * (vec2(u.y, u.x) * k + vec2(vb, vc) - va);
    return vec3(value, deriv);
}

// ---- simplex noise (2D, 3D), [-1, 1]; gradients from jfx_hash22/33

float jfx_simplex12(vec2 p){
    const float F2 = 0.36602540378;   // (sqrt(3) - 1) / 2
    const float G2 = 0.21132486540;   // (3 - sqrt(3)) / 6
    vec2 i = floor(p + (p.x + p.y) * F2);
    vec2 x0 = p - i + (i.x + i.y) * G2;
    vec2 o = x0.x > x0.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 x1 = x0 - o + G2;
    vec2 x2 = x0 - 1.0 + 2.0 * G2;
    float n = 0.0;
    float t0 = 0.5 - dot(x0, x0);
    if (t0 > 0.0){ t0 *= t0; n += t0 * t0 * dot(normalize(jfx_hash22(i) - 0.5), x0); }
    float t1 = 0.5 - dot(x1, x1);
    if (t1 > 0.0){ t1 *= t1; n += t1 * t1 * dot(normalize(jfx_hash22(i + o) - 0.5), x1); }
    float t2 = 0.5 - dot(x2, x2);
    if (t2 > 0.0){ t2 *= t2; n += t2 * t2 * dot(normalize(jfx_hash22(i + 1.0) - 0.5), x2); }
    return clamp(n * 99.2, -1.0, 1.0);
}

float jfx_simplex13(vec3 p){
    const float F3 = 1.0 / 3.0;
    const float G3 = 1.0 / 6.0;
    vec3 i = floor(p + dot(p, vec3(F3)));
    vec3 x0 = p - i + dot(i, vec3(G3));
    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);
    vec3 x1 = x0 - i1 + G3;
    vec3 x2 = x0 - i2 + 2.0 * G3;
    vec3 x3 = x0 - 1.0 + 3.0 * G3;
    vec4 t = max(0.6 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    t = t * t;
    t = t * t;
    vec4 d = vec4(dot(normalize(jfx_hash33(i) - 0.5), x0), dot(normalize(jfx_hash33(i + i1) - 0.5), x1),
                  dot(normalize(jfx_hash33(i + i2) - 0.5), x2), dot(normalize(jfx_hash33(i + 1.0) - 0.5), x3));
    return clamp(dot(t, d) * 32.0, -1.0, 1.0);
}

// ---- MaterialX perlin (noise.ts mxPerlinNoiseFloat2/3, mx_noise_float)

float jfx_mx_fade(float t){
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float jfx_mx_gradient2(uint hash, float x, float y){
    uint h = hash & 7u;
    float u = h < 4u ? x : y;
    float v = 2.0 * (h < 4u ? y : x);
    return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -v : v);
}

float jfx_mx_gradient3(uint hash, float x, float y, float z){
    uint h = hash & 15u;
    float u = h < 8u ? x : y;
    float v = h < 4u ? y : ((h == 12u || h == 14u) ? x : z);
    return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -v : v);
}

float jfx_mx_noise2(vec2 p){
    int X = int(floor(p.x));
    int Y = int(floor(p.y));
    float fx = p.x - float(X);
    float fy = p.y - float(Y);
    float u = jfx_mx_fade(fx);
    float v = jfx_mx_fade(fy);
    float v0 = jfx_mx_gradient2(jfx_mx_hash(X, Y), fx, fy);
    float v1 = jfx_mx_gradient2(jfx_mx_hash(X + 1, Y), fx - 1.0, fy);
    float v2 = jfx_mx_gradient2(jfx_mx_hash(X, Y + 1), fx, fy - 1.0);
    float v3 = jfx_mx_gradient2(jfx_mx_hash(X + 1, Y + 1), fx - 1.0, fy - 1.0);
    float s1 = 1.0 - u;
    return 0.6616 * ((1.0 - v) * (v0 * s1 + v1 * u) + v * (v2 * s1 + v3 * u));
}

float jfx_mx_noise3(vec3 p){
    int X = int(floor(p.x));
    int Y = int(floor(p.y));
    int Z = int(floor(p.z));
    float fx = p.x - float(X);
    float fy = p.y - float(Y);
    float fz = p.z - float(Z);
    float u = jfx_mx_fade(fx);
    float v = jfx_mx_fade(fy);
    float w = jfx_mx_fade(fz);
    float v0 = jfx_mx_gradient3(jfx_mx_hash(X, Y, Z), fx, fy, fz);
    float v1 = jfx_mx_gradient3(jfx_mx_hash(X + 1, Y, Z), fx - 1.0, fy, fz);
    float v2 = jfx_mx_gradient3(jfx_mx_hash(X, Y + 1, Z), fx, fy - 1.0, fz);
    float v3 = jfx_mx_gradient3(jfx_mx_hash(X + 1, Y + 1, Z), fx - 1.0, fy - 1.0, fz);
    float v4 = jfx_mx_gradient3(jfx_mx_hash(X, Y, Z + 1), fx, fy, fz - 1.0);
    float v5 = jfx_mx_gradient3(jfx_mx_hash(X + 1, Y, Z + 1), fx - 1.0, fy, fz - 1.0);
    float v6 = jfx_mx_gradient3(jfx_mx_hash(X, Y + 1, Z + 1), fx, fy - 1.0, fz - 1.0);
    float v7 = jfx_mx_gradient3(jfx_mx_hash(X + 1, Y + 1, Z + 1), fx - 1.0, fy - 1.0, fz - 1.0);
    float s1 = 1.0 - u;
    float t1 = 1.0 - v;
    float r1 = 1.0 - w;
    float result = r1 * (t1 * (v0 * s1 + v1 * u) + v * (v2 * s1 + v3 * u)) +
                   w * (t1 * (v4 * s1 + v5 * u) + v * (v6 * s1 + v7 * u));
    return 0.982 * result;
}

// MaterialX worley (mx_worley_noise_float, 2D, metric 1: the squared euclidean F1)
float jfx_mx_worley2(vec2 p, float jitter){
    int X = int(floor(p.x));
    int Y = int(floor(p.y));
    vec2 localpos = vec2(p.x - float(X), p.y - float(Y));
    float sqdist = 1e6;
    for (int x = -1; x <= 1; x++){
        for (int y = -1; y <= 1; y++){
            int cx = x + X;
            int cy = y + Y;
            vec2 off = vec2(jfx_mx_bits_to_01(jfx_mx_hash(cx, cy, 0)), jfx_mx_bits_to_01(jfx_mx_hash(cx, cy, 1)));
            off = (off - 0.5) * jitter + 0.5;
            vec2 diff = vec2(float(x), float(y)) + off - localpos;
            sqdist = min(sqdist, dot(diff, diff));
        }
    }
    return sqdist;
}

// curl of perlin12: a divergence-free flow (noise.ts curl22)
vec2 jfx_curl22(vec2 p){
    vec2 a = vec2(jfx_perlin12(p + vec2(0.1, 0.0)), jfx_perlin12(p + vec2(0.0, 0.1)));
    vec2 b = vec2(jfx_perlin12(p - vec2(0.1, 0.0)), jfx_perlin12(p - vec2(0.0, 0.1)));
    return (a - b) / 0.1 * 0.5;
}

// fractal perlin12, [0, 1]; octaves at most 8 (noise.ts fbmPerlin)
float jfx_fbm_perlin(vec2 p, float octaves){
    float s = 0.0, m = 0.0, a = 1.0;
    for (int i = 0; i < 8; i++){
        if (float(i) >= octaves)
            break;
        s += a * jfx_perlin12(p);
        m += a;
        a *= 0.5;
        p *= 2.0;
    }
    return s / max(m, 1e-6);
}

// fractal value noise, [0, 1]; octaves at most 8
float jfx_fbm_value(vec2 p, float octaves){
    float s = 0.0, m = 0.0, a = 1.0;
    for (int i = 0; i < 8; i++){
        if (float(i) >= octaves)
            break;
        s += a * jfx_value12(p);
        m += a;
        a *= 0.5;
        p *= 2.0;
    }
    return s / max(m, 1e-6);
}

// p displaced by a 4-octave perlin fbm field that drifts with t (one level of Quilez's warp)
vec2 jfx_domain_warp2(vec2 p, float strength, float t){
    vec2 q = vec2(jfx_fbm_perlin(p + vec2(0.0, 0.0) + t * vec2(0.13, 0.07), 4.0),
                  jfx_fbm_perlin(p + vec2(5.2, 1.3) - t * vec2(0.11, 0.17), 4.0));
    return p + (q - 0.5) * 2.0 * strength;
}

// cellular noise: x F1, y F2 (euclidean, cell units), z the nearest cell's id hash [0, 1)
vec3 jfx_worley(vec2 p, float jitter){
    vec2 cell = floor(p);
    vec2 f = p - cell;
    float f1 = 8.0, f2 = 8.0, id = 0.0;
    for (int y = -1; y <= 1; y++){
        for (int x = -1; x <= 1; x++){
            vec2 o = vec2(float(x), float(y));
            vec2 point = o + 0.5 + (jfx_hash22(cell + o) - 0.5) * jitter;
            float d = length(point - f);
            if (d < f1){
                f2 = f1;
                f1 = d;
                id = jfx_hash12(cell + o + 0.5);
            }else if (d < f2){
                f2 = d;
            }
        }
    }
    return vec3(f1, f2, id);
}

// pixel-space blue noise (noise.ts blue12), floored pixel coordinates, ~[0, 1]
float jfx_blue12(vec2 p){
    float v = 0.0;
    for (int k = 0; k < 9; k++){
        float row = floor(float(k) / 3.0);
        float col = float(k) - row * 3.0;
        v += jfx_hash12(p + vec2(col - 1.0, row - 1.0));
    }
    return 0.9 * (1.125 * jfx_hash12(p) - v / 8.0) + 0.5;
}

#endif // JFX_NOISE_GLSL
