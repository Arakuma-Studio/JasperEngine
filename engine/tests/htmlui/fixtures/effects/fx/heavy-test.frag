#version 450
// @effect     heavy-test
// @kind       paint
// @category   energy
// @summary    A high-cost paint effect of the tests
// @cost       high
// @param      color color #ff8800 -- Colour

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
} params;

vec4 jfx_paint(JfxContext c){
    return params.color;
}

#include "effects/lib/main_paint.glsl"
