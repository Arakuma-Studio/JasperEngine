#version 450
// @effect     crt
// @kind       filter
// @category   stylize
// @summary    Retro CRT monitor: colour fringes, scanlines, phosphor stripes and a vignette
// @source     shaders.com CRTScreen
// @cost       medium
// @suggest-for crt-screen retro monitor
// @param      pixel-size number 128 8 128 -- Density of the phosphor stripes (about half this many across)
// @param      color-shift number 1 0 10 -- Colour fringing (1 shifts red and blue 0.2 % of the width)
// @param      scanline-intensity number 0.3 0 1 -- Darkness of the scanlines
// @param      scanline-frequency number 200 100 800 -- Number of scanlines down the area
// @param      brightness number 1 0.5 2 -- Screen brightness
// @param      contrast number 1 0.5 2 -- Screen contrast around mid grey
// @param      vignette-intensity number 1 0 1 -- Darkening of the corners (0 off)
// @param      vignette-radius number 0.5 0 1 -- How far the vignette reaches inward

// Ported from shaders.com CRTScreen (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts gatherStack(rgbSplit, [adjust, scanlines, phosphorMask, vignetteOverlay,
// opaque]): motionBlur.ts crtSampleUV, rgbSplitCombine, adjustShade, scanlineShade,
// phosphorShade, vignetteShade). The taps are premultiplied as the kit's RTT, and the screen is
// opaque: where the content is transparent it is black.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    float pixelSize;
    float colorShift;
    float scanlineIntensity;
    float scanlineFrequency;
    float brightness;
    float contrast;
    float vignetteIntensity;
    float vignetteRadius;
} params;

vec4 jfx_filter(JfxContext c){
    // rgbSplit: red from +shift, green here, blue from -shift
    float shift = params.colorShift * 0.002;
    float r = fam_src_premul(vec2(c.uv.x + shift, c.uv.y) * c.size).r;
    float g = fam_src_premul(c.px).g;
    float b = fam_src_premul(vec2(c.uv.x - shift, c.uv.y) * c.size).b;
    vec3 color = vec3(r, g, b);
    // adjustShade
    color = ((color - 0.5) * params.contrast + 0.5) * params.brightness;
    // scanlineShade
    float scanlineWave = sin(c.uv.y * params.scanlineFrequency * (3.14159 * 2.0));
    color *= 1.0 - params.scanlineIntensity * (scanlineWave * 0.5 + 0.5);
    // phosphorShade
    float phase = fract(c.uv.x * params.pixelSize * 0.5);
    color *= vec3(sin(phase * 6.28318) * 0.1 + 0.95, sin(phase * 6.28318 + 2.09) * 0.1 + 0.95, sin(phase * 6.28318 + 4.18) * 0.1 + 0.95);
    // vignetteShade
    vec2 centered = c.uv - 0.5;
    float vignetteDistance = length(vec2(centered.x * c.aspect, centered.y));
    float innerEdge = 1.0 - params.vignetteRadius * 0.9;
    float vignetteValue = 1.0 - smoothstep(innerEdge, innerEdge + 0.5, vignetteDistance);
    color *= mix(1.0, vignetteValue, params.vignetteIntensity);
    // opaque
    return vec4(color, 1.0);
}

#include "effects/lib/main_filter.glsl"
