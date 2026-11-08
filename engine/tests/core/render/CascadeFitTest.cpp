// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The stable cascade fit (spec S10 AC1): the slice sphere against the brute-force corners, the
// texel snap (a camera moving inside a texel keeps bit-identical matrices), the extent under
// rotation, the light basis near vertical, monotonic splits, and the fit freeze (coverage is
// never lost, at most one requested refit per frame).

#include "render/shadow/CascadeFit.h"

#include "math/Matrix4.h"
#include "math/Vector3.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

using namespace jasperengine;

namespace {

    bool sameMatrix(const Matrix4& a, const Matrix4& b){
        return std::memcmp((const float*)a, (const float*)b, sizeof(float) * 16) == 0;
    }

    bool finiteMatrix(const Matrix4& m){
        for (int i = 0; i < 16; i++){
            if (!std::isfinite(((const float*)m)[i]))
                return false;
        }
        return true;
    }

    // a camera at position looking along forward (world up Y)
    Matrix4 cameraView(const Vector3& position, const Vector3& forward){
        const Vector3 up = (std::abs(forward.normalized().y) > 0.99f) ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
        return Matrix4::lookAtMatrix(position, position + forward, up);
    }

    CascadeFitInput baseInput(){
        CascadeFitInput input;
        input.tanHalfY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
        input.aspect = 16.0f / 9.0f;
        input.nearClip = 0.1f;
        input.shadowDistance = 100.0f;
        input.lightDir = Vector3(0.3f, -1.0f, 0.4f).normalized();
        input.cascades = 4;
        input.resolution = 2048;
        return input;
    }

    // the slice sphere of cascade c in light space, as the fit computes it
    void sliceSphere(const CascadeFitInput& input, const CascadeFitResult& result, unsigned int c,
                     const Vector3& cameraPosition, const Vector3& cameraForward,
                     double light[3], float& radius){
        const float depth = CascadeFit::sliceBoundingSphereCenter(result.splitNear[c], result.splitFar[c], input.tanHalfY, input.aspect);
        radius = CascadeFit::quantizeRadius(CascadeFit::sliceBoundingSphereRadius(result.splitNear[c], result.splitFar[c], input.tanHalfY, input.aspect));
        const Vector3 forward = cameraForward.normalized();
        const double world[3] = {
            (double)cameraPosition.x + (double)forward.x * depth,
            (double)cameraPosition.y + (double)forward.y * depth,
            (double)cameraPosition.z + (double)forward.z * depth};
        for (int r = 0; r < 3; r++){
            light[r] = (double)result.view[0][r] * world[0] + (double)result.view[1][r] * world[1] + (double)result.view[2][r] * world[2];
        }
    }

}

TEST(SphereRadius, MatchesBruteForceCorners){
    std::mt19937 random(1234);
    std::uniform_real_distribution<float> fov(10.0f, 120.0f);
    std::uniform_real_distribution<float> aspect(0.3f, 3.0f);
    std::uniform_real_distribution<float> nearDist(0.01f, 50.0f);
    std::uniform_real_distribution<float> depth(0.05f, 500.0f);
    for (int i = 0; i < 1000; i++){
        const float t = std::tan(0.5f * fov(random) * 3.14159265f / 180.0f);
        const float a = aspect(random);
        const float n = nearDist(random);
        const float f = n + depth(random);
        const double k2 = (double)t * t * (1.0 + (double)a * a);

        // the smallest on-axis sphere holding both corner rings, by ternary search on its centre
        auto reach = [&](double c){
            const double nearRing = std::sqrt((double)n * n * k2 + (c - n) * (c - n));
            const double farRing = std::sqrt((double)f * f * k2 + (f - c) * (f - c));
            return std::max(nearRing, farRing);
        };
        double lo = 0.0, hi = (double)f * 4.0 + 10.0;
        for (int k = 0; k < 200; k++){
            const double m1 = lo + (hi - lo) / 3.0;
            const double m2 = hi - (hi - lo) / 3.0;
            if (reach(m1) < reach(m2)) hi = m2; else lo = m1;
        }
        const double brute = reach(0.5 * (lo + hi));

        const float radius = CascadeFit::sliceBoundingSphereRadius(n, f, t, a);
        const float centre = CascadeFit::sliceBoundingSphereCenter(n, f, t, a);
        ASSERT_NEAR(radius, brute, brute * 1e-4) << "case " << i;
        // and it holds every corner
        EXPECT_LE(reach(centre), radius * (1.0 + 1e-4)) << "case " << i;
    }
}

