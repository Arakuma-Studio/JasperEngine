// Glints (キラキラ), ported from Pachislop src/generators/glints.ts by SafEight, used with the
// author's permission: the star sprites (its starVert / starFrag, here QuadBatch quads) and the
// pass that writes their light, with the hero's long anamorphic cross (lightFrag, heroFrag).
//
// QuadsGlints, drawn additively into a cleared scratch: v_quad the star's frame in star radii,
// v_color.rgb its linear colour times its intensity, v_extra = (the smallest core half-width in
// star radii, the twinkle phase 0..1, 0, 0); p6 = (ray count 4, 6 or 8, the ray half-width at the
// core in star radii, spectral 0/1, 0); p7 = (twinkle 0..1, twinkle phase, twinkle rotation in
// radians, 0).
// GlintsCompose, fullscreen: TA_TEX0 the stars' light (a target of this size); p6 = (gain, hero
// 0/1, the hero's centre x, y in the drawing space); p7 = (the hero's thickness, streak length,
// vertical spike length in target px, streak angle in radians counter-clockwise); p8 = (linear
// colour, 0).

#ifndef TITLEART_GEN_GLINTS_GLSL
#define TITLEART_GEN_GLINTS_GLSL

#include "titleart/gen/tfx_glint_star.glsl"
#include "titleart/gen/tfx_anamorphic.glsl"

// the quad inputs exist only in QuadBatch's programs
#if TA_GEN_VERTICES != 0
vec4 gen_glintsStar() {
    // one pixel in star radii from the unturned frame (Pachislop's fwidth of its vL)
    vec2 fw = fwidth(v_quad);
    float px = max(fw.x, fw.y);
    // the live twinkle (S83 §6.7.8): intensity x (1 - twinkle x (0.5 - 0.5 cos 2 pi (phase + phase_i)))^2
    // and a turn of up to the twinkle rotation; at their defaults the star is Pachislop's
    float cycle = 2.0 * TA_PI * (ta.p[7].y + v_extra.y);
    float flash = 1.0 - ta.p[7].x * (0.5 - 0.5 * cos(cycle));
    float turn = ta.p[7].z * sin(cycle);
    vec2 v = v_quad;
    if (turn != 0.0) {
        float c = cos(turn), s = sin(turn);
        v = vec2(c * v.x + s * v.y, -s * v.x + c * v.y);
    }
    vec3 light = glintStar(v, int(ta.p[6].x + 0.5), ta.p[6].y, v_extra.x, px, v_color.rgb, ta.p[6].z > 0.5) * (flash * flash);
    // only the colour is read back (GlintsCompose); the coverage stays a valid alpha
    return vec4(light, min(max(light.r, max(light.g, light.b)), 1.0));
}
#endif

// a thin line tapering to nothing at +-len along x: it carries the streak across the whole span,
// where the lens-flare streak alone (exponential falloff) would have faded by mid-length
vec3 glints_taper(vec2 lv, float t, float len, vec3 col) {
    float f = max(1.0 - abs(lv.x) / max(len, 1.0), 0.0);
    float ay = abs(lv.y) / t;
    return mix(col, vec3(1.0), f * f) * (exp(-ay * 30.0) * 1.3 + exp(-ay * 6.0) * 0.2) * pow(f, 1.1);
}

vec4 gen_glintsCompose() {
    vec3 stars = texture(TA_TEX0, ta_uv()).rgb;
    float gain = ta.p[6].x;
    if (ta.p[6].y < 0.5) return gen_lightPremul(stars * gain);
    float t = ta.p[7].x, len = ta.p[7].y, spike = ta.p[7].z, rot = ta.p[7].w;
    vec3 col = ta.p[8].rgb;
    // Pachislop's render px from the hero (+y up): the drawing-space offset in target px
    vec2 v = (gen_spacePos() - ta.p[6].zw) * gen_spaceScale();
    float c = cos(rot), sn = sin(rot);
    vec2 lv = vec2(c * v.x + sn * v.y, -sn * v.x + c * v.y);
    float r = length(v) / (t * 0.6);
    vec3 L = vec3(1.0) * (exp(-r * r * 18.0) * 2.0 + 0.2 / (1.0 + r * r * 6.0)) + col * 0.3 / (1.0 + r * r * 1.5);
    L += anamorphicStreak(lv, t, len / 2.5, col) + glints_taper(lv, t, len, col);
    L += anamorphicStreak(lv.yx, t * 0.8, spike / 2.5, col) + glints_taper(lv.yx, t * 0.8, spike, col);
    return gen_lightPremul((L + stars) * gain);
}

#endif // TITLEART_GEN_GLINTS_GLSL
