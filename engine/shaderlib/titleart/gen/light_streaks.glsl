// Light Streaks (ストリーク光), ported from Pachislop src/generators/lines/lightStreaks.ts
// (streakFrag) and src/generators/common.ts (glowInto's add) by SafEight, used with the author's
// permission: white-cored light lines with a coloured edge, each one quad shaded across its
// width, and the halo blurred from the edge colour alone.
//
// QuadsLightStreaks: v_extra = (half thickness and length in target px, intensity, end margin in
// target px). p6 = (core linear rgb, core width 0..1); p7 = (edge linear rgb, core weight: 2
// burns out to white under Add, 0 the halo's source, edge colour only); p8 = (taper fraction at
// the tail, at the head, chromatic split in target px, brightness).
//
// LightStreakCompose: both halos added as light in one pass (two glowInto passes' sum, alpha the
// union of both): TA_TEX0 the source blurred to a quarter of the halo radius, TA_TEX1 to the full
// radius; p6 = (their gains).

#ifndef TITLEART_GEN_LIGHT_STREAKS_GLSL
#define TITLEART_GEN_LIGHT_STREAKS_GLSL

#include "titleart/gen/lines_common.glsl"

#if TA_GEN_VERTICES == 1

// light at x target px from the axis of a streak of half width hw: a soft coloured falloff to the
// edge plus an anti-aliased core band x the core weight
vec3 ls_lightAt(float x, float hw) {
    float q = clamp(1.0 - x * x / (hw * hw), 0.0, 1.0);
    float cw = hw * ta.p[6].w;
    float core = clamp(max(cw, 0.5) - x + 0.5, 0.0, 1.0) * min(1.0, 2.0 * cw);
    return ta.p[7].rgb * (q * q) + ta.p[6].rgb * (ta.p[7].w * core);
}

vec4 gen_lightStreaksQuad() {
    float tail = ta.p[8].x, head = ta.p[8].y, chroma = ta.p[8].z, bright = ta.p[8].w;
    float L = v_extra.y;
    float s = ((0.5 + 0.5 * v_quad.x) * v_sizePx.x - v_extra.w) / L;     // 0 = tail, 1 = head
    float tp = ln_taperAt(clamp(s, 0.0, 1.0), tail, head);
    float hw0 = v_extra.x * tp;
    float hw = max(hw0, 0.5);
    // sub-pixel widths fade by their true width; tapered ends also dim
    float k = v_extra.z * bright * min(1.0, 2.0 * hw0) * sqrt(tp) * clamp(s * L + 0.5, 0.0, 1.0) * clamp((1.0 - s) * L + 0.5, 0.0, 1.0);
    float y = v_quad.y * 0.5 * v_sizePx.y;
    vec3 light = vec3(ls_lightAt(abs(y - chroma), hw).r, ls_lightAt(abs(y), hw).g, ls_lightAt(abs(y + chroma), hw).b) * k;
    return vec4(light, clamp(max(max(light.r, light.g), light.b), 0.0, 1.0));
}

#else

vec4 gen_lightStreakCompose() {
    vec4 a = texture(TA_TEX0, ta_uv()) * ta.p[6].x;
    vec4 b = texture(TA_TEX1, ta_uv()) * ta.p[6].y;
    float aa = min(a.a, 1.0), ab = min(b.a, 1.0);
    return vec4(a.rgb + b.rgb, 1.0 - (1.0 - aa) * (1.0 - ab));
}

#endif

#endif // TITLEART_GEN_LIGHT_STREAKS_GLSL
