#version 450
// @effect     plasma
// @kind       paint
// @category   energy
// @summary    Domain-warped noise plasma, glowing between two colours
// @source     shaders.com Plasma
// @cost       medium
// @time       speed
// @param      density number 2 0 4 -- Size of the plasma pattern (higher is finer)
// @param      speed number 2 0 5 -- Animation speed
// @param      intensity number 1.5 0.1 3 -- Glow spread (lower pulls the light into tight cores)
// @param      warp number 0.4 0 1 -- Strength of the domain warp
// @param      contrast number 1 0 3 -- Contrast of the pattern
// @param      balance number 50 0 100 -- Balance between the two colours (50 is neutral)
// @param      color-a color #7018be -- Colour of the bright plasma
// @param      color-b color #000000 -- Colour of the background
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix

// Ported from shaders.com Plasma (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(tone(scaledVolume(warped(noiseField('mx3'))))): volumeDomain, domainWarp2 over
// MaterialX noise, unitNoise3, toneGlowInverted, the two-colour mix).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_noise.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float density;
    float speed;
    float intensity;
    float warp;
    float contrast;
    float balance;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    // volumeDomain: the aspect-corrected uv on the z = 0 plane, zoomed by e^(density - 1)
    vec3 coord = vec3(c.uv.x * c.aspect, c.uv.y, 0.0) * exp(params.density - 1.0);
    // warped: the two-level warp on the clock x 0.125, amount x 4
    vec3 warped = fam_warp2(coord, c.anim.x * 0.125, params.warp * 4.0);
    float v = jfx_mx_noise3(warped) * 0.5 + 0.5;
    float t = fam_tone_glow_inverted(v, params.intensity, params.contrast, params.balance);
    return jfx_mix_colors(params.colorA, params.colorB, t, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
