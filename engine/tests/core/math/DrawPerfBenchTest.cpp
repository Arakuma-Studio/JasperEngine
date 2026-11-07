// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU budgets of moved-object math (draw-perf spec 8): a local matrix composed in closed form
// at least 2x faster than T * R * S, the affine AABB transform at least 2x faster than the corner
// loop, the 3x3 determinant at least 4x faster than the 4x4 one (a mirrored draw's test). Printed as
// [bench] lines; the budgets are enforced with JASPER_TEST_PERF_SLACK set (a quiet server).

#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int COUNT = 10000;
    constexpr int ROUNDS = 15;

    template <typename Work>
    double medianMicros(Work&& work){
        std::vector<double> samples;
        for (int round = 0; round < ROUNDS; round++){
            const auto start = std::chrono::steady_clock::now();
            work();
            samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    }

    struct Inputs {
        std::vector<Vector3> positions;
        std::vector<Quaternion> rotations;
        std::vector<Vector3> scales;
        std::vector<Matrix4> models;
        Inputs(){
            for (int i = 0; i < COUNT; i++){
                const float f = (float)i;
                positions.push_back(Vector3(f, f * 0.5f, -f));
                rotations.push_back(Quaternion((float)(i % 360), Vector3(0.3f, 1.0f, 0.2f).normalize()));
                scales.push_back(Vector3(1.0f + 0.001f * f, 1.0f, 2.0f));
                models.push_back(Matrix4::composeTRS(positions.back(), rotations.back(), scales.back()));
            }
        }
    };

    // AABB::transform before draw-perf: 8 corners, each through a divide
    AABB cornerTransform(const AABB& box, const Matrix4& m){
        const Vector3 low = box.getMinimum();
        const Vector3 high = box.getMaximum();
        AABB result;
        for (int corner = 0; corner < 8; corner++)
            result.merge(m * Vector3((corner & 1) ? high.x : low.x, (corner & 2) ? high.y : low.y, (corner & 4) ? high.z : low.z));
        return result;
    }

    void report(const char* what, double general, double affine, double budget){
        std::printf("[bench] %s: %.1f us general, %.1f us affine (%.1fx, budget %.0fx) for %d\n", what, general, affine,
                    affine > 0.0 ? general / affine : 0.0, budget, COUNT);
        if (testsupport::perfSlack() > 0.0){
            EXPECT_GE(general * testsupport::perfSlack(), affine * budget) << what;
        }
    }

}

TEST(DrawPerfBench, ComposeTRS){
    const Inputs in;
    std::vector<Matrix4> out(COUNT);
    const double general = medianMicros([&](){
        for (int i = 0; i < COUNT; i++)
            out[i] = Matrix4::translateMatrix(in.positions[i]) * in.rotations[i].getRotationMatrix() * Matrix4::scaleMatrix(in.scales[i]);
    });
    const double affine = medianMicros([&](){
        for (int i = 0; i < COUNT; i++)
            out[i] = Matrix4::composeTRS(in.positions[i], in.rotations[i], in.scales[i]);
    });
    report("local matrix (T * R * S)", general, affine, 2.0);
}

TEST(DrawPerfBench, AffineAABB){
    const Inputs in;
    const AABB box(Vector3(-1, -2, -0.5f), Vector3(1, 2, 0.5f));
    std::vector<AABB> out(COUNT);
    const double general = medianMicros([&](){
        for (int i = 0; i < COUNT; i++)
            out[i] = cornerTransform(box, in.models[i]);
    });
    const double affine = medianMicros([&](){
        for (int i = 0; i < COUNT; i++)
            out[i] = in.models[i] * box;
    });
    report("AABB transform", general, affine, 2.0);
}

TEST(DrawPerfBench, MirrorSign){
    const Inputs in;
    std::vector<bool> out(COUNT);
    const double general = medianMicros([&](){
        for (int i = 0; i < COUNT; i++)
            out[i] = in.models[i].determinant() < 0.0f;
    });
    const double affine = medianMicros([&](){
        for (int i = 0; i < COUNT; i++)
            out[i] = in.models[i].determinant3x3() < 0.0f;
    });
    report("mirror sign (determinant)", general, affine, 4.0);
}
