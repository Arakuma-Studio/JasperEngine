#version 450
// @effect     tint-wave
// @kind       paint
// @category   gradient
// @summary    A project effect of the tests: a tinted wave
// @source     original
// @cost       low
// @time       speed
// @param      color color #3388ff -- Tint of the wave
// @param      speed number 1 -5 5 -- Waves per second

#include "effects/lib/jfx_common.glsl"
#include "local-wave.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    return vec4(params.color.rgb * local_wave(c.uv.x, c.anim.x), params.color.a);
}

#include "effects/lib/main_paint.glsl"
