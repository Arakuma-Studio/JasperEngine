#version 450
// @effect     glints-test
// @kind       paint
// @category   light
// @summary    A paint effect of the tests, the counterpart of sparkle-test
// @cost       low
// @suggest-for sparkle-test
// @param      color color #ffffff -- Colour

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
} params;

vec4 jfx_paint(JfxContext c){
    return vec4(params.color.rgb, params.color.a * step(0.5, fract(c.uv.x * 4.0)));
}

#include "effects/lib/main_paint.glsl"
