// The waves of library:water (WAVES): their height and slope at a world XZ, shared by the
// vertex stages that raise the surface (library/water_vs.glsl) and the fragment stage that
// lights it (water.frag), so the normal fits the displaced shape. The engine has a C++ copy
// (Water::getWaves, engine/core/object/environment/Water.cpp) for the buoyancy and the Lua
// height queries, on the same clock (the engine-written sceneTime, Scene::getSceneTime): change
// both together.
//
// Two sine waves: one along direction, one turned 50 degrees from it, 0.6 times as long
// and half as high; each travels at speed world units per second. Phases are in cycles,
// wrapped before the sine so a long run keeps its precision.
//
// GERSTNER: four Gerstner waves instead (waterGerstner): along direction, turned +50, -30 and
// +80 degrees, 1, 0.6, 0.37 and 0.23 times as long, 1, 0.5, 0.28 and 0.16 times as high, each
// as fast as the square root of its share of the length (deep water: shorter is slower), at
// phase offsets 0, 0.37, 0.71 and 0.13 cycles. A point of the surface at rest at xz moves up by
// height * sin(phase) and forward along the wave by steepness / (4 k) * cos(phase) (k = 2 pi /
// its length), so the four sideways motions together never fold the surface while the
// steepness (clamped to 0.95) is under 1: the crests sharpen, the troughs flatten. The C++
// twin also inverts it (the surface over a world xz: util/WaterWaves.h waterSurfaceAt).

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

// one Gerstner wave of waterGerstner: d its unit direction, l its length, a its height, c its
// speed, phaseOffset in cycles, s the steepness, weight its share of the tangents
void waterGerstnerWave(vec2 xz, float time, vec2 d, float l, float a, float c, float phaseOffset, float s,
                       float weight, inout vec3 offset, inout vec3 dPdx, inout vec3 dPdz){
    float k = WATER_TWO_PI / l;
    float phase = (dot(d, xz) / l - fract(time * c / l) + phaseOffset) * WATER_TWO_PI;
    float sinPhase = sin(phase);
    float cosPhase = cos(phase);
    float q = s * 0.25 / k;
    offset += vec3(d.x * q * cosPhase, a * sinPhase, d.y * q * cosPhase);
    float w = 0.25 * s * sinPhase * weight;
    float h = a * k * cosPhase * weight;
    dPdx += vec3(-w * d.x * d.x, h * d.x, -w * d.x * d.y);
    dPdz += vec3(-w * d.x * d.y, h * d.y, -w * d.y * d.y);
}

// the world offset of the surface point at rest at xz, and its tangents along world X and Z
// (the normal is cross(dPdz, dPdx)); slopeWeights scale each wave's share of the tangents (the
// fragment stage's distance fade; 1 elsewhere)
vec3 waterGerstner(vec2 xz, float time, vec2 direction, float height, float waveLength, float speed,
                   float steepness, vec4 slopeWeights, out vec3 dPdx, out vec3 dPdz){
    vec2 d0 = direction / max(length(direction), 1e-4);
    vec2 d1 = vec2(d0.x * 0.6428 - d0.y * 0.7660, d0.x * 0.7660 + d0.y * 0.6428);
    vec2 d2 = vec2(d0.x * 0.8660 + d0.y * 0.5000, d0.y * 0.8660 - d0.x * 0.5000);
    vec2 d3 = vec2(d0.x * 0.1736 - d0.y * 0.9848, d0.x * 0.9848 + d0.y * 0.1736);
    float l0 = max(waveLength, 1e-3);
    float s = clamp(steepness, 0.0, 0.95);
    vec3 offset = vec3(0.0);
    dPdx = vec3(1.0, 0.0, 0.0);
    dPdz = vec3(0.0, 0.0, 1.0);
    waterGerstnerWave(xz, time, d0, l0, height, speed, 0.0, s, slopeWeights.x, offset, dPdx, dPdz);
    waterGerstnerWave(xz, time, d1, l0 * 0.6, height * 0.5, speed * 0.7746, 0.37, s, slopeWeights.y, offset, dPdx, dPdz);
    waterGerstnerWave(xz, time, d2, l0 * 0.37, height * 0.28, speed * 0.6083, 0.71, s, slopeWeights.z, offset, dPdx, dPdz);
    waterGerstnerWave(xz, time, d3, l0 * 0.23, height * 0.16, speed * 0.4796, 0.13, s, slopeWeights.w, offset, dPdx, dPdz);
    return offset;
}

#endif
