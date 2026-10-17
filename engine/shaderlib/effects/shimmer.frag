#version 450
// @effect     shimmer
// @kind       paint
// @category   light
// @summary    A skeleton placeholder: a base colour with a soft highlight sweeping across it
// @source     original (skeleton loading)
// @cost       low
// @suggest-for skeleton loading placeholder
// @param      base-color color #d9dce1 -- Colour of the placeholder
// @param      highlight-color color #f5f6f8 -- Colour of the sweeping highlight
// @param      angle angle 15deg -90deg 90deg -- Lean of the highlight from vertical (it sweeps left to right)
// @param      width number 0.4 0.05 1 -- Width of the highlight as a share of the sweep
// @param      duration time 1.4s 0.2s 10s -- Time of one sweep (they follow each other without a pause)

// The usual loading skeleton: the base colour everywhere and a soft highlight (a raised-cosine
// bump `width` wide) crossing from just outside the left edge to just outside the right edge
// every `duration`, along the direction `angle` leans from vertical.

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 baseColor;
    vec4 highlightColor;
    float angle;
    float width;
    float duration;
} params;

vec4 jfx_paint(JfxContext c){
    float progress = fract(c.time / max(params.duration, 1e-3));
    vec2 dir = vec2(cos(params.angle), sin(params.angle));
    float extent = max(c.size.x * abs(dir.x) + c.size.y * abs(dir.y), 1e-3);
    float u = dot(c.px - c.size * 0.5, dir) / extent + 0.5;
    float halfWidth = max(params.width, 1e-3) * 0.5;
    float center = mix(-halfWidth, 1.0 + halfWidth, progress);
    float d = abs(u - center) / halfWidth;
    float bump = d < 1.0 ? 0.5 + 0.5 * cos(d * JFX_PI) : 0.0;
    return mix(params.baseColor, params.highlightColor, bump);
}

#include "effects/lib/main_paint.glsl"
