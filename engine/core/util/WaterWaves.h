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
// WaterWavesTest.GlslTwinPinned holds the SHA-256 of the GLSL functions (waterWaves, and
// waterGerstnerWave with waterGerstner): changing them in water.glsl without this file (or the
// other way round) fails it. Change both together.
//
// waterSurfaceAt is what every CPU reader evaluates (Water, WaterSystem): the waves of a
// material (WaterWaveParams) at a world XZ, for GERSTNER the surface point that the waves move
// over that XZ (Newton's method on the sideways motion, which the GPU has no need of).

#include "math/Vector2.h"
#include "math/Vector3.h"

#include <algorithm>
#include <cmath>

namespace jasperengine{

    struct WaterWaveSample{
        float height = 0.0f;        // above the rest level, world units
        Vector2 slope;              // dh/dx, dh/dz
        float heightRate = 0.0f;    // dh/dt, world units per second
        // the surface's normal there, and how the water there moves (world units per second:
        // the sines only rise and fall, (0, heightRate, 0); a Gerstner surface also rolls back
        // and forth); waterSurfaceAt fills them
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 velocity;
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

    // a point of a Gerstner surface (waterGerstner): its offset from its rest position, the
    // tangents of the surface along world X and Z there, and its velocity
    struct WaterGerstnerSample{
        Vector3 offset;
        Vector3 dPdx = Vector3(1.0f, 0.0f, 0.0f);
        Vector3 dPdz = Vector3(0.0f, 0.0f, 1.0f);
        Vector3 velocity;
    };

    // water.glsl's waterGerstnerWave (velocity added: the phase falls by 2 pi c / l a second)
    inline void waterGerstnerWave(Vector2 xz, float time, Vector2 d, float l, float a, float c, float phaseOffset, float s,
                                  WaterGerstnerSample& sample){
        const float WATER_TWO_PI = 6.28318530718f;
        // float k = WATER_TWO_PI / l;
        const float k = WATER_TWO_PI / l;
        // float phase = (dot(d, xz) / l - fract(time * c / l) + phaseOffset) * WATER_TWO_PI;
        const float dotX = d.x * xz.x;
        const float dotY = d.y * xz.y;
        const float dotDX = dotX + dotY;
        const float travel = time * c;
        const float phase = (dotDX / l - waterFract(travel / l) + phaseOffset) * WATER_TWO_PI;
        const float sinPhase = std::sin(phase);
        const float cosPhase = std::cos(phase);
        // float q = s * 0.25 / k;
        const float q = s * 0.25f / k;
        // offset += vec3(d.x * q * cosPhase, a * sinPhase, d.y * q * cosPhase);
        const float sideways = q * cosPhase;
        sample.offset = sample.offset + Vector3(d.x * sideways, a * sinPhase, d.y * sideways);
        // float w = 0.25 * s * sinPhase * weight; float h = a * k * cosPhase * weight; (weight 1)
        const float w = 0.25f * s * sinPhase;
        const float h = a * k * cosPhase;
        sample.dPdx = sample.dPdx + Vector3(-w * d.x * d.x, h * d.x, -w * d.x * d.y);
        sample.dPdz = sample.dPdz + Vector3(-w * d.x * d.y, h * d.y, -w * d.y * d.y);
        const float omega = WATER_TWO_PI * c / l;
        const float rollSpeed = q * sinPhase * omega;
        sample.velocity = sample.velocity + Vector3(d.x * rollSpeed, -a * cosPhase * omega, d.y * rollSpeed);
    }

    // water.glsl's waterGerstner (every slope weight 1): the point of the surface at rest at the
    // world xz, at a time of the shader clock
    inline WaterGerstnerSample waterGerstner(Vector2 xz, float time, Vector2 direction, float height, float waveLength, float speed,
                                             float steepness){
        // vec2 d0 = direction / max(length(direction), 1e-4);
        const float directionX2 = direction.x * direction.x;
        const float directionY2 = direction.y * direction.y;
        const float directionScale = std::max(std::sqrt(directionX2 + directionY2), 1e-4f);
        const Vector2 d0(direction.x / directionScale, direction.y / directionScale);
        // vec2 d1 = vec2(d0.x * 0.6428 - d0.y * 0.7660, d0.x * 0.7660 + d0.y * 0.6428);
        const float d1xa = d0.x * 0.6428f;
        const float d1xb = d0.y * 0.7660f;
        const float d1ya = d0.x * 0.7660f;
        const float d1yb = d0.y * 0.6428f;
        const Vector2 d1(d1xa - d1xb, d1ya + d1yb);
        // vec2 d2 = vec2(d0.x * 0.8660 + d0.y * 0.5000, d0.y * 0.8660 - d0.x * 0.5000);
        const float d2xa = d0.x * 0.8660f;
        const float d2xb = d0.y * 0.5000f;
        const float d2ya = d0.y * 0.8660f;
        const float d2yb = d0.x * 0.5000f;
        const Vector2 d2(d2xa + d2xb, d2ya - d2yb);
        // vec2 d3 = vec2(d0.x * 0.1736 - d0.y * 0.9848, d0.x * 0.9848 + d0.y * 0.1736);
        const float d3xa = d0.x * 0.1736f;
        const float d3xb = d0.y * 0.9848f;
        const float d3ya = d0.x * 0.9848f;
        const float d3yb = d0.y * 0.1736f;
        const Vector2 d3(d3xa - d3xb, d3ya + d3yb);
        const float l0 = std::max(waveLength, 1e-3f);
        const float s = std::clamp(steepness, 0.0f, 0.95f);
        WaterGerstnerSample sample;
        sample.offset = Vector3(0.0f, 0.0f, 0.0f);
        sample.velocity = Vector3(0.0f, 0.0f, 0.0f);
        waterGerstnerWave(xz, time, d0, l0, height, speed, 0.0f, s, sample);
        waterGerstnerWave(xz, time, d1, l0 * 0.6f, height * 0.5f, speed * 0.7746f, 0.37f, s, sample);
        waterGerstnerWave(xz, time, d2, l0 * 0.37f, height * 0.28f, speed * 0.6083f, 0.71f, s, sample);
        waterGerstnerWave(xz, time, d3, l0 * 0.23f, height * 0.16f, speed * 0.4796f, 0.13f, s, sample);
        return sample;
    }

