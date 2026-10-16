#version 450
// @effect     gradient-map
// @kind       filter
// @category   color
// @summary    Recolours the content by its brightness through a gradient, optionally scrolling
// @source     shaders.com GradientMap
// @cost       low
// @time       speed
// @suggest-for color-grade lut
// @param      preset enum rainbow rainbow|sunset|ocean|fire|neon|pastel|custom -- Built-in gradient, or custom colours (source `palette`)
// @param      color-low color #1a0b2e -- Shadows colour (custom)
// @param      color-mid color #e94560 -- Midtones colour (custom)
// @param      color-high color #f9ed69 -- Highlights colour (custom)
// @param      speed number 0.15 -2 2 -- Scroll speed of the gradient (cycles per second; 0 still)
// @param      contrast number 1 0 3 -- Steepness of the mapping
// @param      black-point number 0 0 1 -- Brightness mapped to the gradient's start
// @param      white-point number 1 0 1 -- Brightness mapped to the gradient's end
// @param      strength number 1 0 1 -- Mix of the mapped colour over the original
// @param      color-space enum oklch linear|oklch|oklab|hsl|hsv|lch -- Colour space the custom colours mix in

// Ported from shaders.com GradientMap (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/color.ts gradientMap: colorOps.ts gradientMapT, gradientMapCosine with the six
// built-in cosine palettes, gradientMapCustomPhase/Select for the cyclic low -> mid -> high -> low
// ramp mixed in the colour space, gradientMapCompose). The phase is the integrated `speed` clock
// (the source multiplies the global clock by it). The options list neon before pastel (the kit's
// palette indices are the other way round); each name keeps its palette.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    vec4 colorLow;
    vec4 colorMid;
    vec4 colorHigh;
    float preset;
    float speed;
    float contrast;
    float blackPoint;
    float whitePoint;
    float strength;
    float colorSpace;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 s = jfx_decode(jfx_src_premul_here());
    // gradientMapT: Rec. 709 luminance through the black and white points and the contrast
    float range = max(params.whitePoint - params.blackPoint, 0.0001);
    float t = clamp((jfx_luma709(s.rgb) - params.blackPoint) / range, 0.0, 1.0);
    t = clamp((t - 0.5) * params.contrast + 0.5, 0.0, 1.0);
    float phase = c.anim.x;
    vec3 mapped;
    if (params.preset < 5.5){
        // the cosine palettes (a + b cos(2 pi (c t + d + phase))): rainbow, sunset, ocean, fire, neon, pastel
        vec3 pa = vec3(0.5);
        vec3 pb = vec3(0.5);
        vec3 pc = vec3(1.0);
        vec3 pd = vec3(0.0, 0.333, 0.667);
        if (params.preset > 0.5 && params.preset < 1.5)
            pd = vec3(0.0, 0.1, 0.2);
        else if (params.preset > 1.5 && params.preset < 2.5)
            pd = vec3(0.3, 0.2, 0.2);
        else if (params.preset > 2.5 && params.preset < 3.5)
            pd = vec3(0.8, 0.9, 0.3);
        else if (params.preset > 3.5 && params.preset < 4.5){
            pc = vec3(2.0, 1.0, 1.0);
            pd = vec3(0.5, 0.2, 0.25);
        }else if (params.preset > 4.5){
            pb = vec3(0.25);
            pd = vec3(0.0, 0.25, 0.5);
        }
        mapped = clamp(pa + pb * cos((pc * t + pd + vec3(phase)) * JFX_TAU), vec3(0.0), vec3(1.0));
    }else{
        // gradientMapCustomPhase + Select: the cyclic three-stop ramp
        float t3 = fract(t + phase) * 3.0;
        float fr = fract(t3);
        float i = floor(t3);
        vec4 a = i < 1.0 ? params.colorLow : (i < 2.0 ? params.colorMid : params.colorHigh);
        vec4 b = i < 1.0 ? params.colorMid : (i < 2.0 ? params.colorHigh : params.colorLow);
        mapped = jfx_mix_colors(a, b, fr, params.colorSpace).rgb;
    }
    return vec4(mix(s.rgb, mapped, params.strength), s.a);
}

#include "effects/lib/main_filter.glsl"
