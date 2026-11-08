// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The affine forms of the matrix operations moved objects take (draw-perf spec 6.3): each equals
// the general operation it replaces (composeTRS, affineMultiply, transformAffine and affineInverse
// compute the general one's non-zero terms in its order: equal up to zeros' sign, checked within a
// few float steps for compilers that fuse differently), the 3x3 determinant has the full one's
// sign, and the affine AABB transform holds every transformed corner of the corner loop.

#include "MathTestHelpers.h"

#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

using namespace jasperengine;

namespace {

    // a few float steps of the larger magnitude
    void expectClose(const Matrix4& actual, const Matrix4& expected, const char* what){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                const float a = actual.get(c, r);
                const float e = expected.get(c, r);
                const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(e)));
                EXPECT_NEAR(a, e, 4e-7f * scale) << what << " [" << c << "][" << r << "]";
            }
        }
    }

    struct Random {
        std::mt19937 engine{1234};
        float range(float low, float high){
            return std::uniform_real_distribution<float>(low, high)(engine);
        }
        Vector3 vector(float low, float high){
            return Vector3(range(low, high), range(low, high), range(low, high));
        }
        Quaternion rotation(){
            Quaternion q(range(-180.0f, 180.0f), vector(-1.0f, 1.0f).normalize());
            return q;
        }
        // a scale with mirrored axes now and then
        Vector3 scale(){
            Vector3 s = vector(0.1f, 4.0f);
            if (range(0.0f, 1.0f) < 0.3f)
                s.x = -s.x;
            return s;
        }
    };

}

TEST(AffineMath, ComposeTRSIsTheProduct){
    Random random;
    for (int i = 0; i < 200; i++){
        const Vector3 t = random.vector(-100.0f, 100.0f);
        const Quaternion r = random.rotation();
        const Vector3 s = random.scale();
        const Matrix4 general = Matrix4::translateMatrix(t) * r.getRotationMatrix() * Matrix4::scaleMatrix(s);
        const Matrix4 composed = Matrix4::composeTRS(t, r, s);
        expectClose(composed, general, "composeTRS");
        EXPECT_TRUE(composed.isAffine());
    }
}

TEST(AffineMath, AffineMultiplyIsTheProduct){
    Random random;
    for (int i = 0; i < 200; i++){
        const Matrix4 a = Matrix4::composeTRS(random.vector(-50.0f, 50.0f), random.rotation(), random.scale());
        const Matrix4 b = Matrix4::composeTRS(random.vector(-50.0f, 50.0f), random.rotation(), random.scale());
        expectClose(a.affineMultiply(b), a * b, "affineMultiply");
    }
}

TEST(AffineMath, TransformAffineIsTheProduct){
    Random random;
    for (int i = 0; i < 200; i++){
        const Matrix4 m = Matrix4::composeTRS(random.vector(-50.0f, 50.0f), random.rotation(), random.scale());
        const Vector3 v = random.vector(-20.0f, 20.0f);
        EXPECT_VEC_NEAR(m.transformAffine(v), m * v, 1e-4f);
    }
}

TEST(AffineMath, AffineInverseIsTheInverse){
    Random random;
    for (int i = 0; i < 200; i++){
        const Matrix4 m = Matrix4::composeTRS(random.vector(-50.0f, 50.0f), random.rotation(), random.scale());
        expectClose(m.affineInverse(), m.inverse(), "affineInverse");
    }
    // a zero scale is singular: the general inverse's result, infinities and all
    const Matrix4 flat = Matrix4::composeTRS(Vector3(1, 2, 3), Quaternion(), Vector3(1, 0, 1));
    const Matrix4 general = flat.inverse();
    const Matrix4 affine = flat.affineInverse();
    for (int c = 0; c < 4; c++){
        for (int r = 0; r < 4; r++){
            const float g = general.get(c, r);
            const float a = affine.get(c, r);
            EXPECT_TRUE((std::isnan(g) && std::isnan(a)) || g == a) << "[" << c << "][" << r << "]";
        }
    }
}

TEST(AffineMath, Determinant3x3HasTheSignOfTheDeterminant){
    Random random;
    for (int i = 0; i < 200; i++){
        const Matrix4 m = Matrix4::composeTRS(random.vector(-50.0f, 50.0f), random.rotation(), random.scale());
        EXPECT_EQ(m.determinant3x3() < 0.0f, m.determinant() < 0.0f);
    }
    EXPECT_LT(Matrix4::scaleMatrix(Vector3(-1, 1, 1)).determinant3x3(), 0.0f);
    EXPECT_GT(Matrix4::scaleMatrix(Vector3(-1, -1, 1)).determinant3x3(), 0.0f);
}

TEST(AffineMath, AffineAABBHoldsEveryCorner){
    Random random;
    for (int i = 0; i < 200; i++){
        const Matrix4 m = Matrix4::composeTRS(random.vector(-50.0f, 50.0f), random.rotation(), random.scale());
        const Vector3 low = random.vector(-5.0f, 0.0f);
        const Vector3 high = low + random.vector(0.0f, 5.0f);
        const AABB box = m * AABB(low, high);
        Vector3 expectedMin(INFINITY, INFINITY, INFINITY);
        Vector3 expectedMax(-INFINITY, -INFINITY, -INFINITY);
        for (int corner = 0; corner < 8; corner++){
            const Vector3 p = m * Vector3((corner & 1) ? high.x : low.x, (corner & 2) ? high.y : low.y, (corner & 4) ? high.z : low.z);
            expectedMin.makeFloor(p);
            expectedMax.makeCeil(p);
        }
        EXPECT_VEC_NEAR(box.getMinimum(), expectedMin, 1e-3f);
        EXPECT_VEC_NEAR(box.getMaximum(), expectedMax, 1e-3f);
    }
}

TEST(AffineMath, ProjectiveAABBKeepsTheCornerLoop){
    // a perspective projection divides by w: the corners' box, which the interval method cannot give
    const Matrix4 projection = Matrix4::perspectiveMatrix(1.0f, 1.0f, 0.1f, 100.0f);
    ASSERT_FALSE(projection.isAffine());
    const AABB box = projection * AABB(Vector3(-1, -1, -10), Vector3(1, 1, -5));
    const Vector3 near = projection * Vector3(1, 1, -5);
    EXPECT_NEAR(box.getMaximum().x, near.x, 1e-5f);
    EXPECT_NEAR(box.getMaximum().y, near.y, 1e-5f);
}
