// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(Vector2, DefaultsToZero){
    EXPECT_EQ(Vector2(), Vector2::ZERO);
    EXPECT_EQ(Vector2::UNIT_X, Vector2(1, 0));
    EXPECT_EQ(Vector2::NEGATIVE_UNIT_Y, Vector2(0, -1));
}

TEST(Vector2, ArithmeticIsComponentwise){
    const Vector2 a(1, 2);
    const Vector2 b(3, -4);
    EXPECT_EQ(a + b, Vector2(4, -2));
    EXPECT_EQ(a - b, Vector2(-2, 6));
    EXPECT_EQ(a * b, Vector2(3, -8));
    EXPECT_EQ(b / a, Vector2(3, -2));
    EXPECT_EQ(a * 3.0f, Vector2(3, 6));
    EXPECT_EQ(a / 2.0f, Vector2(0.5f, 1));
}

TEST(Vector2, ScalarOnEitherSideAddsToBothComponents){
    EXPECT_EQ(Vector2(1, 2) + 1.0f, Vector2(2, 3));
    EXPECT_EQ(1.0f + Vector2(1, 2), Vector2(2, 3));
    EXPECT_EQ(Vector2(1, 2) - 1.0f, Vector2(0, 1));
    EXPECT_EQ(5.0f - Vector2(1, 2), Vector2(4, 3));
    EXPECT_EQ(2.0f * Vector2(1, 2), Vector2(2, 4));
    EXPECT_EQ(4.0f / Vector2(1, 2), Vector2(4, 2));
}

TEST(Vector2, CompoundAssignments){
    Vector2 v(2, 4);
    v += Vector2(1, 1);
    v -= 1.0f;
    v *= Vector2(2, 0.5f);
    v *= 0.5f;
    EXPECT_EQ(v, Vector2(2, 1));
}

TEST(Vector2, DivideAssignmentsMatchTheDivisionOperators){
    Vector2 v(3, -6);
    Vector2& scaled = (v /= 2.0f);
    EXPECT_EQ(&scaled, &v);
    EXPECT_EQ(v, Vector2(1.5f, -3));
    EXPECT_EQ(v, Vector2(3, -6) / 2.0f);

    Vector2 w(3, -6);
    Vector2& divided = (w /= Vector2(2, -4));
    EXPECT_EQ(&divided, &w);
    EXPECT_EQ(w, Vector2(1.5f, 1.5f));
    EXPECT_EQ(w, Vector2(3, -6) / Vector2(2, -4));
}

TEST(Vector2, LengthDistanceAndDot){
    EXPECT_FLOAT_EQ(Vector2(3, 4).length(), 5.0f);
    EXPECT_FLOAT_EQ(Vector2(3, 4).squaredLength(), 25.0f);
    EXPECT_FLOAT_EQ(Vector2(1, 1).distance(Vector2(4, 5)), 5.0f);
    EXPECT_FLOAT_EQ(Vector2(1, 2).dotProduct(Vector2(3, 4)), 11.0f);
}

TEST(Vector2, NormalizesToUnitLength){
    Vector2 v(0, -8);
    EXPECT_FLOAT_EQ(v.normalizeL(), 8.0f);
    EXPECT_EQ(v, Vector2(0, -1));
    EXPECT_EQ(Vector2(5, 0).normalizedCopy(), Vector2::UNIT_X);
}

TEST(Vector2, CrossProductIsTheZOfThe3DCross){
    EXPECT_FLOAT_EQ(Vector2::UNIT_X.crossProduct(Vector2::UNIT_Y), 1.0f);
    EXPECT_FLOAT_EQ(Vector2::UNIT_Y.crossProduct(Vector2::UNIT_X), -1.0f);
}

TEST(Vector2, PerpendicularRotatesAQuarterTurnCounterClockwise){
    EXPECT_EQ(Vector2(1, 0).perpendicular(), Vector2(0, 1));
    EXPECT_EQ(Vector2(0, 1).perpendicular(), Vector2(-1, 0));
}

TEST(Vector2, ReflectMirrorsAboutTheNormal){
    EXPECT_EQ(Vector2(1, -1).reflect(Vector2(0, 1)), Vector2(1, 1));
}

TEST(Vector2, MidPointFloorAndCeil){
    EXPECT_EQ(Vector2(0, 0).midPoint(Vector2(2, -2)), Vector2(1, -1));
    Vector2 low(3, 1);
    low.makeFloor(Vector2(2, 2));
    EXPECT_EQ(low, Vector2(2, 1));
    Vector2 high(3, 1);
    high.makeCeil(Vector2(2, 2));
    EXPECT_EQ(high, Vector2(3, 2));
}

TEST(Vector2, ConvertsFromVector3ByDroppingZ){
    EXPECT_EQ(Vector2(Vector3(1, 2, 3)), Vector2(1, 2));
}

TEST(Vector2, ToString){
    EXPECT_EQ(Vector2(1, -2).toString(), "Vector2(1.000000, -2.000000)");
}

TEST(Vector2, MoveTowardsAndLerp){
    const Vector2 from(0, 0);
    const Vector2 to(3, 4);
    EXPECT_EQ(from.moveTowards(to, 2.5f), Vector2(1.5f, 2.0f));
    EXPECT_EQ(from.moveTowards(to, 10.0f), to);
    EXPECT_EQ(to.moveTowards(to, 1.0f), to);
    EXPECT_EQ(from.moveTowards(to, -5.0f), Vector2(-3, -4));
    EXPECT_EQ(from.lerp(to, 0.5f), Vector2(1.5f, 2.0f));
    EXPECT_EQ(from.lerp(to, 3.0f), to);
    EXPECT_EQ(from.lerp(to, -1.0f), from);
    EXPECT_EQ(from.lerpUnclamped(to, 2.0f), Vector2(6, 8));
}
