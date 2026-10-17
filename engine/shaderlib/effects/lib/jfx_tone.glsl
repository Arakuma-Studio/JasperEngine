// HTML UI effects: tone and dithering helpers. jfx_grain follows the shaders.com kit's
// colorOps.ts filmGrain hash (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt).
#ifndef JFX_TONE_GLSL
#define JFX_TONE_GLSL

#include "effects/lib/jfx_hash.glsl"

// ordered dithering thresholds in [0, 1) of an integer cell (floor(px / pixelSize))
float jfx_bayer2(vec2 cell){
    cell = floor(cell);
    return fract(cell.x * 0.5 + cell.y * cell.y * 0.75);
}

float jfx_bayer4(vec2 cell){
    return jfx_bayer2(floor(cell * 0.5)) * 0.25 + jfx_bayer2(cell);
}

float jfx_bayer8(vec2 cell){
    return jfx_bayer4(floor(cell * 0.5)) * 0.25 + jfx_bayer2(cell);
}

// levels steps per channel (colorOps.ts posterize: floor(c * s) / s, s in [2, 20])
vec3 jfx_posterize(vec3 c, float levels){
    float s = clamp(levels, 2.0, 20.0);
    return floor(c * s) / s;
}

// contrast around mid grey: 1 unchanged
vec3 jfx_contrast(vec3 c, float k){
    return (c - 0.5) * k + 0.5;
}

// signed grain in [-1, 1] of a CSS px position, drifting with t when animated is 1
float jfx_grain(vec2 px, float t, float animated){
    float drift = t * animated;
    return jfx_hash12(floor(px) + vec2(drift, drift * 1.6180339)) * 2.0 - 1.0;
}

#endif // JFX_TONE_GLSL
