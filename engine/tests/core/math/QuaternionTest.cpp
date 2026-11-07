// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Angles are in the engine's default unit, degrees (Engine::isUseDegrees).

#include "MathTestHelpers.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

namespace {
    const float quatTolerance = 1e-5f;
}

TEST(Quaternion, DefaultsToIdentity){
    EXPECT_EQ(Quaternion(), Quaternion::IDENTITY);
    EXPECT_EQ(Quaternion::IDENTITY, Quaternion(1, 0, 0, 0));
}

TEST(Quaternion, IdentityLeavesVectorsAlone){
    EXPECT_VEC_NEAR(Quaternion::IDENTITY * Vector3(1, 2, 3), Vector3(1, 2, 3), quatTolerance);
}

TEST(Quaternion, AxisAngleTakesDegrees){
    const Quaternion q(90.0f, Vector3::UNIT_Z);
    EXPECT_NEAR(q.w, std::cos(0.785398163f), quatTolerance);
    EXPECT_NEAR(q.z, std::sin(0.785398163f), quatTolerance);
    EXPECT_NEAR(q.x, 0.0f, quatTolerance);
    EXPECT_NEAR(q.y, 0.0f, quatTolerance);
}

TEST(Quaternion, QuarterTurnAboutZTurnsXIntoY){
    EXPECT_VEC_NEAR(Quaternion(90.0f, Vector3::UNIT_Z) * Vector3::UNIT_X, Vector3::UNIT_Y, quatTolerance);
}

TEST(Quaternion, QuarterTurnAboutYTurnsZIntoX){
    EXPECT_VEC_NEAR(Quaternion(90.0f, Vector3::UNIT_Y) * Vector3::UNIT_Z, Vector3::UNIT_X, quatTolerance);
}

TEST(Quaternion, QuarterTurnAboutXTurnsYIntoZ){
    EXPECT_VEC_NEAR(Quaternion(90.0f, Vector3::UNIT_X) * Vector3::UNIT_Y, Vector3::UNIT_Z, quatTolerance);
}

TEST(Quaternion, ProductComposesRotationsRightToLeft){
    const Quaternion a(90.0f, Vector3::UNIT_Z);
    const Quaternion b(90.0f, Vector3::UNIT_X);
    const Vector3 v(1, 2, 3);
    EXPECT_VEC_NEAR((a * b) * v, a * (b * v), quatTolerance);
}

TEST(Quaternion, TwoQuarterTurnsMakeAHalfTurn){
    const Quaternion quarter(90.0f, Vector3::UNIT_Y);
    EXPECT_ROTATION_NEAR(quarter * quarter, Quaternion(180.0f, Vector3::UNIT_Y), quatTolerance);
}

TEST(Quaternion, InverseUndoesTheRotation){
    const Quaternion q(37.0f, Vector3(1, 2, 3).normalized());
    const Vector3 v(4, -1, 2);
    EXPECT_VEC_NEAR(q.inverse() * (q * v), v, 1e-4f);
    EXPECT_ROTATION_NEAR(q * q.inverse(), Quaternion::IDENTITY, quatTolerance);
}

TEST(Quaternion, InverseOfAScaledQuaternionDividesByTheNorm){
    const Quaternion q(2, 0, 0, 0);
    EXPECT_ROTATION_NEAR(q.inverse(), Quaternion(0.5f, 0, 0, 0), quatTolerance);
}

TEST(Quaternion, UnitInverseIsTheConjugate){
    EXPECT_EQ(Quaternion(0.5f, 0.5f, -0.5f, 0.5f).unitInverse(), Quaternion(0.5f, -0.5f, 0.5f, -0.5f));
}

TEST(Quaternion, NormIsTheSquaredLength){
    EXPECT_FLOAT_EQ(Quaternion(1, 2, 3, 4).norm(), 30.0f);
    EXPECT_FLOAT_EQ(Quaternion(1, 2, 3, 4).dot(Quaternion(1, 0, 0, 1)), 5.0f);
}

TEST(Quaternion, NormalizeMakesAUnitQuaternion){
    Quaternion q(2, 0, 0, 2);
    // normalizeL returns the norm (squared length) it divided by the square root of
    EXPECT_FLOAT_EQ(q.normalizeL(), 8.0f);
    EXPECT_NEAR(q.norm(), 1.0f, quatTolerance);
    EXPECT_NEAR(Quaternion(0, 3, 0, 0).normalized().x, 1.0f, quatTolerance);
}

TEST(Quaternion, SlerpReturnsTheEndPoints){
    const Quaternion a(10.0f, Vector3::UNIT_Y);
    const Quaternion b(80.0f, Vector3::UNIT_Y);
    EXPECT_ROTATION_NEAR(Quaternion::slerp(0.0f, a, b), a, quatTolerance);
    EXPECT_ROTATION_NEAR(Quaternion::slerp(1.0f, a, b), b, quatTolerance);
}

TEST(Quaternion, SlerpHalfwayIsTheMiddleAngle){
    const Quaternion a(0.0f, Vector3::UNIT_Z);
    const Quaternion b(90.0f, Vector3::UNIT_Z);
    EXPECT_ROTATION_NEAR(Quaternion::slerp(0.5f, a, b), Quaternion(45.0f, Vector3::UNIT_Z), quatTolerance);
}

TEST(Quaternion, SlerpTakesTheShortestPath){
    const Quaternion a(10.0f, Vector3::UNIT_Z);
    const Quaternion b = -Quaternion(30.0f, Vector3::UNIT_Z);   // the same rotation, the other hemisphere
    EXPECT_ROTATION_NEAR(Quaternion::slerp(0.5f, a, b), Quaternion(20.0f, Vector3::UNIT_Z), quatTolerance);
}

