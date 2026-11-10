// What the Lines generators' shaders share, ported from Pachislop src/generators/lines/shared.ts
// (TAPER_GLSL) by SafEight, used with the author's permission.

#ifndef TITLEART_GEN_LINES_COMMON_GLSL
#define TITLEART_GEN_LINES_COMMON_GLSL

// the width factor 0..1 at t along a line whose ends grow from a point over the fractions
// `tail` (t = 0) and `head` (t = 1) of its length (generators/LineGenerators.cpp taperFractions)
float ln_taperAt(float t, float tail, float head) {
    float w = 1.0;
    if (tail > 0.0) w = min(w, t / tail);
    if (head > 0.0) w = min(w, (1.0 - t) / head);
    return clamp(w, 0.0, 1.0);
}

#endif // TITLEART_GEN_LINES_COMMON_GLSL
