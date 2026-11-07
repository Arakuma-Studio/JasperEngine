// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Matrix4 stores columns (get/set take the column first) and multiplies column vectors.
// Angles are in degrees (the engine default), except perspectiveMatrix's field of view.

#include "MathTestHelpers.h"

#include "math/AABB.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

namespace {
    const float m4Tolerance = 1e-5f;

    Matrix4 m4Sample(){
        return Matrix4(2, 0, 1, 3,
                       1, 3, 0, -1,
                       0, 1, 4, 2,
                       0, 0, 0, 1);
    }
}

TEST(Matrix4, DefaultsToIdentity){
    const Matrix4 m;
    for (int col = 0; col < 4; col++){
        for (int row = 0; row < 4; row++){
            EXPECT_FLOAT_EQ(m.get(col, row), col == row ? 1.0f : 0.0f);
        }
    }
}

TEST(Matrix4, ConstructorTakesRowsInReadingOrder){
    const Matrix4 m = m4Sample();
    EXPECT_FLOAT_EQ(m.get(3, 0), 3.0f);    // row 0, column 3
    EXPECT_FLOAT_EQ(m.get(0, 1), 1.0f);    // row 1, column 0
    EXPECT_EQ(m.row(0), Vector4(2, 0, 1, 3));
    EXPECT_EQ(m.column(3), Vector4(3, -1, 2, 1));
}

TEST(Matrix4, SetRowAndColumn){
    Matrix4 m;
    m.setRow(1, Vector4(1, 2, 3, 4));
    EXPECT_EQ(m.row(1), Vector4(1, 2, 3, 4));
    m.setColumn(2, Vector4(5, 6, 7, 8));
    EXPECT_EQ(m.column(2), Vector4(5, 6, 7, 8));
}

TEST(Matrix4, IdentityIsTheNeutralElement){
    const Matrix4 m = m4Sample();
    EXPECT_EQ(m * Matrix4(), m);
    EXPECT_EQ(Matrix4() * m, m);
}

TEST(Matrix4, MultipliesRowsByColumns){
    const Matrix4 a(1, 2, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1);
    const Matrix4 b(1, 0, 0, 0,
                    3, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1);
    const Matrix4 expected(7, 2, 0, 0,
                           3, 1, 0, 0,
                           0, 0, 1, 0,
                           0, 0, 0, 1);
    EXPECT_MATRIX_NEAR(a * b, expected, m4Tolerance);
}

TEST(Matrix4, AddsSubtractsAndScales){
    const Matrix4 m = m4Sample();
    EXPECT_MATRIX_NEAR(m + m, m * 2.0f, m4Tolerance);
    EXPECT_MATRIX_NEAR(m - m, Matrix4() * 0.0f, m4Tolerance);
}

TEST(Matrix4, InverseTimesSelfIsIdentity){
    const Matrix4 m = m4Sample();
    EXPECT_MATRIX_NEAR(m.inverse() * m, Matrix4(), 1e-5f);
    EXPECT_MATRIX_NEAR(m * m.inverse(), Matrix4(), 1e-5f);
}

TEST(Matrix4, InverseOfATransformUndoesIt){
    const Matrix4 m = Matrix4::translateMatrix(1, 2, 3) * Matrix4::rotateMatrix(40.0f, Vector3(0, 1, 0)) * Matrix4::scaleMatrix(2.0f);
    const Vector3 p(4, -5, 6);
    EXPECT_VEC_NEAR(m.inverse() * (m * p), p, 1e-4f);
}

TEST(Matrix4, TransposeSwapsRowsAndColumns){
    const Matrix4 m = m4Sample();
    const Matrix4 t = m.transpose();
    for (int col = 0; col < 4; col++){
        for (int row = 0; row < 4; row++){
            EXPECT_FLOAT_EQ(t.get(col, row), m.get(row, col));
        }
    }
    EXPECT_EQ(t.transpose(), m);
}

TEST(Matrix4, DeterminantOfKnownMatrices){
    EXPECT_FLOAT_EQ(Matrix4().determinant(), 1.0f);
    EXPECT_FLOAT_EQ(Matrix4::scaleMatrix(Vector3(2, 3, 4)).determinant(), 24.0f);
    EXPECT_FLOAT_EQ(m4Sample().determinant(), 25.0f);
}

