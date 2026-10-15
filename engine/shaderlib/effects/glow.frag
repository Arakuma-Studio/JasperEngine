#version 450
// @effect     glow
// @kind       filter
// @category   light
// @summary    The content's bright parts spread into a soft halo added back on top
// @source     shaders.com Glow
// @cost       medium
// @prepass    blur size*0.5 threshold threshold
// @margin     size*1.5
// @suggest-for bloom
// @param      intensity number 1 0 50 -- Brightness of the halo
// @param      threshold number 0.5 0 1 -- How bright a part must be to glow (lower gives more glow)
// @param      size length 25px 0px 100px -- Spread of the halo

// Ported from shaders.com Glow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts bloom: the soft-knee bright extract, blurred, then glowCompose:
// original + bloom x intensity, the halo's alpha over the original's). The extract is the
// engine's bright pass (_bright.frag, the kit's bloomExtractSoftKnee) and the blur is the
// prepass with a sigma of half the size; size 0 leaves the content as it is (the source skips
// the bloom).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    float intensity;
    float threshold;
    float size;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 original = jfx_decode(jfx_src_premul_here());
    if (params.size <= 0.0)
        return original;
    vec4 bloom = jfx_blurred(c.px);
    // glowCompose on premultiplied colours
    vec3 premul = original.rgb * original.a + bloom.rgb * bloom.a * params.intensity;
    float glowAlpha = clamp(bloom.a * params.intensity, 0.0, 1.0);
    float alpha = original.a + glowAlpha * (1.0 - original.a);
    return vec4(alpha > 1e-5 ? premul / alpha : vec3(0.0), alpha);
}

#include "effects/lib/main_filter.glsl"
