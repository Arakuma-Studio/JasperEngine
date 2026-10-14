#version 450
// @effect     stripes
// @kind       paint
// @category   pattern
// @summary    Alternating two-colour stripes at any angle, scrolling with time
// @source     shaders.com Stripes
// @cost       low
// @time       speed
// @param      color-a color #000000 -- First stripe colour
// @param      color-b color #ffffff -- Second stripe colour
// @param      angle angle 45deg -180deg 180deg -- Angle of the stripes
// @param      density number 5 1 30 -- Stripe pairs across the area's height
// @param      balance number 0.5 0 1 -- Share of the first colour in each pair
// @param      softness number 0 0 1 -- Edge softness (0 is crisp)
// @param      speed number 0.2 -1 1 -- Scroll speed in stripe pairs per second
// @param      offset number 0 0 1 -- Phase offset (1 is one pair)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix

// Ported from shaders.com Stripes (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (patternPaints.ts stripesMask/stripeCoordP, aa.ts quilezStepFilter/footprint1, strokeOver).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float angle;
    float density;
    float balance;
    float softness;
    float speed;
    float offset;
    float colorSpace;
} params;

// aa.ts quilezStepFilter: the average of step(threshold, fract(p)) over a footprint w
float stripes_step_filter(float p, float w, float threshold){
    float a = p + w * 0.5;
    float b = p - w * 0.5;
    float oneMinusThreshold = 1.0 - threshold;
    float fa = floor(a) * oneMinusThreshold + max(fract(a) - threshold, 0.0);
    float fb = floor(b) * oneMinusThreshold + max(fract(b) - threshold, 0.0);
    return clamp((fa - fb) / w, 0.0, 1.0);
}

vec4 jfx_paint(JfxContext c){
    // stripeCoordP: the projection onto the stripe direction pivots on the area's centre
    // (aspect-corrected, height 1), scrolled by the integrated clock plus the offset
    vec2 centred = vec2(c.uv.x * c.aspect - c.aspect * 0.5, c.uv.y - 0.5);
    float projected = centred.x * cos(params.angle) + centred.y * sin(params.angle) + c.aspect * 0.5;
    float p = projected * params.density + c.anim.x + params.offset;
    // footprint1: the screen-space footprint widened by the softness
    float w = max(max(abs(dFdx(p)), abs(dFdy(p))) + params.softness, 0.00001);
    float mask = stripes_step_filter(p, w, params.balance);
    return jfx_mix_colors(params.colorA, params.colorB, mask, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
