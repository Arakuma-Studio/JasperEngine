#version 450
// @effect     sunburst
// @kind       paint
// @category   light
// @summary    Soft rays turning around a centre, fading at their outer ends
// @source     shaders.com SunBurst
// @cost       low
// @time       speed
// @suggest-for rays reward-rays
// @param      color color #ffdd88 -- Colour of the rays
// @param      base-color color #000000 -- Colour between and beyond the rays (source: background)
// @param      center position "50% 50%" -- Centre of the burst
// @param      ray-count integer 12 3 64 -- Number of rays
// @param      softness number 0.3 0 1 -- Softness of the rays' sides (0 crisp, 1 wide and soft)
// @param      radius number 0.8 0 1.2 -- How far the rays reach, in shares of the area's height
// @param      feather number 0.5 0 5 -- Share of the radius over which the rays fade out
// @param      speed number 0.2 -2 2 -- Turns of the rays; positive turns clockwise

// Ported from shaders.com SunBurst (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (coverageMix(color, background, withFrame(radialFrame(center), modulate(softRayLobes(spin -1),
// featherMask)))): radialBurstFrame, softRayLobes, radialFeatherMask.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec4 baseColor;
    vec2 center;
    float rayCount;
    float softness;
    float radius;
    float feather;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 frame = fam_radial_burst_frame(c.uv, c.aspect, params.center);
    float count = floor(params.rayCount + 0.5);
    // positive speed turns clockwise: the lobes' phase is the negated clock
    float lobes = fam_soft_ray_lobes(frame.y, count, -c.anim.x, params.softness);
    float coverage = lobes * fam_radial_feather_mask(frame.x, params.radius, params.feather);
    return fam_coverage_mix(params.color, params.baseColor, coverage);
}

#include "effects/lib/main_paint.glsl"
