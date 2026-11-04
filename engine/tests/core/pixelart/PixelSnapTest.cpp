// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC2 (R7, R8, R10, D4): the CPU twin of the pixel-snap rule (pixelart/PixelSnap.h,
// PixelRotation.h). The GPU half of the contract is PixelPerfectGpu.SnapParity.

#include "pixelart/PixelSnap.h"


#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    SnapGrid snapTestGrid(float width, float height, float flipY = 1.0f){
        SnapGrid g;
        g.width = width;
        g.height = height;
        g.flipY = flipY;
        return g;
    }

    // a 2D model: rotation (degrees) about z, then scale; translation t
    Matrix4 snapTestModel(float degrees, float scaleX, float scaleY, float tx = 0.0f, float ty = 0.0f){
        const double radians = (double)degrees * 3.14159265358979323846 / 180.0;
        Matrix4 rotation;
        rotation.set(0, 0, (float)std::cos(radians));
        rotation.set(0, 1, (float)std::sin(radians));
        rotation.set(1, 0, (float)-std::sin(radians));
        rotation.set(1, 1, (float)std::cos(radians));
        return Matrix4::translateMatrix(Vector3(tx, ty, 0.0f)) * rotation * Matrix4::scaleMatrix(Vector3(scaleX, scaleY, 1.0f));
    }

    // the base-pixel lengths of the model's x and y axes
    void snapTestAxisLengths(const Matrix4& m, float& x, float& y){
        x = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1]);
        y = std::sqrt(m[1][0] * m[1][0] + m[1][1] * m[1][1]);
    }

}

// every half-pixel lattice value rounds up, and so does anything within 1e-3 px of it (the
// CPU/GPU rounding differences the bias absorbs)
TEST(PixelSnapTest, HalfPixelLatticeRoundsUp){
    for (int k = -4096; k <= 4096; k++){
        const float half = (float)k + 0.5f;
        ASSERT_EQ(snap(half), (float)(k + 1)) << k;
        ASSERT_EQ(snap(half - 1e-3f), (float)(k + 1)) << k;
        ASSERT_EQ(snap(half + 1e-3f), (float)(k + 1)) << k;
        ASSERT_EQ(snap((float)k), (float)k) << k;
    }
    EXPECT_EQ(kSnapBias, 1.0f / 128.0f);
    // the tie moved to k + 0.5 - 2^-7
    EXPECT_EQ(snap(0.49f), 0.0f);
    EXPECT_EQ(snap(0.493f), 1.0f);
}

TEST(PixelSnapTest, ClipRoundTripsThroughPixels){
    const SnapGrid g = snapTestGrid(320.0f, 180.0f);
    // the centre of the target is pixel (160, 90)
    const Vector2 centre = toPixels(Vector4(0.0f, 0.0f, 0.3f, 1.0f), g);
    EXPECT_FLOAT_EQ(centre.x, 160.0f);
    EXPECT_FLOAT_EQ(centre.y, 90.0f);
    // a clip position half a pixel right of the centre snaps one pixel right, z and w kept
    const float halfPixel = 1.0f / 320.0f;
    const Vector4 snapped = snapClip(Vector4(halfPixel, 0.0f, 0.3f, 1.0f), g);
    EXPECT_FLOAT_EQ(toPixels(snapped, g).x, 161.0f);
    EXPECT_FLOAT_EQ(snapped.z, 0.3f);
    EXPECT_FLOAT_EQ(snapped.w, 1.0f);
    // w scales clip but not pixels
    const Vector4 scaled = snapClip(Vector4(2.0f * halfPixel, 0.0f, 0.6f, 2.0f), g);
    EXPECT_FLOAT_EQ(toPixels(scaled, g).x, 161.0f);
    EXPECT_FLOAT_EQ(scaled.w, 2.0f);
}

