#version 450
// @effect     film-grain
// @kind       filter
// @category   stylize
// @summary    Photographic grain over the content, heavier in the shadows
// @source     shaders.com FilmGrain
// @cost       low
// @suggest-for grain noise
// @param      strength number 0.5 0 1 -- Amount of grain
// @param      bias number 2 0 10 -- How much the grain gathers in the dark areas (0 even)
// @param      animated boolean false -- New grain every frame

// Ported from shaders.com FilmGrain (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/color.ts filmGrain: colorOps.ts filmGrain, one bit-cast hash per device pixel,
// weighted by (1 - Rec. 709 luminance) ^ bias, strength x 0.1). The hash reads the device pixel's
// centre; the source's CPU drift (10 per second while animated) is 10 x the clock.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_hash.glsl"

uniform u_fs_effectParams {
    float strength;
    float bias;
    float animated;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 color = jfx_decode(jfx_src_premul_here());
    float animTime = params.animated > 0.5 ? c.time * 10.0 : 0.0;
    vec2 pixelCoord = floor(c.px * c.dpr) + 0.5;
    float grain = jfx_hash12(pixelCoord + vec2(animTime, animTime * 1.6180339)) * 2.0 - 1.0;
    float brightness = clamp(jfx_luma709(color.rgb), 0.0, 1.0);
    float darkFactor = pow(1.0 - brightness + 1e-6, params.bias);
    return vec4(color.rgb + grain * darkFactor * (params.strength * 0.1), color.a);
}

#include "effects/lib/main_filter.glsl"
