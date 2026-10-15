#version 450
// @effect     block-dissolve
// @kind       filter
// @category   transition
// @summary    Dissolves the content away as a grid of blocks vanishing in random order
// @source     shaders.com BlockDissolve
// @cost       low
// @param      progress percentage 50% -- How far the dissolve has gone (0% all visible, 100% all gone)
// @param      block-size number 0.08 0.01 0.5 -- Size of a block as a share of the area's width
// @param      softness number 0.15 0 1 -- How softly each block fades (0 gives hard on/off blocks)
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com BlockDissolve (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.cells(blockSize).shuffled(): cellShuffleCoord, revealMask, applyReveal).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    float progress;
    float blockSize;
    float softness;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_cell_shuffle_coord(c.uv, c.aspect, params.blockSize);
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.softness, params.invert));
}

#include "effects/lib/main_filter.glsl"
