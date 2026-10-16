#version 450
// @effect     vhs
// @kind       filter
// @category   stylize
// @summary    Worn VHS tape: wobbling lines, head-switch noise and smeared colour
// @source     shaders.com VHS
// @cost       medium
// @time       speed
// @suggest-for tape analog
// @param      wobble number 1 0 5 -- Tape damage: waves, creases and head-switching noise
// @param      scanline-noise number 0.6 0 1 -- Fine jitter of each scanline
// @param      smear number 0.2 -2 2 -- How far the colour trails (right when positive)
// @param      speed number 1 0.1 3 -- Speed of the tape damage

// Ported from shaders.com VHS (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts tapeWarp, chromaSmearTaps and beatPulse: motionBlur.ts vhsSampleUVs with
// burstGate, rowJitter, tapeWave, creaseShift and headSwitch, vhsChromaTapUV, yiqRecombine,
// vhsAcBeat and beatShade). The tape clock is the integrated `speed` clock (the source scales
// the global clock by it); taps are premultiplied and clamped to the area as the kit's.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_geom.glsl"
#include "effects/lib/fam_filters_sample.glsl"
#include "effects/lib/fam_filters_stylize.glsl"

uniform u_fs_effectParams {
    float wobble;
    float scanlineNoise;
    float smear;
    float speed;
} params;

vec4 jfx_filter(JfxContext c){
    float t = c.anim.x;
    vec4 uvs = fam_vhs_sample_uvs(c.uv, t, params.wobble, params.scanlineNoise);
    vec4 luma = fam_src_premul(uvs.xy * c.size);
    // five chroma taps trailing by smear x 0.0075 each (the i = 0 tap weighs 0)
    float smearScale = params.smear * 0.0075;
    vec3 c1 = fam_src_premul(vec2(uvs.z - 1.0 * smearScale, uvs.w) * c.size).rgb;
    vec3 c2 = fam_src_premul(vec2(uvs.z - 2.0 * smearScale, uvs.w) * c.size).rgb;
    vec3 c3 = fam_src_premul(vec2(uvs.z - 3.0 * smearScale, uvs.w) * c.size).rgb;
    vec3 c4 = fam_src_premul(vec2(uvs.z - 4.0 * smearScale, uvs.w) * c.size).rgb;
    vec3 c5 = fam_src_premul(vec2(uvs.z - 5.0 * smearScale, uvs.w) * c.size).rgb;
    vec3 rgb = fam_vhs_yiq_recombine(luma.rgb, c1, c2, c3, c4, c5);
    // beatShade
    rgb = clamp(rgb * fam_vhs_ac_beat(c.uv, t, params.wobble), vec3(0.0), vec3(1.0));
    return jfx_unpremultiply(vec4(min(rgb, vec3(luma.a)), luma.a));
}

#include "effects/lib/main_filter.glsl"
