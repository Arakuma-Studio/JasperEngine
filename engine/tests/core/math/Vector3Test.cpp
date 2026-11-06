// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace jasperengine;

TEST(Vector3, ConstantsAreTheUnitAxes){
    EXPECT_EQ(Vector3::UNIT_X, Vector3(1, 0, 0));
    EXPECT_EQ(Vector3::UNIT_Y, Vector3(0, 1, 0));
    EXPECT_EQ(Vector3::UNIT_Z, Vector3(0, 0, 1));
    EXPECT_EQ(Vector3::UNIT_SCALE, Vector3(1, 1, 1));
}

TEST(Vector3, ScalarConstructorFillsEveryComponent){
    EXPECT_EQ(Vector3(2.5f), Vector3(2.5f, 2.5f, 2.5f));
}

TEST(Vector3, AddsAndSubtractsComponentwise){
    const Vector3 a(1.0f, 2.0f, 3.0f);
    const Vector3 b(4.0f, -5.0f, 0.5f);
    EXPECT_EQ(a + b, Vector3(5.0f, -3.0f, 3.5f));
    EXPECT_EQ(a - b, Vector3(-3.0f, 7.0f, 2.5f));
}

TEST(Vector3, MultipliesAndDividesByAScalar){
    const Vector3 v(1.0f, -2.0f, 4.0f);
    EXPECT_EQ(v * 2.0f, Vector3(2.0f, -4.0f, 8.0f));
    EXPECT_EQ(2.0f * v, Vector3(2.0f, -4.0f, 8.0f));
    EXPECT_EQ(v / 2.0f, Vector3(0.5f, -1.0f, 2.0f));
}

TEST(Vector3, MultipliesComponentwiseByAVector){
    EXPECT_EQ(Vector3(1, 2, 3) * Vector3(4, 5, 6), Vector3(4, 10, 18));
}

TEST(Vector3, CompoundAssignmentsModifyInPlace){
    Vector3 v(1, 1, 1);
    v += Vector3(1, 2, 3);
    EXPECT_EQ(v, Vector3(2, 3, 4));
    v -= Vector3(1, 1, 1);
    EXPECT_EQ(v, Vector3(1, 2, 3));
    v *= 3.0f;
    EXPECT_EQ(v, Vector3(3, 6, 9));
    v /= 3.0f;
    EXPECT_EQ(v, Vector3(1, 2, 3));
}

TEST(Vector3, NegationFlipsEveryComponent){
    EXPECT_EQ(-Vector3(1, -2, 3), Vector3(-1, 2, -3));
}

TEST(Vector3, IndexOperatorReachesEachComponent){
    Vector3 v(7, 8, 9);
    EXPECT_FLOAT_EQ(v[0], 7);
    EXPECT_FLOAT_EQ(v[1], 8);
    EXPECT_FLOAT_EQ(v[2], 9);
    v[1] = 3;
    EXPECT_FLOAT_EQ(v.y, 3);
}

TEST(Vector3, LessAndGreaterNeedEveryComponent){
    EXPECT_TRUE(Vector3(0, 0, 0) < Vector3(1, 1, 1));
    EXPECT_FALSE(Vector3(0, 2, 0) < Vector3(1, 1, 1));
    EXPECT_TRUE(Vector3(2, 2, 2) > Vector3(1, 1, 1));
    EXPECT_FALSE(Vector3(2, 0, 2) > Vector3(1, 1, 1));
}

TEST(Vector3, LengthAndSquaredLength){
    const Vector3 v(2, 3, 6);
    EXPECT_FLOAT_EQ(v.length(), 7.0f);
    EXPECT_FLOAT_EQ(v.squaredLength(), 49.0f);
}

TEST(Vector3, DistanceBetweenPoints){
    EXPECT_FLOAT_EQ(Vector3(1, 1, 1).distance(Vector3(1, 4, 5)), 5.0f);
    EXPECT_FLOAT_EQ(Vector3(1, 1, 1).squaredDistance(Vector3(1, 4, 5)), 25.0f);
}

TEST(Vector3, NormalizesToUnitLength){
    Vector3 v(3.0f, 0.0f, 4.0f);
    const float length = v.normalizeL();
    EXPECT_FLOAT_EQ(length, 5.0f);
    EXPECT_FLOAT_EQ(v.length(), 1.0f);
    EXPECT_FLOAT_EQ(v.x, 0.6f);
    EXPECT_FLOAT_EQ(v.z, 0.8f);
}

