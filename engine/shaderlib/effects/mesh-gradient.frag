#version 450
// @effect     mesh-gradient
// @kind       paint
// @category   gradient
// @summary    Flowing mesh gradient of soft drifting colour swaths whose seams wrap through the palette
// @source     shaders.com MeshGradient
// @cost       medium
// @time       speed
// @suggest-for mesh gradient-mesh
// @param      colors palette "#1a0533 0% #6d2fd1 26% #e04b9e 52% #ff8c42 76% #ffdf8e 100%" -- Palette the colour points take their colours from
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the palette
// @param      points integer 5 2 8 -- Number of colour points scattered across the area
// @param      smoothness number 2 0 5 -- How smoothly the colour points blend into each other
// @param      variation number 0.35 0 1 -- Varies the edge softness across the area: some boundaries crisp, others diffuse
// @param      swirl number 0.3 -1 1 -- Vortex rotation around the centre (negative spins the other way)
// @param      drift number 0.5 0 1 -- How far the colour points wander from their home positions
// @param      wrapping number 0 0 1 -- Wraps the palette back through itself where colours meet
// @param      speed number 1 0 10 -- Animation speed
// @param      seed number 0 0 100 -- Random seed for the point layout and palette assignment

// Ported from shaders.com MeshGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (MeshGradient/index.ts paint: the vortex swirl, warpedPoint (two levels), the softness
// variation, scatterField, wrapRamp, standardPalette, dithered). The kit computes the 8 anchors
// once per frame on the CPU (scatteredAnchors, a mirror of spiralScatter); here each fragment
// computes them, the same math.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_noise.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float colorSpace;
    float points;
    float smoothness;
    float variation;
    float swirl;
    float drift;
    float wrapping;
    float speed;
    float seed;
} params;

JFX_DECLARE_PALETTE(colors)

// the MeshGradient look data
#define DOMAIN_SCALE 1.6
#define WARP_AMOUNT 0.48
#define WARP_RATE 0.125
#define SWIRL_MAX 3.0
#define VAR_FREQ 1.9
#define VAR_STRENGTH 1.4
#define VAR_RATE 0.045
#define WRAP_CYCLES 9.0

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x;
    float seed = params.seed;
    float count = floor(params.points + 0.5);

    // vortex swirl about the centre: the rotation grows with the radius
    vec2 centred = fam_canvas_centred(c.uv, c.aspect);
    float swirlAngle = -SWIRL_MAX * params.swirl * smoothstep(0.0, 1.0, length(centred));
    vec2 swirled = fam_rotate2(centred, cos(swirlAngle), sin(swirlAngle));
    vec2 uvS = vec2(swirled.x + c.aspect * 0.5, swirled.y + 0.5);

    // organic domain warp (two levels), divided back to uv units
    vec3 warped = fam_warp2(vec3(uvS.x * DOMAIN_SCALE, uvS.y * DOMAIN_SCALE, seed * 7.31), t * WARP_RATE, WARP_AMOUNT);
    vec2 q = vec2(warped.x / DOMAIN_SCALE, warped.y / DOMAIN_SCALE);

    // blend softness: inverted smoothness, scaled log-symmetrically by a slow noise
    float basePower = 8.0 / (params.smoothness + 0.5);
    float softness = clamp(basePower, 0.6, 16.0);
    if (params.variation != 0.0){
        float varNoise = jfx_mx_noise3(vec3(q.x * VAR_FREQ, q.y * VAR_FREQ, seed * 3.17 + t * VAR_RATE));
        softness = clamp(basePower * exp(varNoise * params.variation * VAR_STRENGTH), 0.6, 16.0);
    }

    float field = fam_scatter_field(q, count, seed, params.drift, c.aspect, t, softness);
    float wrapped = fam_ramp_wrap(field, 1.0 + params.wrapping * WRAP_CYCLES);
    return fam_rgb_dither(jfx_palette_colors(wrapped, params.colorSpace), c.px * c.dpr, t);
}

#include "effects/lib/main_paint.glsl"
