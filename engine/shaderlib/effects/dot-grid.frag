#version 450
// @effect     dot-grid
// @kind       paint
// @category   pattern
// @summary    Round dots on a square grid, with optional row stagger, drift and twinkle
// @source     shaders.com DotGrid
// @cost       low
// @time       speed
// @suggest-for dots polka
// @param      color color #ffffff -- Colour of the dots
// @param      density number 30 1 200 -- Dots along the area's height
// @param      dot-size number 0.3 0 1 -- Dot diameter as a share of a cell (1 touches the neighbours)
// @param      offset number 0 0 1 -- Shift of every other row (0.5 is the polka stagger)
// @param      speed number 0 -3 3 -- Drift speed of the rows (cells per second)
// @param      speed-variance number 0.3 0 1 -- How much the rows' drift speeds differ
// @param      twinkle number 0 0 1 -- Pulses each dot on its own phase (runs on the clock, also at speed 0)

// Ported from shaders.com DotGrid (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (withAlpha(color, dotLattice): dotGridAlpha with aspectCorrectedUVFlipY, the row stagger and
// drift, discCoverage, the per-dot twinkle on the global clock). The coverage also multiplies
// the colour's alpha (the source replaces it), so a translucent colour stays translucent.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_geom.glsl"
#include "effects/lib/fam_paint_pattern.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float density;
    float dotSize;
    float offset;
    float speed;
    float speedVariance;
    float twinkle;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 correctedUV = vec2(c.uv.x * c.aspect, 1.0 - c.uv.y);
    vec2 gridUV = correctedUV * params.density;
    // per-row shift: the static stagger plus the row's drift (floored mod: rows can be negative)
    float rowIndex = floor(gridUV.y);
    float drift = c.anim.x * fam_row_speed(rowIndex, params.speedVariance);
    float staticStagger = params.offset * jfx_fmod(rowIndex, 2.0);
    vec2 shifted = vec2(gridUV.x - staticStagger - drift, gridUV.y);
    vec2 cell = floor(shifted);
    vec2 local = fract(shifted);
    float centerDistance = length(local - 0.5);
    // circular dots, anti-aliased over the pixel footprint in grid space
    float pixelSize = length(fwidth(correctedUV * params.density));
    float coverage = fam_disc_coverage(centerDistance, params.dotSize * 0.5, pixelSize);
    // twinkle: a per-dot phase on the clock
    float dotPhase = fract(sin(cell.x * 12.9898 + cell.y * 78.233) * 43758.5453);
    float twinkleValue = sin(c.time * 2.0 + dotPhase * JFX_TAU) * 0.5 + 0.5;
    float twinkleModifier = mix(1.0, twinkleValue, params.twinkle);
    return vec4(params.color.rgb, params.color.a * coverage * twinkleModifier);
}

#include "effects/lib/main_paint.glsl"
