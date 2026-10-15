#version 450
// @effect     curl-noise
// @kind       paint
// @category   noise
// @summary    The speed of a swirling curl-noise flow, morphing over time
// @source     shaders.com CurlNoise
// @cost       medium
// @time       speed
// @param      color-a color #ffffff -- Colour of the slow flow
// @param      color-b color #000000 -- Colour of the fast flow
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix
// @param      scale number 2 -2 5 -- Pattern frequency (each +1 roughly doubles it)
// @param      contrast number 0 -1 5 -- Pattern contrast (0 unchanged)
// @param      balance number 0 -1 1 -- Shifts the balance between the two colours
// @param      seed number 0 0 100 -- Random seed (slides the pattern)
// @param      speed number 1 0 5 -- Animation speed

// Ported from shaders.com CurlNoise (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(noiseTone(seededPlane(curlSpeed(0.2))))): aspectScaledDomain, curlMagnitude over
// curl22z of perlin13, noiseToneKColor, the two-colour mix).

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
    vec2 pos = vec2(c.uv.x * c.aspect, c.uv.y) * exp(params.scale) + params.seed;
    // curlMagnitude: the flow speed, sqrt(2)-normalised, evolving at 0.2 x the clock
    float speed = length(fam_curl22z(pos, c.anim.x * 0.2)) / 1.414;
    float k = fam_tone_unit_inverted(speed, params.contrast, params.balance);
    return jfx_mix_colors(params.colorA, params.colorB, k, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
