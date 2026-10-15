#version 450
// @effect     iris-wipe
// @kind       filter
// @category   transition
// @summary    Wipes the content away with a circle growing from a centre
// @source     shaders.com IrisWipe
// @cost       low
// @suggest-for circle-wipe
// @param      progress percentage 50% -- How far the iris has grown (0% all visible, 100% all gone)
// @param      center position "50% 50%" -- Point the iris grows from
// @param      feather number 0.1 0 1 -- Softness of the iris edge
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com IrisWipe (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.radial: radialCornerNormCoord, revealMask, applyReveal).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float progress;
    float feather;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_radial_corner_coord(c.uv, c.aspect, params.center);
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.feather, params.invert));
}

#include "effects/lib/main_filter.glsl"
