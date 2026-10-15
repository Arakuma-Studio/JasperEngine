#version 450
// @effect     hologram
// @kind       filter
// @category   stylize
// @summary    The content as a flickering sci-fi projection: tinted glow, lit edges, scanlines and static
// @source     shaders.com Hologram (adapted: applied to the element's content instead of a shape)
// @cost       medium
// @time       speed, scanline-speed
// @suggest-for projection sci-fi
// @param      color color #5ec8ff -- Colour the projection emits
// @param      brightness number 1.5 0.2 3 -- Overall emission (bright edges blow out toward white)
// @param      fill number 0.45 0 1 -- Translucent glow of the content's inside
// @param      edge-glow number 1 0 2.5 -- Brightness of the lit edges
// @param      scanlines number 0.55 0 1 -- How dark the gaps between the projection lines get
// @param      scanline-scale number 130 20 400 -- Number of projection lines over the element's height
// @param      scanline-speed number 1 0 5 -- Speed of the lines drifting up
// @param      sweep number 0.1 0 1 -- Brightness of the refresh bar sweeping down the projection
// @param      flicker number 0.3 0 1 -- Unstable-projector shimmer and dropouts
// @param      distortion number 0.1 0 1 -- Horizontal wobble of the beam and colour fringes at the edges
// @param      grain number 0.7 0 1 -- Fine moving static over the projection
// @param      speed number 1 0 3 -- Speed of every animated layer (0 freezes the projection)

// Ported from shaders.com Hologram (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (the flat-shape path: the beam wobble on the sampling position, the edge rim and the
// Beer-Lambert fill, the CRT scanlines, the refresh sweep, the flicker, the grain, the chromatic
// fringe and the hot core). Adapted: the shape is the content's alpha. The source's optical
// thickness (the distance inside the shape) is estimated from 8 alpha taps 3 CSS px around the
// pixel: their mean is the fill's thickness, its shortfall the rim; the fringe follows the
// alpha's horizontal slope. The depth scan-lines (the source's depthLines, contour rings on a
// flat shape) are not part of this effect. The scanline clock is integrated on its own rate
// (the source scales the speed clock by it; equal at the defaults).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_noise.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float brightness;
    float fill;
    float edgeGlow;
    float scanlines;
    float scanlineScale;
    float scanlineSpeed;
    float sweep;
    float flicker;
    float distortion;
    float grain;
    float speed;
} params;

vec4 jfx_filter(JfxContext c){
    float t = c.anim.x;
    // beam instability: a horizontal wobble of the sampling position only
    float wob = sin(c.uv.y * 38.0 + t * 2.3) * 0.5 + sin(c.uv.y * 11.5 - t * 1.7) * 0.5;
    vec2 at = c.px + vec2(wob * params.distortion * 0.012 * c.size.x, 0.0);

    // the content's coverage here and its neighbourhood (the shape and its thickness)
    float a0 = jfx_src(at).a;
    const float R = 3.0;
    float ring = 0.0;
    float aR = 0.0;
    float aL = 0.0;
    for (int i = 0; i < 8; i++){
        float ang = JFX_TAU * float(i) / 8.0;
        float a = jfx_src(at + vec2(cos(ang), sin(ang)) * R).a;
        ring += a;
        if (i == 0)
            aR = a;
        if (i == 4)
            aL = a;
    }
    ring /= 8.0;
    float rim = clamp(2.0 * (1.0 - ring), 0.0, 1.0) * params.edgeGlow;
    float bodyFill = clamp(ring, 0.0, 1.0) * params.fill;

    // the projector layers (screen space)
    float scanPhase = c.uv.y * params.scanlineScale - c.anim.y * 3.0;
    float scanMask = mix(1.0, sin(scanPhase * JFX_TAU) * 0.5 + 0.5, params.scanlines);
    float sweepD = c.uv.y - fract(t * 0.16);
    float sweepGlow = exp(sweepD * sweepD * -650.0) * params.sweep;
    float shimmer = sin(t * 40.0) * 0.04 + sin(t * 23.3) * 0.03;
    float drop = smoothstep(0.6, 0.95, jfx_mx_noise2(vec2(t * 6.0, 11.0)) * 0.5 + 0.5);
    float flick = (1.0 + shimmer) * (1.0 - params.flicker * drop * 0.7);
    vec2 devicePx = c.px * c.dpr;
    float gn = jfx_hash12(vec2(devicePx.x + t * 57.3, devicePx.y));
    float grainMul = 1.0 + (gn - 0.5) * params.grain * 0.6;

    // compose: emission, the chromatic fringe along the edge, the hot core
    float glow = bodyFill + rim;
    float core = (glow * scanMask * flick * grainMul + sweepGlow * scanMask) * params.brightness;
    float gradX = aL - aR;    // about the unit outward slope across an edge (alpha falls outward)
    float ca = params.distortion * 0.12 + 0.04;
    float fringe = rim * gradX * ca * 0.1;
    vec3 rgb = params.color.rgb * vec3(core + fringe, core, core - fringe);
    float hot = clamp(rim * 0.5 + sweepGlow, 0.0, 0.8);
    vec3 hotRgb = mix(rgb, vec3(core), vec3(hot));
    float alpha = clamp(core, 0.0, 1.0) * a0;
    return vec4(hotRgb, alpha);
}

#include "effects/lib/main_filter.glsl"
