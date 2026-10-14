// HTML UI effects: colour. Effects work in straight-alpha linear sRGB (CSS colours arrive
// converted by the engine); jfx_encode gives what the render target holds (premultiplied sRGB).
// Ported from the shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): tonemap.ts (linearToSrgb, srgbToLinear) and
// colorMixing.ts (the colour spaces and the alpha-weighted mixColors variants), without their
// P3 conversions (the engine's colours are linear sRGB).
#ifndef JFX_COLOR_GLSL
#define JFX_COLOR_GLSL

#include "effects/lib/jfx_common.glsl"

// tonemap.ts linearToSrgb (the kit's 0.41666 exponent)
vec3 jfx_linear_to_srgb(vec3 c){
    vec3 a = pow(max(c, vec3(0.0)), vec3(0.41666)) * 1.055 - 0.055;
    vec3 b = c * 12.92;
    return mix(a, b, vec3(lessThanEqual(c, vec3(0.0031308))));
}

// tonemap.ts srgbToLinear
vec3 jfx_srgb_to_linear(vec3 c){
    vec3 hi = pow((max(c, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    vec3 lo = c / 12.92;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.04045))));
}

vec4 jfx_premultiply(vec4 c){
    return vec4(c.rgb * c.a, c.a);
}

vec4 jfx_unpremultiply(vec4 c){
    return c.a > 1e-5 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}

// straight linear -> premultiplied sRGB (shaders.com's final tail: linearToSrgb, premultiply)
vec4 jfx_encode(vec4 straightLinear){
    float a = clamp(straightLinear.a, 0.0, 1.0);
    vec3 rgb = jfx_linear_to_srgb(clamp(straightLinear.rgb, 0.0, 1.0));
    return vec4(rgb * a, a);
}

// premultiplied sRGB (a render target's texel) -> straight linear
vec4 jfx_decode(vec4 premultipliedSrgb){
    vec4 s = jfx_unpremultiply(premultipliedSrgb);
    return vec4(jfx_srgb_to_linear(s.rgb), s.a);
}

