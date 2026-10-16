#version 450
// @effect     flow-field
// @kind       filter
// @category   distort
// @summary    Liquifies the content along a drifting, evolving noise flow
// @source     shaders.com FlowField
// @cost       medium
// @time       speed, evolution-speed
// @suggest-for liquify warp
// @param      strength number 0.15 0 0.5 -- How far the flow pushes (share of the height)
// @param      detail number 2 0.5 5 -- Scale of the flow pattern
// @param      speed number 0 0 20 -- Drift of the flow pattern
// @param      evolution-speed number 0 0 20 -- How fast the pattern reshapes
// @param      seed number 0 0 100 -- Another pattern for each value
// @param      edges enum mirror mirror|stretch|transparent|wrap -- What shows where the flow reads beyond the element

// Ported from shaders.com FlowField (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/warps.ts flowNoise: warpMaps.ts flowFieldUV, three MaterialX noise layers read as flow
// angles, combined and normalised; both clocks at a tenth of the integrated speeds, the seed added
// to each; read through edges.ts sampleRemappedExpr: Catmull-Rom, bilinear in the mirror mode).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    float strength;
    float detail;
    float speed;
    float evolutionSpeed;
    float seed;
    float edges;
} params;

vec4 jfx_filter(JfxContext c){
    float time = (c.anim.x + params.seed) * 0.1;
    float evolutionTime = (c.anim.y + params.seed) * 0.1;
    float scale = params.detail;
    vec2 ac = vec2(c.uv.x * c.aspect, c.uv.y);
    float drift1 = time * 0.7;
    float angle1 = jfx_mx_noise3(vec3(ac.x * scale + drift1 * 0.3, ac.y * scale + drift1 * 0.5, evolutionTime * 0.2)) * JFX_TAU;
    float drift2 = time * 0.5;
    float angle2 = jfx_mx_noise3(vec3(ac.x * (scale * 2.3) + drift2 * -0.4 + 173.5, ac.y * (scale * 2.3) + drift2 * 0.3 + 291.7,
                                      evolutionTime * 0.15 + 50.0)) * JFX_TAU;
    float drift3 = time * 0.3;
    float angle3 = jfx_mx_noise3(vec3(ac.x * (scale * 4.7) + drift3 * 0.2 + 527.3, ac.y * (scale * 4.7) + drift3 * -0.6 + 839.1,
                                      evolutionTime * 0.1 + 100.0)) * JFX_TAU;
    vec2 flow = vec2(cos(angle1), sin(angle1)) + vec2(cos(angle2), sin(angle2)) * 0.5 + vec2(cos(angle3), sin(angle3)) * 0.25;
    vec2 normalizedFlow = flow / max(length(flow), 0.001);
    vec2 uv = c.uv + vec2(normalizedFlow.x * params.strength / c.aspect, normalizedFlow.y * params.strength);
    return fam_src_remapped(uv * c.size, fam_edge_mode_mirror_first(params.edges));
}

#include "effects/lib/main_filter.glsl"
