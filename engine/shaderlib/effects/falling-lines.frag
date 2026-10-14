#version 450
// @effect     falling-lines
// @kind       paint
// @category   pattern
// @summary    Streaks falling in columns at their own speeds, fading from a lead to a trail colour
// @source     shaders.com FallingLines
// @cost       low
// @time       speed
// @suggest-for rain matrix
// @param      color-a color #ffffff -- Colour at the leading edge
// @param      color-b color #ffffff00 -- Colour at the end of the trail
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the fade
// @param      angle angle 90deg 0deg 360deg -- Direction of the fall (90deg falls down, 0deg moves right)
// @param      speed number 0.5 0 3 -- Fall speed
// @param      speed-variance number 0.3 0 1 -- How much the columns' speeds differ
// @param      density number 15 1 60 -- Number of columns
// @param      trail-length number 0.35 0.01 1 -- Streak length as a share of the spacing
// @param      balance number 0.5 0 1 -- Midpoint of the lead-to-trail fade
// @param      stroke-width number 0.15 0.02 1 -- Streak width as a share of a column
// @param      rounding number 1 0 1 -- Roundness of the leading cap

// Ported from shaders.com FallingLines (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (withAlpha(strokeOver(colorB, colorA, fade), alpha x mask): fallingLinesField, colHash,
// fallingCapAlpha, fallingBalanceRemap).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_pattern.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float colorSpace;
    float angle;
    float speed;
    float speedVariance;
    float density;
    float trailLength;
    float balance;
    float strokeWidth;
    float rounding;
} params;

vec4 jfx_paint(JfxContext c){
    // aspect-corrected and centred, so the angle pivots on the centre; u across, v along the fall
    vec2 centeredUV = vec2(c.uv.x * c.aspect, c.uv.y) - vec2(c.aspect * 0.5, 0.5);
    float cosA = cos(params.angle);
    float sinA = sin(params.angle);
    float u = centeredUV.y * cosA - centeredUV.x * sinA;
    float v = centeredUV.x * cosA + centeredUV.y * sinA;

    // columns, each with its own speed and phase (colHash)
    float density = params.density;
    float colF = u * density;
    float colIdx = floor(colF);
    float colLocalU = fract(colF);
    vec2 h = vec2(fract(sin(colIdx * 127.1) * 43758.5453123), fract(sin(colIdx * 311.7) * 38291.3421));
    float speedMod = 1.0 + (h.x - 0.5) * params.speedVariance * 2.0;
    float animOffset = fract(c.anim.x * speedMod);
    float vPeriodic = fract(v - animOffset - h.y);

    float trailLen = params.trailLength;
    float withinStreak = clamp(vPeriodic / trailLen, 0.0, 1.0);
    float fwV = clamp(fwidth(v), 0.001, 0.1);
    float fwU = clamp(fwidth(u) * density, 0.001, 0.1);
    float streakMask = 1.0 - smoothstep(trailLen - fwV, trailLen + fwV, vPeriodic);
    float hw = params.strokeWidth * 0.5;
    float distFromCenter = abs(colLocalU - 0.5);
    float widthMask = 1.0 - smoothstep(hw - fwU, hw + fwU, distFromCenter);
    float rectMask = streakMask * widthMask;

    // fallingCapAlpha: the elliptical leading cap
    float maxDepth = hw * params.rounding;
    float gapDist = max(vPeriodic - trailLen, 0.0) * density;
    float tcap = clamp(gapDist / max(maxDepth, 0.0001), 0.0, 1.0);
    float capHW = hw * sqrt(max(1.0 - tcap * tcap, 0.0));
    float capAlpha = (1.0 - smoothstep(max(capHW - fwU, 0.0), max(capHW + fwU, 0.001), distFromCenter)) * smoothstep(0.0, fwU, capHW);
    float finalMask = max(rectMask, capAlpha);

    // fallingBalanceRemap, then the trail -> lead colour
    float balanceNum = 2.0 * params.balance - 1.0;
    float balanceDen = max(2.0 * (1.0 - params.balance), 0.001);
    float fade = clamp((withinStreak + balanceNum) / balanceDen, 0.0, 1.0);
    vec4 streakColor = jfx_mix_colors(params.colorB, params.colorA, fade, params.colorSpace);
    return vec4(streakColor.rgb, streakColor.a * finalMask);
}

#include "effects/lib/main_paint.glsl"
