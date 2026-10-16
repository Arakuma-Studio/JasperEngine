#version 450
// @effect     zoom-blur
// @kind       filter
// @category   blur
// @summary    Radial zoom blur streaking the content out of a centre point
// @source     shaders.com ZoomBlur
// @cost       medium
// @suggest-for radial-blur
// @param      intensity number 30 0 100 -- Strength of the zoom (100 pulls in from twice the distance to the centre)
// @param      center position "50% 50%" -- Point the zoom streaks out of

// Ported from shaders.com ZoomBlur (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts motionBlur with blurPath.zoom: motionBlur.ts zoomBlurTapCoord, 32 taps
// weighted by gaussianTapWeights(32, 0.8), summed premultiplied; taps beyond the area repeat its
// border as the kit's clamp sampler does).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_common.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float intensity;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 32; i++){
        vec2 uv = fam_zoom_blur_tap(params.center, params.intensity, c.uv, c.aspect, float(i));
        sum += fam_src_premul(uv * c.size) * FAM_MOTION_WEIGHTS[i];
    }
    return jfx_unpremultiply(sum);
}

#include "effects/lib/main_filter.glsl"
