#version 450
// @effect     godrays
// @kind       paint
// @category   light
// @summary    Volumetric light rays streaming out from a point
// @source     shaders.com Godrays
// @cost       medium
// @time       speed
// @suggest-for light-rays crepuscular
// @param      center position "0% 0%" -- Where the rays come from
// @param      density number 0.3 0 1 -- How many ray sectors there are
// @param      intensity number 0.8 0 1 -- How visible the rays are in their sectors
// @param      spotty number 1 0 1 -- How much the rays break into drifting spots
// @param      speed number 0.5 0 2 -- Speed of the rays streaming out
// @param      ray-color color #4283fb -- Colour of the rays
// @param      base-color color transparent -- Colour under the rays (source: backgroundColor)

// Ported from shaders.com Godrays (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (coverageOver(rayColor, backgroundColor, withFrame(seamlessRadialFrame(center), noiseRays)):
// godraysFrame, godraysRayStack, coverageOverComposite).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_common.glsl"

uniform u_fs_effectParams {
    vec4 rayColor;
    vec4 baseColor;
    vec2 center;
    float density;
    float intensity;
    float spotty;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    vec4 frame = fam_godrays_frame(c.uv, c.aspect, params.center);
    float coverage = fam_godrays_ray_stack(frame, c.anim.x, params.density, params.intensity, params.spotty);
    return fam_coverage_over(params.rayColor, params.baseColor, coverage);
}

#include "effects/lib/main_paint.glsl"
