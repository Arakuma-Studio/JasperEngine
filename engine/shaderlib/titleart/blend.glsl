// Title Art's 18 blend modes (docs/specs/S80-title-art-core.md §6.4), the TitleBlend order:
// 0 normal, 1 add, 2 screen, 3 multiply, 4 overlay, 5 soft light, 6 hard light, 7 darken,
// 8 lighten, 9 color dodge, 10 color burn, 11 linear burn, 12 difference, 13 exclusion, 14 hue,
// 15 saturation, 16 color, 17 luminosity.
//
// The separable formulas are adapted from effects/lib/fam_sim_blend.glsl (ported from
// shaders.com's gpu/kit/blend.ts, MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt); the four non-separable ones are the PDF blend-mode
// specification's (SetLum, SetSat, ClipColor, Photoshop's luminosity weights), the formulas
// Pachislop's overlays use for Color. The functions take straight colours in whatever space the
// caller blends in (the overlay effects pass sRGB-encoded values, as Photoshop and Pachislop do);
// the modes whose formula uses 1 - x clamp their inputs to 0..1, the others keep values above 1.

#ifndef TITLEART_BLEND_GLSL
#define TITLEART_BLEND_GLSL

float ta_blendLum(vec3 c) {
    return dot(c, vec3(0.3, 0.59, 0.11));
}

vec3 ta_clipColor(vec3 c) {
    float l = ta_blendLum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}

vec3 ta_setLum(vec3 c, float l) {
    return ta_clipColor(c + (l - ta_blendLum(c)));
}

float ta_blendSat(vec3 c) {
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

// c with saturation s, its hue kept (SetSat of the PDF spec)
vec3 ta_setSat(vec3 c, float s) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float range = mx - mn;
    if (range <= 1e-6) return vec3(0.0);
    return (c - mn) * (s / range);
}

// B(cb, cs): backdrop b, source s
vec3 ta_blend(int mode, vec3 b, vec3 s) {
    if (mode == 0) return s;
    if (mode == 1) return b + s;
    if (mode == 3) return b * s;
    if (mode == 7) return min(b, s);
    if (mode == 8) return max(b, s);
    if (mode == 12) return abs(b - s);
    b = clamp(b, 0.0, 1.0);
    s = clamp(s, 0.0, 1.0);
    if (mode == 2) return 1.0 - (1.0 - b) * (1.0 - s);
    if (mode == 4) return mix(b * s * 2.0, 1.0 - (1.0 - b) * (1.0 - s) * 2.0, step(vec3(0.5), b));
    if (mode == 5) {
        vec3 dark = b * s * 2.0 + b * b * (1.0 - s * 2.0);
        vec3 light = b * (1.0 - s) * 2.0 + sqrt(b) * (s * 2.0 - 1.0);
        return mix(dark, light, step(vec3(0.5), s));
    }
    if (mode == 6) return mix(b * s * 2.0, 1.0 - (1.0 - b) * (1.0 - s) * 2.0, step(vec3(0.5), s));
    if (mode == 9) {
        // W3C: b = 0 gives 0, else s = 1 gives 1, else min(1, b / (1 - s))
        vec3 r = min(vec3(1.0), b / max(1.0 - s, vec3(1e-6)));
        r = mix(r, vec3(1.0), step(vec3(1.0 - 1e-6), s));
        return r * step(vec3(1e-6), b);
    }
    if (mode == 10) {
        // W3C: b = 1 gives 1, else s = 0 gives 0, else 1 - min(1, (1 - b) / s)
        vec3 r = (1.0 - min(vec3(1.0), (1.0 - b) / max(s, vec3(1e-6)))) * step(vec3(1e-6), s);
        return mix(r, vec3(1.0), step(vec3(1.0 - 1e-6), b));
    }
    if (mode == 11) return max(b + s - 1.0, vec3(0.0));
    if (mode == 13) return b + s - b * s * 2.0;
    if (mode == 14) return ta_setLum(ta_setSat(s, ta_blendSat(b)), ta_blendLum(b));
    if (mode == 15) return ta_setLum(ta_setSat(b, ta_blendSat(s)), ta_blendLum(b));
    if (mode == 16) return ta_setLum(s, ta_blendLum(b));
    if (mode == 17) return ta_setLum(b, ta_blendLum(s));
    return s;
}

#endif // TITLEART_BLEND_GLSL
