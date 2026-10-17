#version 450
// @effect     tilt-shift
// @kind       filter
// @category   blur
// @summary    Tilt-shift lens: a sharp band through a point, the rest blurred like a miniature
// @source     shaders.com TiltShift
// @cost       medium
// @backdrop   yes
// @prepass    blur intensity*0.2
// @param      intensity number 50 0 100 -- Blur at the far edges (the blur's sigma is a fifth of this in px)
// @param      width number 0.3 0 1 -- Width of the sharp band (share of the height)
// @param      falloff number 0.3 0 1 -- Distance over which the blur ramps to full strength
// @param      angle angle 0deg 0deg 360deg -- Rotation of the sharp band (0deg horizontal)
// @param      center position "50% 50%" -- Point the sharp band runs through

// Ported from shaders.com TiltShift (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts tiltShift: blur.ts tiltShiftBlurAmount, the perpendicular distance from
// the focus line ramped from the sharp band over the falloff, mixing the sharp content with the
// blurred one, premultiplied). Adapted: the kit's variable-radius compute blur (radius
// intensity x 0.36 x the amount) is the blur prepass at its full sigma, mixed in by the amount.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_common.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float intensity;
    float width;
    float falloff;
    float angle;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 sharp = jfx_decode(jfx_src_premul_here());
    // tiltShiftBlurAmount
    vec2 perp = vec2(-sin(params.angle), cos(params.angle));
    vec2 centered = c.uv - params.center;
    float distFromLine = abs(dot(vec2(centered.x * c.aspect, centered.y), perp));
    float focusWidth = params.width * 0.5;
    float amount = fam_filters_smoothstep(focusWidth, focusWidth + params.falloff, distFromLine);
    vec4 blurred = jfx_blurred(c.px);
    return jfx_unpremultiply(mix(jfx_premultiply(sharp), jfx_premultiply(blurred), amount));
}

#include "effects/lib/main_filter.glsl"
