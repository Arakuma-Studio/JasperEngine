#version 450
// @effect     noise-dissolve
// @kind       filter
// @category   transition
// @summary    Dissolves the content away through soft organic noise blobs, with an optional burn edge
// @source     shaders.com NoiseDissolve (+ original burn edge)
// @cost       low
// @suggest-for dissolve burn
// @param      progress percentage 50% -- How far the dissolve has gone (0% all visible, 100% all gone)
// @param      scale number 3 0.5 20 -- Frequency of the pattern (higher gives smaller blobs)
// @param      softness number 0.25 0 1 -- How softly the edge fades out (0 gives hard-edged blobs)
// @param      seed number 0 0 100 -- Pattern variation
// @param      invert boolean false -- Swap the ends: the content appears as progress goes from 0% to 100%
// @param      edge-color color transparent -- Colour of the burning edge on the content next to the front (transparent: none)
// @param      edge-width number 0.05 0 0.3 -- Width of the burning edge in coverage units

// Ported from shaders.com NoiseDissolve (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (reveal over coverage.noise: fbmCoverageCoord, revealMask, applyReveal). The burn edge is the
// engine's: the content within edge-width of the front takes the edge colour, the front's travel
// widened by edge-width times the colour's alpha so the edge is gone at both ends (transparent
// edge: the source's timing exactly).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_reveal.glsl"

uniform u_fs_effectParams {
    vec4 edgeColor;
    float progress;
    float scale;
    float softness;
    float seed;
    float invert;
    float edgeWidth;
} params;

vec4 jfx_filter(JfxContext c){
    float coord = fam_fbm_coverage_coord(c.uv, c.aspect, params.scale, params.seed);
    bool inverted = params.invert > 0.5;
    float cov = inverted ? 1.0 - coord : coord;
    float p = inverted ? 1.0 - params.progress : params.progress;
    float f = max(params.softness, 0.0001);
    float ew = params.edgeWidth * clamp(params.edgeColor.a, 0.0, 1.0);
    // revealMask with the front's travel widened by the edge
    float front = p * (1.0 + 2.0 * f + ew) - f - ew;
    float mask = smoothstep(front - f, front + f, cov);
    // the burn: the content that is still there but within ew of the front
    float burn = ew > 0.0 ? 1.0 - smoothstep(front + ew - f, front + ew + f, cov) : 0.0;
    vec4 s = jfx_decode(jfx_src_premul_here());
    vec3 rgb = mix(s.rgb, params.edgeColor.rgb, clamp(burn, 0.0, 1.0) * params.edgeColor.a);
    return vec4(rgb, s.a * mask);
}

#include "effects/lib/main_filter.glsl"