TEST(Vector3, NormalizedLeavesTheOriginalAlone){
    const Vector3 v(0, 0, 9);
    EXPECT_EQ(v.normalized(), Vector3::UNIT_Z);
    EXPECT_EQ(v, Vector3(0, 0, 9));
}

TEST(Vector3, NormalizingZeroKeepsZero){
    Vector3 v;
    EXPECT_FLOAT_EQ(v.normalizeL(), 0.0f);
    EXPECT_EQ(v, Vector3::ZERO);
}

TEST(Vector3, DotProductOfPerpendicularVectorsIsZero){
    EXPECT_FLOAT_EQ(Vector3::UNIT_X.dotProduct(Vector3::UNIT_Y), 0.0f);
    EXPECT_FLOAT_EQ(Vector3(1, 2, 3).dotProduct(Vector3(4, -5, 6)), 12.0f);
}

TEST(Vector3, AbsDotProductSumsAbsoluteProducts){
    EXPECT_FLOAT_EQ(Vector3(1, 2, 3).absDotProduct(Vector3(4, -5, 6)), 32.0f);
}

TEST(Vector3, CrossProductFollowsTheRightHandRule){
    EXPECT_EQ(Vector3::UNIT_X.crossProduct(Vector3::UNIT_Y), Vector3::UNIT_Z);
    EXPECT_EQ(Vector3::UNIT_Y.crossProduct(Vector3::UNIT_X), -Vector3::UNIT_Z);
    EXPECT_EQ(Vector3::UNIT_Y.crossProduct(Vector3::UNIT_Z), Vector3::UNIT_X);
}

TEST(Vector3, CrossProductIsPerpendicularToBothInputs){
    const Vector3 a(1, 2, 3);
    const Vector3 b(-2, 0.5f, 4);
    const Vector3 c = a.crossProduct(b);
    EXPECT_NEAR(c.dotProduct(a), 0.0f, 1e-5f);
    EXPECT_NEAR(c.dotProduct(b), 0.0f, 1e-5f);
}

TEST(Vector3, MidPointIsHalfway){
    EXPECT_EQ(Vector3(0, 0, 0).midPoint(Vector3(2, 4, -6)), Vector3(1, 2, -3));
}

TEST(Vector3, LerpInterpolatesLinearly){
    const Vector3 a(0, 0, 0);
    const Vector3 b(10, -10, 4);
    EXPECT_EQ(a.lerp(b, 0.0f), a);
    EXPECT_EQ(a.lerp(b, 1.0f), b);
    EXPECT_EQ(a.lerp(b, 0.5f), Vector3(5, -5, 2));
}

TEST(Vector3, MoveTowardsStepsWithoutOvershooting){
    const Vector3 from(0, 0, 0);
    const Vector3 to(10, 0, 0);
    EXPECT_EQ(from.moveTowards(to, 3.0f), Vector3(3, 0, 0));
    EXPECT_EQ(from.moveTowards(to, 20.0f), to);
    EXPECT_EQ(to.moveTowards(to, 1.0f), to);
}

TEST(Vector3, MakeFloorAndCeilTakeComponentwiseExtremes){
    Vector3 low(1, 5, 3);
    low.makeFloor(Vector3(2, 4, 3));
    EXPECT_EQ(low, Vector3(1, 4, 3));
    Vector3 high(1, 5, 3);
    high.makeCeil(Vector3(2, 4, 3));
    EXPECT_EQ(high, Vector3(2, 5, 3));
}

TEST(Vector3, PerpendicularIsAUnitVectorAtRightAngles){
    for (const Vector3& v : {Vector3(1, 0, 0), Vector3(0, 3, 0), Vector3(1, 2, 3)}){
        const Vector3 p = v.perpendicular();
        EXPECT_NEAR(p.length(), 1.0f, 1e-5f);
        EXPECT_NEAR(p.dotProduct(v), 0.0f, 1e-5f);
    }
}

TEST(Vector3, ReflectMirrorsAboutTheNormal){
    EXPECT_EQ(Vector3(1, -1, 0).reflect(Vector3::UNIT_Y), Vector3(1, 1, 0));
}

TEST(Vector3, IsValidRejectsNanAndInfinity){
    EXPECT_TRUE(Vector3(1, 2, 3).isValid());
    EXPECT_FALSE(Vector3(std::numeric_limits<float>::quiet_NaN(), 0, 0).isValid());
    EXPECT_FALSE(Vector3(0, std::numeric_limits<float>::infinity(), 0).isValid());
}

TEST(Vector3, ToStringListsTheComponents){
    EXPECT_EQ(Vector3(1, 2, 3).toString(), "Vector3(1.000000, 2.000000, 3.000000)");
}
