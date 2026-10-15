#version 450
// @effect     ripple-wipe
// @kind       filter
// @category   transition
// @summary    Wipes the content away as concentric rings vanishing one after another
// @source     shaders.com RippleWipe
// @cost       low
// @param      progress percentage 50% -- How far the rings have gone (0% all visible, 100% all gone)
// @param      center position "50% 50%" -- Point the rings spread from
// @param      rings integer 8 2 40 -- Number of rings
// @param      feather number 0.2 0 1 -- How much the rings' timing overlaps (0 gives discrete ring pops)
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com RippleWipe (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.radial(center).bands(rings): radialCornerNormCoord, bandMidpointCoord,
// revealMask, applyReveal).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float progress;
    float rings;
    float feather;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_band_midpoint_coord(fam_radial_corner_coord(c.uv, c.aspect, params.center), floor(params.rings + 0.5));
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.feather, params.invert));
}

#include "effects/lib/main_filter.glsl"
