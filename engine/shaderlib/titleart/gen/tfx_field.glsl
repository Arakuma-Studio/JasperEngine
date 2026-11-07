// The effect library's distance field (effects/TitleFx.h fx::distanceField) as generators read it:
// signed distance to the alpha edge in target px, + outside, in TA_RAW0 (R16F, or RGBA8 fixed
// point with TA_DISTANCE_FIXED: 1/64 px around 32768, +-512 px). gen_sd samples it bilinearly
// with four texelFetch (the raw slot is never filtered: a device may not filter its format), so
// Pachislop's filtered `sd()` (src/generators/energy/shared.ts SD_GLSL) keeps its smoothness;
// past the target it reads "far outside".

#ifndef TITLEART_GEN_TFX_FIELD_GLSL
#define TITLEART_GEN_TFX_FIELD_GLSL

float gen_readDistance(ivec2 q) {
    vec4 t = texelFetch(TA_RAW0, clamp(q, ivec2(0), ivec2(TA_SIZE) - 1), 0);
#ifdef TA_DISTANCE_FIXED
    return (ta_bytes16(t.r, t.g) - 32768.0) / 64.0;
#else
    return t.r;
#endif
}

// the field at a point in target px (pixel centres at +0.5)
float gen_sd(vec2 frag) {
    if (frag.x < 0.0 || frag.y < 0.0 || frag.x > TA_SIZE.x || frag.y > TA_SIZE.y) return 1e4;
    vec2 p = frag - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    ivec2 q = ivec2(i);
    float a = gen_readDistance(q), b = gen_readDistance(q + ivec2(1, 0));
    float c = gen_readDistance(q + ivec2(0, 1)), d = gen_readDistance(q + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

#endif // TITLEART_GEN_TFX_FIELD_GLSL