float jfx_luma709(vec3 c){
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float jfx_luma601(vec3 c){
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// ---- OKLab / OKLCh (colorMixing.ts rgbToOklab, oklabToRgb, oklabToOklch, oklchToOklab)

vec3 jfx_rgb_to_oklab(vec3 rgb){
    float l = rgb.r * 0.4122214708 + rgb.g * 0.5363325363 + rgb.b * 0.0514459929;
    float m = rgb.r * 0.2119034982 + (rgb.g * 0.6806995451 + rgb.b * 0.1073969566);
    float s = rgb.r * 0.0883024619 + rgb.g * 0.2817188376 + rgb.b * 0.6299787005;
    // sign-preserving cube root (pow is undefined for negative bases)
    float l_ = sign(l) * pow(abs(l), 1.0 / 3.0);
    float m_ = sign(m) * pow(abs(m), 1.0 / 3.0);
    float s_ = sign(s) * pow(abs(s), 1.0 / 3.0);
    return vec3(l_ * 0.2104542553 + m_ * 0.7936177850 - s_ * 0.0040720468,
                l_ * 1.9779984951 - m_ * 2.4285922050 + s_ * 0.4505937099,
                l_ * 0.0259040371 + m_ * 0.7827717662 - s_ * 0.8086757660);
}

vec3 jfx_oklab_to_rgb(vec3 lab){
    float l_ = lab.x + lab.y * 0.3963377774 + lab.z * 0.2158037573;
    float m_ = lab.x - lab.y * 0.1055613458 - lab.z * 0.0638541728;
    float s_ = lab.x - lab.y * 0.0894841775 - lab.z * 1.2914855480;
    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;
    return vec3(l * 4.0767416621 - m * 3.3077115913 + s * 0.2309699292,
                l * -1.2684380046 + m * 2.6097574011 - s * 0.3413193965,
                l * -0.0041960863 - m * 0.7034186147 + s * 1.7076147010);
}

vec3 jfx_oklab_to_oklch(vec3 lab){
    return vec3(lab.x, sqrt(lab.y * lab.y + lab.z * lab.z), atan(lab.z, lab.y));
}

vec3 jfx_oklch_to_oklab(vec3 lch){
    return vec3(lch.x, lch.y * cos(lch.z), lch.y * sin(lch.z));
}

// ---- HSL / HSV (hue in radians, colorMixing.ts)

vec3 jfx_rgb_by_sector(float c, float x, float sector){
    if (sector < 1.0) return vec3(c, x, 0.0);
    if (sector < 2.0) return vec3(x, c, 0.0);
    if (sector < 3.0) return vec3(0.0, c, x);
    if (sector < 4.0) return vec3(0.0, x, c);
    if (sector < 5.0) return vec3(x, 0.0, c);
    return vec3(c, 0.0, x);
}

float jfx_hue_of(vec3 rgb, float maxVal, float delta){
    float d = max(delta, 0.0001);
    float hue = maxVal == rgb.r ? (rgb.g - rgb.b) / d : (maxVal == rgb.g ? 2.0 + (rgb.b - rgb.r) / d : 4.0 + (rgb.r - rgb.g) / d);
    return hue * 1.0471975512;
}

vec3 jfx_rgb_to_hsl(vec3 rgb){
    float maxVal = max(max(rgb.r, rgb.g), rgb.b);
    float minVal = min(min(rgb.r, rgb.g), rgb.b);
    float delta = maxVal - minVal;
    float l = (maxVal + minVal) * 0.5;
    float s = delta / max(1.0 - abs(l * 2.0 - 1.0), 0.0001);
    return vec3(jfx_hue_of(rgb, maxVal, delta), s, l);
}

vec3 jfx_hsl_to_rgb(vec3 hsl){
    float c = hsl.y * (1.0 - abs(hsl.z * 2.0 - 1.0));
    float hPrime = hsl.x / 1.0471975512;
    float x = c * (1.0 - abs(mod(hPrime, 2.0) - 1.0));
    return jfx_rgb_by_sector(c, x, floor(hPrime)) + (hsl.z - c * 0.5);
}

vec3 jfx_rgb_to_hsv(vec3 rgb){
    float maxVal = max(max(rgb.r, rgb.g), rgb.b);
    float minVal = min(min(rgb.r, rgb.g), rgb.b);
    float delta = maxVal - minVal;
    return vec3(jfx_hue_of(rgb, maxVal, delta), delta / max(maxVal, 0.0001), maxVal);
}

vec3 jfx_hsv_to_rgb(vec3 hsv){
    float c = hsv.z * hsv.y;
    float hPrime = hsv.x / 1.0471975512;
    float x = c * (1.0 - abs(mod(hPrime, 2.0) - 1.0));
    return jfx_rgb_by_sector(c, x, floor(hPrime)) + (hsv.z - c);
}

// ---- CIE Lab / LCh (D65)

float jfx_lab_f(float t){
    return t > 0.008856451679 ? pow(t, 1.0 / 3.0) : t * 7.787037037 + 0.137931034;
}

float jfx_lab_f_inverse(float t){
    return t > 0.206896552 ? t * t * t : (t - 0.137931034) * 0.128418549;
}

vec3 jfx_rgb_to_lab(vec3 rgb){
    float x = (rgb.r * 0.4124564 + rgb.g * 0.3575761 + rgb.b * 0.1804375) / 0.95047;
    float y = rgb.r * 0.2126729 + rgb.g * 0.7151522 + rgb.b * 0.0721750;
    float z = (rgb.r * 0.0193339 + rgb.g * 0.1191920 + rgb.b * 0.9503041) / 1.08883;
    float fx = jfx_lab_f(x), fy = jfx_lab_f(y), fz = jfx_lab_f(z);
    return vec3(fy * 116.0 - 16.0, (fx - fy) * 500.0, (fy - fz) * 200.0);
}

vec3 jfx_lab_to_rgb(vec3 lab){
    float fy = (lab.x + 16.0) / 116.0;
    float fx = lab.y / 500.0 + fy;
    float fz = fy - lab.z / 200.0;
    float x = jfx_lab_f_inverse(fx) * 0.95047;
    float y = jfx_lab_f_inverse(fy);
    float z = jfx_lab_f_inverse(fz) * 1.08883;
    return vec3(x * 3.2404542 - y * 1.5371385 - z * 0.4985314,
                x * -0.9692660 + y * 1.8760108 + z * 0.0415560,
                x * 0.0556434 - y * 0.2040259 + z * 1.0572252);
}

vec3 jfx_lab_to_lch(vec3 lab){
    return vec3(lab.x, sqrt(lab.y * lab.y + lab.z * lab.z), atan(lab.z, lab.y));
}

vec3 jfx_lch_to_lab(vec3 lch){
    return vec3(lch.x, lch.y * cos(lch.z), lch.y * sin(lch.z));
}

// ---- mixing (colorMixing.ts mixColors*: alpha-weighted, the alpha is the weighted sum)

// a colour in a working space: 0 linear, 1 oklch, 2 oklab, 3 hsl, 4 hsv, 5 lch
vec3 jfx_to_space(vec3 rgb, float space){
    if (space < 0.5) return rgb;
    if (space < 1.5) return jfx_oklab_to_oklch(jfx_rgb_to_oklab(rgb));
    if (space < 2.5) return jfx_rgb_to_oklab(rgb);
    if (space < 3.5) return jfx_rgb_to_hsl(rgb);
    if (space < 4.5) return jfx_rgb_to_hsv(rgb);
    return jfx_lab_to_lch(jfx_rgb_to_lab(rgb));
}

vec3 jfx_from_space(vec3 c, float space){
    if (space < 0.5) return c;
    if (space < 1.5) return jfx_oklab_to_rgb(jfx_oklch_to_oklab(c));
    if (space < 2.5) return jfx_oklab_to_rgb(c);
    if (space < 3.5) return jfx_hsl_to_rgb(c);
    if (space < 4.5) return jfx_hsv_to_rgb(c);
    return jfx_lab_to_rgb(jfx_lch_to_lab(c));
}

vec4 jfx_mix_colors(vec4 a, vec4 b, float t, float space){
    float weightA = a.a * (1.0 - t);
    float weightB = b.a * t;
    float total = weightA + weightB;
    float safe = max(total, 0.001);
    vec3 mixed = (jfx_to_space(a.rgb, space) * weightA + jfx_to_space(b.rgb, space) * weightB) / safe;
    return vec4(jfx_from_space(mixed, space), total);
}

// the position of stop i of a palette (4 per vec4)
float jfx_palette_stop(vec4 stops[2], int i){
    vec4 four = i < 4 ? stops[0] : stops[1];
    int k = i < 4 ? i : i - 4;
    return k == 0 ? four.x : (k == 1 ? four.y : (k == 2 ? four.z : four.w));
}

// a palette (the palette parameter type: colors, stop positions 0..1 ascending, count 2..8) at
// t, clamped to its first and last stops; mixed segment by segment in `space`
vec4 jfx_palette(vec4 colors[8], vec4 stops[2], float count, float t, float space){
    int n = int(clamp(count, 1.0, 8.0) + 0.5);
    vec4 color = colors[0];
    for (int i = 1; i < 8; i++){
        if (i >= n)
            break;
        float a = jfx_palette_stop(stops, i - 1);
        float b = jfx_palette_stop(stops, i);
        if (t <= a)
            break;
        float local = b > a ? clamp((t - a) / (b - a), 0.0, 1.0) : 1.0;
        color = jfx_mix_colors(colors[i - 1], colors[i], local, space);
    }
    return color;
}

// the palette repeated: t wraps into [0, 1)
vec4 jfx_palette_wrap(vec4 colors[8], vec4 stops[2], float count, float t, float space){
    return jfx_palette(colors, stops, count, fract(t), space);
}

// The GL backends flatten the params block, and an array member cannot be passed whole to a
// function there: JFX_DECLARE_PALETTE(<member>) (after the params block) declares
// vec4 jfx_palette_<member>(float t, float space) and jfx_palette_wrap_<member>(t, space),
// which copy the palette parameter's arrays first.
#define JFX_DECLARE_PALETTE(member) \
    vec4 jfx_palette_##member(float t, float space){ \
        vec4 colors[8]; \
        for (int i = 0; i < 8; i++) \
            colors[i] = params.member[i]; \
        vec4 stops[2]; \
        stops[0] = params.member##Stops[0]; \
        stops[1] = params.member##Stops[1]; \
        return jfx_palette(colors, stops, params.member##Count, t, space); \
    } \
    vec4 jfx_palette_wrap_##member(float t, float space){ \
        return jfx_palette_##member(fract(t), space); \
    }

// Inigo Quilez's cosine palette: a + b * cos(2 pi (c t + d))
vec3 jfx_cosine_palette(float t, vec3 a, vec3 b, vec3 c, vec3 d){
    return a + b * cos(JFX_TAU * (c * t + d));
}

// hue rotation about the grey axis (Rodrigues), radians
vec3 jfx_hue_rotate(vec3 rgb, float radians){
    const vec3 k = vec3(0.57735026919);
    float c = cos(radians);
    return rgb * c + cross(k, rgb) * sin(radians) + k * dot(k, rgb) * (1.0 - c);
}

// saturation around the Rec. 709 luma: 0 grey, 1 unchanged
vec3 jfx_saturate(vec3 rgb, float s){
    return mix(vec3(jfx_luma709(rgb)), rgb, s);
}

#endif // JFX_COLOR_GLSL
