#version 450
// @effect     neon-border
// @kind       paint
// @category   light
// @summary    A lit neon tube along the element's rounded border, with a soft glow around it
// @source     shaders.com Neon (adapted: tube along the border, sizes in px)
// @cost       medium
// @time       flicker-speed, flow-speed
// @geometry   bounds extent
// @suggest-for neon
// @param      color color #00ddff -- Colour of the tube
// @param      secondary-color color #ff00aa -- Colour of the tube's shadow side (two-tone look)
// @param      secondary-blend number 0.5 0 1 -- Blend between one colour (0) and the two-tone tube (1)
// @param      glow-color color #00ddff -- Colour of the glow around the tube
// @param      tube-thickness length 4px 1px 32px -- Width of the tube inside the border
// @param      intensity number 1.5 0.5 4 -- Overall brightness
// @param      hot-core number 0.6 0 1 -- The white-hot line in the middle of the tube
// @param      glow-intensity number 0.6 0 2 -- Strength of the glow
// @param      glow-radius length 16px 1px 96px -- How far the glow reaches outside the tube
// @param      light-angle angle 300deg 0deg 360deg -- Direction the tube is lit from
// @param      specular number 0.5 0 2 -- Brightness of the tube's highlight
// @param      flicker-speed number 0 0 5 -- Speed of the sporadic flicker (0 off)
// @param      flicker-amount number 0.2 0 1 -- How often the tube flickers off
// @param      flow-speed number 0 0 5 -- Speed of the brightness flowing around the tube (0 off)
// @param      flow-amount number 0.3 0 1 -- Strength of the flowing brightness
// @param      extent length 32px 0px 128px -- How far the glow may reach outside the element

// Ported from shaders.com Neon (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (kit/effects/neon.ts: neonSmoothEPS, neonGlow, neonFlicker, neonFlow, neonComposite, the 2D
// path). Adapted: the tube runs inside the element's rounded border (jfx_box_sdf, CSS px), the
// tube and the glow are sized in px, and the field taps are the box distance. The source's
// specular size (0.5) and corner smoothing (0.15) are fixed at their defaults, its other
// shape-unit lengths (the smoothing stencil, the soft edge, the glow's inside cut) follow the
// tube's width in the ratio of the source's defaults (its 0.01-unit tube); the flicker and flow
// clocks are integrated (@time). The glow is cut beyond 1.5 glow radii as in the source; it fades
// out over the extent's last pixels so the geometry's edge never cuts it.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sdf.glsl"
#include "effects/lib/fam_light_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec4 secondaryColor;
    vec4 glowColor;
    float secondaryBlend;
    float tubeThickness;
    float intensity;
    float hotCore;
    float glowIntensity;
    float glowRadius;
    float lightAngle;
    float specular;
    float flickerSpeed;
    float flickerAmount;
    float flowSpeed;
    float flowAmount;
    float extent;
} params;

// light-direction constants (lz 0.7): 1 / sqrt(1.49), lz normalised, the half vector's z and length
#define NEON_INV_LIGHT_LEN 0.8192319205190405
#define NEON_LZN 0.5734623443633283
#define NEON_HZ 1.5734623443633282
#define NEON_INV_H_LEN 0.5637114097207191

