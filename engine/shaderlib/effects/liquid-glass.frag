#version 450
// @effect     liquid-glass
// @kind       filter
// @category   glass
// @summary    A glass lens on the element's rounded box: refracts, splits and lights what is behind
// @source     shaders.com Glass (adapted: shape = element rounded box, thickness in px)
// @cost       high
// @backdrop   yes
// @prepass    blur blur
// @margin     thickness*0.5+8px
// @suggest-for refraction lens apple-glass
// @param      refraction number 1 0 2 -- How hard the edges bend what is behind
// @param      thickness length 24px 1px 200px -- Depth of the bent rim from the edge inward
// @param      blur length 0px 0px 20px -- Frost: blur of what is behind (the gaussian's sigma)
// @param      aberration number 0.5 0 1 -- Colour split along the refraction
// @param      inner-zoom number 1 0.5 3 -- Magnification of what is seen through the glass
// @param      light-angle angle 300deg 0deg 360deg -- Direction the light comes from
// @param      highlight number 0.05 0 2 -- Lit rim and specular glint on the light-facing edge
// @param      highlight-color color #ffffff -- Colour of the lit rim and glint
// @param      highlight-softness number 0.5 0 1 -- Spread of the specular glint
// @param      fresnel number 0.1 0 1 -- Soft glow around the inside of the edge
// @param      fresnel-softness number 0.1 0 1 -- Width of the edge glow
// @param      fresnel-color color #ffffff -- Colour of the edge glow
// @param      edge-softness number 0.1 0 1 -- Softness of the glass's edge
// @param      tint-color color #ffffff -- Tint of the glass
// @param      tint-intensity number 0 0 1 -- Strength of the tint
// @param      tint-preserve-luminosity boolean true -- Keep the brightness of what is behind when tinting

// Ported from shaders.com Glass (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (gpu/kit/effects/glass.ts applyGlassEffect: glassLensUVs, the blurred lens taps with the
// three-tap aberration, glassComposite with glassRefrStrength, glassBorderHighlight, glassTint,
// glassSpecular and glassFresnelRim). Adapted (phase-5 spec 3.15 example 4): the shape is the
// element's rounded box as a flat 2D plate (jfx_box_sdf in CSS px, its normal for the
// gradient), the refraction fades over `thickness` px from the edge, the lens offset is
// -normal x refraction x 0.15 x the shorter side x the strength, the zoom is about the box's
// centre, and the frost is the blur prepass. The kit's shape-unit widths take the shorter side
// as 0.7 units.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_sdf.glsl"
#include "effects/lib/fam_filters_glass.glsl"

uniform u_fs_effectParams {
    vec4 highlightColor;
    vec4 fresnelColor;
    vec4 tintColor;
    float refraction;
    float thickness;
    float blur;
    float aberration;
    float innerZoom;
    float lightAngle;
    float highlight;
    float highlightSoftness;
    float fresnel;
    float fresnelSoftness;
    float edgeSoftness;
    float tintIntensity;
    float tintPreserveLuminosity;
} params;

vec4 jfx_filter(JfxContext c){
    float unitPx = fam_glass_unit(c.size);
    float pxH = 1.0 / max(c.dpr, 1e-3);
    float sdf = jfx_box_sdf(c.px, c.size, c.radii);
    vec2 grad = jfx_box_normal(c.px, c.size, c.radii);
    // glassLensUVs
    float refrStrength = fam_glass_refr_strength(sdf, params.thickness);
    vec2 offset = -grad * (params.refraction * 0.15) * min(c.size.x, c.size.y) * refrStrength;
    vec2 center = c.size * 0.5;
    vec2 lens = center + (c.px - center) / max(params.innerZoom, 1e-3) + offset;
    vec2 chr = offset * (params.aberration * 0.06);
    vec4 g = jfx_blurred(lens);
    vec4 blurred = vec4(jfx_blurred(lens + chr).r, g.g, jfx_blurred(lens - chr).b, g.a);
    vec4 base = jfx_decode(jfx_src_premul_here());
    // glassComposite
    float sharp = max(params.edgeSoftness * 0.5, 0.001);
    float w1 = max(sharp * 0.03125 * unitPx, pxH * 1.5);
    float rb1 = clamp(-sdf / w1, 0.0, 1.0);
    float rb2 = fam_glass_border_highlight(sdf, params.edgeSoftness, unitPx, pxH, grad, params.lightAngle, params.highlight);
    vec3 tinted = fam_glass_tint(blurred.rgb, params.tintColor.rgb, params.tintIntensity, params.tintPreserveLuminosity);
    float spec = fam_glass_specular(grad, params.lightAngle, params.highlight, params.highlightSoftness, refrStrength);
    float rim = fam_glass_fresnel_rim(sdf, unitPx, params.fresnel, params.fresnelSoftness, rb1);
    vec4 lighting = vec4(tinted + params.highlightColor.rgb * (rb2 + spec) + params.fresnelColor.rgb * rim, blurred.a + rb2);
    return mix(base, lighting, smoothstep(0.0, 1.0, rb1));
}

#include "effects/lib/main_filter.glsl"
