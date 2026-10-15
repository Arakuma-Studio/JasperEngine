#version 450
// @effect     venetian-blinds
// @kind       filter
// @category   transition
// @summary    Wipes the content away as strips closing all at once, like venetian blinds
// @source     shaders.com VenetianBlinds
// @cost       low
// @suggest-for blinds
// @param      progress percentage 50% -- How far the blinds have closed (0% all visible, 100% all gone)
// @param      angle angle 0deg 0deg 360deg -- Direction each strip closes in (0deg left to right: vertical strips)
// @param      strip-count integer 5 2 60 -- Number of strips across the area
// @param      feather number 0.15 0 1 -- Softness of each strip's closing edge
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com VenetianBlinds (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.directional(angle).tiled(stripCount): directionalCoord, tilePhase,
// revealMask, applyReveal).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    float progress;
    float angle;
    float stripCount;
    float feather;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_tile_phase(fam_directional_coord(c.uv, c.aspect, params.angle), floor(params.stripCount + 0.5));
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.feather, params.invert));
}

#include "effects/lib/main_filter.glsl"
