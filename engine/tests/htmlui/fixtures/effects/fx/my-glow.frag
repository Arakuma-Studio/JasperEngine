#version 450
// @effect     my-glow
// @kind       paint
// @category   light
// @summary    A soft glow from the centre of the element, pulsing
// @source     original
// @cost       low
// @time       speed
// @param      color color #59a6ff -- Glow colour
// @param      intensity number 0.6 0 1 -- Strength of the glow
// @param      speed number 1 0 5 -- Pulses per second

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float intensity;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 p = (c.uv - 0.5) * 2.0;
    float pulse = 0.75 + 0.25 * sin(c.anim.x * 6.2831853);
    float glow = exp(-dot(p, p) * 2.5) * params.intensity * pulse;
    return vec4(params.color.rgb, params.color.a * glow);
}

#include "effects/lib/main_paint.glsl"
