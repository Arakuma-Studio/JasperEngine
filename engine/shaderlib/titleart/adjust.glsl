// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/adjustments.ts (the operations) and src/engine/shaders.ts
// (adjustMaterial, the adjustment layer's mix) by SafEight, used with the author's permission.

// The fourteen adjustment operations of §6.1 on one pixel, shared by adjustment layers and the
// `adjust` effect (render/Adjustments.cpp sets them up): unpremultiply, clamp to 0..1, encode
// sRGB (Photoshop adjusts encoded values, so an adjustment layer clips light above 1), the
// operation, decode, premultiply with the alpha unchanged. Curves, levels and
// brightness/contrast read a 256-entry tone LUT baked on the CPU; the others are closed form.

#define TA_ADJ_LUT 0            // brightness_contrast, levels, curves
#define TA_ADJ_EXPOSURE 3
#define TA_ADJ_VIBRANCE 4
#define TA_ADJ_HUE_SATURATION 5
#define TA_ADJ_COLOR_BALANCE 6
#define TA_ADJ_BLACK_WHITE 7
#define TA_ADJ_PHOTO_FILTER 8
#define TA_ADJ_GRADIENT_MAP 9
#define TA_ADJ_INVERT 10
#define TA_ADJ_POSTERIZE 11
#define TA_ADJ_THRESHOLD 12

// Photoshop's luminosity (Lum() of the PDF blend-mode spec)
float taAdjLum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 taAdjClipColor(vec3 c) {
    float l = taAdjLum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}
