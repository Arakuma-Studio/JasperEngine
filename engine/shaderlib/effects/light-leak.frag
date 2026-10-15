#version 450
// @effect     light-leak
// @kind       filter
// @category   light
// @summary    Warm overexposed film light bleeding in from a point, with streaks and slow breathing
// @source     shaders.com LightLeak
// @cost       low
// @time       speed
// @param      position position "95% 40%" -- Where the light leaks in from (the beam points at the centre)
// @param      spread number 0.55 0.1 1.5 -- Size of the pool of light
// @param      intensity number 0.15 0 1 -- Strength of the leak
// @param      streaks number 0.6 0 1 -- Strength of the bands crossing the beam
// @param      color-hot color #fff3c4 -- Colour of the hottest light
// @param      color-mid color #ff7a2f -- Colour of the middle of the light
// @param      color-fringe color #a63d8f -- Colour of the dim fringe
// @param      flicker number 0.35 0 1 -- Depth of the slow breathing
// @param      speed number 1 0 4 -- Speed of the drift and breathing
// @param      seed number 0 0 100 -- Re-rolls the drift

// Ported from shaders.com LightLeak (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (screenGlow over heatRamp.taps(dithered(heat)): beamLocalFrame, leakDrift, leakBloom,
// leakStreaks, heatDither, chromaticHeatTaps, screenGlowComposite).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_light_common.glsl"

uniform u_fs_effectParams {
    vec4 colorHot;
    vec4 colorMid;
    vec4 colorFringe;
    vec2 position;
    float spread;
    float intensity;
    float streaks;
    float flicker;
    float speed;
    float seed;
} params;

vec4 jfx_filter(JfxContext c){
    float t = c.anim.x * 0.6 + params.seed * 7.9;
    vec2 frame = fam_beam_local_frame(c.uv, c.aspect, params.position);
    vec4 drift = fam_leak_drift(t, params.seed, params.flicker);
    vec2 bloom = fam_leak_bloom(frame.x, frame.y, params.spread, drift);
    float streaks = fam_leak_streaks(frame.x, frame.y, params.spread, params.streaks, drift);
    // main x 1.25 + shoulder + streaks x 0.85, times the intensity (x 2.2) and the breathing
    float heat = (bloom.x * 1.25 + bloom.y + streaks * 0.85) * params.intensity * 2.2 * drift.x;
    vec3 leak = fam_chromatic_heat_taps(fam_heat_dither(heat, c.uv, t), params.colorHot.rgb, params.colorMid.rgb, params.colorFringe.rgb);
    return fam_screen_glow(jfx_decode(jfx_src_premul_here()), leak);
}

#include "effects/lib/main_filter.glsl"
