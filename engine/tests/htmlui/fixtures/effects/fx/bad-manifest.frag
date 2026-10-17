#version 450
// @effect     bad-manifest
// @kind       paint
// @category   gradient
// @param      amount number 2 0 1 -- A default outside its range

#include "effects/lib/jfx_common.glsl"
vec4 jfx_paint(JfxContext c){ return vec4(1.0); }
#include "effects/lib/main_paint.glsl"
