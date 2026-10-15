#version 450
// @effect     glints
// @kind       paint
// @category   light
// @summary    Scattered star glints twinkling over the element
// @source     original (paint counterpart of Sparkle)
// @cost       medium
// @time       twinkle-speed
// @suggest-for sparkles sparkle twinkle
// @param      color color #ffffff -- Colour of the glints
// @param      density number 20 1 200 -- Glints per 10 000 CSS px² (per 100 x 100 px)
// @param      size length 8px 2px 64px -- Length of a glint's rays
// @param      rays number 1 0 1 -- Brightness of the rays next to the core
// @param      twinkle-speed number 1 0 5 -- Speed of the twinkle
// @param      colorize number 0.3 0 1 -- How much each glint takes a hue of its own (0 all the colour)
// @param      seed number 0 0 100 -- Re-rolls where the glints sit and when they blink

// The glints of shaders.com Sparkle (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt) as a paint: one scattered point per square cell (the cell's
// area is 10 000 / density px², the four nearest cells read), each a star (a gaussian core and
// two exponential streaks along the axes, faded out by half a cell) blinking at its own rate
// (flashes: max(sin, 0)^8, rates 0.8 to 2 rad/s) at one of two levels. Without content beneath
// to colour it, a glint's tint is a hue of its own (a cosine rainbow) mixed in by colorize.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float density;
    float size;
    float rays;
    float twinkleSpeed;
    float colorize;
    float seed;
} params;

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x;
    float cellSize = sqrt(10000.0 / max(params.density, 1e-3));
    float size = max(params.size, 1.0);
    // star falloffs in px for a glint of `size`: Sparkle's 8 px look scaled
    float s = 8.0 / size;
    float core = 0.9 * s * s;
    float rayWidth = 1.6 * s;
    float rayRate = 3.0 / size;
    vec2 salt = vec2(params.seed * 97.13, params.seed * 31.71);
    vec2 base = floor(c.px / cellSize - 0.5);
    vec3 rgb = vec3(0.0);
    float total = 0.0;
    for (int k = 0; k < 4; k++){
        vec2 cell = base + vec2(float(k & 1), float(k >> 1));
        vec3 random = jfx_hash32(cell + salt);
        vec2 point = (cell + 0.5 + (random.xy - 0.5) * 0.6) * cellSize;
        vec2 offset = c.px - point;
        float flash = max(sin(t * (0.8 + random.z * 1.2) + random.x * 6.283185307179586), 0.0);
        flash *= flash;
        flash *= flash;
        flash *= flash;
        float level = 0.35 + 0.65 * step(0.45, random.z);
        float ax = abs(offset.x);
        float ay = abs(offset.y);
        float star = exp(-(offset.x * offset.x + offset.y * offset.y) * core) +
                     (exp(-(ax * rayWidth + ay * rayRate)) + exp(-(ay * rayWidth + ax * rayRate))) * 0.25 * params.rays;
        star *= 1.0 - smoothstep(cellSize * 0.3, cellSize * 0.5, max(ax, ay));
        float hue = fract(random.y * 7.31 + random.z);
        vec3 tint = vec3(cos(hue * JFX_TAU), cos((hue + 1.0 / 3.0) * JFX_TAU), cos((hue + 2.0 / 3.0) * JFX_TAU)) * 0.5 + 0.5;
        float glint = star * flash * level;
        rgb += mix(params.color.rgb, params.color.rgb * tint, params.colorize) * glint;
        total += glint;
    }
    float alpha = clamp(total, 0.0, 1.0);
    return vec4(total > 1e-5 ? rgb / total : params.color.rgb, alpha * params.color.a);
}

#include "effects/lib/main_paint.glsl"