vec3 taAdjSetLum(vec3 c, float l) { return taAdjClipColor(c + (l - taAdjLum(c))); }
vec3 taRgbToHsl(vec3 c) {
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
float taHueChannel(float p, float q, float t) {
    t = fract(t);
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}
vec3 taHslToRgb(vec3 h) {
    if (h.y <= 0.0) return vec3(h.z);
    float q = h.z < 0.5 ? h.z * (1.0 + h.y) : h.z + h.y - h.z * h.y;
    float p = 2.0 * h.z - q;
    return vec3(taHueChannel(p, q, h.x + 1.0 / 3.0), taHueChannel(p, q, h.x), taHueChannel(p, q, h.x - 1.0 / 3.0));
}
// texel centre of 8-bit level x (0..1) in a 256-wide LUT
float taLutU(float x) { return x * (255.0 / 256.0) + 0.5 / 256.0; }

// Largest chroma scale <= k keeping every channel of l + (c - l) k inside 0..1
float taFitScale(vec3 c, float l, float k) {
    vec3 d = c - l;
    float m = k;
    for (int i = 0; i < 3; i++) {
        if (d[i] > 1e-6) m = min(m, (1.0 - l) / d[i]);
        else if (d[i] < -1e-6) m = min(m, -l / d[i]);
    }
    return m;
}
vec3 taScaleChroma(vec3 c, float k) {
    float l = taAdjLum(c);
    return l + (c - l) * (k > 1.0 ? taFitScale(c, l, k) : k);
}

// p[2] = (op, a, b, c), p[3] = (d, e, f, g), p[4], p[5]: the operation's values (Adjustments.cpp)
// the LUT is TA_TEX1 (a tone LUT, or the gradient map's gradient)
vec3 taAdjust(vec3 c) {
    int op = int(ta.p[2].x + 0.5);
    vec4 a = ta.p[2], b = ta.p[3], q = ta.p[4], r = ta.p[5];
    if (op == TA_ADJ_LUT) {
        return vec3(texture(TA_TEX1, vec2(taLutU(c.r), 0.5)).r, texture(TA_TEX1, vec2(taLutU(c.g), 0.5)).g, texture(TA_TEX1, vec2(taLutU(c.b), 0.5)).b);
    }
    if (op == TA_ADJ_EXPOSURE) {
        // Adobe: exposure in linear light: scale, offset, a plain power
        vec3 l = clamp(taToLinear(c) * exp2(a.y) + a.z, 0.0, 1.0);
        return taToSrgb(pow(l, vec3(1.0 / a.w)));
    }
    if (op == TA_ADJ_VIBRANCE) {
        float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        c = taScaleChroma(c, 1.0 + a.y * (1.0 - sat));
        return taScaleChroma(c, 1.0 + a.z);
    }
    if (op == TA_ADJ_HUE_SATURATION) {
        // a.y hue (turns), a.z saturation, a.w lightness, b.x colorize
        if (b.x > 0.5) {
            c = taHslToRgb(vec3(a.y, a.z, taAdjLum(c)));
        } else {
            vec3 h = taRgbToHsl(c);
            h.x = fract(h.x + a.y);
            c = taHslToRgb(h);
            float mx = max(c.r, max(c.g, c.b));
            float mn = min(c.r, min(c.g, c.b));
            float d = mx - mn;
            if (d > 1e-6) {
                float v = mx + mn;
                float L = v * 0.5;
                float S = L < 0.5 ? d / v : d / (2.0 - v);
                if (a.z >= 0.0) {
                    float k = a.z + S >= 1.0 ? S : 1.0 - a.z;
                    c = c + (c - L) * (1.0 / k - 1.0);
                } else {
                    c = L + (c - L) * (1.0 + a.z);
                }
            }
        }
        return a.w >= 0.0 ? c + (1.0 - c) * a.w : c * (1.0 + a.w);
    }
    if (op == TA_ADJ_COLOR_BALANCE) {
        // b.xyz shadows, q.xyz midtones, r.xyz highlights, a.y preserve luminosity
        vec3 s = 6.75 * c * (1.0 - c) * (1.0 - c);
        vec3 m = 4.0 * c * (1.0 - c);
        vec3 h = 6.75 * c * c * (1.0 - c);
        vec3 o = clamp(c + b.xyz * 0.14 * s + q.xyz * 0.2 * m + r.xyz * 0.14 * h, 0.0, 1.0);
        return a.y > 0.5 ? taAdjSetLum(o, taAdjLum(c)) : o;
    }
    if (op == TA_ADJ_BLACK_WHITE) {
        // a.y reds, a.z yellows, a.w greens, b.x cyans, b.y blues, b.z magentas, b.w tint, q.xyz tint colour
        float mx = max(c.r, max(c.g, c.b));
        float mn = min(c.r, min(c.g, c.b));
        float md = c.r + c.g + c.b - mx - mn;
        float wp, ws;
        if (c.r >= c.g && c.r >= c.b) { wp = a.y; ws = c.g >= c.b ? a.z : b.z; }
        else if (c.g >= c.b) { wp = a.w; ws = c.r >= c.b ? a.z : b.x; }
        else { wp = b.y; ws = c.r >= c.g ? b.z : b.x; }
        float g = clamp(mn + (md - mn) * ws + (mx - md) * wp, 0.0, 1.0);
        return b.w > 0.5 ? taAdjSetLum(q.xyz, g) : vec3(g);
    }
    if (op == TA_ADJ_PHOTO_FILTER) {
        // b.xyz colour (sRGB), a.y density, a.z preserve luminosity
        vec3 o = mix(c, c * b.xyz, a.y);
        return a.z > 0.5 ? taAdjSetLum(o, taAdjLum(c)) : o;
    }
    if (op == TA_ADJ_GRADIENT_MAP) {
        // the LUT holds the gradient's sRGB colours and straight alpha; a.y reverse
        float l = clamp(taAdjLum(c), 0.0, 1.0);
        vec4 g = texture(TA_TEX1, vec2(taLutU(a.y > 0.5 ? 1.0 - l : l), 0.5));
        return mix(c, g.rgb, g.a);
    }
    if (op == TA_ADJ_INVERT) return 1.0 - c;
    if (op == TA_ADJ_POSTERIZE) {
        // Photoshop on 8-bit levels: index = floor(v n / 256), output = round(index 255 / (n - 1))
        vec3 i = floor(floor(c * 255.0 + 0.5) * a.y / 256.0);
        return floor(i * 255.0 / (a.y - 1.0) + 0.5) / 255.0;
    }
    if (op == TA_ADJ_THRESHOLD) return vec3(step(a.y, floor(taAdjLum(c) * 255.0 + 0.5)));
    return c;
}
