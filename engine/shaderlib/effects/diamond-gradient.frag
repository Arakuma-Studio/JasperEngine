#version 450
// @effect     diamond-gradient
// @kind       paint
// @category   gradient
// @summary    A diamond-shaped gradient spreading out from a centre, sharp or rounded to a square
// @source     shaders.com DiamondGradient
// @cost       low
// @param      colors palette "#4ffb4a #4f1238" -- Colours of the gradient from the centre out
// @param      center position "50% 50%" -- Centre of the diamond
// @param      size number 0.7 0.01 2 -- Size as a share of the area's height
// @param      rotation angle 0deg 0deg 360deg -- Rotation of the diamond
// @param      repeat number 1 1 16 -- Number of rings (1 is one gradient)
// @param      roundness number 0 0 1 -- 0 is a sharp diamond, 1 a square
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the gradient

// Ported from shaders.com DiamondGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(rings(dist.diamond), stops): diamondDist, repeatRings).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    vec2 center;
    float size;
    float rotation;
    float repeat;
    float roundness;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

vec4 jfx_paint(JfxContext c){
    // diamondDist: the centred delta turned back by the rotation, L1 blended to L-infinity
    vec2 delta = fam_centered_delta(c.uv, params.center, c.aspect);
    vec2 rotated = fam_rotate2(delta, cos(params.rotation), -sin(params.rotation));
    float absDx = abs(rotated.x);
    float absDy = abs(rotated.y);
    float dist = mix(absDx + absDy, max(absDx, absDy), params.roundness);
    return jfx_palette_colors(fam_repeat_rings(dist / params.size, params.repeat), params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
