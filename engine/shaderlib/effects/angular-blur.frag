#version 450
// @effect     angular-blur
// @kind       filter
// @category   blur
// @summary    Rotational blur sweeping the content around a centre point
// @source     shaders.com AngularBlur
// @cost       medium
// @suggest-for spin-blur rotational-blur
// @param      intensity number 20 0 100 -- Strength of the sweep (100 sweeps about 29 degrees)
// @param      center position "50% 50%" -- Point the content turns around

// Ported from shaders.com AngularBlur (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts motionBlur with blurPath.orbit: motionBlur.ts angularBlurTapCoord, 32 taps
// centred on the pixel and weighted by gaussianTapWeights(32, 0.8), summed premultiplied; taps
// beyond the area repeat its border as the kit's clamp sampler does).

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
        vec2 uv = fam_angular_blur_tap(params.center, params.intensity, c.uv, c.aspect, float(i));
        sum += fam_src_premul(uv * c.size) * FAM_MOTION_WEIGHTS[i];
    }
    return jfx_unpremultiply(sum);
}

#include "effects/lib/main_filter.glsl"
