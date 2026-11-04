// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include <gtest/gtest.h>

#include <limits>

using namespace jasperengine;

TEST(Vector4, ConstantsAndConstructors){
    EXPECT_EQ(Vector4(), Vector4::ZERO);
    EXPECT_EQ(Vector4::UNIT_W, Vector4(0, 0, 0, 1));
    EXPECT_EQ(Vector4(Vector3(1, 2, 3), 4), Vector4(1, 2, 3, 4));
    EXPECT_EQ(Vector4(Vector2(1, 2), 3, 4), Vector4(1, 2, 3, 4));
    EXPECT_EQ(Vector4(2.0f), Vector4(2, 2, 2, 2));
}

TEST(Vector4, ArithmeticIsComponentwise){
    const Vector4 a(1, 2, 3, 4);
    const Vector4 b(4, 3, 2, 1);
    EXPECT_EQ(a + b, Vector4(5, 5, 5, 5));
    EXPECT_EQ(a - b, Vector4(-3, -1, 1, 3));
    EXPECT_EQ(a * b, Vector4(4, 6, 6, 4));
    EXPECT_EQ(a * 2.0f, Vector4(2, 4, 6, 8));
    EXPECT_EQ(a / 2.0f, Vector4(0.5f, 1, 1.5f, 2));
    EXPECT_EQ(-a, Vector4(-1, -2, -3, -4));
}

TEST(Vector4, DotProductIncludesW){
    EXPECT_FLOAT_EQ(Vector4(1, 2, 3, 4).dotProduct(Vector4(1, 1, 1, 1)), 10.0f);
}

TEST(Vector4, DivideByWProjectsXYZ){
    Vector4 v(2, 4, 6, 2);
    v.divideByW();
    EXPECT_EQ(v, Vector4(1, 2, 3, 2));
}

TEST(Vector4, IsNaNSeesAnyComponent){
    EXPECT_FALSE(Vector4(1, 2, 3, 4).isNaN());
    EXPECT_TRUE(Vector4(1, 2, 3, std::numeric_limits<float>::quiet_NaN()).isNaN());
}

TEST(Vector4, AssigningAVector3KeepsW){
    Vector4 v(9, 9, 9, 5);
    v = Vector3(1, 2, 3);
    EXPECT_EQ(v, Vector4(1, 2, 3, 5));
}

TEST(Vector4, ToString){
    EXPECT_EQ(Vector4(1, 2, 3, 4).toString(), "Vector4(1.000000, 2.000000, 3.000000, 4.000000)");
}

TEST(Vector4, MoveTowardsAndLerp){
    const Vector4 from(0, 0, 0, 0);
    const Vector4 to(2, 2, 2, 2);  // 4 apart
    EXPECT_EQ(from.moveTowards(to, 2.0f), Vector4(1, 1, 1, 1));
    EXPECT_EQ(from.moveTowards(to, 10.0f), to);
    EXPECT_EQ(to.moveTowards(to, 1.0f), to);
    EXPECT_EQ(from.lerp(to, 0.25f), Vector4(0.5f, 0.5f, 0.5f, 0.5f));
    EXPECT_EQ(from.lerp(to, 2.0f), to);
    EXPECT_EQ(from.lerpUnclamped(to, -1.0f), Vector4(-2, -2, -2, -2));
}
