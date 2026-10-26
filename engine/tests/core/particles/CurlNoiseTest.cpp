// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The reference samples and the field's properties are OpenEngine's
// Engine/Modules/Noise/Tests/GradientCurl3DTests.cpp, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License (see NOTICE.md), ported; the SIMD comparisons are ours.

#include "math/noise/CurlNoiseBatch.h"
#include "math/noise/GradientCurl3D.h"
#include "math/noise/GradientCurl3DSimd.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::noise;

namespace {

    Curl3 point(float x, float y, float z){
        Curl3 p;
        p[0] = x; p[1] = y; p[2] = z;
        return p;
    }

    float length(const Curl3& v){ return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

    const Curl3 kPoint = point(0.285f, 0.045f, 0.915f);

    Curl3 batchSample(CurlBackend backend, int potentials, const Curl3& p, uint32_t seed){
        float x = p[0], y = p[1], z = p[2], ox, oy, oz;
        curlNoiseBatch(backend, potentials, &x, &y, &z, 1, seed, &ox, &oy, &oz);
        return point(ox, oy, oz);
    }

}

// OpenEngine's golden: the field its particle noise has shown since the gradient option shipped
TEST(CurlNoise, Golden){
    const Curl3 p = point(0.17f * 0.5f + 0.2f, -0.31f * 0.5f + 0.2f, 1.43f * 0.5f + 0.2f);
    const float expected[3] = {1.7340943814f, 0.9994704724f, -0.7334085702f};
    const Curl3 reference = gradientCurl3D(p, 42);
    const Curl3 simd = batchSample(CurlBackend::Native, 3, p, 42);
    const Curl3 scalar = batchSample(CurlBackend::Scalar, 3, p, 42);
    for (uint32_t axis = 0; axis < 3; ++axis){
        EXPECT_NEAR(reference[axis], expected[axis], 2e-6f);
        EXPECT_NEAR(simd[axis], expected[axis], 1e-5f) << curlNativeBackend();
        EXPECT_NEAR(scalar[axis], expected[axis], 1e-5f);
    }
}

TEST(CurlNoise, SeedReplaysAndChangesTheField){
    const Curl3 first = gradientCurl3D(kPoint, 42);
    const Curl3 replay = gradientCurl3D(kPoint, 42);
    const Curl3 other = gradientCurl3D(kPoint, 43);
    for (uint32_t axis = 0; axis < 3; ++axis)
        EXPECT_FLOAT_EQ(first[axis], replay[axis]);
    const Curl3 d = point(first[0] - other[0], first[1] - other[1], first[2] - other[2]);
    EXPECT_GT(length(d), 0.001f);
}

TEST(CurlNoise, TheFieldIsDivergenceFree){
    constexpr float kStep = 0.001f;
    for (int potentials : {3, 2}){
        float divergence = 0.0f;
        for (uint32_t axis = 0; axis < 3; ++axis){
            Curl3 left = kPoint, right = kPoint;
            left[axis] -= kStep;
            right[axis] += kStep;
            const Curl3 a = potentials == 3 ? gradientCurl3D(right, 42) : gradientCurl2(right, 42);
            const Curl3 b = potentials == 3 ? gradientCurl3D(left, 42) : gradientCurl2(left, 42);
            divergence += (a[axis] - b[axis]) / (2.0f * kStep);
        }
        EXPECT_NEAR(divergence, 0.0f, 0.01f) << potentials << " potentials";
    }
}

TEST(CurlNoise, TheWrapIsContinuous){
    for (uint32_t axis = 0; axis < 3; ++axis){
        Curl3 left = point(0.37f, 0.53f, 0.79f);
        Curl3 right = left;
        left[axis] = -0.0001f;
        right[axis] = 0.0001f;
        const Curl3 a = gradientCurl3D(left, 42), b = gradientCurl3D(right, 42);
        EXPECT_LT(length(point(a[0] - b[0], a[1] - b[1], a[2] - b[2])), 0.01f);
    }
}

TEST(CurlNoise, ANonFinitePointGivesZero){
    for (const float invalid : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()}){
        EXPECT_FLOAT_EQ(length(gradientCurl3D(point(invalid, 0.0f, 0.0f), 42)), 0.0f);
        EXPECT_FLOAT_EQ(length(gradientCurl3D(point(0.0f, 0.0f, invalid), 42)), 0.0f);
        EXPECT_FLOAT_EQ(length(batchSample(CurlBackend::Native, 3, point(invalid, 0.0f, 0.0f), 42)), 0.0f);
        EXPECT_FLOAT_EQ(length(batchSample(CurlBackend::Scalar, 3, point(0.0f, invalid, 0.0f), 42)), 0.0f);
    }
}

