#version 450
// @effect     twirl
// @kind       filter
// @category   distort
// @summary    Twists the content around a centre, more the farther out
// @source     shaders.com Twirl
// @cost       low
// @suggest-for swirl-distort vortex
// @param      intensity number 1 -5 5 -- Twist (radians per area height of distance; negative turns the other way)
// @param      center position "50% 50%" -- Centre of the twist
// @param      radius number 1 0 2 -- Distance (share of the height) beyond which the twist stops growing
// @param      edges enum stretch stretch|transparent|mirror|wrap -- What shows where the twist reads beyond the element

// Ported from shaders.com Twirl (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/warps.ts twirl: warpMaps.ts twirlUV, the aspect-corrected offset from the centre turned by
// intensity x its length; read through edges.ts sampleRemappedExpr: Catmull-Rom, bilinear in the
// mirror mode). `radius` (not in the source) caps the length the angle grows with: beyond it the
// content turns as a whole; at the default 1 an area up to 1.7 times wider than high twists as
// the source does.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float intensity;
    float radius;
    float edges;
} params;

vec4 jfx_filter(JfxContext c){
    vec2 delta = c.uv - params.center;
    vec2 acd = vec2(delta.x * c.aspect, delta.y);
    float angle = params.intensity * min(length(acd), params.radius);
    float cosA = cos(angle);
    float sinA = sin(angle);
    vec2 uv = vec2((cosA * acd.x - sinA * acd.y) / c.aspect + params.center.x, sinA * acd.x + cosA * acd.y + params.center.y);
    return fam_src_remapped(uv * c.size, params.edges);
}

#include "effects/lib/main_filter.glsl"
