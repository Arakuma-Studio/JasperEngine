// HTML UI effects, light family (p5-fx-light): the light fields the light effects share. Ported
// from the shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): lightfields.ts (radialBurstFrame, seamlessAngularField,
// angularSineLobes, softRayLobes, radialFeatherMask, anisotropicGaussianSpot, streakBand,
// heatRamp3, chromaticHeatTaps, screenGlowComposite, beamLocalFrame) and gradientPaints.ts (the
// godrays frame and ray stack, coverageOverComposite, the light-leak drift, bloom and streaks,
// heatDither), with their constants.
//
// Positions arrive authored (0-1, y down): the kit stores them flipped and its bodies recover the
// authored y (`1 - center.y`), so these read them as they come.
#ifndef FAM_LIGHT_COMMON_GLSL
#define FAM_LIGHT_COMMON_GLSL

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_noise.glsl"

// WGSL's smoothstep with equal edges is a step (the divide gives +-inf, clamped); GLSL leaves it
// undefined, so a port whose edges can meet (a zero radius or width) uses this
float fam_light_smoothstep(float edge0, float edge1, float x){
    if (edge1 > edge0)
        return smoothstep(edge0, edge1, x);
    return x > edge0 ? 1.0 : 0.0;
}

// ---- radial frames

// radialBurstFrame: (distance, angle) about the centre, aspect corrected (x scaled)
vec2 fam_radial_burst_frame(vec2 uv, float aspect, vec2 center){
    float dx = uv.x * aspect - center.x * aspect;
    float dy = uv.y - center.y;
    return vec2(sqrt(dx * dx + dy * dy), atan(dy, dx));
}

// seamlessAngularField: (angle, the angle wrapped into [0, 2pi), the blend between them that
// hides the atan seam along -x)
vec3 fam_seamless_angular_field(vec2 delta){
    float angle = atan(delta.y, delta.x);
    float angleWrapped = angle - JFX_TAU * floor(angle / JFX_TAU);
    float blend = smoothstep(-0.15, 0.15, delta.x);
    return vec3(angle, angleWrapped, blend);
}

// angularSineLobes: pow(sin(angle * count + phase) * 0.5 + 0.5, sharpness)
float fam_angular_sine_lobes(float angle, float count, float phase, float sharpness){
    float t = sin(angle * count + phase) * 0.5 + 0.5;
    return pow(t, sharpness);
}

// softRayLobes: the lobes with a softness slider (0.3 / (softness + 0.05) as the exponent)
float fam_soft_ray_lobes(float angle, float count, float phase, float softness){
    float sharpness = 1.0 / (softness + 0.05) * 0.3;
    return fam_angular_sine_lobes(angle, count, phase, sharpness);
}

// radialFeatherMask: 1 inside, fading between radius * (1 - feather) and radius
float fam_radial_feather_mask(float dist, float radius, float feather){
    float falloffStart = radius * (1.0 - feather);
    return 1.0 - fam_light_smoothstep(falloffStart, radius, dist);
}

// ---- composites

// coverageMix: the background to the light colour by coverage, rgb and alpha
vec4 fam_coverage_mix(vec4 color, vec4 background, float coverage){
    return vec4(mix(background.rgb, color.rgb, coverage), mix(background.a, color.a, coverage));
}

// coverageOverComposite: the light colour over the background by coverage (straight alpha)
vec4 fam_coverage_over(vec4 rayColor, vec4 bgColor, float coverage){
    float rayA = coverage * rayColor.a;
    float finalAlpha = rayA + bgColor.a * (1.0 - rayA);
    float safeAlpha = clamp(finalAlpha, 0.001, 1.0);
    vec3 finalColor = (rayColor.rgb * rayA + bgColor.rgb * bgColor.a * (1.0 - rayA)) / safeAlpha;
    return vec4(finalColor, finalAlpha);
}

// ---- godrays (gradientPaints.ts)

// the godrays' value noise over the bit-cast hash12
float fam_godrays_value_noise(vec2 st){
    vec2 i = floor(st);
    vec2 f = fract(st);
    float a = jfx_hash12(i);
    float b = jfx_hash12(vec2(i.x + 1.0, i.y));
    float c = jfx_hash12(vec2(i.x, i.y + 1.0));
    float e = jfx_hash12(vec2(i.x + 1.0, i.y + 1.0));
    vec2 u = f * f * (f * -2.0 + 3.0);
    return mix(mix(a, b, u.x), mix(c, e, u.x), u.y);
}