// AC4: over 10 000 random points (negative cells and the 256-cell wrap included) the 4-wide
// kernels match the copied reference within 1e-5, and the SIMD back end is bit-identical to
// the scalar one
TEST(CurlNoise, SimdMatchesReference){
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> coord(-300.0f, 300.0f);
    const size_t n = 10000;
    std::vector<float> x(n), y(n), z(n);
    for (size_t i = 0; i < n; i++){ x[i] = coord(rng); y[i] = coord(rng); z[i] = coord(rng); }
    for (int potentials : {3, 2}){
        std::vector<float> sx(n), sy(n), sz(n), vx(n), vy(n), vz(n);
        curlNoiseBatch(CurlBackend::Native, potentials, x.data(), y.data(), z.data(), n, 1234, vx.data(), vy.data(), vz.data());
        curlNoiseBatch(CurlBackend::Scalar, potentials, x.data(), y.data(), z.data(), n, 1234, sx.data(), sy.data(), sz.data());
        size_t mismatches = 0;
        float worst = 0.0f;
        for (size_t i = 0; i < n; i++){
            const Curl3 ref = potentials == 3 ? gradientCurl3D(point(x[i], y[i], z[i]), 1234) : gradientCurl2(point(x[i], y[i], z[i]), 1234);
            const float got[3] = {vx[i], vy[i], vz[i]};
            for (int a = 0; a < 3; a++){
                const float err = std::fabs(got[a] - ref[a]) / std::max(1.0f, std::fabs(ref[a]));
                worst = std::max(worst, err);
            }
            if (vx[i] != sx[i] || vy[i] != sy[i] || vz[i] != sz[i]) mismatches++;
        }
        EXPECT_LE(worst, 1e-5f) << potentials << " potentials, " << curlNativeBackend();
        EXPECT_EQ(mismatches, 0u) << potentials << " potentials: SIMD and scalar differ";
    }
}

// The 8-wide path (AVX2 processors) gives the 4-wide path's samples bit for bit, at any
// frequency and offset and for both fields
TEST(CurlNoise, WideMatchesNativeExactly){
    if (!curlWideAvailable()){
        GTEST_SKIP() << "no 8-wide curl noise on this processor";
    }
    std::mt19937 rng(41);
    std::uniform_real_distribution<float> coordinate(-700.0f, 700.0f);
    const size_t count = 4096;
    std::vector<float> x(count), y(count), z(count);
    for (size_t i = 0; i < count; i++){
        x[i] = coordinate(rng);
        y[i] = coordinate(rng);
        z[i] = coordinate(rng);
    }
    for (int potentials : {2, 3}){
        const float frequency = 0.37f, offset = 2.5f;
        std::vector<float> wx(count), wy(count), wz(count);
        curlNoiseWide(potentials, x.data(), y.data(), z.data(), count, frequency, offset, 1234u, wx.data(), wy.data(), wz.data());
        std::vector<float> sx(count), sy(count), sz(count), nx(count), ny(count), nz(count);
        for (size_t i = 0; i < count; i++){
            sx[i] = x[i] * frequency + offset;
            sy[i] = y[i] * frequency + offset;
            sz[i] = z[i] * frequency + offset;
        }
        curlNoiseBatch(CurlBackend::Native, potentials, sx.data(), sy.data(), sz.data(), count, 1234u, nx.data(), ny.data(), nz.data());
        for (size_t i = 0; i < count; i++){
            ASSERT_EQ(std::memcmp(&wx[i], &nx[i], 4), 0) << potentials << " potentials, sample " << i;
            ASSERT_EQ(std::memcmp(&wy[i], &ny[i], 4), 0) << potentials << " potentials, sample " << i;
            ASSERT_EQ(std::memcmp(&wz[i], &nz[i], 4), 0) << potentials << " potentials, sample " << i;
        }
    }
}
