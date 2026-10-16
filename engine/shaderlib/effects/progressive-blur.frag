#version 450
// @effect     progressive-blur
// @kind       filter
// @category   blur
// @summary    Blur growing from a point along a direction, like a fade-to-blur edge
// @source     shaders.com ProgressiveBlur
// @cost       medium
// @backdrop   yes
// @prepass    blur intensity*0.2
// @suggest-for gradient-blur fade-blur
// @param      intensity number 50 0 100 -- Strongest blur (the blur's sigma is a fifth of this in px)
// @param      angle angle 0deg 0deg 360deg -- Direction the blur grows toward (0deg along x)
// @param      center position "0% 50%" -- Point where the blur begins
// @param      falloff number 1 0 1 -- Distance over which the blur ramps to full strength

// Ported from shaders.com ProgressiveBlur (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts progressiveBlur: blur.ts progressiveBlurAmount, the forward distance from
// the centre along the direction ramped over the falloff; the colour is the blur's, the alpha the
// sharp content's). Adapted: the kit's variable-radius compute blur (radius intensity x 0.36 x
// the amount) is the blur prepass at its full sigma, mixed with the sharp content by the amount.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_common.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float intensity;
    float angle;
    float falloff;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 sharp = jfx_decode(jfx_src_premul_here());
    // progressiveBlurAmount
    vec2 direction = vec2(cos(params.angle) / c.aspect, sin(params.angle));
    float directionalDist = max(0.0, dot(c.uv - params.center, direction));
    float amount = fam_filters_smoothstep(0.0, params.falloff, directionalDist);
    vec4 blurred = jfx_premultiply(jfx_blurred(c.px));
    vec3 rgb = mix(sharp.rgb * sharp.a, blurred.rgb, amount);
    return jfx_unpremultiply(vec4(rgb, sharp.a));
}

#include "effects/lib/main_filter.glsl"
