#version 450
// @effect     radial-wipe
// @kind       filter
// @category   transition
// @summary    Wipes the content away with a clock-hand sweep around a centre
// @source     shaders.com RadialWipe
// @cost       low
// @suggest-for clock-wipe
// @param      progress percentage 50% -- How far the sweep has gone (0% all visible, 100% all gone)
// @param      start-angle angle 0deg 0deg 360deg -- Where the sweep begins (0deg points right, 90deg down)
// @param      direction enum cw cw|ccw|both -- Which way the hand turns (both: two hands meeting opposite the start)
// @param      center position "50% 50%" -- Point the hand turns around
// @param      feather number 0.1 0 1 -- Softness of the sweeping edge
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%

// Ported from shaders.com RadialWipe (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.angular: angularCoord, revealMask, applyReveal). The compile-time
// direction is a runtime enum.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float progress;
    float startAngle;
    float direction;
    float feather;
    float invert;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_angular_coord(c.uv, c.aspect, params.center, params.startAngle, params.direction);
    return fam_reveal_here(fam_reveal_mask(coord, params.progress, params.feather, params.invert));
}

#include "effects/lib/main_filter.glsl"
