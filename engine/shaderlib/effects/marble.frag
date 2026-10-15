#version 450
// @effect     marble
// @kind       paint
// @category   pattern
// @summary    Marble veins: a turbulence-bent sine wave through a three-colour ladder
// @source     shaders.com Marble
// @cost       medium
// @time       speed
// @param      color-a color #ffffff -- Base colour
// @param      color-b color #3a2d54 -- Vein colour
// @param      color-c color #0f0f0f -- Deep vein colour
// @param      scale number 2 0.1 10 -- Size of the pattern (higher is finer)
// @param      turbulence number 10 0 50 -- How much the veins are bent
// @param      speed number 0.05 0 0.25 -- Animation speed
// @param      seed number 0 0 100 -- Random seed for variation
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix

// Ported from shaders.com Marble (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (Marble/index.ts paint: three decorrelated surfaceNoiseAt octaves, the vein wave, the depth
// noise, colorLadder3).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    float scale;
    float turbulence;
    float speed;
    float seed;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x;
    float seed = params.seed;
    vec2 pos = vec2(c.uv.x * c.aspect, c.uv.y) * params.scale;
    float x = pos.x;
    float y = pos.y;
    // turbulence: three octaves, each with its own seed multiplier and time rate
    float turbulence = jfx_mx_noise2(vec2(x + seed, y + t))
                     + jfx_mx_noise2(vec2(x * 2.0 + seed * 1.7, y * 2.0 + t * 0.7)) * 0.5
                     + jfx_mx_noise2(vec2(x * 4.0 + seed * 3.1, y * 4.0 + t * 1.3)) * 0.25;
    // the vein wave and the slow cross-axis depth of the deep veins
    float vein = sin(x * 3.141592653589793 + turbulence * params.turbulence) * 0.5 + 0.5;
    float depth = jfx_mx_noise2(vec2(y * 1.5 + seed * 2.0, x * 1.2)) * 0.3 + 0.5;
    vec4 cAB = jfx_mix_colors(params.colorA, params.colorB, smoothstep(0.3, 0.6, vein), params.colorSpace);
    return jfx_mix_colors(cAB, params.colorC, smoothstep(0.6, 0.9, vein) * depth, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
