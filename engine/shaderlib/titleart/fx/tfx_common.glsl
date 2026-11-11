// What S83's effect programs share (docs/specs/S83-title-effects-generators.md R9, D14):
// light encoded the Pachislop way (RGB may exceed 1, alpha the brightest channel clamped),
// premultiplied "over", the distance field's gradient and the canvas mapping. Light helpers are
// ported from Pachislop src/effects/b/common.ts (HEAD: addLight, lightPremul, over) by SafEight,
// used with the author's permission.
//
// Light targets: on the RGBA8 path the light accumulators hold light / headroom (4, D14), so a
// program writing one multiplies by 1 / headroom and one reading it by headroom; the C++ side
// passes the factor (TfxPrograms.h lightHeadroom).

#ifndef TITLEART_FX_COMMON_GLSL
#define TITLEART_FX_COMMON_GLSL

// premultiplied src over dst
vec4 tfx_over(vec4 src, vec4 dst) {
    return src + dst * (1.0 - src.a);
}

// additive light into premultiplied dst, its alpha the light's brightest channel (so it survives
// export on a transparent background)
vec4 tfx_addLight(vec4 dst, vec3 light) {
    light = max(light, 0.0);
    float la = clamp(max(max(light.r, light.g), light.b), 0.0, 1.0);
    return vec4(dst.rgb + light, dst.a + (1.0 - dst.a) * la);
}

// a colour of light as premultiplied rgba, alpha its brightest channel
vec4 tfx_lightPremul(vec3 light) {
    light = max(light, 0.0);
    return vec4(light, clamp(max(max(light.r, light.g), light.b), 0.0, 1.0));
}

// the signed distance (+ outside) at an integer texel, clamped to the field (whose size may
// differ from the pass's target: the drip origins and the stitch layout write smaller ones)
float tfx_distanceAt(ivec2 q) {
    return fx_readDistance(clamp(q, ivec2(0), textureSize(TA_RAW0, 0) - 1));
}

// the distance bilinearly between texel centres (a smooth field for marching and shading)
float tfx_distanceSmooth(vec2 p) {
    vec2 q = p - 0.5;
    ivec2 i = ivec2(floor(q));
    vec2 f = q - floor(q);
    float a = tfx_distanceAt(i), b = tfx_distanceAt(i + ivec2(1, 0));
    float c = tfx_distanceAt(i + ivec2(0, 1)), d = tfx_distanceAt(i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// the outward normal of the distance field at p (frame px, y down): its central-difference
// gradient, normalised; zero where the field is flat
vec2 tfx_distanceNormal(vec2 p) {
    ivec2 i = ivec2(p);
    vec2 g = vec2(tfx_distanceAt(i + ivec2(1, 0)) - tfx_distanceAt(i - ivec2(1, 0)),
                  tfx_distanceAt(i + ivec2(0, 1)) - tfx_distanceAt(i - ivec2(0, 1)));
    float l = length(g);
    return l > 1e-4 ? g / l : vec2(0.0);
}

// canvas px (from the canvas centre `centre`, given in target px, y down) of a target pixel
vec2 tfx_canvasPx(vec2 p, vec2 centre) {
    return (p - centre) / TA_PX;
}

// sRGB-encoded straight colour of premultiplied linear light, for effects that blend the way
// Photoshop does
vec3 tfx_srgbOf(vec4 c) {
    return ta_toSrgb(ta_unpremul(c));
}

// a 2-octave value noise in 0..1 (Pachislop's vnoise from titleart/render/noise.glsl)
float tfx_noise2(vec2 p) {
    return 0.65 * vnoise(p) + 0.35 * vnoise(p * 2.03 + 5.2);
}

// fbm of `octaves` (1..8) value-noise octaves, 0..1
float tfx_fbm(vec2 p, int octaves) {
    float a = 0.5, s = 0.0, w = 0.0;
    for (int i = 0; i < 8; i++) {
        if (i >= octaves) break;
        s += a * vnoise(p);
        w += a;
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return s / max(w, 1e-4);
}

// Worley distances to the nearest and second-nearest seed of a unit jittered grid: (F1, F2,
// the nearest cell's hash)
vec3 tfx_worley(vec2 p, float seed) {
    vec2 i = floor(p);
    float f1 = 8.0, f2 = 8.0, id = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 c = i + vec2(x, y);
            vec2 o = hash22(c + seed * 17.13);
            float d = length(c + o - p);
            if (d < f1) {
                f2 = f1;
                f1 = d;
                id = hash12(c + seed * 3.7);
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    return vec3(f1, f2, id);
}

#endif // TITLEART_FX_COMMON_GLSL
