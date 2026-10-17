#version 450
// @effect     lib-test
// @kind       paint
// @category   noise
// @summary    EffectLibTest: the include library evaluated at fixed inputs, one case per pixel
// @cost       low

// Pixel (i, row) of a 32 x 3 area holds case i of a row; the inputs are exact in f32 (multiples
// of powers of two), so the bit-cast hashes see the bits tools/htmlui/kit_reference.py sees.
//   row 0: R hash22(p).x, G hash22(p).y, B perlin12(p)
//   row 1: R mx_noise3(q) * 0.5 + 0.5, G fbm_perlin(p, 4), B linear_to_srgb(i / 32)
//   row 2: R oklab(rgb).L, G oklab(rgb).a + 0.5, B oklab(rgb).b + 0.5
// Each value v is written so that the target holds v (sRGB of the linear value of v).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"

vec4 jfx_paint(JfxContext c){
    float i = floor(c.px.x);
    float row = floor(c.px.y);
    vec2 p = vec2(i * 0.5 - 7.25, i * 0.25 + 0.375);
    vec3 q = vec3(i * 0.5 - 7.25, i * 0.25 + 0.375, i * 0.125 - 1.0625);
    vec3 v;
    if (row < 0.5){
        vec2 h = jfx_hash22(p);
        v = vec3(h.x, h.y, jfx_perlin12(p));
    }else if (row < 1.5){
        v = vec3(jfx_mx_noise3(q) * 0.5 + 0.5, jfx_fbm_perlin(p, 4.0), jfx_linear_to_srgb(vec3(i / 32.0)).x);
    }else{
        vec3 rgb = vec3(fract(i * 0.375), fract(i * 0.625), fract(i * 0.8125));
        vec3 lab = jfx_rgb_to_oklab(rgb);
        v = vec3(lab.x, lab.y + 0.5, lab.z + 0.5);
    }
    return vec4(jfx_srgb_to_linear(clamp(v, 0.0, 1.0)), 1.0);
}

#include "effects/lib/main_paint.glsl"
