#version 450
// @effect     perlin-noise
// @kind       paint
// @category   noise
// @summary    Smooth gradient noise that morphs in place over time
// @source     shaders.com PerlinNoise
// @cost       low
// @time       speed
// @suggest-for noise
// @param      color-a color #ffffff -- Colour of the low noise values
// @param      color-b color #000000 -- Colour of the high noise values
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix
// @param      scale number 2 -2 5 -- Pattern frequency (each +1 roughly doubles it)
// @param      contrast number 0 -1 5 -- Pattern contrast (0 unchanged)
// @param      balance number 0 -1 1 -- Shifts the balance between the two colours
// @param      seed number 0 0 100 -- Random seed (slides the pattern)
// @param      speed number 1 0 5 -- Animation speed

// Ported from shaders.com PerlinNoise (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(noiseTone(seededPlane(evolving(noiseField('perlin'), 0.3))))): aspectScaledDomain,
// timeAxisDomain, perlin13, noiseToneKColor, the two-colour mix).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_noise.glsl"

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
    // aspectScaledDomain: aspect-corrected uv scaled by e^scale, offset by the seed
    vec2 pos = vec2(c.uv.x * c.aspect, c.uv.y) * exp(params.scale) + params.seed;
    // timeAxisDomain: the third axis walks with the clock, so the noise morphs in place
    float n = fam_perlin13(vec3(pos, c.anim.x * 0.3));
    float k = fam_tone_unit_inverted(n, params.contrast, params.balance);
    return jfx_mix_colors(params.colorA, params.colorB, k, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
