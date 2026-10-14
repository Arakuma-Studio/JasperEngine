#version 450
// @effect     simplex-noise
// @kind       paint
// @category   noise
// @summary    Soft MaterialX noise that morphs in place over time
// @source     shaders.com SimplexNoise
// @cost       low
// @time       speed
// @param      color-a color #ffffff -- Colour of the low noise values
// @param      color-b color #000000 -- Colour of the high noise values
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix
// @param      scale number 2 -2 5 -- Pattern frequency (each +1 roughly doubles it)
// @param      contrast number 0 -1 5 -- Pattern contrast (0 unchanged)
// @param      balance number 0 -1 1 -- Shifts the balance between the two colours
// @param      seed number 0 0 100 -- Random seed (slides the pattern)
// @param      speed number 1 0 5 -- Animation speed

// Ported from shaders.com SimplexNoise (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(signedTone(seededPlane(evolving(noiseField('mx3signed'), 0.5))))): aspectScaledDomain,
// timeAxisDomain, mxNoiseFloat3, toneSignedInverted, the two-colour mix). The source's noise is
// MaterialX's 3D gradient noise despite the name.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float colorSpace;
    float scale;
    float contrast;
    float balance;
    float seed;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 pos = vec2(c.uv.x * c.aspect, c.uv.y) * exp(params.scale) + params.seed;
    float n = jfx_mx_noise3(vec3(pos, c.anim.x * 0.5));
    float k = fam_tone_signed_inverted(n, params.contrast, params.balance);
    return jfx_mix_colors(params.colorA, params.colorB, k, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
