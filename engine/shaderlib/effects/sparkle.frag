#version 450
// @effect     sparkle
// @kind       filter
// @category   light
// @summary    Twinkling star glints over the bright parts of the content
// @source     shaders.com Sparkle
// @cost       medium
// @time       speed
// @param      size number 100 10 200 -- Spacing between glints in CSS px (smaller gives more glints)
// @param      intensity number 10 0 20 -- Brightness of the glints
// @param      threshold number 0.2 0 1 -- How bright the content must be for a glint (0 all but black, 1 only white)
// @param      expand number 0 0 100 -- Lets glints appear up to this many CSS px outside the bright parts
// @param      ray-length number 5 0 20 -- Length of the four rays around a glint
// @param      colorize number 0.45 0 1 -- How much a glint takes the colour beneath it (0 pure white)
// @param      speed number 1 0 5 -- Speed of the twinkle
// @param      seed number 0 0 100 -- Re-rolls where the glints sit and when they blink

// Ported from shaders.com Sparkle (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (two scatterPoints grids, the second at 2/3 the spacing; brightestNear, flashes, starGlint;
// the glints added as light over the content, its coverage grown by the brightest glint
// channel). The compile-time expand (4 ring taps when above 0) is a runtime branch.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    float size;
    float intensity;
    float threshold;
    float expand;
    float rayLength;
    float colorize;
    float speed;
    float seed;
} params;

const vec3 SPARKLE_LUMA = vec3(0.2126, 0.7152, 0.0722);

// brightestNear: the source at the point, or the brightest of a ring of 4 taps within expand of it
vec4 sparkle_brightest_near(vec2 point, float layerAngle, out float luma){
    vec4 color = jfx_src(point);
    luma = dot(color.rgb, SPARKLE_LUMA) * color.a;
    if (params.expand > 0.0){
        for (int i = 0; i < 4; i++){
            float a = layerAngle + JFX_TAU * float(i) / 4.0;
            vec4 tap = jfx_src(point + vec2(cos(a), sin(a)) * params.expand);
            float tapLuma = dot(tap.rgb, SPARKLE_LUMA) * tap.a;
            color = tapLuma > luma ? tap : color;
            luma = max(tapLuma, luma);
        }
    }
    return color;
}

// one grid of glints over the four nearest cells
vec3 sparkle_layer(vec2 px, float gridScale, float layer, float t){
    float cellSize = params.size * gridScale;
    float rayRate = 0.45 / max(params.rayLength, 0.01);
    float seed = params.seed + layer * 0.31;
    vec2 salt = vec2(seed * 97.13, seed * 31.71);
    vec2 base = floor(px / cellSize - 0.5);
    vec3 sum = vec3(0.0);
    for (int k = 0; k < 4; k++){
        vec2 cell = base + vec2(float(k & 1), float(k >> 1));
        vec3 random = jfx_hash32(cell + salt);
        vec2 point = (cell + 0.5 + (random.xy - 0.5) * 0.6) * cellSize;
        vec2 offset = px - point;
        float luma;
        vec4 source = sparkle_brightest_near(point, radians(layer * 45.0), luma);
        float presence = smoothstep(params.threshold, params.threshold + 0.12, sqrt(luma));
        // flashes: max(sin(t * rate + offset), 0)^8
        float flash = max(sin(t * (0.8 + random.z * 1.2) + random.x * 6.283185307179586), 0.0);
        flash *= flash;
        flash *= flash;
        flash *= flash;
        float level = 0.35 + 0.65 * step(0.45, random.z);
        // starGlint: a gaussian core and two exponential streaks, gone by half a cell
        float ax = abs(offset.x);
        float ay = abs(offset.y);
        float star = exp(-(offset.x * offset.x + offset.y * offset.y) * 0.9) +
                     (exp(-(ax * 1.6 + ay * rayRate)) + exp(-(ay * 1.6 + ax * rayRate))) * 0.25;
        star *= 1.0 - smoothstep(cellSize * 0.5 * 0.6, cellSize * 0.5, max(ax, ay));
        vec3 rgb = source.rgb;
        vec3 hue = rgb / max(dot(rgb, SPARKLE_LUMA), 0.001) * 0.6;
        vec3 tint = mix(vec3(1.0), hue, params.colorize);
        sum += tint * (star * flash * presence * level * params.intensity);
    }
    return sum;
}

vec4 jfx_filter(JfxContext c){
    float t = c.anim.x;
    vec3 glow = sparkle_layer(c.px, 1.0, 0.0, t) + sparkle_layer(c.px, 2.0 / 3.0, 1.0, t);
    // the content plus the glints as light, premultiplied
    vec4 child = jfx_decode(jfx_src_premul_here());
    vec3 premul = child.rgb * child.a + glow;
    float spill = max(max(glow.r, glow.g), glow.b);
    float alpha = min(child.a + spill, 1.0);
    return vec4(alpha > 1e-5 ? premul / alpha : vec3(0.0), alpha);
}

#include "effects/lib/main_filter.glsl"