TEST(Quaternion, SlerpOfEqualQuaternionsIsThatQuaternion){
    const Quaternion a(25.0f, Vector3::UNIT_X);
    EXPECT_ROTATION_NEAR(Quaternion::slerp(0.3f, a, a), a, quatTolerance);
}

TEST(Quaternion, NlerpIsNormalizedAndHitsTheEndPoints){
    const Quaternion a(0.0f, Vector3::UNIT_X);
    const Quaternion b(60.0f, Vector3::UNIT_X);
    EXPECT_ROTATION_NEAR(Quaternion::nlerp(0.0f, a, b), a, quatTolerance);
    EXPECT_ROTATION_NEAR(Quaternion::nlerp(1.0f, a, b), b, quatTolerance);
    EXPECT_NEAR(Quaternion::nlerp(0.4f, a, b).norm(), 1.0f, quatTolerance);
}

TEST(Quaternion, RotationMatrixRotatesLikeTheQuaternion){
    const Quaternion q(50.0f, Vector3(1, -1, 2).normalized());
    const Vector3 v(0.5f, 2, -3);
    EXPECT_VEC_NEAR(q.getRotationMatrix() * v, q * v, 1e-4f);
}

TEST(Quaternion, RoundTripsThroughAMatrix4){
    const Quaternion q(120.0f, Vector3(0, 1, 1).normalized());
    EXPECT_ROTATION_NEAR(Quaternion(q.getRotationMatrix()), q, 1e-4f);
}

TEST(Quaternion, RoundTripsThroughAMatrix3){
    const Quaternion q(-70.0f, Vector3(1, 0, 1).normalized());
    EXPECT_ROTATION_NEAR(Quaternion(q.getRotationMatrix().linear()), q, 1e-4f);
}

TEST(Quaternion, FromMatrixCoversEveryBranch){
    // a trace above zero, then the three largest-diagonal cases (half turns)
    for (const Vector3& axis : {Vector3::UNIT_X, Vector3::UNIT_Y, Vector3::UNIT_Z}){
        for (float angle : {30.0f, 180.0f}){
            const Quaternion q(angle, axis);
            EXPECT_ROTATION_NEAR(Quaternion(q.getRotationMatrix()), q, 1e-4f) << angle << " about " << axis.toString();
        }
    }
}

TEST(Quaternion, EulerConstructorAppliesXThenYThenZ){
    const Quaternion q(90.0f, 0.0f, 90.0f);
    const Quaternion expected = Quaternion(90.0f, Vector3::UNIT_Z) * Quaternion(90.0f, Vector3::UNIT_X);
    EXPECT_ROTATION_NEAR(q, expected, quatTolerance);
}

TEST(Quaternion, EulerOrderSelectsTheProduct){
    const Quaternion x(30.0f, Vector3::UNIT_X);
    const Quaternion y(40.0f, Vector3::UNIT_Y);
    const Quaternion z(50.0f, Vector3::UNIT_Z);
    EXPECT_ROTATION_NEAR(Quaternion(30.0f, 40.0f, 50.0f, RotationOrder::XYZ), x * (y * z), quatTolerance);
    EXPECT_ROTATION_NEAR(Quaternion(30.0f, 40.0f, 50.0f, RotationOrder::YXZ), y * (x * z), quatTolerance);
    EXPECT_ROTATION_NEAR(Quaternion(30.0f, 40.0f, 50.0f, RotationOrder::ZXY), z * (x * y), quatTolerance);
}

TEST(Quaternion, EulerAnglesRoundTripForOneAxis){
    const Vector3 angles = Quaternion(0.0f, 0.0f, 35.0f).getEulerAngles(RotationOrder::ZYX);
    EXPECT_VEC_NEAR(angles, Vector3(0, 0, 35), 1e-3f);
}

TEST(Quaternion, AxesAreTheRotatedUnitVectors){
    const Quaternion q(90.0f, Vector3::UNIT_Z);
    EXPECT_VEC_NEAR(q.xAxis(), Vector3::UNIT_Y, quatTolerance);
    EXPECT_VEC_NEAR(q.yAxis(), -Vector3::UNIT_X, quatTolerance);
    EXPECT_VEC_NEAR(q.zAxis(), Vector3::UNIT_Z, quatTolerance);
}

TEST(Quaternion, FromAxesBuildsThatBasis){
    const Quaternion q(Vector3::UNIT_Y, -Vector3::UNIT_X, Vector3::UNIT_Z);
    EXPECT_ROTATION_NEAR(q, Quaternion(90.0f, Vector3::UNIT_Z), quatTolerance);
}

TEST(Quaternion, LookRotationPointsZAtTheTarget){
    const Quaternion q = Quaternion::lookRotation(Vector3(1, 0, 0), Vector3::UNIT_Y);
    EXPECT_VEC_NEAR(q * Vector3::UNIT_Z, Vector3::UNIT_X, quatTolerance);
    EXPECT_VEC_NEAR(q * Vector3::UNIT_Y, Vector3::UNIT_Y, quatTolerance);
}

TEST(Quaternion, LookRotationOfZeroIsIdentity){
    EXPECT_EQ(Quaternion::lookRotation(Vector3::ZERO, Vector3::UNIT_Y), Quaternion::IDENTITY);
}

TEST(Quaternion, LookRotationWithForwardAlongUpStillLooksForward){
    const Quaternion q = Quaternion::lookRotation(Vector3::UNIT_Y, Vector3::UNIT_Y);
    EXPECT_VEC_NEAR(q * Vector3::UNIT_Z, Vector3::UNIT_Y, quatTolerance);
}
