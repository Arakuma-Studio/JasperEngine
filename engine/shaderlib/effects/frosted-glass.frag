#version 450
// @effect     frosted-glass
// @kind       filter
// @category   glass
// @summary    Blurred, tinted glass over what lies behind the element, with a lit edge
// @source     original (Glass kit glassTint/glassBorderHighlight, gpu/kit/effects/glass.ts:170-199)
// @cost       medium
// @backdrop   yes
// @prepass    blur blur
// @margin     blur*3
// @suggest-for glass glassmorphism frost blur-glass acrylic
// @param      blur length 12px 0px 64px -- Blur radius (the gaussian's sigma)
// @param      tint color #ffffff1f -- Tint colour; its alpha is the tint's strength
// @param      saturation number 1.2 0 3 -- Saturation of what is behind (1 unchanged)
// @param      brightness number 1 0 2 -- Brightness of what is behind (1 unchanged)
// @param      noise number 0.04 0 0.3 -- Frost grain
// @param      highlight number 0.35 0 1 -- Strength of the lit edge
// @param      light-angle angle 300deg 0deg 360deg -- Direction the edge is lit from

// The tint and the edge highlight follow the shaders.com Glass kit (MIT, Copyright (c) 2026
// Shader Effects Inc.; see effects/LICENSE.shaders-com.txt): glass.ts glassTint and
// glassBorderHighlight, on the element's rounded box in CSS px.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_sdf.glsl"

uniform u_fs_effectParams {
    vec4 tint;
    float blur;
    float saturation;
    float brightness;
    float noise;
    float highlight;
    float lightAngle;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 behind = jfx_blurred(c.px);
    vec3 rgb = jfx_saturate(behind.rgb, params.saturation) * params.brightness;
    // glassTint (no luminosity preservation): the tint's alpha is its intensity
    rgb = mix(rgb, params.tint.rgb, params.tint.a);
    // frost grain, one value per device pixel
    rgb += (jfx_hash12(floor(c.px * c.dpr) + 0.5) - 0.5) * params.noise;
    // glassBorderHighlight: a thin ring inside the rounded edge, lit where it faces the light
    float sdf = jfx_box_sdf(c.px, c.size, c.radii);
    vec2 normal = jfx_box_normal(c.px, c.size, c.radii);
    float pxH = 1.0;
    float w2 = pxH * 1.5;
    float ring = clamp((pxH - sdf) / w2, 0.0, 1.0) - clamp(-sdf / w2, 0.0, 1.0);
    float lightFacing = clamp(dot(normal, vec2(cos(params.lightAngle), sin(params.lightAngle))) * 0.5 + 0.5, 0.0, 1.0);
    rgb += vec3(ring * lightFacing * params.highlight);
    return vec4(rgb, behind.a);
}

#include "effects/lib/main_filter.glsl"
