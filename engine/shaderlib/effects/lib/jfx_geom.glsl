// HTML UI effects: geometry helpers. jfx_fmod is the shaders.com kit's geom.ts flooredMod1
// (MIT, Copyright (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt).
#ifndef JFX_GEOM_GLSL
#define JFX_GEOM_GLSL

vec2 jfx_rotate(vec2 p, float radians){
    float c = cos(radians);
    float s = sin(radians);
    return vec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

// (radius, angle in radians)
vec2 jfx_to_polar(vec2 p){
    return vec2(length(p), atan(p.y, p.x));
}

vec2 jfx_from_polar(vec2 rt){
    return vec2(cos(rt.y), sin(rt.y)) * rt.x;
}

// floored modulo: [0, m) for a positive m, negative x included (not WGSL's truncated %)
float jfx_fmod(float x, float m){
    return x - m * floor(x / m);
}

vec2 jfx_fmod(vec2 x, vec2 m){
    return x - m * floor(x / m);
}

// a uv beyond 0..1 brought back by an edge mode: 0 stretch (clamp), 1 transparent (as is:
// jfx_edge_alpha is 0 outside), 2 mirror, 3 wrap
vec2 jfx_edge_uv(vec2 uv, float mode){
    if (mode < 0.5)
        return clamp(uv, 0.0, 1.0);
    if (mode < 1.5)
        return uv;
    if (mode < 2.5)
        return 1.0 - abs(1.0 - jfx_fmod(uv, vec2(2.0)));
    return fract(uv);
}

// 0 outside 0..1 in the transparent mode, else 1
float jfx_edge_alpha(vec2 uv, float mode){
    if (mode < 0.5 || mode > 1.5)
        return 1.0;
    vec2 inside = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return inside.x * inside.y;
}

#endif // JFX_GEOM_GLSL