// a Y-flipped pass (GL render targets) rounds the logical position: the mirror of the unflipped
// result, with no asymmetry at the ties
TEST(PixelSnapTest, FlippedGridIsTheMirror){
    const SnapGrid up = snapTestGrid(320.0f, 180.0f, 1.0f);
    const SnapGrid flipped = snapTestGrid(320.0f, 180.0f, -1.0f);
    for (int i = -400; i <= 400; i++){
        // the lattice and points between it
        const float y = (float)i / 180.0f + (i % 3 == 0 ? 1.0f / 180.0f : 0.0f);
        const float x = (float)i / 300.0f;
        const Vector4 a = snapClip(Vector4(x, y, 0.0f, 1.0f), up);
        const Vector4 b = snapClip(Vector4(x, -y, 0.0f, 1.0f), flipped);
        ASSERT_EQ(a.x, b.x) << i;
        ASSERT_EQ(a.y, -b.y) << i;
    }
}

TEST(PixelSnapTest, CenterDeltaPutsThePivotOnTheGrid){
    const SnapGrid g = snapTestGrid(256.0f, 224.0f);
    const Vector4 pivot(0.1234f, -0.4321f, 0.5f, 1.0f);
    const Vector4 delta = snapCenterDelta(pivot, g);
    const Vector4 moved(pivot.x + delta.x, pivot.y + delta.y, pivot.z + delta.z, pivot.w + delta.w);
    const Vector2 p = toPixels(moved, g);
    EXPECT_NEAR(p.x, std::round(p.x), 1e-3f);
    EXPECT_NEAR(p.y, std::round(p.y), 1e-3f);
    EXPECT_EQ(delta.z, 0.0f);
    EXPECT_EQ(delta.w, 0.0f);
    // the delta is the snap of the pivot
    const Vector4 snapped = snapClip(pivot, g);
    EXPECT_FLOAT_EQ(moved.x, snapped.x);
    EXPECT_FLOAT_EQ(moved.y, snapped.y);
}

// the smoothing offset of a direct pass: drawables snap before it and move with it
TEST(PixelSnapTest, OffsetShiftsTheGrid){
    SnapGrid g = snapTestGrid(320.0f, 180.0f);
    g.offsetX = 0.25f;
    const Vector4 snapped = snapClip(Vector4(0.0f, 0.0f, 0.0f, 1.0f), g);
    // 160 - 0.25 rounds to 160, then moves back by the offset
    EXPECT_FLOAT_EQ((snapped.x * 0.5f + 0.5f) * 320.0f, 160.25f);
}

