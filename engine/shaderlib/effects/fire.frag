#version 450
// @effect     fire
// @kind       paint
// @category   energy
// @summary    Flames rising from an edge: domain-warped fractal noise through a fire palette
// @source     original (domain-warped fbm flame; palette after GradientMap `fire`)
// @cost       medium
// @time       speed
// @suggest-for flames burn
// @param      colors palette "#00000000 0% #8a1500 30% #ff5a00 60% #ffd27a 85% #ffffff 100%" -- Palette from cold (transparent) to the hottest core
// @param      speed number 1 0 5 -- Speed of the rising flames
// @param      scale number 1 0.25 4 -- Size of the flame detail (higher is finer)
// @param      turbulence number 1 0 3 -- How much the flames twist and flicker
// @param      height number 0.8 0.1 1.5 -- How far the flames reach, as a share of the area
// @param      direction angle 0deg -180deg 180deg -- Direction the flames rise (0deg up, 90deg right)
// @param      seed number 0 0 100 -- Random seed for variation
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the palette

// Original effect. The noise is the include library's (the kit's perlin12 and fbmPerlin, MIT,
// Copyright (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt): an fbm field
// scrolling along the rise direction, displaced by a two-component fbm warp (one level of Inigo
// Quilez's domain warping), shaped by the distance from the base edge into a heat value that
// reads the palette.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float speed;
    float scale;
    float turbulence;
    float height;
    float direction;
    float seed;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x;
    // the rise direction in y-down area coordinates (0deg up, clockwise); h runs 0 at the base
    // edge to 1 at the opposite edge, x across the flames (aspect-corrected)
    vec2 up = vec2(sin(params.direction), -cos(params.direction));
    vec2 across = vec2(-up.y, up.x);
    vec2 q = fam_canvas_centred(c.uv, c.aspect);
    float extent = 0.5 * (abs(up.x) * c.aspect + abs(up.y));
    float h = (dot(q, up) + extent) / max(2.0 * extent, 1e-6);
    float x = dot(q, across);

    vec2 p = vec2(x, h) * params.scale * 2.5 + params.seed * vec2(7.13, 3.71);
    // the warp field scrolls up a little slower than the flames, so the tongues twist as they rise
    vec2 warpP = p * 1.3 - vec2(0.0, t * 1.1);
    vec2 w = vec2(jfx_fbm_perlin(warpP, 4.0), jfx_fbm_perlin(warpP + vec2(5.2, 1.3), 4.0)) - 0.5;
    float n = jfx_fbm_perlin(p * vec2(1.6, 1.0) + w * params.turbulence * 1.6 - vec2(0.0, t * 1.6), 5.0);

    // heat: hot at the base, cooling with the height, the noise cutting tongues into it
    float rise = h / max(params.height, 1e-3);
    float heat = (1.0 - rise) * 1.25 - (1.0 - n) * 1.25 + 0.3;
    heat *= smoothstep(1.0, 0.7, rise);
    return jfx_palette_colors(clamp(heat, 0.0, 1.0), params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
