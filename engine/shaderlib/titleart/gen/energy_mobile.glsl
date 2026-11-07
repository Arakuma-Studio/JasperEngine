// The mobile tier's Fire and Smoke (docs/specs/S83-title-effects-generators.md §6.7.11, §6.7.12,
// D12), after Pachislop src/generators/energy/{fire,smoke,shared}.ts, ported by SafEight's
// permission: the upstream distance E (tfx_upstream.glsl) baked once into a field, and the
// flame and smoke passes reading it at their swayed points, with their noise from three fetches
// of the tiling noise texture (tfx_noise_tex.glsl) instead of twelve value noises. They run at
// half resolution (generators/TitleGenMobile.h drawReduced); the shading after E and the noise
// is fire.glsl's and smoke.glsl's.
//
// The texture's fields are fbm-like sums with mean 0.5: where Pachislop takes fbm (amplitudes
// summing to 0.96875) they are scaled to its mean, and where it takes one value noise their
// spread (0.6 of a single value noise's) is scaled back up.
//
// UpstreamField: TA_RAW0 the content's distance field; p6 = (flow direction x, y in the drawing
// space (unit, y up), march length in target px, side weight). Writes E / length clamped to
// 0..1: R16F, or with TA_DISTANCE_FIXED 16-bit fixed point in RG.
// FireMobile: TA_TEX0 the gradient, TA_TEX1 the noise texture, TA_RAW0 the content's distance
// field (over the edges), TA_RAW1 the baked field; p6..p8 as fire.glsl's, p9.x = the field's
// march length in target px.
// SmokeMobile: TA_TEX1 the noise texture, TA_RAW1 the baked field; p6..p9 as smoke.glsl's,
// p10.x = the field's march length in target px.

#ifndef TITLEART_GEN_ENERGY_MOBILE_GLSL
#define TITLEART_GEN_ENERGY_MOBILE_GLSL

#include "titleart/gen/tfx_noise_tex.glsl"
#include "titleart/gen/tfx_upstream.glsl"

vec4 gen_upstreamField() {
    float len = max(ta.p[6].z, 1.0);
    float E = gen_upstream(gl_FragCoord.xy, normalize(gen_dirToTarget(ta.p[6].xy)), len, ta.p[6].w);
    float v = clamp(E / len, 0.0, 1.0);
#ifdef TA_DISTANCE_FIXED
    return vec4(ta_split16(v * 65535.0), 0.0, 1.0);
#else
    return vec4(v, 0.0, 0.0, 1.0);
#endif
}

float em_readField(ivec2 q) {
    vec4 t = texelFetch(TA_RAW1, clamp(q, ivec2(0), ivec2(TA_SIZE) - 1), 0);
#ifdef TA_DISTANCE_FIXED
    return ta_bytes16(t.r, t.g) / 65535.0;
#else
    return t.r;
#endif
}

// the baked E at a point in target px (bilinear between pixel centres, as gen_sd); past the
// target nothing lies upstream
float em_upstream(vec2 frag, float len) {
    if (frag.x < 0.0 || frag.y < 0.0 || frag.x > TA_SIZE.x || frag.y > TA_SIZE.y) return len;
    vec2 p = frag - 0.5;
    vec2 i = floor(p);
    vec2 f = p - i;
    ivec2 q = ivec2(i);
    float a = em_readField(q), b = em_readField(q + ivec2(1, 0));
    float c = em_readField(q + ivec2(0, 1)), d = em_readField(q + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y) * len;
}

