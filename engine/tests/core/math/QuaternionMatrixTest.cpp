// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "math/Quaternion.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace jasperengine;

TEST(QuaternionMatrix, PreservesNonUnitCoefficientsAndColumnOrder){
    // Non-unit quaternions are accepted without normalization. Distinct entries
    // also catch transposing the column-indexed matrix into constructor rows.
    const Matrix4 matrix = Quaternion(2, 3, 4, 5).getRotationMatrix();
    const float rows[4][4] = {
        {-81, 4, 46, 0},
        {44, -67, 28, 0},
        {14, 52, -49, 0},
        {0, 0, 0, 1}
    };
    for (int row = 0; row < 4; ++row){
        for (int column = 0; column < 4; ++column){
            EXPECT_EQ(matrix.get(column, row), rows[row][column]);
        }
    }
}

TEST(QuaternionMatrix, PreservesSignedZeroCoefficients){
    const Matrix4 matrix = Quaternion(0.0f, -0.0f, 0.0f, 0.0f).getRotationMatrix();
    EXPECT_TRUE(std::signbit(matrix.get(0, 2)));
    EXPECT_TRUE(std::signbit(matrix.get(1, 0)));
    EXPECT_FALSE(std::signbit(matrix.get(0, 1)));
    EXPECT_FALSE(std::signbit(matrix.get(1, 2)));
    EXPECT_EQ(matrix.get(3, 3), 1.0f);
}

TEST(QuaternionMatrix, KeepsHomogeneousEntriesFiniteForNonFiniteRotations){
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const Quaternion& quaternion : {Quaternion(infinity, infinity, infinity, infinity), Quaternion(nan, nan, nan, nan)}){
        const Matrix4 matrix = quaternion.getRotationMatrix();
        for (int axis = 0; axis < 3; ++axis){
            EXPECT_EQ(matrix.get(3, axis), 0.0f);
            EXPECT_EQ(matrix.get(axis, 3), 0.0f);
        }
        EXPECT_EQ(matrix.get(3, 3), 1.0f);
    }
}