// godraysRaysShape: the noise on both atan branches, cross-faded over the seam
float fam_godrays_rays_shape(float a, float aWrapped, float blend, float r, float freq, float intensity){
    float nLeft = pow(fam_godrays_value_noise(vec2(a * freq, r)), intensity);
    float nRight = pow(fam_godrays_value_noise(vec2(aWrapped * freq, r)), intensity);
    return mix(nRight, nLeft, blend);
}

// godraysFrame: (angle, wrapped angle, blend, radius) about the centre, aspect corrected
vec4 fam_godrays_frame(vec2 uv, float aspect, vec2 center){
    vec2 delta = uv - center;
    vec2 shapeUV = vec2(delta.x * aspect, delta.y);
    vec3 ang = fam_seamless_angular_field(shapeUV);
    return vec4(ang, length(shapeUV));
}

// godraysRayLayer: the product of two seam-blended shapes at their schedule (rMulA, tRateA,
// rMulB, tRateB) and frequencies (fMul, fBMul)
float fam_godrays_ray_layer(vec4 frame, float animTime, float spots, float intensityCalc, float dens, vec4 sched, vec2 freqs){
    float rA = frame.w * sched.x - animTime * sched.y;
    float rB = frame.w * sched.z * (1.0 + spots) - animTime * sched.w;
    float f = dens * freqs.x;
    float shapeA = fam_godrays_rays_shape(frame.x, frame.y, frame.z, rA, f, intensityCalc);
    float shapeB = fam_godrays_rays_shape(frame.x, frame.y, frame.z, rB, f * freqs.y, intensityCalc);
    return shapeA * shapeB;
}

// godraysRayStack: the two tuned layers, the clamped coverage
float fam_godrays_ray_stack(vec4 frame, float animTimeRaw, float density, float intensity, float spotty){
    float animTime = animTimeRaw * 0.2;
    float spots = 6.5 * abs(spotty);
    float intensityCalc = 4.0 - 3.0 * clamp(intensity, 0.0, 1.0);
    float dens = 6.0 * density;
    float layer1 = fam_godrays_ray_layer(frame, animTime, spots, intensityCalc, dens, vec4(1.0, 3.0, 0.5, 2.0), vec2(5.0, 4.0));
    float layer2 = fam_godrays_ray_layer(frame, animTime, spots, intensityCalc, dens, vec4(1.4, 2.5, 0.7, 1.8), vec2(4.5, 3.5));
    return clamp(layer1 + layer2 * 0.7, 0.0, 1.0);
}

// ---- light leak (gradientPaints.ts, lightfields.ts)

// anisotropicGaussianSpot: exp(-(du^2 + dv^2) * sharpness)
float fam_gaussian_spot(float du, float dv, float sharpness){
    return exp((du * du + dv * dv) * -sharpness);
}

// streakBand: a gaussian band across u peaking at center
float fam_streak_band(float u, float center, float width){
    return exp((u - center) * (u - center) * -1.0 / (width * width));
}

// beamLocalFrame: (along, lateral) of a beam anchored at the authored point, aimed at the centre
vec2 fam_beam_local_frame(vec2 uv, float aspect, vec2 anchorAuthored){
    vec2 pA = vec2(uv.x * aspect, uv.y);
    vec2 anchor = vec2(anchorAuthored.x * aspect, anchorAuthored.y);
    vec2 toCenter = vec2(aspect * 0.5, 0.5) - anchor;
    float cLen = length(toCenter);
    float dirX = cLen < 0.02 ? 1.0 : toCenter.x / max(cLen, 0.001);
    float dirY = cLen < 0.02 ? 0.0 : toCenter.y / max(cLen, 0.001);
    float relX = pA.x - anchor.x;
    float relY = pA.y - anchor.y;
    return vec2(relX * dirX + relY * dirY, relY * dirX - relX * dirY);
}

