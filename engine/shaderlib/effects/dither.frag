#version 450
// @effect     dither
// @kind       filter
// @category   stylize
// @summary    Ordered or noise dithering of the content into two inks on a pixel grid
// @source     shaders.com Dither (adapted: Floyd–Steinberg omitted, it needs sequential error diffusion)
// @cost       low
// @suggest-for bayer 1-bit
// @param      pattern enum bayer4 bayer4|bayer2|bayer8|clustered-dot|blue-noise|white-noise -- Dither pattern
// @param      pixel-size length 4px 1px 20px -- Size of a dither cell
// @param      threshold number 0.5 0 1 -- Brightness cut point (0.5 neutral)
// @param      spread number 1 0 1 -- How much of the brightness range dithers (lower leaves more solid areas)
// @param      color-mode enum custom custom|source -- Two inks of your own, or darkened and brightened content
// @param      color-a color transparent -- Dark ink (custom mode)
// @param      color-b color #ffffff -- Light ink (custom mode)

// Ported from shaders.com Dither (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/paint/patterns.ts pixelGrid, quantise, ditherInks: patternPaints.ts ditherCoord,
// ditherPixUV, ditherPeriodic, ditherBlueNoise, ditherWhiteNoise, ditherLuma, ditherOrderedResult,
// ditherComposeCustom, ditherComposeSource). The grid is in CSS px from the area's top-left
// corner (the kit's logical viewport). Adapted: no Floyd–Steinberg pattern.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/fam_filters_sample.glsl"
#include "effects/lib/fam_filters_stylize.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float pattern;
    float pixelSize;
    float threshold;
    float spread;
    float colorMode;
} params;

vec4 jfx_filter(JfxContext c){
    float pixelSize = max(params.pixelSize, 1.0);
    vec2 coord = floor(c.px / pixelSize);
    // the content sampled once per cell (ditherPixUV)
    vec4 source = fam_src_clamped((coord + 0.5) * pixelSize);
    // the threshold field: the options are bayer4, bayer2, bayer8, clustered-dot, blue-noise, white-noise
    float field;
    if (params.pattern > 4.5){
        field = jfx_hash12(coord);
    }else if (params.pattern > 3.5){
        field = fam_dither_blue_noise(coord);
    }else{
        vec4 periodic = fam_dither_periodic(coord);
        field = params.pattern < 0.5 ? periodic.y : (params.pattern < 1.5 ? periodic.x : (params.pattern < 2.5 ? periodic.z : periodic.w));
    }
    // ditherLuma: Rec. 601 x alpha
    float luminance = dot(source.rgb, vec3(0.299, 0.587, 0.114)) * source.a;
    float level = fam_dither_ordered_result(field, luminance, params.threshold, params.spread);
    if (params.colorMode < 0.5)
        return mix(params.colorA, params.colorB, level);
    vec3 dark = source.rgb * 0.3;
    vec3 bright = min(source.rgb * 1.3, vec3(1.0));
    return vec4(mix(dark, bright, vec3(level)), source.a);
}

#include "effects/lib/main_filter.glsl"
