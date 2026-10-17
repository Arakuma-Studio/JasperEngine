#version 450
// @effect     smoke
// @kind       paint
// @category   energy
// @summary    Drifting clouds of smoke or fog in two colours
// @source     original (stateless fbm smoke; look and params after shaders.com Fog)
// @cost       medium
// @time       speed
// @suggest-for fog mist
// @param      color-a color #e0e0e0 -- Primary smoke colour
// @param      color-b color #888888 -- Secondary colour, varying across the clouds
// @param      density number 0.6 0 1 -- How much of the area the smoke covers
// @param      scale number 1 0.25 4 -- Size of the clouds (higher is finer)
// @param      turbulence number 1 0 3 -- How much the clouds curl and churn
// @param      speed number 1 0.1 3 -- Drift speed
// @param      direction angle 0deg -180deg 180deg -- Direction the smoke drifts (0deg up, 90deg right)
// @param      seed number 0 0 999 -- Random seed for variation
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the two-colour mix

// Original effect after the look and the parameters of shaders.com Fog (MIT, Copyright (c) 2026
// Shader Effects Inc.; see effects/LICENSE.shaders-com.txt), whose fluid simulation is out of
// scope (phase-5 spec section 2). Fog's output is kept: the two colours mixed by a variation
// field, the alpha the fog density. Here both fields are stateless: domain-warped fbm (the
// include library's fbmPerlin) drifting along the direction and churning in place.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float density;
    float scale;
    float turbulence;
    float speed;
    float direction;
    float seed;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x * 0.12;
    // drift along the direction (0deg up, clockwise, y down)
    vec2 drift = vec2(sin(params.direction), -cos(params.direction));
    vec2 seedOffset = vec2(fract(params.seed * 0.618034) * 97.0, fract(params.seed * 0.414214) * 61.0);
    vec2 p = vec2(c.uv.x * c.aspect, c.uv.y) * params.scale * 2.0 + seedOffset - drift * t * 2.0;

    // the churn: a slowly evolving warp field
    vec2 w = vec2(jfx_fbm_perlin(p * 0.7 + vec2(t * 0.9, -t * 0.6), 4.0),
                  jfx_fbm_perlin(p * 0.7 + vec2(5.2 - t * 0.7, 1.3 + t * 0.8), 4.0)) - 0.5;
    vec2 q = p + w * params.turbulence * 1.2;
    float cloud = jfx_fbm_perlin(q, 5.0);
    float variation = jfx_fbm_perlin(q * 0.6 + vec2(17.3, 9.1) + t * 0.5, 3.0);

    // density: how much of the cloud field shows, soft-edged
    float threshold = mix(0.75, 0.25, params.density);
    float fog = smoothstep(threshold - 0.22, threshold + 0.22, cloud) * smoothstep(0.0, 0.08, params.density);
    vec4 color = jfx_mix_colors(params.colorA, params.colorB, smoothstep(0.35, 0.65, variation), params.colorSpace);
    return vec4(color.rgb, color.a * fog);
}

#include "effects/lib/main_paint.glsl"