TEST(PixelSnapTest, GridAlignment){
    const SnapGrid g = snapTestGrid(320.0f, 180.0f);
    const Matrix4 projection = Matrix4::orthoMatrix(0.0f, 320.0f, 0.0f, 180.0f, -10.0f, 10.0f);
    for (float degrees : {0.0f, 90.0f, 180.0f, 270.0f, -90.0f})
        EXPECT_TRUE(isGridAligned(projection * snapTestModel(degrees, 16.0f, 16.0f, 10.0f, 20.0f), g)) << degrees;
    // flips and scales
    EXPECT_TRUE(isGridAligned(projection * snapTestModel(0.0f, -1.0f, 1.0f), g));
    EXPECT_TRUE(isGridAligned(projection * snapTestModel(90.0f, 2.0f, -3.0f), g));
    // a hair off the grid is off it
    EXPECT_FALSE(isGridAligned(projection * snapTestModel(0.01f, 16.0f, 16.0f), g));
    EXPECT_FALSE(isGridAligned(projection * snapTestModel(37.0f, 1.0f, 1.0f), g));
    EXPECT_FALSE(isGridAligned(projection * snapTestModel(45.0f, 1.0f, 1.0f), g));
    // a perspective draw has no pixel size
    const Matrix4 perspective = Matrix4::perspectiveMatrix(1.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    EXPECT_FALSE(isGridAligned(perspective * snapTestModel(0.0f, 1.0f, 1.0f, 0.0f, 0.0f), g));
}

TEST(PixelSnapTest, RoundRotation90KeepsScaleAndFlips){
    const SnapGrid g = snapTestGrid(320.0f, 180.0f);
    struct Case { float degrees; float sx, sy; float expectDegrees; };
    const Case cases[] = {
        {37.0f, 2.0f, 3.0f, 0.0f}, {44.0f, 1.0f, 1.0f, 0.0f}, {46.0f, 1.0f, 1.0f, 90.0f}, {80.0f, 4.0f, 1.0f, 90.0f},
        {170.0f, 1.0f, 2.0f, 180.0f}, {-100.0f, 3.0f, 3.0f, -90.0f}, {37.0f, -2.0f, 3.0f, 0.0f}, {100.0f, 2.0f, -1.0f, 90.0f},
        // ties at 45 degrees go to the step nearer 0
        {45.0f, 1.0f, 1.0f, 0.0f}, {-45.0f, 1.0f, 1.0f, 0.0f}, {135.0f, 1.0f, 1.0f, 90.0f}, {-135.0f, 1.0f, 1.0f, -90.0f},
    };
    for (const Case& c : cases){
        SCOPED_TRACE(::testing::Message() << c.degrees << " scale " << c.sx << ", " << c.sy);
        const Matrix4 model = snapTestModel(c.degrees, c.sx, c.sy, 7.0f, -3.0f);
        const Matrix4 rounded = roundRotation90(model);
        const Matrix4 expected = snapTestModel(c.expectDegrees, c.sx, c.sy, 7.0f, -3.0f);
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                EXPECT_NEAR(rounded[col][row], expected[col][row], 1e-5f) << col << "," << row;
        float lx, ly, ex, ey;
        snapTestAxisLengths(rounded, lx, ly);
        snapTestAxisLengths(model, ex, ey);
        EXPECT_NEAR(lx, ex, 1e-5f);
        EXPECT_NEAR(ly, ey, 1e-5f);
        EXPECT_EQ(rounded.determinant() < 0.0f, model.determinant() < 0.0f);
        // the result is drawn on the grid
        EXPECT_TRUE(isGridAligned(Matrix4::orthoMatrix(0.0f, 320.0f, 0.0f, 180.0f, -10.0f, 10.0f) * rounded, g));
        EXPECT_FALSE(hasOffGridRotation(rounded));
    }
    // a model already on the grid is left as it is
    const Matrix4 aligned = snapTestModel(90.0f, 2.0f, 2.0f, 1.0f, 1.0f);
    EXPECT_FALSE(hasOffGridRotation(aligned));
    EXPECT_TRUE(hasOffGridRotation(snapTestModel(37.0f, 1.0f, 1.0f)));
}

TEST(PixelSnapTest, RotationStep){
    EXPECT_EQ(snapRotationStep(Vector2(0.9f, 0.1f)), Vector2(1.0f, 0.0f));
    EXPECT_EQ(snapRotationStep(Vector2(0.1f, 0.9f)), Vector2(0.0f, 1.0f));
    EXPECT_EQ(snapRotationStep(Vector2(-0.9f, 0.2f)), Vector2(-1.0f, 0.0f));
    EXPECT_EQ(snapRotationStep(Vector2(0.2f, -0.9f)), Vector2(0.0f, -1.0f));
    EXPECT_EQ(snapRotationStep(Vector2(1.0f, 1.0f)), Vector2(1.0f, 0.0f));
    EXPECT_EQ(snapRotationStep(Vector2(-1.0f, 1.0f)), Vector2(0.0f, 1.0f));
    EXPECT_EQ(snapRotationStep(Vector2(0.0f, 0.0f)), Vector2(0.0f, 0.0f));
}

TEST(PixelSnapTest, SnapWorld){
    EXPECT_EQ(snapWorld(Vector3(10.5f, 4.25f, 3.3f), 1), Vector3(11.0f, 4.0f, 3.3f));
    EXPECT_EQ(snapWorld(Vector3(1.03f, -0.5f, 0.0f), 16), Vector3(1.0f, -0.5f, 0.0f));
    EXPECT_FLOAT_EQ(snapWorld(0.53125f, 16), 0.5625f);
    // a non-positive PPU is the default 1
    EXPECT_FLOAT_EQ(snapWorld(2.6f, 0), 3.0f);
}
