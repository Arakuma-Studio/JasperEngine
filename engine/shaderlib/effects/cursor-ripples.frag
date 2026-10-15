#version 450
// @effect     cursor-ripples
// @kind       filter
// @category   interactive
// @summary    Water ripples spreading from the pointer through the content
// @source     shaders.com CursorRipples (adapted: analytic ripple events replace the wave grid, CursorRipples/index.ts:12-26)
// @cost       medium
// @pointer    ripples
// @param      intensity number 10 0 20 -- Strength of the ripples' distortion
// @param      decay number 3 0.5 20 -- How fast a ripple fades (per second)
// @param      wavelength length 28px 4px 200px -- Distance between the crests of a ripple
// @param      wave-speed number 300 50 2000 -- Speed of a ripple spreading out, in CSS px per second
// @param      chromatic-split number 1 0 3 -- Colour separation along the ripples
// @param      trigger enum move move|press -- Ripples where the pointer moves, or only where it presses
// @param      edges enum stretch stretch|transparent|mirror|wrap -- What shows where the distortion reads beyond the element

// Ported from shaders.com CursorRipples (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (displaceBy: the displacement field's gradient through chromaticDisplaceUVs, the three
// channels read with the edge mode). Adapted: the 128x128 damped wave grid stirred by the
// pointer is replaced by the engine's ripple events (a press, and with trigger move every 40 px
// of movement): each is a ring wave packet of one wavelength travelling out at wave-speed and
// fading as exp(-decay x age), whose radial slope is the displacement (a quarter wavelength at
// strength 1 and intensity 10).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    float intensity;
    float decay;
    float wavelength;
    float waveSpeed;
    float chromaticSplit;
    float trigger;
    float edges;
} params;

uniform u_fs_effectRipples {
    vec4 events[8];    // xy local px, z age s, w strength
} ripples;

vec4 jfx_filter(JfxContext c){
    float wavelength = max(params.wavelength, 1.0);
    float k = JFX_TAU / wavelength;
    vec2 dispPx = vec2(0.0);
    for (int i = 0; i < 8; i++){
        vec4 e = ripples.events[i];
        if (e.w <= 0.0)
            continue;
        vec2 d = c.px - e.xy;
        float r = length(d);
        float x = (r - params.waveSpeed * e.z) / wavelength;   // wavelengths from the front
        float envelope = e.w * exp(-params.decay * e.z) * exp(-x * x * 2.0);
        // the slope of the packet sin(k (r - front)) x its envelope, along the radius
        float slope = cos(x * JFX_TAU) * envelope;
        dispPx += (r > 1e-3 ? d / r : vec2(0.0)) * slope * wavelength * 0.25;
    }
    if (dispPx == vec2(0.0))
        return jfx_decode(jfx_src_premul_here());
    // chromaticDisplaceUVs: the displacement in uv, times strength x 0.1, clamped to 0.15
    vec2 disp = clamp(dispPx * frame.size.zw * (params.intensity * 0.1), vec2(-0.15), vec2(0.15));
    float chromaticScale = params.chromaticSplit * 0.1;
    vec2 uv = c.uv;
    vec4 r = jfx_src_edge((uv - disp * (1.0 + chromaticScale)) * c.size, params.edges);
    vec4 g = jfx_src_edge((uv - disp) * c.size, params.edges);
    vec4 b = jfx_src_edge((uv - disp * (1.0 - chromaticScale)) * c.size, params.edges);
    return vec4(r.r, g.g, b.b, g.a);
}

#include "effects/lib/main_filter.glsl"
