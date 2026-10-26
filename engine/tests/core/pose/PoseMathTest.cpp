// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The radian helpers of PoseMath.h (spec 4.7)

#include "TestUtil.h"

#include <gtest/gtest.h>

using namespace posetest;

TEST(PoseMath, AxisAngleAndFromTo){
    const Quatd q = axisAngleRad(Vec3d(0, 1, 0), POSE_PI / 2);
    const Vec3d v = q * Vec3d(0, 0, 1);
    EXPECT_NEAR(v.x, 1.0, 1e-12);
    EXPECT_NEAR(v.z, 0.0, 1e-12);
    // the float overload is in radians whatever the engine's default unit
    const Quaternion f = axisAngleRad(Vector3(0, 1, 0), static_cast<float>(POSE_PI / 2));
    EXPECT_NEAR(f.w, std::cos(POSE_PI / 4), 1e-6);

    for (const Vec3d& to : {Vec3d(1, 0, 0), Vec3d(0, -1, 0), Vec3d(0.3, 0.2, -0.9), Vec3d(0, 0, -1)}){
        const Vec3d from(0, 0, 1);
        const Vec3d r = fromToRotation(from, to * 3.0) * from;
        EXPECT_LT((r - to.normalized()).length(), 1e-12);
    }
    EXPECT_NEAR(angleBetween(Vec3d(1, 0, 0), Vec3d(0, 1, 0)), POSE_PI / 2, 1e-12);
    EXPECT_NEAR(angleBetween(q, -q), 0.0, 1e-12);
    EXPECT_NEAR(angleBetween(Quatd(), q), POSE_PI / 2, 1e-12);
}

TEST(PoseMath, PowerAndSlerpUnclamped){
    const Quatd q = axisAngleRad(Vec3d(1, 1, 0), 0.8);
    EXPECT_NEAR(angleBetween(powRotation(q, 1.5), axisAngleRad(Vec3d(1, 1, 0), 1.2)), 0.0, 1e-12);
    EXPECT_NEAR(angleBetween(powRotation(q, -0.25), axisAngleRad(Vec3d(1, 1, 0), -0.2)), 0.0, 1e-12);
    // shortest path: -q is the same rotation
    EXPECT_NEAR(angleBetween(powRotation(-q, 0.5), axisAngleRad(Vec3d(1, 1, 0), 0.4)), 0.0, 1e-12);
    const Quatd a = axisAngleRad(Vec3d(0, 0, 1), 0.2), b = axisAngleRad(Vec3d(0, 0, 1), 0.6);
    EXPECT_NEAR(angleBetween(slerpUnclamped(a, b, 1.5), axisAngleRad(Vec3d(0, 0, 1), 0.8)), 0.0, 1e-12);
    EXPECT_NEAR(angleBetween(slerpUnclamped(a, b, -1.0), axisAngleRad(Vec3d(0, 0, 1), -0.2)), 0.0, 1e-12);
    EXPECT_NEAR(angleBetween(slerpUnclamped(a, b, 0.0), a), 0.0, 1e-12);
}

TEST(PoseMath, SwingTwistAndRotateTowards){
    std::mt19937 rng(17);
    for (int i = 0; i < 100; i++){
        const Quatd q(randomRotation(rng, 3.0));
        const Vec3d axis(0.2, 1.0, -0.3);
        Quatd swing, twist;
        swingTwist(q, axis, swing, twist);
        EXPECT_NEAR(angleBetween(swing * twist, q), 0.0, 1e-6);
        EXPECT_NEAR(swing.vec().dot(axis.normalized()), 0.0, 1e-6);
        EXPECT_LT(twist.vec().cross(axis).length(), 1e-6);
    }
    const Quatd from, to = axisAngleRad(Vec3d(0, 1, 0), 1.0);
    EXPECT_NEAR(angleBetween(rotateTowards(from, to, 0.25), from), 0.25, 1e-12);
    EXPECT_NEAR(angleBetween(rotateTowards(from, to, 2.0), to), 0.0, 1e-12);
}

TEST(PoseMath, ScalarHelpers){
    EXPECT_DOUBLE_EQ(positiveFmod(-0.25, 1.0), 0.75);
    EXPECT_DOUBLE_EQ(positiveFmod(2.5, 1.0), 0.5);
    EXPECT_GE(positiveFmod(-1e-18, 1.0), 0.0);
    EXPECT_LT(positiveFmod(-1e-18, 1.0), 1.0);
    EXPECT_FLOAT_EQ(positiveFmod(-0.5f, 2.0f), 1.5f);
    EXPECT_DOUBLE_EQ(smoothstep(0.0, 1.0, 0.5), 0.5);
    EXPECT_DOUBLE_EQ(smoothstep(0.0, 1.0, -1.0), 0.0);
    EXPECT_DOUBLE_EQ(smoothstep(0.0, 1.0, 2.0), 1.0);
    EXPECT_DOUBLE_EQ(smootherstep(0.0, 1.0, 0.5), 0.5);
    EXPECT_NEAR(smootherstep(0.0, 1.0, 0.25), 0.103515625, 1e-12);
}