TEST(Matrix4, TranslateMovesPointsNotDirections){
    const Matrix4 t = Matrix4::translateMatrix(Vector3(1, 2, 3));
    EXPECT_VEC_NEAR(t * Vector3(1, 1, 1), Vector3(2, 3, 4), m4Tolerance);
    EXPECT_VEC_NEAR(t * Vector4(1, 1, 1, 0), Vector4(1, 1, 1, 0), m4Tolerance);
    EXPECT_EQ(Matrix4::translateMatrix(1, 2, 3), t);
}

TEST(Matrix4, TranslateInPlaceAppliesTheTranslationFirst){
    Matrix4 m = Matrix4::scaleMatrix(2.0f);
    m.translateInPlace(1, 0, 0);
    EXPECT_VEC_NEAR(m * Vector3::ZERO, Vector3(2, 0, 0), m4Tolerance);
}

TEST(Matrix4, ScaleMatrixScalesEachAxis){
    EXPECT_VEC_NEAR(Matrix4::scaleMatrix(Vector3(2, 3, 4)) * Vector3(1, 1, 1), Vector3(2, 3, 4), m4Tolerance);
    EXPECT_VEC_NEAR(Matrix4::scaleMatrix(5.0f) * Vector3(1, 2, 3), Vector3(5, 10, 15), m4Tolerance);
}

TEST(Matrix4, RotateMatrixAboutAnAxisIsCounterClockwise){
    EXPECT_VEC_NEAR(Matrix4::rotateMatrix(90.0f, Vector3::UNIT_Z) * Vector3::UNIT_X, Vector3::UNIT_Y, m4Tolerance);
    EXPECT_VEC_NEAR(Matrix4::rotateMatrix(90.0f, Vector3::UNIT_X) * Vector3::UNIT_Y, Vector3::UNIT_Z, m4Tolerance);
    EXPECT_VEC_NEAR(Matrix4::rotateMatrix(90.0f, Vector3::UNIT_Y) * Vector3::UNIT_Z, Vector3::UNIT_X, m4Tolerance);
}

TEST(Matrix4, RotateMatrixNormalizesTheAxis){
    EXPECT_MATRIX_NEAR(Matrix4::rotateMatrix(30.0f, Vector3(0, 0, 5)), Matrix4::rotateMatrix(30.0f, Vector3::UNIT_Z), m4Tolerance);
}

// the single-axis helpers: rotateYMatrix turns like rotateMatrix about Y, rotateXMatrix and
// rotateZMatrix the other way round (the current contract, pinned here)
TEST(Matrix4, RotateXMatrixTurnsYTowardMinusZ){
    EXPECT_VEC_NEAR(Matrix4::rotateXMatrix(90.0f) * Vector3::UNIT_Y, -Vector3::UNIT_Z, m4Tolerance);
}

TEST(Matrix4, RotateYMatrixTurnsXTowardMinusZ){
    EXPECT_VEC_NEAR(Matrix4::rotateYMatrix(90.0f) * Vector3::UNIT_X, -Vector3::UNIT_Z, m4Tolerance);
    EXPECT_MATRIX_NEAR(Matrix4::rotateYMatrix(25.0f), Matrix4::rotateMatrix(25.0f, Vector3::UNIT_Y), m4Tolerance);
}

TEST(Matrix4, RotateZMatrixTurnsXTowardMinusY){
    EXPECT_VEC_NEAR(Matrix4::rotateZMatrix(90.0f) * Vector3::UNIT_X, -Vector3::UNIT_Y, m4Tolerance);
}

TEST(Matrix4, RotationsKeepLengths){
    const Vector3 v(1, 2, 3);
    EXPECT_NEAR((Matrix4::rotateMatrix(73.0f, Vector3(1, 1, 0)) * v).length(), v.length(), 1e-5f);
}

TEST(Matrix4, LookAtMovesTheEyeToTheOriginLookingDownMinusZ){
    const Matrix4 view = Matrix4::lookAtMatrix(Vector3(0, 0, 5), Vector3::ZERO, Vector3::UNIT_Y);
    EXPECT_VEC_NEAR(view * Vector3(0, 0, 5), Vector3::ZERO, m4Tolerance);
    EXPECT_VEC_NEAR(view * Vector3::ZERO, Vector3(0, 0, -5), m4Tolerance);
    EXPECT_VEC_NEAR(view * Vector3(1, 0, 5), Vector3(1, 0, 0), m4Tolerance);
}

