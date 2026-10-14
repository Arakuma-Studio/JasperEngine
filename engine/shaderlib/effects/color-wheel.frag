#version 450
// @effect     color-wheel
// @kind       paint
// @category   gradient
// @summary    Bands cycling through the hue wheel or three colours, drifting along a direction
// @source     shaders.com ColorWheel
// @cost       low
// @time       speed
// @suggest-for rainbow hue-cycle
// @param      mode enum rainbow rainbow|custom -- The full hue wheel, or a cycle through the three colours
// @param      color-a color #ff0000 -- First colour (custom mode)
// @param      color-b color #00ff88 -- Second colour (custom mode)
// @param      color-c color #0066ff -- Third colour (custom mode)
// @param      scale number 1 0.1 10 -- How many cycles fit across the area (1 is a gentle, wide cycle)
// @param      angle angle 0deg -180deg 180deg -- Direction of the cycle
// @param      speed number 0.05 -1 1 -- Cycles per second
// @param      color-space enum oklch linear|oklch|oklab|hsl|hsv|lch -- Colour space of the custom mix

// Ported from shaders.com ColorWheel (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (colorWheel: colorWheelT, colorWheelRainbow, colorWheelPhase, colorWheelSelect).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    float mode;
    float scale;
    float angle;
    float speed;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    // colorWheelT: the projection from the area's centre on the direction, plus aspect / 2 (the
    // phase the uncentred projection had at angle 0)
    vec2 centred = fam_canvas_centred(c.uv, c.aspect);
    float projected = centred.x * cos(params.angle) + centred.y * sin(params.angle) + c.aspect * 0.5;
    float t = fract(projected * params.scale * 0.2 + c.anim.x);
    if (params.mode < 0.5){
        // colorWheelRainbow: the procedural hue cycle
        float h6 = t * 6.0;
        float r = clamp(abs(h6 - 3.0) - 1.0, 0.0, 1.0);
        float g = clamp(2.0 - abs(h6 - 2.0), 0.0, 1.0);
        float b = clamp(2.0 - abs(h6 - 4.0), 0.0, 1.0);
        return vec4(r, g, b, 1.0);
    }
    // the three-colour cycle: the segment's mix, picked by the floored segment index
    float t3 = t * 3.0;
    float si = fract(t3);
    float floorT3 = floor(t3);
    vec4 c01 = jfx_mix_colors(params.colorA, params.colorB, si, params.colorSpace);
    vec4 c12 = jfx_mix_colors(params.colorB, params.colorC, si, params.colorSpace);
    vec4 c20 = jfx_mix_colors(params.colorC, params.colorA, si, params.colorSpace);
    vec4 seg1 = mix(c20, c12, step(floorT3, 1.5));
    return mix(seg1, c01, step(floorT3, 0.5));
}

#include "effects/lib/main_paint.glsl"
