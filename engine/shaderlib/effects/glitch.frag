#version 450
// @effect     glitch
// @kind       filter
// @category   stylize
// @summary    Digital corruption bursts: displaced bands, mirrored blocks, RGB split and colour bars
// @source     shaders.com Glitch
// @cost       medium
// @time       speed
// @suggest-for datamosh corrupt
// @param      intensity number 0.5 0 1 -- Glitch strength and how often bursts happen
// @param      speed number 1 0.1 5 -- How fast the glitch pattern changes
// @param      rgb-shift number 5 0 20 -- Red and blue split in the active bands
// @param      block-density number 10 2 50 -- Number of horizontal glitch bands
// @param      color-bar-intensity number 0.2 0 1 -- Neon colour bars in the glitched blocks
// @param      mirror-amount number 0.3 0 1 -- Chance of a glitched block showing flipped content
// @param      scanline-intensity number 0.2 0 1 -- Scanlines in the distorted regions

// Ported from shaders.com Glitch (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts glitchFrame, glitchRgbSplit, colorBarFills, distortedScanlines:
// motionBlur.ts glitchGeom with burstPulse, bandJitter, blockShift and mirrorFlips,
// glitchSplitUVs, fillBarsShade, distortScanShade). The glitch clock is the integrated `speed`
// clock (the source scales the global clock by it); taps are premultiplied as the kit's.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"
#include "effects/lib/fam_filters_stylize.glsl"

uniform u_fs_effectParams {
    float intensity;
    float speed;
    float rgbShift;
    float blockDensity;
    float colorBarIntensity;
    float mirrorAmount;
    float scanlineIntensity;
} params;

vec4 jfx_filter(JfxContext c){
    FamGlitchGeom g = fam_glitch_geom(c.uv, c.anim.x, params.intensity, params.blockDensity, params.mirrorAmount);
    // glitchSplitUVs + glitchRgbSplit: the alpha from the centred green tap
    float rgbOffset = params.rgbShift * 0.003 * g.spread;
    vec4 green = fam_src_premul(g.mirroredUV * c.size);
    float red = fam_src_premul(vec2(clamp(g.mirroredUV.x + rgbOffset, 0.0, 1.0), g.mirroredUV.y) * c.size).r;
    float blue = fam_src_premul(vec2(clamp(g.mirroredUV.x - rgbOffset, 0.0, 1.0), g.mirroredUV.y) * c.size).b;
    vec4 color = vec4(red, green.g, blue, green.a);
    color = fam_glitch_fill_bars(color, c.uv, g, params.colorBarIntensity);
    // distortScanShade: device-pixel scanlines where a band is distorting
    float scanlineWave = sin(c.uv.y * c.size.y * c.dpr * 3.14159265) * 0.5 + 0.5;
    color.rgb *= 1.0 - params.scanlineIntensity * scanlineWave * 0.3 * g.distortion;
    return jfx_unpremultiply(vec4(min(color.rgb, vec3(color.a)), color.a));
}

#include "effects/lib/main_filter.glsl"
