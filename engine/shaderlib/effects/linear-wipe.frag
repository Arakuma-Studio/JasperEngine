#version 450
// @effect     linear-wipe
// @kind       filter
// @category   transition
// @summary    Wipes the content away along a straight, feathered edge
// @source     shaders.com LinearWipe
// @cost       low
// @suggest-for wipe
// @param      progress percentage 50% -- How far the wipe has gone (0% all visible, 100% all gone)
// @param      angle angle 0deg 0deg 360deg -- Direction the edge travels (0deg left to right, 90deg top to bottom)
// @param      feather number 0.1 0 1 -- Softness of the edge
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com LinearWipe (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.directional: directionalCoord, revealMask, applyReveal).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    float progress;
    float angle;
    float feather;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_directional_coord(c.uv, c.aspect, params.angle);
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.feather, params.invert));
}

#include "effects/lib/main_filter.glsl"
