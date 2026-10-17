#version 450
// @effect     radial-pulse
// @kind       paint
// @category   gradient
// @summary    A radial gradient whose rings can pulse outward from the centre
// @source     shaders.com RadialGradient (adapted: speed moves rings outward)
// @cost       low
// @time       speed
// @suggest-for radial-gradient pulse
// @param      colors palette "#ff0000 #0000ff" -- Colours of the gradient from the centre out
// @param      center position "50% 50%" -- Centre of the gradient
// @param      radius number 1 0 2 -- Radius as a share of the area's height
// @param      repeat number 1 1 20 -- Number of rings (1 is one gradient)
// @param      aspect number 1 0.1 4 -- Ellipse ratio (1 is a circle, larger squeezes it horizontally)
// @param      speed number 0 -5 5 -- Rings per second moving outward (negative moves inward)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the gradient

// Ported from shaders.com RadialGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (rampOver(rings(dist.radial), stops): radialDist without the skew, repeatRings). Adapted: the
// integrated speed clock moves the rings outward; once it has moved, the rings wrap (as they do
// for repeat above 1) instead of holding the end colours.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    vec2 center;
    float radius;
    float repeat;
    float aspect;
    float speed;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

vec4 jfx_paint(JfxContext c){
    // radialDist: the centred delta, stretched on x by the ellipse ratio, over the radius
    vec2 delta = fam_centered_delta(c.uv, params.center, c.aspect);
    vec2 stretched = vec2(delta.x * params.aspect, delta.y);
    float dist = length(stretched) / max(params.radius, 1e-6);
    // repeatRings, the rings moved outward by the clock
    float s = dist * params.repeat - c.anim.x;
    float t = (params.repeat > 1.0001 || c.anim.x != 0.0) ? fract(s) : clamp(s, 0.0, 1.0);
    return jfx_palette_colors(t, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
