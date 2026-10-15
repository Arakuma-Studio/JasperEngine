#version 450
// @effect     halftone
// @kind       filter
// @category   stylize
// @summary    Printed halftone dots: one screen by brightness, or four CMYK ink plates
// @source     shaders.com Halftone
// @cost       medium
// @suggest-for comic print
// @param      style enum classic classic|cmyk -- One dot screen by brightness, or CMYK plates over paper
// @param      frequency number 100 10 300 -- Dots per area height
// @param      angle angle 45deg 0deg 360deg -- Screen angle of the classic style
// @param      cyan-angle angle 15deg 0deg 360deg -- Screen angle of the cyan plate
// @param      magenta-angle angle 75deg 0deg 360deg -- Screen angle of the magenta plate
// @param      yellow-angle angle 0deg 0deg 360deg -- Screen angle of the yellow plate
// @param      black-angle angle 45deg 0deg 360deg -- Screen angle of the black plate
// @param      misprint number 0 0 0.01 -- Mis-registration between the plates (colour fringing)
// @param      misprint-angle angle 0deg 0deg 360deg -- Direction the plates drift apart
// @param      paper-color color #ffffff -- Paper where no ink lands
// @param      cyan-color color #00ffff -- Cyan ink
// @param      magenta-color color #ff00ff -- Magenta ink
// @param      yellow-color color #ffff00 -- Yellow ink
// @param      black-color color #000000 -- Black (key) ink

// Ported from shaders.com Halftone (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/paint/patterns.ts dotScreen and cmykPress: patternPaints.ts halftoneClassic,
// halftonePlateUV, halftoneChannelC/M/Y/K, halftonePlateGrid, halftoneTransmission). The plates
// are laid down cyan, magenta, yellow, black, each sampled a quarter turn further round the
// misprint angle; the alpha is the unshifted content's.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"
#include "effects/lib/fam_filters_stylize.glsl"

uniform u_fs_effectParams {
    vec4 paperColor;
    vec4 cyanColor;
    vec4 magentaColor;
    vec4 yellowColor;
    vec4 blackColor;
    float style;
    float frequency;
    float angle;
    float cyanAngle;
    float magentaAngle;
    float yellowAngle;
    float blackAngle;
    float misprint;
    float misprintAngle;
} params;

// halftonePlateUV: the plate's registration offset
vec2 plate_uv(vec2 uv, float plateIndex, float aspect){
    float a = params.misprintAngle + plateIndex * JFX_PI * 0.5;
    return vec2(uv.x + cos(a) * params.misprint, uv.y + sin(a) * params.misprint / aspect);
}

vec4 jfx_filter(JfxContext c){
    vec4 child = fam_src_clamped(c.px);
    if (params.style < 0.5){
        // halftoneClassic: the dot grows with the brightness, all four channels masked
        vec2 grid = fam_halftone_grid(c.uv, c.aspect, params.angle, params.frequency);
        float brightness = dot(child.rgb, vec3(0.299, 0.587, 0.114));
        float dotSize = brightness * 0.7 + 0.15;
        float dotPattern = 1.0 - smoothstep(dotSize - 0.1, dotSize + 0.1, length(grid));
        return child * dotPattern;
    }
    vec3 printed = params.paperColor.rgb;
    float cyan = fam_halftone_cmyk(fam_src_clamped(plate_uv(c.uv, 0.0, c.aspect) * c.size).rgb).x;
    printed *= fam_halftone_transmission(params.cyanColor, fam_halftone_plate(c.uv, c.aspect, params.cyanAngle, cyan, params.frequency));
    float magenta = fam_halftone_cmyk(fam_src_clamped(plate_uv(c.uv, 1.0, c.aspect) * c.size).rgb).y;
    printed *= fam_halftone_transmission(params.magentaColor, fam_halftone_plate(c.uv, c.aspect, params.magentaAngle, magenta, params.frequency));
    float yellow = fam_halftone_cmyk(fam_src_clamped(plate_uv(c.uv, 2.0, c.aspect) * c.size).rgb).z;
    printed *= fam_halftone_transmission(params.yellowColor, fam_halftone_plate(c.uv, c.aspect, params.yellowAngle, yellow, params.frequency));
    float black = fam_halftone_cmyk(fam_src_clamped(plate_uv(c.uv, 3.0, c.aspect) * c.size).rgb).w;
    printed *= fam_halftone_transmission(params.blackColor, fam_halftone_plate(c.uv, c.aspect, params.blackAngle, black, params.frequency));
    return vec4(printed, child.a);
}

#include "effects/lib/main_filter.glsl"
