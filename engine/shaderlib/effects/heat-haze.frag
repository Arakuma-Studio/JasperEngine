#version 450
// @effect     heat-haze
// @kind       filter
// @category   distort
// @summary    Shimmering heat haze: the content wavers in noise rising through it
// @source     original (FlowField-derived)
// @cost       low
// @time       speed
// @suggest-for heat shimmer-distort mirage
// @param      strength length 3px 0px 32px -- How far the content wavers
// @param      scale number 1 0.25 4 -- Scale of the shimmer (larger is finer)
// @param      speed number 1 0 5 -- Speed the shimmer rises
// @param      direction angle 0deg -180deg 180deg -- Direction the heat rises (0deg up, clockwise)

// After the shaders.com kit's FlowField (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): two MaterialX noise layers (cells of 40 / scale CSS px and
// 2.3 times finer) scroll along the rising direction at 40 px per second times the integrated
// `speed` clock while they evolve; their sum pushes the content across the rise (and a third as
// much along it) by up to `strength` px.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    float strength;
    float scale;
    float speed;
    float direction;
} params;

vec4 jfx_filter(JfxContext c){
    vec2 rise = vec2(sin(params.direction), -cos(params.direction));
    vec2 across = vec2(-rise.y, rise.x);
    float t = c.anim.x;
    vec2 p = (c.px - rise * (t * 40.0)) * (params.scale / 40.0);
    float n1 = jfx_mx_noise3(vec3(p, t * 0.5));
    float n2 = jfx_mx_noise3(vec3(p * 2.3 + vec2(173.5, 291.7), t * 0.8 + 50.0));
    float wave = n1 + 0.5 * n2;
    vec2 disp = (across * wave + rise * (n2 * 0.33)) * params.strength;
    return fam_src_clamped(c.px + disp);
}

#include "effects/lib/main_filter.glsl"
