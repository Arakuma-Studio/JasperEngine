// Title Art's adjustments (docs/specs/S80-title-art-core.md §6.1 Adjustment, §6.4): the one
// implementation the `adjust` effect and adjustment layers share. Ported from Pachislop
// src/engine/adjustments.ts by SafEight, used with the author's permission.
//
// ta_adjust(op, c) works on sRGB-encoded straight colour 0..1 (Photoshop adjusts the encoded
// document values; an adjustment clips light above 1, documented). The caller unpremultiplies,
// clamps and encodes before and decodes and premultiplies after, alpha unchanged. Tone-curve
// types (Brightness/Contrast, Levels, Curves) arrive baked into the 256-entry lookup
// TA_ADJUST_LUT (R, G, B = each channel's output, TitleAdjust.cpp); Gradient Map reads its ramp
// from the same slot. The op numbers are TitleAdjustOp's (effects/TitleAdjust.h):
// 0 lut, 1 exposure, 2 vibrance, 3 hue/saturation, 4 color balance, 5 black & white,
// 6 photo filter, 7 gradient map, 8 invert, 9 posterize, 10 threshold.
// Values: A, B, C as the C++ side packs them per op (TitleAdjust.cpp).

#ifndef TITLEART_ADJUST_GLSL
#define TITLEART_ADJUST_GLSL

// Photoshop's luminosity (Lum() of the PDF blend-mode spec) and the Color blend's SetLum
float adj_lum(vec3 c) {
    return dot(c, vec3(0.3, 0.59, 0.11));
}

vec3 adj_clipColor(vec3 c) {
    float l = adj_lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}

vec3 adj_setLum(vec3 c, float l) {
    return adj_clipColor(c + (l - adj_lum(c)));
}

vec3 adj_rgb2hsl(vec3 c) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float l = (mx + mn) * 0.5;
    float d = mx - mn;
    if (d < 1e-6) return vec3(0.0, 0.0, l);
    float s = l < 0.5 ? d / (mx + mn) : d / (2.0 - mx - mn);
    float h;
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    return vec3(h / 6.0, s, l);
}

