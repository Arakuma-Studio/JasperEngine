#version 450
// @effect     pixelate
// @kind       filter
// @category   stylize
// @summary    Big square or round pixels of the content
// @source     shaders.com Pixelate (adapted: cell size in px)
// @cost       low
// @suggest-for pixel mosaic
// @param      size length 8px 1px 256px -- Size of a cell
// @param      gap number 0 0 0.95 -- Space between cells as a share of the cell
// @param      roundness number 0 0 1 -- Corner roundness of a cell (1 is a circle)

// Ported from shaders.com Pixelate (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (motionBlur.ts pixelateSample/pixelateGather). Cells are `size` CSS px from the area's
// top-left corner instead of a count along the longest side.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    float size;
    float gap;
    float roundness;
} params;

vec4 jfx_filter(JfxContext c){
    float size = max(params.size, 1.0);
    vec2 cell = floor(c.px / size);
    // the cell's first texel (pixelateSample samples the floored cell origin)
    vec4 s = jfx_src(cell * size + vec2(0.5));
    // the rounded-rectangle cell mask
    vec2 local = fract(c.px / size) - 0.5;
    float halfSize = 0.5 - params.gap * 0.5;
    float cornerRadius = params.roundness * halfSize;
    vec2 q = max(abs(local) - halfSize + cornerRadius, 0.0);
    float sdf = length(q) - cornerRadius;
    float mask = sdf <= 0.0 ? 1.0 : 0.0;
    return vec4(s.rgb, s.a * mask);
}

#include "effects/lib/main_filter.glsl"
