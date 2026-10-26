// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Copied from OpenEngine's Engine/Modules/Noise/Include/Noise/GradientCurl3D.h, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License (see NOTICE.md); the vector
// type is a plain float[3] here and the namespace is ours. The operations and their order are
// unchanged: this scalar version is the reference (and golden source) of the 4-wide one in
// GradientCurl3DSimd.h, which the particle Noise module runs.

#ifndef NOISE_GRADIENTCURL3D_H
#define NOISE_GRADIENTCURL3D_H

#include <cmath>
#include <cstdint>

// ---- Curl of 3D gradient noise ----------------------------------------------
//
// A divergence-free vector field for turbulence (particles, wind): the curl of
// three seeded gradient-noise potentials, one per axis. The potentials use
// quintic fade and an integer hash per lattice corner; their analytic
// derivatives give the curl without finite differences. A sample is a pure
// function of the point and the seed.

namespace jasperengine { namespace noise {

    struct Curl3 {
        float v[3] = {0.0f, 0.0f, 0.0f};
        float& operator[](uint32_t i){ return v[i]; }
        const float& operator[](uint32_t i) const { return v[i]; }
    };

    namespace gradientcurl3d {

        // Lattice coordinates wrap every this many cells, which keeps the float-to-integer
        // conversion defined at any distance from the origin.
        inline constexpr float kPeriod = 256.0f;
        inline constexpr uint32_t kAxisPrimes[3] = {0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u};

        inline uint32_t hash(uint32_t x){
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            return x ^ (x >> 16);
        }

        // The gradient of one seeded potential at `point`, in lattice units.
        inline Curl3 potentialGradient(const Curl3& point, uint32_t seed){
            uint32_t cell[3]{};
            Curl3 fraction, fade, fadeSlope;
            for (uint32_t axis = 0; axis < 3; ++axis){
                float wrapped = std::fmod(point[axis], kPeriod);
                if (wrapped < 0.0f)
                    wrapped += kPeriod;
                const float whole = std::floor(wrapped);
                cell[axis] = static_cast<uint32_t>(whole);
                const float t = wrapped - whole;
                fraction[axis] = t;
                fade[axis] = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
                fadeSlope[axis] = 30.0f * t * t * (t - 1.0f) * (t - 1.0f);
            }
            Curl3 result;
            for (uint32_t corner = 0; corner < 8; ++corner){
                uint32_t h = seed;
                Curl3 delta, weights, slopes;
                for (uint32_t axis = 0; axis < 3; ++axis){
                    const uint32_t upper = (corner >> axis) & 1u;
                    h ^= ((cell[axis] + upper) & 255u) * kAxisPrimes[axis];
                    delta[axis] = fraction[axis] - static_cast<float>(upper);
                    weights[axis] = upper ? fade[axis] : 1.0f - fade[axis];
                    slopes[axis] = upper ? fadeSlope[axis] : -fadeSlope[axis];
                }
                h = hash(h);
                // One of the twelve edge-midpoint gradients: a zero axis picked by the hash, signs by its bits.
                const uint32_t absent = h % 3u;
                uint32_t sign = h >> 2;
                Curl3 gradient;
                for (uint32_t axis = 0; axis < 3; ++axis){
                    if (axis != absent){
                        gradient[axis] = (sign & 1u) ? 1.0f : -1.0f;
                        sign >>= 1;
                    }
                }
                const float dot = gradient[0] * delta[0] + gradient[1] * delta[1] + gradient[2] * delta[2];
                const float weight = weights[0] * weights[1] * weights[2];
                for (uint32_t axis = 0; axis < 3; ++axis){
                    float weightSlope = slopes[axis];
                    for (uint32_t other = 0; other < 3; ++other){
                        if (other != axis)
                            weightSlope *= weights[other];
                    }
                    result[axis] += gradient[axis] * weight + dot * weightSlope;
                }
            }
            return result;
        }

    }

    // The curl of three seeded gradient-noise potentials at `point`, in lattice units: one lattice
    // cell per unit, so a caller sampling a world position scales it by the field's frequency first.
    // The field is divergence-free, continuous across the 256-cell wrap, and its magnitude does not
    // depend on the frequency the caller chose. A non-finite point gives zero.
    inline Curl3 gradientCurl3D(const Curl3& point, uint32_t seed){
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2]))
            return {};
        Curl3 potential[3];
        for (uint32_t component = 0; component < 3; ++component){
            // Each potential reads the point with its axes rotated, so the three are independent.
            Curl3 rotated;
            for (uint32_t axis = 0; axis < 3; ++axis)
                rotated[axis] = point[(component + axis + 1) % 3];
            const Curl3 gradient = gradientcurl3d::potentialGradient(rotated, gradientcurl3d::hash(seed + component * 0x9e3779b9u));
            for (uint32_t axis = 0; axis < 3; ++axis)
                potential[component][(component + axis + 1) % 3] = gradient[axis];
        }
        Curl3 curl;
        curl[0] = potential[2][1] - potential[1][2];
        curl[1] = potential[0][2] - potential[2][0];
        curl[2] = potential[1][0] - potential[0][1];
        return curl;
    }

}}

#endif // NOISE_GRADIENTCURL3D_H