float adj_hueChannel(float p, float q, float t) {
    t = fract(t);
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

vec3 adj_hsl2rgb(vec3 h) {
    if (h.y <= 0.0) return vec3(h.z);
    float q = h.z < 0.5 ? h.z * (1.0 + h.y) : h.z + h.y - h.z * h.y;
    float p = 2.0 * h.z - q;
    return vec3(adj_hueChannel(p, q, h.x + 1.0 / 3.0), adj_hueChannel(p, q, h.x), adj_hueChannel(p, q, h.x - 1.0 / 3.0));
}

// the largest chroma scale <= k keeping every channel of l + (c - l) * k inside 0..1
float adj_fitScale(vec3 c, float l, float k) {
    vec3 d = c - l;
    float m = k;
    for (int i = 0; i < 3; i++) {
        if (d[i] > 1e-6) m = min(m, (1.0 - l) / d[i]);
        else if (d[i] < -1e-6) m = min(m, -l / d[i]);
    }
    return m;
}

vec3 adj_scaleChroma(vec3 c, float k) {
    float l = adj_lum(c);
    return l + (c - l) * (k > 1.0 ? adj_fitScale(c, l, k) : k);
}

// texel centre of 8-bit level x (0..1) in a 256-wide lookup
float adj_lutU(float x) {
    return x * (255.0 / 256.0) + 0.5 / 256.0;
}

vec3 ta_adjust(int op, vec3 c, vec4 A, vec4 B, vec4 C) {
    if (op == 0) {
        return vec3(texture(TA_ADJUST_LUT, vec2(adj_lutU(c.r), 0.5)).r, texture(TA_ADJUST_LUT, vec2(adj_lutU(c.g), 0.5)).g,
                    texture(TA_ADJUST_LUT, vec2(adj_lutU(c.b), 0.5)).b);
    }
    if (op == 1) {
        // Adobe: Exposure works in linear light (gamma 1.0): exposure scales, offset adds, gamma
        // is a plain power. A = (exposure, offset, gamma)
        vec3 l = clamp(ta_toLinear(c) * exp2(A.x) + A.y, 0.0, 1.0);
        return ta_toSrgb(pow(l, vec3(1.0 / A.z)));
    }
    if (op == 2) {
        // chroma scaled around the luminosity; Vibrance weights the boost by how unsaturated the
        // colour is, and both stop where a channel would clip. A = (vibrance, saturation) / 100
        float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        c = adj_scaleChroma(c, 1.0 + A.x * (1.0 - sat));
        return adj_scaleChroma(c, 1.0 + A.y);
    }
    if (op == 3) {
        // Hue rotates in HSL; Saturation is the published reverse-engineering of Photoshop's;
        // Lightness blends toward white or black; Colorize: HSL(hue, saturation, luminosity).
        // A = (hue 0..1, saturation, lightness, colorize)
        if (A.w > 0.5) {
            c = adj_hsl2rgb(vec3(A.x, A.y, adj_lum(c)));
        } else {
            vec3 h = adj_rgb2hsl(c);
            h.x = fract(h.x + A.x);
            c = adj_hsl2rgb(h);
            float mx = max(c.r, max(c.g, c.b));
            float mn = min(c.r, min(c.g, c.b));
            float d = mx - mn;
            if (d > 1e-6) {
                float v = mx + mn;
                float L = v * 0.5;
                float S = L < 0.5 ? d / v : d / (2.0 - v);
                if (A.y >= 0.0) {
                    float a = A.y + S >= 1.0 ? S : 1.0 - A.y;
                    c = c + (c - L) * (1.0 / a - 1.0);
                } else {
                    c = L + (c - L) * (1.0 + A.y);
                }
            }
        }
        return A.z >= 0.0 ? c + (1.0 - c) * A.z : c * (1.0 + A.z);
    }
    if (op == 4) {
        // a Curves-like bump per channel that never moves pure black or white, peaking at 1/3,
        // 1/2 and 2/3. A.xyz shadows, B.xyz midtones, C.xyz highlights (/ 100), A.w preserve
        vec3 s = 6.75 * c * (1.0 - c) * (1.0 - c);
        vec3 m = 4.0 * c * (1.0 - c);
        vec3 h = 6.75 * c * c * (1.0 - c);
        vec3 r = clamp(c + A.xyz * 0.14 * s + B.xyz * 0.2 * m + C.xyz * 0.14 * h, 0.0, 1.0);
        return A.w > 0.5 ? adj_setLum(r, adj_lum(c)) : r;
    }
    if (op == 5) {
        // grey = min + (mid - min) x secondary + (max - mid) x primary. A = (reds, yellows,
        // greens, cyans), B = (blues, magentas, tint), C.xyz the tint colour (sRGB)
        float mx = max(c.r, max(c.g, c.b));
        float mn = min(c.r, min(c.g, c.b));
        float md = c.r + c.g + c.b - mx - mn;
        float wp, ws;
        if (c.r >= c.g && c.r >= c.b) { wp = A.x; ws = c.g >= c.b ? A.y : B.y; }
        else if (c.g >= c.b) { wp = A.z; ws = c.r >= c.b ? A.y : A.w; }
        else { wp = B.x; ws = c.r >= c.g ? B.y : A.w; }
        float g = clamp(mn + (md - mn) * ws + (mx - md) * wp, 0.0, 1.0);
        return B.z > 0.5 ? adj_setLum(C.xyz, g) : vec3(g);
    }
    if (op == 6) {
        // a coloured filter multiplies by its colour, Density mixes it in, Preserve Luminosity
        // puts the luminosity back. A.xyz the colour (sRGB), A.w density, B.x preserve
        vec3 r = mix(c, c * A.xyz, A.w);
        return B.x > 0.5 ? adj_setLum(r, adj_lum(c)) : r;
    }
    if (op == 7) {
        // the ramp replaces each pixel by its luminosity (left = shadows); its alpha lets the
        // original show. A.x reverse
        float l = clamp(adj_lum(c), 0.0, 1.0);
        vec4 g = texture(TA_ADJUST_LUT, vec2(adj_lutU(A.x > 0.5 ? 1.0 - l : l), 0.5));
        return mix(c, g.rgb, g.a);
    }
    if (op == 8) return 1.0 - c;
    if (op == 9) {
        // Photoshop on 8-bit levels: index = floor(v n / 256), out = round(index 255 / (n - 1))
        vec3 i = floor(floor(c * 255.0 + 0.5) * A.x / 256.0);
        return floor(i * 255.0 / (A.x - 1.0) + 0.5) / 255.0;
    }
    if (op == 10) return vec3(step(A.x, floor(adj_lum(c) * 255.0 + 0.5)));
    return c;
}

#endif // TITLEART_ADJUST_GLSL
