// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 18 blend modes of §6.4 (TitleBlend, values 0..17), the internal `disjoint` (18),
// `erase` (19) and `subtract` (20) on
// premultiplied linear colour. The mode functions are adapted from
// engine/shaderlib/effects/lib/fam_sim_blend.glsl (shaders.com's kit, MIT, Copyright (c) 2026
// Shader Effects Inc.; effects/LICENSE.shaders-com.txt): there they take straight colour with
// an "over" tail; here the inputs are unpremultiplied (alpha > 0), the mode runs on linear
// straight colour (Pachislop's choice for its modes, src/engine/shaders.ts, so ported looks
// match) and the result is composed premultiplied, co = s (1 - a_b) + b (1 - a_s) + a_s a_b
// B(cb, cs). Modes whose formula takes 1 - x clamp their colour inputs to 0..1; normal, add,
// multiply, darken, lighten and difference keep values above 1. render/TitleBlend.cpp is the
// same maths on the CPU (TitleArtBlendGpu.AllModes compares them).

#define TA_BLEND_NORMAL 0
#define TA_BLEND_ADD 1
#define TA_BLEND_DISJOINT 18
#define TA_BLEND_ERASE 19           // internal: d (1 - a_s), the 2D stack's knockout without float blending
#define TA_BLEND_SUBTRACT 20        // internal: max(d - s, 0) in colour, d's alpha (the glow split without float blending)

float taBlendLum(vec3 c) { return c.x * 0.2126 + c.y * 0.7152 + c.z * 0.0722; }

bool taBlendKeepsHdr(int mode) {
    return mode == 0 || mode == 1 || mode == 3 || mode == 7 || mode == 8 || mode == 12;
}

vec3 taBlendFn(vec3 b, vec3 o, int mode) {
    if (!taBlendKeepsHdr(mode)) {
        b = clamp(b, 0.0, 1.0);
        o = clamp(o, 0.0, 1.0);
    }
    if (mode == 1) return b + o;
    if (mode == 2) return 1.0 - (1.0 - b) * (1.0 - o);
    if (mode == 3) return b * o;
    if (mode == 4) return mix(b * o * 2.0, 1.0 - (1.0 - b) * (1.0 - o) * 2.0, step(vec3(0.5), b));
    if (mode == 5) {
        vec3 softDark = b * o * 2.0 + b * b * (1.0 - o * 2.0);
        vec3 softLight = b * (1.0 - o) * 2.0 + sqrt(max(b, vec3(0.0))) * (o * 2.0 - 1.0);
        return mix(softDark, softLight, step(vec3(0.5), o));
    }
    if (mode == 6) return mix(b * o * 2.0, 1.0 - (1.0 - b) * (1.0 - o) * 2.0, step(vec3(0.5), o));
    if (mode == 7) return min(b, o);
    if (mode == 8) return max(b, o);
    if (mode == 9) {
        vec3 oneMinus = 1.0 - o;
        return mix(b, min(b / max(oneMinus, vec3(1e-6)), vec3(1.0)), step(vec3(0.001), oneMinus));
    }
    if (mode == 10) {
        vec3 burned = max(1.0 - (1.0 - b) / (o + 0.0001), vec3(0.0));
        return mix(vec3(0.0), burned, step(vec3(0.0001), o));
    }
    if (mode == 11) return max(b + o - 1.0, vec3(0.0));
    if (mode == 12) return abs(b - o);
    if (mode == 13) return b + o - b * o * 2.0;
    if (mode == 14) {
        float bl = taBlendLum(b);
        float ol = taBlendLum(o);
        vec3 oc = o - ol;
        float len = length(oc);
        vec3 dir = len > 0.0001 ? oc / len : vec3(0.0);
        return dir * length(b - bl) + bl;
    }
    if (mode == 15) {
        float bl = taBlendLum(b);
        float ol = taBlendLum(o);
        vec3 bc = b - bl;
        float len = length(bc);
        vec3 dir = len > 0.0001 ? bc / len : vec3(0.0);
        return dir * length(o - ol) + bl;
    }
    if (mode == 16) return o + (taBlendLum(b) - taBlendLum(o));
    if (mode == 17) return b * (taBlendLum(o) / (taBlendLum(b) + 0.0001));
    return o;
}

// `s` over `d` (both premultiplied linear) by `mode`
vec4 taBlend(vec4 d, vec4 s, int mode) {
    if (mode == TA_BLEND_NORMAL) return s + d * (1.0 - s.a);
    if (mode == TA_BLEND_DISJOINT) return s + d * (d.a > 0.0 ? min(1.0, (1.0 - s.a) / d.a) : 1.0);
    if (mode == TA_BLEND_ERASE) return d * (1.0 - s.a);
    if (mode == TA_BLEND_SUBTRACT) return vec4(max(d.rgb - s.rgb, vec3(0.0)), d.a);
    float oa = s.a + d.a - s.a * d.a;
    if (mode == TA_BLEND_ADD) return vec4(d.rgb + s.rgb, oa);
    vec3 cb = d.a > 0.0 ? d.rgb / d.a : vec3(0.0);
    vec3 cs = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    vec3 co = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.a * d.a * taBlendFn(cb, cs, mode);
    return vec4(co, oa);
}