TEST(Snap, TranslationWithinTexelKeepsVP){
    CascadeFitInput input = baseInput();
    const Vector3 forward = Vector3(0.2f, -0.3f, -1.0f).normalized();
    Vector3 position(12.3f, 4.5f, -7.8f);
    input.cameraView = cameraView(position, forward);
    const CascadeFitResult first = CascadeFit::fit(input, nullptr);
    ASSERT_EQ(first.cascades, 4u);

    // the largest step along light-space X that keeps every cascade's centre in its texel
    double margin = 1e30;
    for (unsigned int c = 0; c < first.cascades; c++){
        double light[3];
        float radius;
        sliceSphere(input, first, c, position, forward, light, radius);
        const double texel = first.texelWorld[c];
        const double offset = light[0] - std::round(light[0] / texel) * texel;
        margin = std::min(margin, texel * 0.5 - std::abs(offset));
    }
    ASSERT_GT(margin, 0.0);
    const Vector3 lightX(first.view[0][0], first.view[1][0], first.view[2][0]);

    for (float fraction : {0.25f, 0.5f, -0.25f, -0.5f}){
        const Vector3 moved = position + lightX * (float)(margin * fraction);
        input.cameraView = cameraView(moved, forward);
        const CascadeFitResult result = CascadeFit::fit(input, nullptr);
        for (unsigned int c = 0; c < result.cascades; c++){
            EXPECT_TRUE(sameMatrix(result.viewProj[c], first.viewProj[c])) << "cascade " << c << " step " << fraction;
        }
    }

    // a whole texel moves cascade 0's grid by exactly one texel
    const Vector3 stepped = position + lightX * first.texelWorld[0];
    input.cameraView = cameraView(stepped, forward);
    const CascadeFitResult step = CascadeFit::fit(input, nullptr);
    EXPECT_FALSE(sameMatrix(step.viewProj[0], first.viewProj[0]));
    EXPECT_NEAR(step.centerX[0] - first.centerX[0], first.texelWorld[0], first.texelWorld[0] * 1e-3);
}

TEST(Snap, RotationKeepsExtent){
    CascadeFitInput input = baseInput();
    const Vector3 position(3.0f, 2.0f, 1.0f);
    input.cameraView = cameraView(position, Vector3(0.0f, -0.2f, -1.0f));
    const CascadeFitResult first = CascadeFit::fit(input, nullptr);
    for (int step = 1; step < 360; step++){
        const float yaw = (float)step * 3.14159265f / 180.0f;
        input.cameraView = cameraView(position, Vector3(std::sin(yaw), -0.2f, -std::cos(yaw)));
        const CascadeFitResult result = CascadeFit::fit(input, nullptr);
        for (unsigned int c = 0; c < result.cascades; c++){
            ASSERT_EQ(result.radius[c], first.radius[c]) << "cascade " << c << " yaw " << step;
            ASSERT_EQ(result.texelWorld[c], first.texelWorld[c]) << "cascade " << c << " yaw " << step;
        }
    }
}

TEST(Basis, NearVerticalLightIsFinite){
    CascadeFitInput input = baseInput();
    input.cameraView = cameraView(Vector3(0, 5, 0), Vector3(0, -0.5f, -1));
    const float tilt = 0.1f * 3.14159265f / 180.0f;
    for (float sign : {-1.0f, 1.0f}){
        for (float azimuth = 0.0f; azimuth < 6.28f; azimuth += 0.5f){
            for (float angle : {0.0f, tilt * 0.5f, tilt}){
                input.lightDir = Vector3(std::sin(angle) * std::cos(azimuth), sign * std::cos(angle), std::sin(angle) * std::sin(azimuth));
                const Matrix4 rotation = CascadeFit::lightRotation(input.lightDir);
                ASSERT_TRUE(finiteMatrix(rotation));
                // orthonormal: the forward is the light direction
                const Vector3 forward(-rotation[0][2], -rotation[1][2], -rotation[2][2]);
                EXPECT_NEAR(forward.dotProduct(input.lightDir.normalized()), 1.0f, 1e-5f);
                const CascadeFitResult result = CascadeFit::fit(input, nullptr);
                ASSERT_EQ(result.cascades, 4u);
                for (unsigned int c = 0; c < result.cascades; c++){
                    ASSERT_TRUE(finiteMatrix(result.viewProj[c]));
                    ASSERT_GT(result.radius[c], 0.0f);
                }
            }
        }
    }
}