// leakDrift: (flicker gain, streak drift, elongation drift, width drift)
vec4 fam_leak_drift(float t, float seed, float flicker){
    float nA = jfx_value12(vec2(t * 0.9, seed * 0.53 + 11.7));
    float nB = jfx_value12(vec2(t * 2.7 + 41.3, seed * 0.29 + 5.1));
    float breathe = nA * 0.65 + nB * 0.35;
    float flickerMod = mix(1.0, 0.25 + 1.5 * breathe, flicker);
    float evo1 = jfx_value12(vec2(t * 0.13 + 3.1, seed * 0.41 + 27.0));
    float evo2 = jfx_value12(vec2(t * 0.09 + 17.7, seed * 0.61 + 8.4));
    float evo3 = jfx_value12(vec2(t * 0.17 + 51.2, seed * 0.23 + 2.9));
    return vec4(flickerMod, evo1, evo2, evo3);
}

// leakBloom: (main, shoulder) of the pool at the anchor
vec2 fam_leak_bloom(float u, float v, float spread, vec4 drift){
    float sp = max(spread, 0.05);
    float elong = sp * (1.5 + 0.7 * drift.z);
    float du = u / sp;
    float dv = v / elong;
    float mainSpot = fam_gaussian_spot(du, dv, 1.9);
    float shoulder = fam_gaussian_spot(du, dv, 0.45) * 0.22;
    return vec2(mainSpot, shoulder);
}

// leakStreaks: the two drifting bands across the beam, times streaks
float fam_leak_streaks(float u, float v, float spread, float streaks, vec4 drift){
    float sp = max(spread, 0.05);
    float elong = sp * (1.5 + 0.7 * drift.z);
    float dv = v / elong;
    float alongFade = exp(dv * dv * -0.55);
    float s1c = sp * (0.42 + 0.22 * drift.y);
    float s1w = max(sp * (0.07 + 0.07 * drift.w), 0.008);
    float streak1 = fam_streak_band(u, s1c, s1w) * alongFade;
    float s2c = sp * (0.8 + 0.3 * drift.y);
    float s2w = max(sp * (0.16 + 0.12 * drift.w), 0.015);
    float streak2 = fam_streak_band(u, s2c, s2w) * alongFade * 0.45;
    return (streak1 + streak2) * streaks;
}

// heatDither: +-0.006 of hash noise on a heat field, floored at 0
float fam_heat_dither(float heat, vec2 uv, float t){
    float dithered = heat + (jfx_hash12(uv * 1024.0 + vec2(t, t * 1.7)) - 0.5) * 0.012;
    return max(dithered, 0.0);
}

// heatRamp3: fringe -> mid -> hot as the heat rises, the energy scaled by the heat (to 1.6)
vec3 fam_heat_ramp3(float heat, vec3 hot, vec3 mid, vec3 fringe){
    float toMid = smoothstep(0.0, 0.5, heat);
    float toHot = smoothstep(0.55, 1.15, heat);
    vec3 rgb = mix(mix(fringe, mid, vec3(toMid)), hot, vec3(toHot));
    return rgb * clamp(heat, 0.0, 1.6);
}

// chromaticHeatTaps: the ramp read at x1.08 / x1 / x0.88 the heat for r / g / b
vec3 fam_chromatic_heat_taps(float heat, vec3 hot, vec3 mid, vec3 fringe){
    vec3 rampR = fam_heat_ramp3(heat * 1.08, hot, mid, fringe);
    vec3 rampG = fam_heat_ramp3(heat, hot, mid, fringe);
    vec3 rampB = fam_heat_ramp3(heat * 0.88, hot, mid, fringe);
    return vec3(rampR.x, rampG.y, rampB.z);
}

// screenGlowComposite: emitted light screened over a straight colour, pushed by x0.16, the alpha
// raised by the light's Rec.601 luma (x0.85)
vec4 fam_screen_glow(vec4 color, vec3 leakRgb){
    vec3 screened = 1.0 - (1.0 - clamp(color.rgb, 0.0, 1.0)) * (1.0 - clamp(leakRgb, 0.0, 1.0));
    vec3 outRgb = clamp(screened + leakRgb * 0.16, 0.0, 1.0);
    float leakLuma = clamp(dot(leakRgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
    float outA = clamp(color.a + leakLuma * 0.85, 0.0, 1.0);
    return vec4(outRgb, outA);
}

#endif // FAM_LIGHT_COMMON_GLSL
