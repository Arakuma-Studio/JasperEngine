// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_MATH_HELPERS_H
#define JASPER_TEST_MATH_HELPERS_H

#include "math/Matrix3.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <gtest/gtest.h>

#include <cmath>

namespace jasperengine { namespace test {

    inline ::testing::AssertionResult vectorNear(const Vector3& actual, const Vector3& expected, float tolerance){
        if (std::fabs(actual.x - expected.x) <= tolerance && std::fabs(actual.y - expected.y) <= tolerance &&
            std::fabs(actual.z - expected.z) <= tolerance)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << actual.toString() << " is not within " << tolerance << " of " << expected.toString();
    }

    inline ::testing::AssertionResult vectorNear(const Vector2& actual, const Vector2& expected, float tolerance){
        if (std::fabs(actual.x - expected.x) <= tolerance && std::fabs(actual.y - expected.y) <= tolerance)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << actual.toString() << " is not within " << tolerance << " of " << expected.toString();
    }

    inline ::testing::AssertionResult vectorNear(const Vector4& actual, const Vector4& expected, float tolerance){
        if (std::fabs(actual.x - expected.x) <= tolerance && std::fabs(actual.y - expected.y) <= tolerance &&
            std::fabs(actual.z - expected.z) <= tolerance && std::fabs(actual.w - expected.w) <= tolerance)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << actual.toString() << " is not within " << tolerance << " of " << expected.toString();
    }

    // q and -q are the same rotation
    inline ::testing::AssertionResult rotationNear(const Quaternion& actual, const Quaternion& expected, float tolerance){
        const float same = std::fabs(actual.w - expected.w) + std::fabs(actual.x - expected.x) +
                           std::fabs(actual.y - expected.y) + std::fabs(actual.z - expected.z);
        const float opposite = std::fabs(actual.w + expected.w) + std::fabs(actual.x + expected.x) +
                               std::fabs(actual.y + expected.y) + std::fabs(actual.z + expected.z);
        if (same <= tolerance || opposite <= tolerance)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << actual.toString() << " is not the rotation " << expected.toString();
    }

    inline ::testing::AssertionResult matrixNear(const Matrix4& actual, const Matrix4& expected, float tolerance){
        for (int col = 0; col < 4; col++){
            for (int row = 0; row < 4; row++){
                if (std::fabs(actual.get(col, row) - expected.get(col, row)) > tolerance)
                    return ::testing::AssertionFailure() << "element (col " << col << ", row " << row << ") " << actual.get(col, row)
                                                         << " != " << expected.get(col, row) << "\n" << actual.toString();
            }
        }
        return ::testing::AssertionSuccess();
    }

    inline ::testing::AssertionResult matrixNear(const Matrix3& actual, const Matrix3& expected, float tolerance){
        for (int col = 0; col < 3; col++){
            for (int row = 0; row < 3; row++){
                if (std::fabs(actual.get(col, row) - expected.get(col, row)) > tolerance)
                    return ::testing::AssertionFailure() << "element (col " << col << ", row " << row << ") " << actual.get(col, row)
                                                         << " != " << expected.get(col, row);
            }
        }
        return ::testing::AssertionSuccess();
    }

} }

#define EXPECT_VEC_NEAR(actual, expected, tolerance) EXPECT_TRUE(::jasperengine::test::vectorNear((actual), (expected), (tolerance)))
#define EXPECT_ROTATION_NEAR(actual, expected, tolerance) EXPECT_TRUE(::jasperengine::test::rotationNear((actual), (expected), (tolerance)))
#define EXPECT_MATRIX_NEAR(actual, expected, tolerance) EXPECT_TRUE(::jasperengine::test::matrixNear((actual), (expected), (tolerance)))

#endif //JASPER_TEST_MATH_HELPERS_H
