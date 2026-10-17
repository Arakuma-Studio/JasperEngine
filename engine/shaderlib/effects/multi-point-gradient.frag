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

// mpgWeight: inverse distance to a power, in aspect-corrected space (as exp2 of the negated
// logarithm: no reciprocal)
float mpg_weight(vec2 uvAspect, vec2 pos, float power){
    vec2 d = uvAspect - pos;
    float dist = sqrt(dot(d, d)) + 0.001;
    return exp2(-power * log2(dist));
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
    // The kit mixes the colours as a running weighted average, A with B by wB / (wA + wB), that
    // with C by wC / (wA + wB + wC) and so on, each mix weighting by alpha (jfx_mix_colors). That
    // chain is the closed form below: colour = sum(c_i a_i w_i) / sum(a_i w_i), alpha =
    // sum(a_i w_i) / sum(w_i), in the mix's space; each colour enters its space once and the result
    // leaves it once (the kit's chain left and entered the space at every step: the same values up
    // to rounding, as the space conversions invert each other).
    float aA = params.colorA.a * wA;
    float aB = params.colorB.a * wB;
    float aC = params.colorC.a * wC;
    float aD = params.colorD.a * wD;
    float aE = params.colorE.a * wE;
    float weights = wA + wB + wC + wD + wE;
    float alphaWeights = aA + aB + aC + aD + aE;
    float alpha = alphaWeights / weights;
    float safe = max(alphaWeights, 0.001 * weights);
    float space = params.colorSpace;
    // the linear space on a branch of its own: no conversions
    if (space < 0.5){
        vec3 lin = params.colorA.rgb * aA + params.colorB.rgb * aB + params.colorC.rgb * aC + params.colorD.rgb * aD + params.colorE.rgb * aE;
        return vec4(lin / safe, alpha);
    }
    vec3 mixed = jfx_to_space(params.colorA.rgb, space) * aA + jfx_to_space(params.colorB.rgb, space) * aB +
                 jfx_to_space(params.colorC.rgb, space) * aC + jfx_to_space(params.colorD.rgb, space) * aD +
                 jfx_to_space(params.colorE.rgb, space) * aE;
    return vec4(jfx_from_space(mixed / safe, space), alpha);
}

#include "effects/lib/main_paint.glsl"
