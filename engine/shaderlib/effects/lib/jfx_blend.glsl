// HTML UI effects: blend modes on straight colours, after the shaders.com kit's blend.ts (MIT,
// Copyright (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt).
#ifndef JFX_BLEND_GLSL
#define JFX_BLEND_GLSL

// top blended over base: 0 normal, 1 multiply, 2 screen, 3 overlay, 4 soft-light (Pegtop),
// 5 add, 6 difference, 7 color-dodge. The result replaces base where the top layer covers it.
vec3 jfx_blend(vec3 base, vec3 top, float mode){
    if (mode < 0.5)
        return top;
    if (mode < 1.5)
        return base * top;
    if (mode < 2.5)
        return 1.0 - (1.0 - base) * (1.0 - top);
    if (mode < 3.5)
        return mix(2.0 * base * top, 1.0 - 2.0 * (1.0 - base) * (1.0 - top), step(vec3(0.5), base));
    if (mode < 4.5){
        vec3 dark = 2.0 * base * top + base * base * (1.0 - 2.0 * top);
        vec3 light = 2.0 * base * (1.0 - top) + sqrt(max(base, 0.0)) * (2.0 * top - 1.0);
        return mix(dark, light, step(vec3(0.5), top));
    }
    if (mode < 5.5)
        return base + top;
    if (mode < 6.5)
        return abs(base - top);
    vec3 oneMinusTop = 1.0 - top;
    return mix(base, min(base / max(oneMinusTop, vec3(1e-4)), vec3(1.0)), step(vec3(0.001), oneMinusTop));
}

#endif // JFX_BLEND_GLSL