TEST(Matrix4, LookAtFromTheSideRotatesTheWorld){
    const Matrix4 view = Matrix4::lookAtMatrix(Vector3(5, 0, 0), Vector3::ZERO, Vector3::UNIT_Y);
    // the target is straight ahead, 5 units down -Z; world +Z is to the left
    EXPECT_VEC_NEAR(view * Vector3::ZERO, Vector3(0, 0, -5), m4Tolerance);
    EXPECT_VEC_NEAR(view * Vector3(5, 0, 1), Vector3(-1, 0, 0), m4Tolerance);
    EXPECT_VEC_NEAR(view * Vector3(5, 1, 0), Vector3(0, 1, 0), m4Tolerance);
}

TEST(Matrix4, OrthoMapsTheBoxToTheClipCube){
    const Matrix4 ortho = Matrix4::orthoMatrix(-2, 2, -1, 1, 1, 11);
    EXPECT_FLOAT_EQ(ortho.get(0, 0), 0.5f);
    EXPECT_FLOAT_EQ(ortho.get(1, 1), 1.0f);
    EXPECT_FLOAT_EQ(ortho.get(2, 2), -0.2f);
    EXPECT_FLOAT_EQ(ortho.get(3, 2), -1.2f);
    EXPECT_VEC_NEAR(ortho * Vector3(2, 1, -1), Vector3(1, 1, -1), m4Tolerance);
    EXPECT_VEC_NEAR(ortho * Vector3(-2, -1, -11), Vector3(-1, -1, 1), m4Tolerance);
}

TEST(Matrix4, PerspectiveTakesTheFieldOfViewInRadians){
    const float fov = 1.57079633f;   // 90 degrees
    const Matrix4 p = Matrix4::perspectiveMatrix(fov, 2.0f, 1.0f, 3.0f);
    EXPECT_NEAR(p.get(0, 0), 0.5f, m4Tolerance);
    EXPECT_NEAR(p.get(1, 1), 1.0f, m4Tolerance);
    EXPECT_NEAR(p.get(2, 2), -2.0f, m4Tolerance);
    EXPECT_NEAR(p.get(2, 3), -1.0f, m4Tolerance);
    EXPECT_NEAR(p.get(3, 2), -3.0f, m4Tolerance);
    EXPECT_NEAR(p.get(3, 3), 0.0f, m4Tolerance);
}

TEST(Matrix4, PerspectiveMapsNearAndFarToTheClipRange){
    const Matrix4 p = Matrix4::perspectiveMatrix(1.0f, 1.5f, 0.5f, 100.0f);
    EXPECT_NEAR((p * Vector3(0, 0, -0.5f)).z, -1.0f, 1e-4f);
    EXPECT_NEAR((p * Vector3(0, 0, -100.0f)).z, 1.0f, 1e-4f);
}

TEST(Matrix4, FrustumOfASymmetricVolumeMatchesPerspective){
    const float near = 1.0f;
    const float top = near * std::tan(0.5f);
    const Matrix4 frustum = Matrix4::frustumMatrix(-top * 1.5f, top * 1.5f, -top, top, near, 50.0f);
    EXPECT_MATRIX_NEAR(frustum, Matrix4::perspectiveMatrix(1.0f, 1.5f, near, 50.0f), 1e-4f);
}

TEST(Matrix4, LinearIsTheUpperLeft3x3){
    const Matrix3 m = m4Sample().linear();
    for (int col = 0; col < 3; col++){
        for (int row = 0; row < 3; row++){
            EXPECT_FLOAT_EQ(m.get(col, row), m4Sample().get(col, row));
        }
    }
}

