// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's IK solvers (docs/specs/S21-animation.md AC5, R11, D11): two-bone reaches within
// 1 mm with the mid joint on the pole side, straightens toward an unreachable target, FABRIK
// converges, look-at never passes its clamp, non-uniform scale chains, the spring's half-life.

#include <gtest/gtest.h>

#include "animation/AnimatorIK.h"

#include <cmath>
#include <random>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace {

    // a chain of `count` bones along +y, `length` apart (bone 0 at the origin)
    struct Chain{
        Pose local;
        std::vector<int> parents;
        std::vector<Affine> world;
        std::vector<Vector3> positions;
        std::vector<Quaternion> rotations;
        std::vector<Vector3> scales;
        IKContext context;

        Chain(int count, float length, Vector3 scale = Vector3(1, 1, 1)): local((size_t)count, 0){
            parents.resize((size_t)count);
            for (int i = 0; i < count; i++){
                parents[(size_t)i] = i - 1;
                local.setPosition((size_t)i, i == 0 ? Vector3(0, 0, 0) : Vector3(0, length, 0));
                local.setScale((size_t)i, scale);
            }
            world.resize((size_t)count);
            positions.resize((size_t)count);
            rotations.resize((size_t)count);
            scales.resize((size_t)count);
            context.local = &local;
            context.parents = parents.data();
            context.space = RigSpace{world.data(), positions.data(), rotations.data(), scales.data()};
            refreshRigSpace(context, 0);
        }
    };

    float angleBetween(const Vector3& a, const Vector3& b){
        const float d = a.normalized().dotProduct(b.normalized());
        return std::acos(std::min(1.0f, std::max(-1.0f, d))) * 57.29577951f;
    }

}