TEST(Splits, ManualAndAutoMonotonic){
    CascadeFitInput input = baseInput();
    for (CascadeSplitMode mode : {CascadeSplitMode::AUTO, CascadeSplitMode::MANUAL}){
        for (unsigned int cascades = 1; cascades <= 4; cascades++){
            input.mode = mode;
            input.cascades = cascades;
            float splits[CASCADE_FIT_MAX];
            CascadeFit::computeSplits(input, splits);
            float previous = input.nearClip;
            for (unsigned int c = 0; c < cascades; c++){
                EXPECT_GT(splits[c], previous) << "mode " << (int)mode << " cascades " << cascades << " split " << c;
                previous = splits[c];
            }
            EXPECT_FLOAT_EQ(splits[cascades - 1], input.shadowDistance);
        }
    }

    // Unity's default 4-cascade fractions
    input.mode = CascadeSplitMode::MANUAL;
    input.cascades = 4;
    float splits[CASCADE_FIT_MAX];
    CascadeFit::computeSplits(input, splits);
    EXPECT_NEAR(splits[0], 6.7f, 1e-3f);
    EXPECT_NEAR(splits[1], 20.0f, 1e-3f);
    EXPECT_NEAR(splits[2], 46.7f, 1e-3f);

    // a bad manual set still gives every cascade some depth
    input.splits[0] = 0.5f;
    input.splits[1] = 0.2f;
    input.splits[2] = 2.0f;
    CascadeFit::computeSplits(input, splits);
    float previous = input.nearClip;
    for (unsigned int c = 0; c < 4; c++){
        EXPECT_GT(splits[c], previous);
        previous = splits[c];
    }

    // the blend overlap: the next slice starts 0.175 of the cascade's depth before the split
    input = baseInput();
    input.cameraView = cameraView(Vector3(0, 0, 0), Vector3(0, 0, -1));
    const CascadeFitResult result = CascadeFit::fit(input, nullptr);
    for (unsigned int c = 0; c + 1 < result.cascades; c++){
        const float band = CASCADE_BLEND_FRACTION * (result.splitFar[c] - result.splitNear[c]);
        EXPECT_NEAR(result.splitNear[c + 1], result.splitFar[c] - band, 1e-3f);
        EXPECT_FLOAT_EQ(result.blendStart[c], result.splitNear[c + 1]);
    }
}

TEST(Freeze, NeverLosesCoverage){
    CascadeFitInput input = baseInput();
    input.freeze = true;
    std::mt19937 random(99);
    std::uniform_real_distribution<float> step(-0.15f, 0.15f);
    std::uniform_real_distribution<float> turn(-0.01f, 0.01f);
    Vector3 position(0.0f, 2.0f, 0.0f);
    float yaw = 0.0f, pitch = -0.1f;
    CascadeFitResult previous;
    bool hasPrevious = false;
    int kept = 0;
    for (int i = 0; i < 10000; i++){
        position += Vector3(step(random), step(random) * 0.2f, step(random));
        if (i == 5000){
            float largestRadius = 0.0f;
            for (unsigned int c = 0; c < previous.cascades; c++)
                largestRadius = std::max(largestRadius, previous.radius[c]);
            const Vector3 lightX(previous.view[0][0], previous.view[1][0], previous.view[2][0]);
            position += lightX * (4.0f * largestRadius);
        }
        yaw += turn(random);
        pitch = std::clamp(pitch + turn(random) * 0.5f, -1.2f, 1.2f);
        const Vector3 forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
        input.cameraView = cameraView(position, forward);
        const CascadeFitResult result = CascadeFit::fit(input, hasPrevious ? &previous : nullptr);
        for (unsigned int c = 0; c < result.cascades; c++){
            double light[3];
            float radius;
            sliceSphere(input, result, c, position, forward, light, radius);
            const double slack = 1e-4 * result.radius[c];
            ASSERT_LE(std::abs(light[0] - result.centerX[c]) + radius, result.radius[c] + slack) << "step " << i << " cascade " << c;
            ASSERT_LE(std::abs(light[1] - result.centerY[c]) + radius, result.radius[c] + slack) << "step " << i << " cascade " << c;
            ASSERT_LE(light[2] + radius, result.nearZ[c] + slack) << "step " << i << " cascade " << c;
            ASSERT_GE(light[2] - radius, result.farZ[c] - slack) << "step " << i << " cascade " << c;
            if (!result.refit[c])
                kept++;
        }
        previous = result;
        hasPrevious = true;
    }
    // and the freeze does keep fits
    EXPECT_GT(kept, 10000);
}

TEST(Freeze, AtMostOneRequestedRefitPerFrame){
    CascadeFitInput input = baseInput();
    input.freeze = true;
    Vector3 position(0.0f, 2.0f, 0.0f);
    const Vector3 forward = Vector3(0.1f, -0.2f, -1.0f).normalized();
    input.cameraView = cameraView(position, forward);
    CascadeFitResult previous = CascadeFit::fit(input, nullptr);
    int requestedRefits = 0;
    for (int i = 0; i < 3000; i++){
        // a steady walk: every cascade consumes its guard band in time
        position += Vector3(0.011f, 0.0f, -0.017f);
        input.cameraView = cameraView(position, forward);
        const CascadeFitResult result = CascadeFit::fit(input, &previous);
        int requested = 0;
        for (unsigned int c = 0; c < result.cascades; c++){
            if (result.refit[c] && result.guardUsed[c] >= 0.5f && result.guardUsed[c] < 1.0f)
                requested++;
        }
        ASSERT_LE(requested, 1) << "step " << i;
        requestedRefits += requested;
        previous = result;
    }
    EXPECT_GT(requestedRefits, 0);
}
