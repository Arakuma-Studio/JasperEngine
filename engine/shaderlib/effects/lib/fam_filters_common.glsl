// HTML UI effects, filter family (p5-fx-filters): the pieces the filters share. Ported from the
// shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): motionBlur.ts (the 32-tap Gaussian gather's weights,
// blur.ts gaussianTapWeights(32, 0.8), and the three tap trajectories), lens.ts
// (chromaticOffsetUV) and fields.ts (radialFalloffMask), with their constants.
//
// Positions arrive authored (0-1, y down): the kit stores them flipped and its bodies recover the
// authored y (`1 - center.y`), so these read them as they come. Angles are radians.
#ifndef FAM_FILTERS_COMMON_GLSL
#define FAM_FILTERS_COMMON_GLSL

#include "effects/lib/jfx_common.glsl"

// WGSL's smoothstep with equal edges is a step; GLSL leaves it undefined
float fam_filters_smoothstep(float edge0, float edge1, float x){
    if (edge1 > edge0)
        return smoothstep(edge0, edge1, x);
    return x > edge0 ? 1.0 : 0.0;
}

// ---- the motion blurs (motionBlur.ts): 32 taps weighted by gaussianTapWeights(32, 0.8)

const float FAM_MOTION_WEIGHTS[32] = float[32](
    0.0183392457, 0.0202184988, 0.0221458243, 0.0240996256, 0.02605579, 0.0279881182, 0.0298688608, 0.0316693479,
    0.0333606948, 0.0349145588, 0.036303921, 0.0375038638, 0.0384923119, 0.039250707, 0.0397645869, 0.0400240444,
    0.0400240444, 0.0397645869, 0.039250707, 0.0384923119, 0.0375038638, 0.036303921, 0.0349145588, 0.0333606948,
    0.0316693479, 0.0298688608, 0.0279881182, 0.02605579, 0.0240996256, 0.0221458243, 0.0202184988, 0.0183392457);

// linearBlurTapCoord: tapT in [-0.5, 0.5]; the streak is the (aspect-corrected) direction x
// intensity over the area, doubled (px are CSS px)
vec2 fam_linear_blur_tap(vec2 uv, float angle, float intensity, vec2 size, float tapT){
    float aspect = size.x / max(size.y, 1e-6);
    float bvX = (cos(angle) / aspect * intensity) / max(size.x, 1e-6) * 2.0;
    float bvY = (sin(angle) * intensity) / max(size.y, 1e-6) * 2.0;
    return vec2(uv.x + bvX * tapT, uv.y + bvY * tapT);
}

// angularBlurTapCoord: the aspect-corrected offset from the centre turned by
// (tap - 15.5) x intensity x 0.005 / 31, a sweep centred on the pixel
vec2 fam_angular_blur_tap(vec2 center, float intensity, vec2 uv, float aspect, float tapIndex){
    float angleStep = (intensity * 0.005) / 31.0;
    float angle = (tapIndex - 15.5) * angleStep;
    float cosA = cos(angle);
    float sinA = sin(angle);
    float acX = (uv.x - center.x) * aspect;
    float acY = uv.y - center.y;
    return vec2((acX * cosA - acY * sinA) / aspect + center.x, acX * sinA + acY * cosA + center.y);
}

// zoomBlurTapCoord: the offset from the centre scaled down by 1 + intensity x 0.01 x tap / 31
vec2 fam_zoom_blur_tap(vec2 center, float intensity, vec2 uv, float aspect, float tapIndex){
    float scale = 1.0 + intensity * 0.01 * (tapIndex / 31.0);
    float acdX = (uv.x - center.x) * aspect;
    float acdY = uv.y - center.y;
    return vec2(acdX / scale / aspect + center.x, acdY / scale + center.y);
}

// ---- lens.ts chromaticOffsetUV: a channel's offset along `angle`, strength 1 = a tenth of the area
vec2 fam_chromatic_offset_uv(vec2 uv, float angle, float strength, float aspect, float channelOffset){
    vec2 direction = vec2(cos(angle) / aspect, sin(angle));
    return uv + direction * (strength * 0.1) * channelOffset;
}

// ---- fields.ts radialFalloffMask: 0 inside `radius` of the centre (height units), 1 beyond
// radius + falloff
float fam_radial_falloff_mask(vec2 uv, float aspect, vec2 center, float radius, float falloff){
    float dist = length(vec2(uv.x * aspect, uv.y) - vec2(center.x * aspect, center.y));
    return fam_filters_smoothstep(radius, radius + falloff, dist);
}

#endif // FAM_FILTERS_COMMON_GLSL
