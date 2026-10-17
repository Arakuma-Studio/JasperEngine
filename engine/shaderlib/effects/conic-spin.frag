#version 450
// @effect     conic-spin
// @kind       paint
// @category   gradient
// @summary    A conic (angular) gradient that can spin around its centre
// @source     shaders.com ConicGradient (adapted: speed in turns/s)
// @cost       low
// @time       speed
// @suggest-for conic-gradient spin rotating-border
// @param      colors palette "#ff0080 #00bfff" -- Colours of the sweep
// @param      center position "50% 50%" -- Centre of the sweep
// @param      rotation angle 0deg -3600deg 3600deg -- Where the sweep starts (0deg: at the left of the centre)
// @param      repeat number 1 1 24 -- Number of sweeps around the centre
// @param      speed number 0.1 -5 5 -- Spin speed in turns per second
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the gradient

// Ported from shaders.com ConicGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(tiles(dist.conic), stops): conicSweep, repeatWrap). Adapted: the integrated speed
// clock adds turns to the rotation.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    vec2 center;
    float rotation;
    float repeat;
    float speed;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

vec4 jfx_paint(JfxContext c){
    // conicSweep: 0..1 once around, counter-clockwise from the left, plus the rotation in turns
    vec2 delta = fam_centered_delta(c.uv, params.center, c.aspect);
    float angle = atan(delta.y, delta.x);
    float normalized = 0.5 - angle / JFX_TAU;
    float t = normalized + params.rotation / JFX_TAU + c.anim.x;
    return jfx_palette_colors(fam_repeat_wrap(t, params.repeat), params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
