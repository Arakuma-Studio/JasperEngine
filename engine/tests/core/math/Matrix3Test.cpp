// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MathTestHelpers.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {
    const float m3Tolerance = 1e-5f;

    Matrix3 m3Sample(){
        return Matrix3(2, 0, 1,
                       1, 3, 0,
                       0, 1, 4);
    }
}

TEST(Matrix3, DefaultsToIdentity){
    EXPECT_MATRIX_NEAR(Matrix3(), Matrix3(1, 0, 0, 0, 1, 0, 0, 0, 1), 0.0f);
}

TEST(Matrix3, ConstructorTakesRowsInReadingOrder){
    const Matrix3 m = m3Sample();
    EXPECT_FLOAT_EQ(m.get(2, 0), 1.0f);
    EXPECT_FLOAT_EQ(m.get(0, 1), 1.0f);
    EXPECT_EQ(m.row(1), Vector3(1, 3, 0));
    EXPECT_EQ(m.column(2), Vector3(1, 0, 4));
}

TEST(Matrix3, MultipliesColumnVectors){
    EXPECT_VEC_NEAR(m3Sample() * Vector3(1, 1, 1), Vector3(3, 4, 5), m3Tolerance);
}

TEST(Matrix3, ProductAppliesTheRightOperandFirst){
    const Matrix3 a = m3Sample();
    const Matrix3 b = Matrix3::scaleMatrix(Vector3(1, 2, 3));
    const Vector3 v(1, -1, 2);
    EXPECT_VEC_NEAR((a * b) * v, a * (b * v), m3Tolerance);
}

TEST(Matrix3, Determinant){
    EXPECT_FLOAT_EQ(m3Sample().determinant(), 25.0f);
    EXPECT_FLOAT_EQ(Matrix3::scaleMatrix(2.0f).determinant(), 8.0f);
}

TEST(Matrix3, InverseTimesSelfIsIdentity){
    const Matrix3 m = m3Sample();
    EXPECT_MATRIX_NEAR(m.inverse(1e-6f) * m, Matrix3(), 1e-5f);
}

TEST(Matrix3, CalcInverseRefusesASingularMatrix){
    Matrix3 inverse;
    EXPECT_FALSE(Matrix3(1, 2, 3, 2, 4, 6, 0, 0, 1).calcInverse(inverse, 1e-6f));
    EXPECT_TRUE(m3Sample().calcInverse(inverse, 1e-6f));
}

TEST(Matrix3, TransposeSwapsRowsAndColumns){
    const Matrix3 t = m3Sample().transpose();
    EXPECT_EQ(t.row(0), Vector3(2, 1, 0));
    EXPECT_EQ(t.transpose(), m3Sample());
}

TEST(Matrix3, RotateMatrixMatchesMatrix4){
    const Matrix3 r = Matrix3::rotateMatrix(65.0f, Vector3(1, 2, 2).normalized());
    EXPECT_MATRIX_NEAR(r, Matrix4::rotateMatrix(65.0f, Vector3(1, 2, 2).normalized()).linear(), m3Tolerance);
    EXPECT_VEC_NEAR(Matrix3::rotateMatrix(90.0f, Vector3::UNIT_Z) * Vector3::UNIT_X, Vector3::UNIT_Y, m3Tolerance);
}

TEST(Matrix3, ScaleMatrix){
    EXPECT_VEC_NEAR(Matrix3::scaleMatrix(Vector3(2, 3, 4)) * Vector3(1, 1, 1), Vector3(2, 3, 4), m3Tolerance);
}

TEST(Matrix3, DecomposeQDUSplitsRotationAndScale){
    const Matrix3 rotation = Matrix3::rotateMatrix(30.0f, Vector3::UNIT_Y);
    const Matrix3 m = rotation * Matrix3::scaleMatrix(Vector3(2, 3, 4));
    Matrix3 q;
    Vector3 d, u;
    m.decomposeQDU(q, d, u);
    EXPECT_MATRIX_NEAR(q, rotation, 1e-4f);
    EXPECT_VEC_NEAR(d, Vector3(2, 3, 4), 1e-4f);
    EXPECT_VEC_NEAR(u, Vector3::ZERO, 1e-4f);
}
