// The waves of library:water (WAVES): their height and slope at a world XZ, shared by the
// vertex stages that raise the surface (library/water_vs.glsl) and the fragment stage that
// lights it (water.frag), so the normal fits the displaced shape. The engine has a C++ copy
// (Water::getWaves, engine/core/object/environment/Water.cpp) for the buoyancy and the Lua
// height queries, on the same clock (frame.time.x): change both together.
//
// Two sine waves: one along direction, one turned 50 degrees from it, 0.6 times as long
// and half as high; each travels at speed world units per second. Phases are in cycles,
// wrapped before the sine so a long run keeps its precision.

#ifndef WATER_GLSL
#define WATER_GLSL

const float WATER_TWO_PI = 6.28318530718;

// x height (world units), yz its slope along world X and Z
vec3 waterWaves(vec2 xz, float time, vec2 direction, float height, float waveLength, float speed){
    vec2 first = direction / max(length(direction), 1e-4);
    // turned 50 degrees
    vec2 second = vec2(first.x * 0.6428 - first.y * 0.7660, first.x * 0.7660 + first.y * 0.6428);
    float length1 = max(waveLength, 1e-3);
    float length2 = length1 * 0.6;

    float phase1 = (dot(first, xz) / length1 - fract(time * speed / length1)) * WATER_TWO_PI;
    float phase2 = (dot(second, xz) / length2 - fract(time * speed / length2) + 0.37) * WATER_TWO_PI;

    float h = height * (sin(phase1) + 0.5 * sin(phase2));
    vec2 slope = height * WATER_TWO_PI * (first * (cos(phase1) / length1) + second * (0.5 * cos(phase2) / length2));
    return vec3(h, slope);
}

#endif