    // The waves of a library:water material (Water::readWaves): WAVES, GERSTNER and their values
    struct WaterWaveParams{
        bool waves = false;
        bool gerstner = false;
        Vector2 direction = Vector2(1.0f, 0.3f);
        float height = 0.0f;
        float length = 1.0f;
        float speed = 0.0f;
        float steepness = 0.0f;

        // the most the surface leaves its rest level (world units): 1.5 heights for the sines,
        // 1.94 (the four Gerstner heights) for GERSTNER, 0 without waves
        float getBand() const{
            if (!waves)
                return 0.0f;
            return std::fabs(height) * (gerstner ? 1.94f : 1.5f);
        }
        // the most a point of the surface moves sideways (world units): steepness / 4 times the
        // sum of the four 1 / k, 0 for the sines
        float getHorizontalReach() const{
            if (!waves || !gerstner)
                return 0.0f;
            const float s = std::clamp(steepness, 0.0f, 0.95f);
            return s * 0.25f * std::max(length, 1e-3f) * 2.2f / 6.28318530718f;
        }
    };

    // The surface the waves make over a world xz at a time of the shader clock: its height above
    // the rest level, slope, normal and the water's velocity there. GERSTNER finds the rest point
    // the waves move over xz by Newton's method on its sideways motion (at most 6 steps, from xz;
    // the 2x2 Jacobian of the motion; a plain fixed-point step where it is nearly singular).
    inline WaterWaveSample waterSurfaceAt(const WaterWaveParams& params, Vector2 xz, float time){
        WaterWaveSample result;
        if (!params.waves)
            return result;
        if (!params.gerstner){
            result = waterWaves(xz, time, params.direction, params.height, params.length, params.speed);
            // as water.frag bends the flat surface's normal: (0, 1, 0) - (slope x, 0, slope z)
            result.normal = Vector3(-result.slope.x, 1.0f, -result.slope.y).normalized();
            result.velocity = Vector3(0.0f, result.heightRate, 0.0f);
            return result;
        }

        const float tolerance = 1e-5f * std::max(params.length, 1e-3f);
        Vector2 rest = xz;
        WaterGerstnerSample point = waterGerstner(rest, time, params.direction, params.height, params.length, params.speed, params.steepness);
        for (int step = 0; step < 6; step++){
            const Vector2 residual(rest.x + point.offset.x - xz.x, rest.y + point.offset.z - xz.y);
            if (residual.x * residual.x + residual.y * residual.y <= tolerance * tolerance)
                break;
            // the sideways map's Jacobian: columns dPdx.xz and dPdz.xz
            const float a = point.dPdx.x, b = point.dPdz.x, c = point.dPdx.z, d = point.dPdz.z;
            const float det = a * d - b * c;
            if (det > 1e-4f){
                rest.x -= (d * residual.x - b * residual.y) / det;
                rest.y -= (a * residual.y - c * residual.x) / det;
            }else{
                rest.x -= residual.x;
                rest.y -= residual.y;
            }
            point = waterGerstner(rest, time, params.direction, params.height, params.length, params.speed, params.steepness);
        }

        Vector3 normal = point.dPdz.crossProduct(point.dPdx);
        if (normal.y <= 1e-6f)
            normal = Vector3(0.0f, 1.0f, 0.0f);
        result.normal = normal.normalized();
        result.height = point.offset.y;
        result.slope = Vector2(-result.normal.x / result.normal.y, -result.normal.z / result.normal.y);
        result.velocity = point.velocity;
        result.heightRate = point.velocity.y;
        return result;
    }

}

#endif //WATERWAVES_H
