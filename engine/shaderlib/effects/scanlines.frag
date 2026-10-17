#version 450
// @effect     scanlines
// @kind       filter
// @category   stylize
// @summary    Horizontal scanlines over the content, optionally scrolling and flickering
// @source     original (CRTScreen's scanline term)
// @cost       low
// @time       speed
// @param      intensity number 0.3 0 1 -- How dark the lines get
// @param      spacing length 3px 1px 16px -- Distance between two lines
// @param      thickness number 0.5 0.1 1 -- Share of a period the dark line takes (0.5 is a sine wave)
// @param      speed number 0 -100 100 -- Scroll speed of the lines, CSS px per second (down when positive)
// @param      flicker number 0 0 1 -- Brightness flicker of the whole screen

// The line term follows the shaders.com kit's CRT scanlineShade (motionBlur.ts; MIT, Copyright
// (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt):
// 1 - intensity x (sin(2 pi y / period) x 0.5 + 0.5), with the period in CSS px. `thickness`
// stretches the dark half of the wave (0.5 is the kit's sine); `speed` scrolls the lines on the
// integrated clock; `flicker` dims the whole content by a hashed amount 24 times a second.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_hash.glsl"

uniform u_fs_effectParams {
    float intensity;
    float spacing;
    float thickness;
    float speed;
    float flicker;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 s = jfx_decode(jfx_src_premul_here());
    float spacing = max(params.spacing, 0.5);
    // 0 at a line's centre, 0.5 halfway between two lines
    float x = abs(fract((c.px.y - c.anim.x) / spacing) - 0.5);
    // the thickness moves the wave's half-height point from a quarter period to thickness / 2
    float halfT = clamp(params.thickness, 0.1, 1.0) * 0.5;
    float warped = x < halfT ? x * 0.25 / halfT : 0.25 + (x - halfT) * 0.25 / max(0.5 - halfT, 1e-4);
    float line = 0.5 + 0.5 * cos(JFX_TAU * warped);
    float shade = 1.0 - params.intensity * line;
    // flicker: one hashed dimming per 1/24 s
    shade *= 1.0 - params.flicker * 0.25 * jfx_hash11(floor(c.time * 24.0) + 0.5);
    return vec4(s.rgb * shade, s.a);
}

#include "effects/lib/main_filter.glsl"
