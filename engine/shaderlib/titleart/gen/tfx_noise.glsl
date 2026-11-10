// Pachislop's hash and value noise (src/effects/lib.ts NOISE_GLSL), ported by SafEight's
// permission; hash12 / hash22 are Dave Hoskins' "Hash without Sine" (MIT). Its presets were tuned
// against these spectra, so title generators use them and not the HTML UI's jfx_noise
// (docs/specs/S83-title-effects-generators.md §4.2, D7). Every title shader is highp (R30).

#ifndef TITLEART_GEN_TFX_NOISE_GLSL
#define TITLEART_GEN_TFX_NOISE_GLSL

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}

float fbm(vec2 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 5; i++) {
        s += a * vnoise(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return s;
}

#endif // TITLEART_GEN_TFX_NOISE_GLSL