vec4 jfx_paint(JfxContext c){
    const float SPECULAR_SIZE = 0.5;
    const float CORNER_SMOOTHING = 0.15;
    float tube = max(params.tubeThickness, 0.5);
    float unitPx = tube / 0.01;                          // px of one source unit (its tube is 0.01)
    float smoothEPS = (0.008 + CORNER_SMOOTHING * 0.03) * unitPx;
    float softPx = max(1.0 / max(c.dpr, 1e-3), 0.004 * unitPx);
    float glowR = max(params.glowRadius, 0.001);

    // the 5-point stencil of the box distance (corner smoothing and the outward normal)
    float sdfCenter = jfx_box_sdf(c.px, c.size, c.radii);
    float sdfRight = jfx_box_sdf(c.px + vec2(smoothEPS, 0.0), c.size, c.radii);
    float sdfLeft = jfx_box_sdf(c.px - vec2(smoothEPS, 0.0), c.size, c.radii);
    float sdfUp = jfx_box_sdf(c.px + vec2(0.0, smoothEPS), c.size, c.radii);
    float sdfDown = jfx_box_sdf(c.px - vec2(0.0, smoothEPS), c.size, c.radii);
    float gradX = (sdfRight - sdfLeft) / (smoothEPS * 2.0);
    float gradY = (sdfUp - sdfDown) / (smoothEPS * 2.0);
    float smoothedSdf = (sdfCenter + sdfRight + sdfLeft + sdfUp + sdfDown) / 5.0;
    float sdf = mix(sdfCenter, smoothedSdf, CORNER_SMOOTHING);
    float gradLen = max(sqrt(gradX * gradX + gradY * gradY), 0.0001);
    float gnx = gradX / gradLen;
    float gny = gradY / gradLen;

    // the tube's band and its semicircular cross-section
    float depthNorm = clamp(-sdf / max(tube, 0.001), 0.0, 1.0);
    float innerMask = smoothstep(tube + softPx, tube - softPx, -sdf);
    float outerMask = smoothstep(softPx, -softPx, sdf);
    float tubeMask = outerMask * innerMask;
    float cylinderZ = sqrt(max(depthNorm * (2.0 - depthNorm), 0.0));
    float cylinderR = 1.0 - depthNorm;
    vec3 n = vec3(gnx * cylinderR, gny * cylinderR, cylinderZ);

    // the directional light (z 0.7), Blinn-Phong
    float lxn = cos(params.lightAngle) * NEON_INV_LIGHT_LEN;
    float lyn = sin(params.lightAngle) * NEON_INV_LIGHT_LEN;
    float diffuse = clamp(n.x * lxn + n.y * lyn + n.z * NEON_LZN, 0.0, 1.0);
    float ndh = n.x * (lxn * NEON_INV_H_LEN) + n.y * (lyn * NEON_INV_H_LEN) + n.z * (NEON_HZ * NEON_INV_H_LEN);
    float shininess = exp2(8.0 - SPECULAR_SIZE * 7.0);
    float specular = pow(clamp(ndh, 0.0, 1.0), shininess) * params.specular;

    // neonFlow: a brightness band turning around the element's centre
    vec2 q = c.px - c.size * 0.5;
    float flowWave = sin(atan(q.y, q.x) * 2.0 + c.anim.y) * 0.5 + 0.5;
    float flowMod = mix(1.0, flowWave, params.flowAmount);

    // two-tone shading, self-emission and the hot core
    vec3 twoTone = mix(params.secondaryColor.rgb, params.color.rgb, diffuse);
    vec3 baseColor = mix(params.color.rgb, twoTone, params.secondaryBlend);
    vec3 tubeRgb = baseColor * (0.12 + diffuse * 0.55 + cylinderZ * 0.35) + specular;
    float coreProfile = smoothstep(0.55, 1.0, depthNorm);
    vec3 hotCoreCol = mix(params.color.rgb, vec3(1.0), 0.75);
    vec3 tubeWithCore = mix(tubeRgb, hotCoreCol, coreProfile * params.hotCore * flowMod);
    vec3 tubeFlowed = tubeWithCore * mix(1.0, flowMod, 0.4);

    // neonGlow: a tight and a wide exponential outside the tube
    float sdfOutside = max(sdf, 0.0);
    float innerGlow = exp(-sdfOutside * (12.0 / glowR)) * 0.7;
    float outerGlow = exp(-sdfOutside * (4.0 / glowR)) * 0.35;
    float glowMasked = (innerGlow + outerGlow) * params.glowIntensity * smoothstep(-0.003 * unitPx, 0.003 * unitPx, sdf);

    // neonFlicker: three irrational sines over a threshold
    float flickerTime = c.anim.x;
    float combined = sin(flickerTime * 17.13) + sin(flickerTime * 31.71) * 0.5 + sin(flickerTime * 47.29) * 0.3;
    float threshold = mix(-2.0, 0.8, params.flickerAmount);
    float flicker = smoothstep(threshold - 0.1, threshold + 0.1, combined);
    float intensityMul = params.intensity * flicker;

    vec3 finalTube = tubeFlowed * intensityMul;
    vec3 finalGlow = params.glowColor.rgb * (glowMasked * intensityMul);
    vec3 rgb = mix(finalGlow, finalTube, tubeMask);
    float alpha = max(tubeMask, clamp(glowMasked * 0.6, 0.0, 1.0));
    // beyond the glow's reach: exactly nothing (the source's cut); fading over the extent's end
    float threshOut = glowR * 1.5 + softPx + smoothEPS;
    float fadeW = min(params.extent * 0.5, 4.0);
    float keep = (sdfCenter > threshOut ? 0.0 : 1.0) * (1.0 - fam_light_smoothstep(params.extent - fadeW, params.extent, sdfCenter));
    return vec4(rgb, alpha * keep);
}

#include "effects/lib/main_paint.glsl"
