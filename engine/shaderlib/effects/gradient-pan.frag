#version 450
// @effect     gradient-pan
// @kind       paint
// @category   gradient
// @summary    A linear gradient whose colours scroll along its axis
// @source     shaders.com LinearGradient (adapted: speed scrolls the stops)
// @cost       low
// @time       speed
// @suggest-for animated-gradient linear-gradient
// @param      colors palette "#1aff00 #0000ff" -- Colours of the gradient from start to end
// @param      angle angle 0deg 0deg 360deg -- Direction of the gradient (0deg runs left to right, 90deg top to bottom)
// @param      speed number 0.2 -5 5 -- Scroll speed in gradient lengths per second
// @param      edges enum mirror mirror|stretch|wrap -- What the colours do past the ends: bounce back, hold the end colours, or repeat
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the gradient

// Ported from shaders.com LinearGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(dist.linear, stops, edges): gradientRawT with the source's default start (0%, 50%)
// and end (100%, 50%), gradientEdgeMirror/Wrap/Clamp01). Adapted: the integrated speed clock
// scrolls the gradient parameter, so the stops pan along the axis.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float angle;
    float speed;
    float edges;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

// gradientRawT from start (0, 0.5) to end (1, 0.5): rotate the uv by -angle about the midpoint
// in aspect-corrected space, then project on the start -> end axis
float pan_raw_t(vec2 uv, float aspect, float angle){
    const vec2 startPos = vec2(0.0, 0.5);
    const vec2 endPos = vec2(1.0, 0.5);
    vec2 gradientVector = endPos - startPos;
    float gradientLength = length(gradientVector);
    vec2 gradientDir = normalize(gradientVector);
    float angleRad = angle * -1.0;
    vec2 midpoint = (startPos + endPos) * 0.5;
    vec2 centeredUV = uv - midpoint;
    vec2 rotated = fam_rotate2(vec2(centeredUV.x * aspect, centeredUV.y), cos(angleRad), sin(angleRad));
    vec2 rotatedUV = vec2(rotated.x / aspect, rotated.y) + midpoint;
    return dot(rotatedUV - startPos, gradientDir) / max(gradientLength, 1e-6);
}

vec4 jfx_paint(JfxContext c){
    float t = pan_raw_t(c.uv, c.aspect, params.angle) - c.anim.x;
    float finalT;
    if (params.edges < 0.5)
        finalT = fam_edge_mirror(t);
    else if (params.edges < 1.5)
        finalT = clamp(t, 0.0, 1.0);
    else
        finalT = fract(t);
    return jfx_palette_colors(clamp(finalT, 0.0, 1.0), params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
