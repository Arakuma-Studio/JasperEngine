#version 450
// @effect     swirl
// @kind       paint
// @category   gradient
// @summary    Two colours swirling through layered flowing waves, with a soft shimmer
// @source     shaders.com Swirl (adapted: source `blend` renamed `balance`)
// @cost       medium
// @time       speed
// @param      color-a color #1275d8 -- First colour
// @param      color-b color #e19136 -- Second colour
// @param      speed number 1 0 5 -- Animation speed
// @param      detail number 1 0 5 -- Frequency of the swirl layers
// @param      balance number 50 0 100 -- Balance between the two colours (50 is neutral; source: blend)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the mix

// Ported from shaders.com Swirl (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shimmered(rampOver(threshold(flowField)), pulse(flowField)): flowLayers, unitThreshold,
// shimmerPulse). The flow pattern reads the raw uv (no aspect correction), as in the source.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float speed;
    float detail;
    float balance;
    float colorSpace;
} params;

// gradientPaints.ts flowLayers: three nested sin/cos layers, each warping the previous; signed
float swirl_flow_layers(vec2 uv, float detail, float t){
    float freq1 = detail;
    vec2 d1 = vec2(uv.x + (sin(uv.y * (freq1 * 1.7) + t * 0.8) * 0.12 + cos(uv.x * (freq1 * 0.9) - t * 0.5) * 0.05),
                   uv.y + (cos(uv.x * (freq1 * 1.3) - t * 0.6) * 0.12 + sin(uv.y * (freq1 * 1.1) + t * 0.7) * 0.05));
    float pattern1 = sin(d1.x * (freq1 * 2.1) + d1.y * (freq1 * 1.8) + t * 0.4);

    float freq2 = detail * 2.1;
    vec2 d2 = vec2(d1.x + (cos(d1.y * (freq2 * 2.7) - t * 0.45) * 0.07 + sin(d1.x * (freq2 * 1.9) + t * 0.6) * 0.04),
                   d1.y + (sin(d1.x * (freq2 * 2.3) + t * 0.65) * 0.07 + cos(d1.y * (freq2 * 1.6) - t * 0.4) * 0.04));
    float pattern2 = cos(d2.x * (freq2 * 1.4) - d2.y * (freq2 * 1.9) + t * 0.35);

    float freq3 = detail * 3.7;
    vec2 d3 = vec2(d2.x + (sin(d2.y * (freq3 * 1.8) + t * 0.85) * 0.04 + cos(d2.x * (freq3 * 1.3) - t * 0.55) * 0.025 +
                           sin((d2.x + d2.y) * (freq3 * 0.7) + t * 0.9) * 0.02),
                   d2.y + (cos(d2.x * (freq3 * 1.6) - t * 0.75) * 0.04 + sin(d2.y * (freq3 * 1.1) + t * 0.5) * 0.025 +
                           cos((d2.x + d2.y) * (freq3 * 0.8) - t * 0.95) * 0.02));
    float pattern3 = sin(d3.x * (freq3 * 1.1) + d3.y * (freq3 * 1.5) - t * 0.55);

    return pattern1 * 0.45 + pattern2 * 0.35 + pattern3 * 0.2;
}

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x;
    float pattern = swirl_flow_layers(c.uv, params.detail, t);
    // unitThreshold: the signed pattern to 0..1, the balance bias (+-0.3), a soft threshold
    float mask = smoothstep(0.3, 0.7, pattern * 0.5 + 0.5 + (params.balance - 50.0) * 0.006);
    vec4 color = jfx_mix_colors(params.colorA, params.colorB, mask, params.colorSpace);
    // shimmerPulse: a subtle brightness pulse, on the whole colour as the source multiplies it
    float gain = sin(t * 2.5 + pattern * 8.0) * 0.015 + 1.0;
    return color * gain;
}

#include "effects/lib/main_paint.glsl"
