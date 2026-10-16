#version 450
// @effect     vignette
// @kind       filter
// @category   stylize
// @summary    Darkens (or tints) the content toward its edges around a clear centre
// @source     shaders.com Vignette
// @cost       low
// @param      color color #000000 -- Colour at the edges
// @param      center position "50% 50%" -- Centre of the clear area
// @param      radius number 0.5 0 1.5 -- Radius of the clear area (share of the height)
// @param      falloff number 0.5 0.01 1.5 -- Width of the fade from clear to full vignette
// @param      intensity number 1 0 1 -- Strength of the vignette

// Ported from shaders.com Vignette (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std tintToward(color, radialMask(...).times(intensity)): fields.ts radialFalloffMask and
// colorMixing.ts mixToward, the alpha kept).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec2 center;
    float radius;
    float falloff;
    float intensity;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 s = jfx_decode(jfx_src_premul_here());
    float amount = fam_radial_falloff_mask(c.uv, c.aspect, params.center, params.radius, params.falloff) * params.intensity;
    return vec4(mix(s.rgb, params.color.rgb, amount), s.a);
}

#include "effects/lib/main_filter.glsl"
