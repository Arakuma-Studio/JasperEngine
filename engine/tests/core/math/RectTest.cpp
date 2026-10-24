// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include "math/Rect.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(Rect, DefaultsToTheUnitRect){
    EXPECT_EQ(Rect().getVector(), Vector4(0, 0, 1, 1));
    EXPECT_TRUE(Rect().isNormalized());
    EXPECT_TRUE(Rect(0, 0, 0, 0).isZero());
    EXPECT_FALSE(Rect(0, 0, 1, 0).isZero());
}

TEST(Rect, AccessorsAndIndex){
    Rect rect(1, 2, 3, 4);
    EXPECT_FLOAT_EQ(rect.getX(), 1);
    EXPECT_FLOAT_EQ(rect.getHeight(), 4);
    EXPECT_FLOAT_EQ(rect[2], 3);
    rect.setWidth(7);
    EXPECT_EQ(rect.getVector(), Vector4(1, 2, 7, 4));
}

TEST(Rect, ContainsIncludesTheEdges){
    Rect rect(0, 0, 10, 5);
    EXPECT_TRUE(rect.contains(Vector2(0, 0)));
    EXPECT_TRUE(rect.contains(Vector2(10, 5)));
    EXPECT_FALSE(rect.contains(Vector2(10.1f, 2)));
    EXPECT_FALSE(rect.contains(Vector2(2, -0.1f)));
}

TEST(Rect, FitOnRectClipsToTheOuterRect){
    Rect rect(-5, 2, 20, 20);
    rect.fitOnRect(Rect(0, 0, 10, 10));
    EXPECT_FLOAT_EQ(rect.getX(), 0);
    EXPECT_FLOAT_EQ(rect.getY(), 2);
    EXPECT_FLOAT_EQ(rect.getWidth(), 10);
    EXPECT_FLOAT_EQ(rect.getHeight(), 8);
}

TEST(Rect, FitOnRectOfADisjointRectIsEmpty){
    Rect rect(20, 20, 5, 5);
    rect.fitOnRect(Rect(0, 0, 10, 10));
    EXPECT_FLOAT_EQ(rect.getWidth(), 0);
    EXPECT_FLOAT_EQ(rect.getHeight(), 0);
}

TEST(Rect, IsNormalizedWithinZeroToOne){
    EXPECT_TRUE(Rect(0, 0, 1, 1).isNormalized());
    EXPECT_TRUE(Rect(0.25f, 0.5f, 0.5f, 0.5f).isNormalized());
    EXPECT_FALSE(Rect(0, 0, 2, 1).isNormalized());
}

TEST(Rect, EqualityAndAssignment){
    Rect a(1, 2, 3, 4);
    Rect b;
    b = Vector4(1, 2, 3, 4);
    EXPECT_TRUE(a == b);
    b.setY(9);
    EXPECT_TRUE(a != b);
}
