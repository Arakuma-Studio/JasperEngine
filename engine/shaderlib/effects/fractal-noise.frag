#version 450
// @effect     fractal-noise
// @kind       paint
// @category   noise
// @summary    Multi-octave fractal noise (fBm) that morphs like drifting clouds
// @source     shaders.com FractalNoise
// @cost       medium
// @time       speed
// @suggest-for fbm clouds
// @param      color-a color #000000 -- Colour of the low values
// @param      color-b color #ffffff -- Colour of the high values
// @param      octaves integer 4 1 8 -- Number of noise layers
// @param      detail number 2 1 4 -- Frequency step between the layers (lacunarity)
// @param      contrast number 0.5 0.1 1 -- Weight step between the layers (persistence)
// @param      speed number 0.15 0 1 -- Animation speed
// @param      angle angle 0deg -180deg 180deg -- Rotation of the pattern
// @param      seed number 0 0 100 -- Random seed for variation
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix

// Ported from shaders.com FractalNoise (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (fractalNoise: rotatedCenteredDomain, fractalSum (the gated golden-angle fBm over
// mxNoiseFloat2 at base frequency 2), fbmNormalize, the two-colour ramp).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_noise.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float octaves;
    float detail;
    float contrast;
    float speed;
    float angle;
    float seed;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    // rotatedCenteredDomain: aspect-corrected, centred, rotated by the angle
    float au = c.uv.x * c.aspect - c.aspect * 0.5;
    float av = c.uv.y - 0.5;
    float cosA = cos(params.angle);
    float sinA = sin(params.angle);
    vec2 pos = vec2(au * cosA + av * sinA, au * -sinA + av * cosA);
    vec2 sum = fam_fbm_golden_mx2(pos, 2.0, params.detail, params.contrast, c.anim.x, vec2(params.seed, params.seed * 0.7),
                                  floor(params.octaves + 0.5));
    // fbmNormalize: weight-normalised, [-1, 1] to [0, 1]
    float v = clamp(sum.x / sum.y * 0.5 + 0.5, 0.0, 1.0);
    return jfx_mix_colors(params.colorA, params.colorB, v, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
