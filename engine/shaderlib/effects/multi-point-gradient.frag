#version 450
// @effect     multi-point-gradient
// @kind       paint
// @category   gradient
// @summary    Five coloured points anywhere in the area, each pixel taking the colours of the nearest
// @source     shaders.com MultiPointGradient
// @cost       low
// @param      color-a color #4776e6 -- Colour of point A
// @param      position-a position "20% 20%" -- Position of point A
// @param      color-b color #c44dff -- Colour of point B
// @param      position-b position "80% 20%" -- Position of point B
// @param      color-c color #1abc9c -- Colour of point C
// @param      position-c position "20% 80%" -- Position of point C
// @param      color-d color #f8bbd9 -- Colour of point D
// @param      position-d position "80% 80%" -- Position of point D
// @param      color-e color #ff8c42 -- Colour of point E
// @param      position-e position "50% 50%" -- Position of point E
// @param      smoothness number 2 0 5 -- How far each point's colour reaches
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix

// Ported from shaders.com MultiPointGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (pointCloudGradient: mpgFactors, the running inverse-distance-weighted mix of the five colours).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    vec4 colorD;
    vec4 colorE;
    vec2 positionA;
    vec2 positionB;
    vec2 positionC;
    vec2 positionD;
    vec2 positionE;
    float smoothness;
    float colorSpace;
} params;

// mpgWeight: inverse distance to a power, in aspect-corrected space
float mpg_weight(vec2 uvAspect, vec2 pos, float power){
    float dx = uvAspect.x - pos.x;
    float dy = uvAspect.y - pos.y;
    float dist = sqrt(dx * dx + dy * dy) + 0.001;
    return 1.0 / pow(dist, power);
}

vec4 jfx_paint(JfxContext c){
    vec2 uvAspect = vec2(c.uv.x * c.aspect, c.uv.y);
    vec2 scale = vec2(c.aspect, 1.0);
    // higher smoothness, lower power: the points reach further
    float power = min(8.0 / (params.smoothness + 0.5), 8.0);
    float wA = mpg_weight(uvAspect, params.positionA * scale, power);
    float wB = mpg_weight(uvAspect, params.positionB * scale, power);
    float wC = mpg_weight(uvAspect, params.positionC * scale, power);
    float wD = mpg_weight(uvAspect, params.positionD * scale, power);
    float wE = mpg_weight(uvAspect, params.positionE * scale, power);
    // the running weighted average as four mix factors
    float acc1 = wA;
    float t1 = wB / (acc1 + wB);
    float acc2 = acc1 + wB;
    float t2 = wC / (acc2 + wC);
    float acc3 = acc2 + wC;
    float t3 = wD / (acc3 + wD);
    float acc4 = acc3 + wD;
    float t4 = wE / (acc4 + wE);
    float space = params.colorSpace;
    // the linear space on a branch of its own, the same mixes without the colour-space
    // conversions (software rasterisers evaluate every space of jfx_mix_colors otherwise)
    if (space < 0.5){
        vec4 lin = fam_mix_in_space(params.colorA, params.colorB, t1);
        lin = fam_mix_in_space(lin, params.colorC, t2);
        lin = fam_mix_in_space(lin, params.colorD, t3);
        return fam_mix_in_space(lin, params.colorE, t4);
    }
    vec4 acc = jfx_mix_colors(params.colorA, params.colorB, t1, space);
    acc = jfx_mix_colors(acc, params.colorC, t2, space);
    acc = jfx_mix_colors(acc, params.colorD, t3, space);
    return jfx_mix_colors(acc, params.colorE, t4, space);
}

#include "effects/lib/main_paint.glsl"
