#version 450
// @effect     sparkle-test
// @kind       filter
// @category   light
// @summary    A filter effect of the tests
// @cost       low
// @param      amount number 0.5 0 1 -- Strength

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    float amount;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 s = jfx_src(c.px);
    return vec4(mix(s.rgb, vec3(1.0), params.amount), s.a);
}

#include "effects/lib/main_filter.glsl"
