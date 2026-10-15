#version 450
// @effect     motion-blur
// @kind       filter
// @category   blur
// @summary    Directional streak blur of the content along an angle
// @source     shaders.com LinearBlur
// @cost       medium
// @suggest-for linear-blur directional-blur
// @param      intensity number 30 0 100 -- Strength of the streak (about twice this many px long)
// @param      angle angle 0deg 0deg 360deg -- Direction of the streak (0deg along x, clockwise)

// Ported from shaders.com LinearBlur (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts motionBlur with blurPath.linear: motionBlur.ts linearBlurTapCoord, 32
// taps weighted by gaussianTapWeights(32, 0.8), summed premultiplied; taps beyond the area repeat
// its border as the kit's clamp sampler does). The streak's pixels are CSS px.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_common.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    float intensity;
    float angle;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 32; i++){
        vec2 uv = fam_linear_blur_tap(c.uv, params.angle, params.intensity, c.size, float(i) / 31.0 - 0.5);
        sum += fam_src_premul(uv * c.size) * FAM_MOTION_WEIGHTS[i];
    }
    return jfx_unpremultiply(sum);
}

#include "effects/lib/main_filter.glsl"
