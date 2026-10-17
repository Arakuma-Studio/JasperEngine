#version 450
// @effect     duotone
// @kind       filter
// @category   color
// @summary    Recolours the content with two colours by its brightness
// @source     shaders.com Duotone
// @cost       low
// @param      color-a color #ff0000 -- Colour of the dark areas
// @param      color-b color #023af4 -- Colour of the bright areas
// @param      amount number 0.5 0 1 -- Brightness where the two colours meet (source `blend`)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space the two colours mix in

// Ported from shaders.com Duotone (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/color.ts duotone: colorOps.ts duotoneT, smoothstep(amount - 0.5, amount + 0.5,
// Rec. 601 luminance), then the two colours' mixColors in the colour space; the content's alpha).
// The linear space mixes on a branch of its own: llvmpipe would run every space of
// jfx_mix_colors.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float amount;
    float colorSpace;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 s = jfx_decode(jfx_src_premul_here());
    float t = smoothstep(params.amount - 0.5, params.amount + 0.5, jfx_luma601(s.rgb));
    vec3 rgb;
    if (params.colorSpace < 0.5){
        // mixColors in linear space: alpha-weighted
        float weightA = params.colorA.a * (1.0 - t);
        float weightB = params.colorB.a * t;
        rgb = (params.colorA.rgb * weightA + params.colorB.rgb * weightB) / max(weightA + weightB, 0.001);
    }else{
        rgb = jfx_mix_colors(params.colorA, params.colorB, t, params.colorSpace).rgb;
    }
    return vec4(rgb, s.a);
}

#include "effects/lib/main_filter.glsl"