vec4 gen_fireMobile() {
    vec2 dir = ta.p[6].xy;
    float H = ta.p[6].z, W = ta.p[6].w;
    float turb = ta.p[7].x, tt = ta.p[7].y, seedN = ta.p[7].z;
    float intensity = ta.p[8].x, inside = ta.p[8].y;
    bool overEdge = ta.p[8].z > 0.5;
    float fieldLen = ta.p[9].x;
    float px = gen_spaceScale();

    vec2 pc = gen_spacePos();
    vec2 perp = vec2(-dir.y, dir.x);
    float fy = dot(pc, dir), fx = dot(pc, perp);
    vec2 seed = vec2(seedN * 17.31, seedN * 7.13);
    // the sway (fbm), the tongues (0.7 fbm + 0.3 value noise, mean 0.489) and the flicker (a
    // value noise): one fetch each
    float sway = gen_noiseTex(vec2(fx / (W * 3.0), fy / (W * 3.5) - tt * 0.7) + seed).r * 0.96875 - 0.5;
    vec2 frag = gl_FragCoord.xy + gen_dirToTarget(perp * (sway * W * turb * 1.4));
    float Hr = H * px;
    float E = em_upstream(frag, fieldLen);
    float n = gen_noiseTex(vec2(fx / W, fy / (W * 4.0) - tt * 1.1) + seed).g * 0.978;
    n = clamp((n - 0.5) * (1.6 + 1.6 * turb) + 0.5, 0.0, 1.0);
    float len = Hr * mix(1.0, 0.3 + 1.3 * n, clamp(turb * 1.3, 0.0, 1.0));
    float f = clamp(1.0 - E / max(len, 1.0), 0.0, 1.0);
    float flicker = (gen_noiseTex(vec2(fx / (W * 0.3), fy / (W * 0.6) - tt * 3.0) + seed.yx).r - 0.5) * 1.6;
    f += flicker * 0.9 * f * (1.0 - f);
    f = clamp(f, 0.0, 1.0);
    float a = smoothstep(0.0, 0.35, f);
    vec3 c = gen_ramp(TA_TEX0, 1.0 - f).rgb;
    c *= 1.0 + 0.6 * smoothstep(0.8, 1.0, f);    // white-hot core
    vec4 fire = vec4(c * intensity * a, a);
    if (overEdge) fire *= 1.0 - smoothstep(0.0, inside * px, max(-gen_sd(gl_FragCoord.xy), 0.0));
    return fire;
}

vec4 gen_smokeMobile() {
    vec2 dir = ta.p[6].xy;
    float H = ta.p[6].z, W = ta.p[6].w;
    float tt = ta.p[7].x, seedN = ta.p[7].y, soft = ta.p[7].w;
    vec3 col = ta.p[8].rgb, col2 = ta.p[9].rgb;
    float fieldLen = ta.p[10].x;
    float px = gen_spaceScale();

    vec2 pc = gen_spacePos();
    vec2 perp = vec2(-dir.y, dir.x);
    float fy = dot(pc, dir), fx = dot(pc, perp);
    vec2 seed = vec2(seedN * 11.7, seedN * 3.9);
    vec2 q = vec2(fx, fy - tt * W * 1.1) / (W * 2.0) + seed;
    // the domain warp's two components from one fetch, the billows and the plumes from one each
    vec2 w = gen_noiseTex(q * 0.6 + vec2(0.0, tt * 0.12)) - 0.5;
    float bil = gen_noiseTex(q + w * 1.5).r;
    float plume = gen_noiseTex(vec2(fx / (W * 2.5), tt * 0.08) + seed.yx).g;
    vec2 frag = gl_FragCoord.xy + gen_dirToTarget(perp * (w.x * W * 2.0));
    float Hr = H * px;
    float len = Hr * (0.45 + 0.9 * plume);
    float E = em_upstream(frag, fieldLen);
    float I = clamp(1.0 - E / max(len, 1.0), 0.0, 1.0);
    float dens = pow(I, 0.8) * (0.3 + 1.1 * bil);
    dens = smoothstep(0.08, 0.08 + soft, dens);
    vec3 c = mix(col2, col, smoothstep(0.3, 0.75, bil));
    return vec4(c * dens, dens);
}

#endif // TITLEART_GEN_ENERGY_MOBILE_GLSL
