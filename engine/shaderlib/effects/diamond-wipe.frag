#version 450
// @effect     diamond-wipe
// @kind       filter
// @category   transition
// @summary    Wipes the content away with a grid of diamonds growing from their cells' centres
// @source     shaders.com DiamondWipe
// @cost       low
// @param      progress percentage 50% -- How far the diamonds have grown (0% all visible, 100% all gone)
// @param      size number 0.15 0.02 0.5 -- Size of a diamond's cell as a share of the area's width
// @param      feather number 0.1 0 1 -- Softness of the diamonds' edges
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com DiamondWipe (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.cells(size).diamond(): cellDiamondCoord, revealMask, applyReveal).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    float progress;
    float size;
    float feather;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_cell_diamond_coord(c.uv, c.aspect, params.size);
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.feather, params.invert));
}

#include "effects/lib/main_filter.glsl"