TEST(AnimatorIK, TwoBoneReachesWithThePoleSide){
    Chain chain(3, 1.0f);
    // a slight bend so the chain has a plane
    chain.local.setRotation(1, Quaternion(0.9998477f, 0.0174524f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 target(0.5f, 1.2f, 0.3f);
    const Vector3 pole(0, 1, 2);
    solveTwoBone(chain.context, 0, 1, 2, target, &pole, nullptr, 1.0f);
    EXPECT_LE((chain.positions[2] - target).length(), 0.001f);
    // the mid joint lies on the pole's side of the line from the root to the target
    const Vector3 axis = (target - chain.positions[0]).normalized();
    Vector3 mid = chain.positions[1] - axis * chain.positions[1].dotProduct(axis);
    Vector3 side = pole - axis * pole.dotProduct(axis);
    EXPECT_GT(mid.dotProduct(side), 0.0f);
    // the bones keep their lengths
    EXPECT_NEAR((chain.positions[1] - chain.positions[0]).length(), 1.0f, 1e-4f);
    EXPECT_NEAR((chain.positions[2] - chain.positions[1]).length(), 1.0f, 1e-4f);
}

TEST(AnimatorIK, TwoBoneUnreachableTargetStraightensTheChain){
    Chain chain(3, 1.0f);
    chain.local.setRotation(1, Quaternion(0.9238795f, 0.3826834f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 target(5, 0, 0);
    solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f);
    const Vector3 direction = (target - chain.positions[0]).normalized();
    EXPECT_LE(angleBetween(chain.positions[2] - chain.positions[0], direction), 0.1f);
    EXPECT_LE(angleBetween(chain.positions[1] - chain.positions[0], direction), 5.0f) << "almost straight (0.999 of the reach)";
    EXPECT_NEAR((chain.positions[2] - chain.positions[0]).length(), 1.998f, 1e-3f);
}

TEST(AnimatorIK, TwoBoneWeightBlendsAndZeroDoesNothing){
    Chain chain(3, 1.0f);
    chain.local.setRotation(1, Quaternion(0.9238795f, 0.3826834f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 before = chain.positions[2];
    solveTwoBone(chain.context, 0, 1, 2, Vector3(1, 0.5f, 0), nullptr, nullptr, 0.0f);
    EXPECT_EQ(chain.positions[2], before);
    solveTwoBone(chain.context, 0, 1, 2, Vector3(1, 0.5f, 0), nullptr, nullptr, 0.5f);
    EXPECT_GT((chain.positions[2] - before).length(), 0.01f);
    EXPECT_GT((chain.positions[2] - Vector3(1, 0.5f, 0)).length(), 0.01f);
}

TEST(AnimatorIK, FabrikConverges){
    std::mt19937 random(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int converged = 0;
    for (int test = 0; test < 8; test++){
        Chain chain(6, 0.5f);
        for (int i = 1; i < 6; i++){
            Quaternion q(1.0f, u(random) * 0.2f, u(random) * 0.2f, u(random) * 0.2f);
            q.normalize();
            chain.local.setRotation((size_t)i, q);
        }
        refreshRigSpace(chain.context, 0);
        // a reachable target: within 80 % of the reach
        Vector3 target(u(random), std::fabs(u(random)) + 0.2f, u(random));
        target = target.normalized() * (0.8f * 2.5f * (0.3f + 0.7f * std::fabs(u(random))));
        const int bones[6] = {0, 1, 2, 3, 4, 5};
        solveFabrik(chain.context, bones, 6, target, 10, 0.001f, 1.0f);
        if ((chain.positions[5] - target).length() <= 0.001f) converged++;
        for (int i = 0; i < 5; i++) EXPECT_NEAR((chain.positions[(size_t)i + 1] - chain.positions[(size_t)i]).length(), 0.5f, 1e-4f);
    }
    EXPECT_EQ(converged, 8);
}

TEST(AnimatorIK, LookAtNeverExceedsItsClamp){
    const int bones[3] = {0, 1, 2};
    const float weights[3] = {0.2f, 0.3f, 0.5f};
    const Vector3 forward(0, 0, 1);
    for (int i = 0; i < 36; i++){
        Chain c(3, 0.3f);
        const float angle = (float)i * 10.0f * 0.0174532925f;
        const Vector3 target = c.positions[2] + Vector3(std::sin(angle), 0.3f, std::cos(angle)) * 3.0f;
        const Vector3 before = c.rotations[2] * forward;
        solveLookAt(c.context, bones, weights, 3, forward, target, 45.0f, 1.0f);
        const Vector3 after = c.rotations[2] * forward;
        EXPECT_LE(angleBetween(before, after), 45.0f + 0.05f) << "target at " << i * 10 << " degrees";
        if (i == 0){
            EXPECT_LE(angleBetween(after, target - c.positions[2]), 0.5f) << "an in-range target is reached";
        }
    }
}

TEST(AnimatorIK, NonUniformScaleChains){
    Chain chain(3, 1.0f, Vector3(1, 2, 1));
    // world bone lengths are scaled by the parents' y scale: 2, then 4
    EXPECT_NEAR(chain.positions[1].y, 2.0f, 1e-5f);
    EXPECT_NEAR(chain.positions[2].y, 6.0f, 1e-5f);
    chain.local.setRotation(1, Quaternion(0.9998477f, 0.0174524f, 0, 0));
    refreshRigSpace(chain.context, 0);
    const Vector3 target(1.5f, 3.0f, 1.0f);
    solveTwoBone(chain.context, 0, 1, 2, target, nullptr, nullptr, 1.0f);
    EXPECT_LE((chain.positions[2] - target).length(), 0.01f);
}

TEST(AnimatorIK, SpringHalfLife){
    float velocity = 0.0f;
    float x = 1.0f;
    const float halfLife = 0.08f;
    float t = 0.0f;
    while (t < halfLife - 1e-6f){
        x = springDamp(x, 0.0f, velocity, halfLife, 0.001f);
        t += 0.001f;
    }
    EXPECT_NEAR(x, 0.5f, 0.01f);
    for (int i = 0; i < 2000; i++) x = springDamp(x, 0.0f, velocity, halfLife, 0.001f);
    EXPECT_NEAR(x, 0.0f, 1e-4f) << "critically damped: no overshoot";
    EXPECT_GE(x, -1e-6f);
    EXPECT_EQ(shortestArc(Vector3(1, 0, 0), Vector3(1, 0, 0)).w, 1.0f);
    const Quaternion half = shortestArc(Vector3(1, 0, 0), Vector3(-1, 0, 0));
    EXPECT_NEAR(std::fabs(half.w), 0.0f, 1e-6f);
}
