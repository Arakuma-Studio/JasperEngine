#version 450
// @effect     ripple-test
// @kind       paint
// @category   interactive
// @summary    A project effect of the tests: rings where the pointer pressed (EffectRuntimeTest)
// @source     original
// @cost       low
// @pointer    ripples
// @param      color color #ffffff -- Ring colour
// @param      trigger enum press press|move -- What starts a ring

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float trigger;
} params;

uniform u_fs_effectRipples {
    vec4 points[8];    // xy local px, z age s, w strength; newest first
} ripples;

vec4 jfx_paint(JfxContext c){
    float ring = 0.0;
    for (int i = 0; i < 8; i++){
        vec4 r = ripples.points[i];
        float d = abs(length(c.px - r.xy) - r.z * 120.0);
        ring = max(ring, r.w * (1.0 - smoothstep(0.0, 3.0, d)) * (1.0 - clamp(r.z, 0.0, 1.0)));
    }
    return vec4(params.color.rgb, params.color.a * ring);
}

#include "effects/lib/main_paint.glsl"
