#version 450
// @effect     posterize
// @kind       filter
// @category   color
// @summary    Reduces each colour channel of the content to a few flat levels
// @source     shaders.com Posterize
// @cost       low
// @param      levels number 5 2 20 -- Levels per channel (source `intensity`; lower is flatter)

// Ported from shaders.com Posterize (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/color.ts posterize: colorOps.ts posterize, floor(c x levels) / levels per channel
// of the straight linear colour, levels clamped to 2-20).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_tone.glsl"

uniform u_fs_effectParams {
    float levels;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 s = jfx_decode(jfx_src_premul_here());
    return vec4(jfx_posterize(s.rgb, params.levels), s.a);
}

#include "effects/lib/main_filter.glsl"
