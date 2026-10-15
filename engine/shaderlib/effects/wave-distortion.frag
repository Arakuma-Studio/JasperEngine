#version 450
// @effect     wave-distortion
// @kind       filter
// @category   distort
// @summary    Pushes the content sideways in travelling waves of a chosen shape
// @source     shaders.com WaveDistortion
// @cost       low
// @time       speed
// @suggest-for wave wobble
// @param      strength number 0.3 0 1 -- How far the waves push (0.3 moves 15 % of the height)
// @param      frequency number 1 0.1 10 -- Waves across the area
// @param      speed number 1 0 5 -- Speed the waves travel
// @param      angle angle 0deg 0deg 360deg -- Direction of the push (0deg along x; the waves run across it)
// @param      wave-type enum sine sine|triangle|square|sawtooth|bounce -- Shape of the wave
// @param      edges enum stretch stretch|transparent|mirror|wrap -- What shows where the waves read beyond the element

// Ported from shaders.com WaveDistortion (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/warps.ts wave: warpMaps.ts wavePhase, waveformSine/Triangle/Square/Sawtooth/Bounce and
// waveApply, the phase advancing at half the integrated clock; read through edges.ts
// sampleRemappedExpr: Catmull-Rom, bilinear in the mirror mode).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    float strength;
    float frequency;
    float speed;
    float angle;
    float waveType;
    float edges;
} params;

vec4 jfx_filter(JfxContext c){
    float cosA = cos(params.angle);
    float sinA = sin(params.angle);
    // wavePhase
    vec2 centered = c.uv - 0.5;
    float rotatedY = centered.x * c.aspect * sinA + centered.y * cosA;
    float phase = (rotatedY + 0.5) * params.frequency * JFX_TAU + c.anim.x * 0.5;
    // the waveform
    float normalizedPhase = fract(phase / JFX_TAU);
    float wave;
    if (params.waveType < 0.5)
        wave = sin(phase);
    else if (params.waveType < 1.5)
        wave = abs(normalizedPhase * 2.0 - 1.0) * 2.0 - 1.0;
    else if (params.waveType < 2.5)
        wave = step(0.5, normalizedPhase) * 2.0 - 1.0;
    else if (params.waveType < 3.5)
        wave = normalizedPhase * 2.0 - 1.0;
    else
        wave = abs(sin(phase));
    // waveApply
    float displacement = wave * params.strength * 0.5;
    vec2 uv = vec2(c.uv.x + displacement * cosA / c.aspect, c.uv.y + displacement * sinA);
    return fam_src_remapped(uv * c.size, params.edges);
}

#include "effects/lib/main_filter.glsl"