TEST(Matrix4, DecomposeRecoversTranslationRotationAndScale){
    const Quaternion rotation(35.0f, Vector3(1, 2, 0).normalized());
    const Matrix4 m = Matrix4::translateMatrix(3, -2, 7) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(Vector3(2, 3, 4));
    Vector3 position, scale;
    Quaternion recovered;
    m.decompose(position, scale, recovered);
    EXPECT_VEC_NEAR(position, Vector3(3, -2, 7), 1e-4f);
    EXPECT_VEC_NEAR(scale, Vector3(2, 3, 4), 1e-4f);
    EXPECT_ROTATION_NEAR(recovered, rotation, 1e-4f);
}

TEST(Matrix4, DecomposeStandardRecoversTheSameParts){
    const Quaternion rotation(-60.0f, Vector3::UNIT_Z);
    const Matrix4 m = Matrix4::translateMatrix(1, 2, 3) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(Vector3(1, 2, 0.5f));
    Vector3 position, scale;
    Quaternion recovered;
    m.decomposeStandard(position, scale, recovered);
    EXPECT_VEC_NEAR(position, Vector3(1, 2, 3), 1e-4f);
    EXPECT_VEC_NEAR(scale, Vector3(1, 2, 0.5f), 1e-4f);
    EXPECT_ROTATION_NEAR(recovered, rotation, 1e-4f);
}

TEST(Matrix4, TransformingAnAABBTransformsItsCorners){
    const AABB box(Vector3(-1, -1, -1), Vector3(1, 1, 1));
    const AABB moved = Matrix4::translateMatrix(5, 0, 0) * box;
    EXPECT_VEC_NEAR(moved.getMinimum(), Vector3(4, -1, -1), m4Tolerance);
    EXPECT_VEC_NEAR(moved.getMaximum(), Vector3(6, 1, 1), m4Tolerance);
}

TEST(Matrix4, EqualityComparesEveryElement){
    Matrix4 a = m4Sample();
    Matrix4 b = m4Sample();
    EXPECT_TRUE(a == b);
    b.set(2, 3, 9.0f);
    EXPECT_TRUE(a != b);
}

TEST(Matrix4, IsValidRejectsNan){
    Matrix4 m;
    EXPECT_TRUE(m.isValid());
    m.set(1, 1, std::nanf(""));
    EXPECT_FALSE(m.isValid());
}

namespace {
    // the NDC depth of a view-space point under a projection
    float m4NdcDepth(const Matrix4& projection, const Vector3& view){
        const Vector4 clip = projection * Vector4(view.x, view.y, view.z, 1.0f);
        return clip.z / clip.w;
    }

    // the oblique projection puts the near plane on the clip plane (points on it at NDC -1, the
    // side it faces inside, the other side clipped) and keeps the x, y and w rows
    void m4ExpectObliqueNearClip(const Matrix4& projection){
        // view-space plane y + 0.02 z + 1 = 0, facing +y; the frustum's far corner on its facing
        // side stays in front of it, as the method needs (a reflection plane under the eye)
        const Vector4 plane(0.0f, 1.0f, 0.02f, 1.0f);
        const Matrix4 oblique = projection.obliqueNearClip(plane);
        EXPECT_NEAR(m4NdcDepth(oblique, Vector3(0.0f, -0.9f, -5.0f)), -1.0f, 1e-4f);
        EXPECT_NEAR(m4NdcDepth(oblique, Vector3(0.5f, -0.8f, -10.0f)), -1.0f, 1e-4f);
        const float front = m4NdcDepth(oblique, Vector3(0.0f, 0.0f, -5.0f));
        EXPECT_GT(front, -1.0f);
        EXPECT_LT(front, 1.0f);
        EXPECT_LT(m4NdcDepth(oblique, Vector3(0.0f, -2.0f, -5.0f)), -1.0f);
        for (int col = 0; col < 4; col++){
            EXPECT_FLOAT_EQ(oblique.get(col, 0), projection.get(col, 0));
            EXPECT_FLOAT_EQ(oblique.get(col, 1), projection.get(col, 1));
            EXPECT_FLOAT_EQ(oblique.get(col, 3), projection.get(col, 3));
        }
    }
}

TEST(Matrix4, ObliqueNearClipPerspective){
    m4ExpectObliqueNearClip(Matrix4::perspectiveMatrix(1.0f, 1.5f, 0.5f, 100.0f));
}

TEST(Matrix4, ObliqueNearClipOrthographic){
    m4ExpectObliqueNearClip(Matrix4::orthoMatrix(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f));
}
