#version 450
// @effect     sine-wave
// @kind       paint
// @category   pattern
// @summary    A soft travelling sine-wave stroke
// @source     shaders.com SineWave
// @cost       low
// @time       speed
// @suggest-for wave
// @param      color color #ffffff -- Colour of the wave
// @param      amplitude number 0.15 0 1 -- Height of the wave (share of the area's height)
// @param      frequency number 1 0.1 20 -- Waves across the area's height
// @param      speed number 1 -5 5 -- Travel speed
// @param      angle angle 0deg 0deg 360deg -- Direction of the wave
// @param      position position "50% 50%" -- Where the wave's axis runs through
// @param      thickness number 0.2 0 2 -- Thickness of the stroke
// @param      softness number 0.4 0 1 -- Edge softness

// Ported from shaders.com SineWave (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (sineStroke: sineWaveMask, the colour's alpha times the mask).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_pattern.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec2 position;
    float amplitude;
    float frequency;
    float speed;
    float angle;
    float thickness;
    float softness;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 centered = fam_centered_delta(c.uv, params.position, c.aspect);
    vec2 rotated = fam_rotate2(centered, cos(params.angle), sin(params.angle));
    float sineWave = sin(rotated.x * params.frequency * JFX_TAU + c.anim.x) * params.amplitude;
    float distanceFromWave = abs(rotated.y - sineWave);
    float halfThickness = params.thickness * 0.5;
    float halfSoftness = params.softness * 0.5;
    float mask = 1.0 - fam_smoothstep(halfThickness - halfSoftness, halfThickness + halfSoftness, distanceFromWave);
    return vec4(params.color.rgb, params.color.a * mask);
}

#include "effects/lib/main_paint.glsl"
