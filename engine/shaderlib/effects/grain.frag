#version 450
// @effect     grain
// @kind       paint
// @category   noise
// @summary    A film-grain overlay: per-pixel speckles that lighten and darken what is below
// @source     original (FilmGrain look, gpu/kit/colorOps.ts:267)
// @cost       low
// @suggest-for noise-overlay film-grain-overlay
// @param      amount number 0.12 0 1 -- Strength of the grain
// @param      scale number 1 0.5 4 -- Size of a grain in device pixels
// @param      animated boolean true -- The grain changes every frame
// @param      colored boolean false -- Coloured speckles instead of the colour
// @param      color color #ffffff -- Colour of the light speckles (the dark ones are black)

// Original effect after the shaders.com FilmGrain look (MIT, Copyright (c) 2026 Shader Effects
// Inc.; see effects/LICENSE.shaders-com.txt): the kit's filmGrain hash (hash12 of the device
// pixel, drifting along two incommensurate axes at 10 x the clock when animated). FilmGrain is a
// filter that adds the signed grain to the content; as a paint layer the grain is drawn over what
// is below: a positive grain is the colour at amount x grain alpha, a negative one black.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_tone.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float amount;
    float scale;
    float animated;
    float colored;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 cell = floor(c.px * c.dpr / max(params.scale, 0.5));
    // the kit's grain clock: 10 x the time while animated
    float t = c.time * 10.0;
    float grain = jfx_grain(cell, t, params.animated);
    vec3 tint = params.color.rgb;
    if (params.colored > 0.5){
        float drift = t * params.animated;
        tint *= jfx_hash32(cell + vec2(drift * 0.7548777, drift * 0.5698403));
    }
    float strength = params.amount * abs(grain) * params.color.a;
    return grain > 0.0 ? vec4(tint, strength) : vec4(0.0, 0.0, 0.0, strength);
}

#include "effects/lib/main_paint.glsl"
