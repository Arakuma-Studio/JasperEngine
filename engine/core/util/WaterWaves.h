// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATERWAVES_H
#define WATERWAVES_H

// The CPU twin of waterWaves in engine/shaderlib/library/water.glsl, the waves library:water
// draws with WAVES: a line-for-line float32 translation (the same constants, the same order of
// operations, GLSL's fract as x - floor(x), the same max guards), so what floats on the water
// (Buoyancy, Water.sample) rides the waves on screen. Each product of a dot product is its own
// statement, so a compiler that fuses within an expression (clang's FP_CONTRACT on) computes
// what the GPU's plain dot does on every target.
//
// WaterWavesTest.GlslTwinPinned holds the SHA-256 of the GLSL function: changing water.glsl's
// waterWaves without this file (or the other way round) fails it. Change both together.

#include "math/Vector2.h"

#include <algorithm>
#include <cmath>

namespace jasperengine{

    struct WaterWaveSample{
        float height = 0.0f;        // above the rest level, world units
        Vector2 slope;              // dh/dx, dh/dz
        float heightRate = 0.0f;    // dh/dt, world units per second
    };

    inline float waterFract(float x){
        return x - std::floor(x);
    }

    // xz: world position, time: the shader clock (Engine::getShaderTime), direction: the
    // material's waveDirection, height/waveLength/speed: waveHeight/waveLength/waveSpeed
    inline WaterWaveSample waterWaves(Vector2 xz, float time, Vector2 direction, float height, float waveLength, float speed){
        const float WATER_TWO_PI = 6.28318530718f;

        // vec2 first = direction / max(length(direction), 1e-4);
        const float directionX2 = direction.x * direction.x;
        const float directionY2 = direction.y * direction.y;
        const float directionLength = std::sqrt(directionX2 + directionY2);
        const float directionScale = std::max(directionLength, 1e-4f);
        const Vector2 first(direction.x / directionScale, direction.y / directionScale);
        // turned 50 degrees
        // vec2 second = vec2(first.x * 0.6428 - first.y * 0.7660, first.x * 0.7660 + first.y * 0.6428);
        const float secondXa = first.x * 0.6428f;
        const float secondXb = first.y * 0.7660f;
        const float secondYa = first.x * 0.7660f;
        const float secondYb = first.y * 0.6428f;
        const Vector2 second(secondXa - secondXb, secondYa + secondYb);
        // float length1 = max(waveLength, 1e-3);
        const float length1 = std::max(waveLength, 1e-3f);
        // float length2 = length1 * 0.6;
        const float length2 = length1 * 0.6f;

        // float phase1 = (dot(first, xz) / length1 - fract(time * speed / length1)) * WATER_TWO_PI;
        const float dot1x = first.x * xz.x;
        const float dot1y = first.y * xz.y;
        const float dot1 = dot1x + dot1y;
        const float travel = time * speed;
        const float phase1 = (dot1 / length1 - waterFract(travel / length1)) * WATER_TWO_PI;
        // float phase2 = (dot(second, xz) / length2 - fract(time * speed / length2) + 0.37) * WATER_TWO_PI;
        const float dot2x = second.x * xz.x;
        const float dot2y = second.y * xz.y;
        const float dot2 = dot2x + dot2y;
        const float phase2 = (dot2 / length2 - waterFract(travel / length2) + 0.37f) * WATER_TWO_PI;

        // float h = height * (sin(phase1) + 0.5 * sin(phase2));
        const float sin1 = std::sin(phase1);
        const float sin2 = std::sin(phase2);
        const float halfSin2 = 0.5f * sin2;
        WaterWaveSample sample;
        sample.height = height * (sin1 + halfSin2);

        // vec2 slope = height * WATER_TWO_PI * (first * (cos(phase1) / length1) + second * (0.5 * cos(phase2) / length2));
        const float cos1 = std::cos(phase1);
        const float cos2 = std::cos(phase2);
        const float c1 = cos1 / length1;
        const float c2 = 0.5f * cos2 / length2;
        const float amplitude = height * WATER_TWO_PI;
        const float slopeX1 = first.x * c1;
        const float slopeX2 = second.x * c2;
        const float slopeZ1 = first.y * c1;
        const float slopeZ2 = second.y * c2;
        sample.slope = Vector2(amplitude * (slopeX1 + slopeX2), amplitude * (slopeZ1 + slopeZ2));

        // d/dt of h: each phase falls by WATER_TWO_PI * speed / length a second
        const float rate1 = cos1 * (WATER_TWO_PI * speed / length1);
        const float rate2 = 0.5f * cos2 * (WATER_TWO_PI * speed / length2);
        sample.heightRate = -height * (rate1 + rate2);
        return sample;
    }

}

#endif //WATERWAVES_H
