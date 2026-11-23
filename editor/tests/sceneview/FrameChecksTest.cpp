// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S99 R7, R8, AC2: the frame checks' parts. The z-fighting plane buckets (coplanar, same-facing
// faces overlapping by more than 1 mm, wherever and however tilted the plane is, and nothing
// else), the luminance thresholds, the projected bounds, and the bucket check's cost on two dense
// meshes against its budget (perfSlack()).

#include "sceneview/FrameChecks.h"
#include "math/Quaternion.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace FrameChecksTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::framechecks;

namespace {

    // a quad of two triangles, counter-clockwise seen from +Y, size x size at `center` (y up),
    // then rotated by `rotation` about `pivot`
    std::vector<Triangle> quad(const Vector3& center, float size, const Quaternion& rotation = Quaternion(),
                               const Vector3& pivot = Vector3::ZERO){
        const float h = size * 0.5f;
        Vector3 c[4] = {center + Vector3(-h, 0.0f, -h), center + Vector3(h, 0.0f, -h),
                        center + Vector3(-h, 0.0f, h), center + Vector3(h, 0.0f, h)};
        for (Vector3& p : c) p = pivot + rotation * (p - pivot);
        return {{c[0], c[2], c[1]}, {c[1], c[2], c[3]}};
    }

    std::vector<Triangle> flipped(std::vector<Triangle> faces){
        for (Triangle& t : faces) std::swap(t.b, t.c);
        return faces;
    }

    // whether any face of b z-fights with one of a, through the buckets as the check runs them
    bool zFights(const std::vector<Triangle>& a, const std::vector<Triangle>& b, const Vector3& origin, float radius){
        PlaneBuckets buckets(COPLANAR_TOLERANCE, origin, radius);
        buckets.build(a);
        for (const Triangle& t : b){
            if (buckets.findOverlap(t) >= 0) return true;
        }
        return false;
    }

    // a grid of cells x cells quads on y = height, `size` wide
    std::vector<Triangle> frameChecksGrid(int cells, float size, float height){
        std::vector<Triangle> faces;
        const float step = size / (float)cells;
        for (int z = 0; z < cells; z++){
            for (int x = 0; x < cells; x++){
                const std::vector<Triangle> q = quad(Vector3(-size * 0.5f + (x + 0.5f) * step, height, -size * 0.5f + (z + 0.5f) * step), step);
                faces.insert(faces.end(), q.begin(), q.end());
            }
        }
        return faces;
    }

}

TEST(FrameChecksZFighting, CoplanarOverlappingQuadsFight){
    const std::vector<Triangle> floor = quad(Vector3::ZERO, 4.0f);
    // a decal on the floor, and the same 0.5 mm above it (within the 1 mm)
    EXPECT_TRUE(zFights(floor, quad(Vector3(0.5f, 0.0f, 0.3f), 1.0f), Vector3::ZERO, 2.0f));
    EXPECT_TRUE(zFights(floor, quad(Vector3(0.5f, 0.0005f, 0.3f), 1.0f), Vector3::ZERO, 2.0f));
    // a duplicate of the floor itself
    EXPECT_TRUE(zFights(floor, floor, Vector3::ZERO, 2.0f));
}

TEST(FrameChecksZFighting, SeparatedOppositeOrTouchingFacesDoNotFight){
    const std::vector<Triangle> floor = quad(Vector3::ZERO, 4.0f);
    // 2 mm above: past the tolerance
    EXPECT_FALSE(zFights(floor, quad(Vector3(0.5f, 0.002f, 0.3f), 1.0f), Vector3::ZERO, 2.0f));
    // facing down: a back-to-back panel, not a fight
    EXPECT_FALSE(zFights(floor, flipped(quad(Vector3(0.5f, 0.0f, 0.3f), 1.0f)), Vector3::ZERO, 2.0f));
    // tiles sharing an edge, and tiles overlapping by half a millimetre
    EXPECT_FALSE(zFights(quad(Vector3::ZERO, 1.0f), quad(Vector3(1.0f, 0.0f, 0.0f), 1.0f), Vector3(0.5f, 0.0f, 0.0f), 1.0f));
    EXPECT_FALSE(zFights(quad(Vector3::ZERO, 1.0f), quad(Vector3(0.9995f, 0.0f, 0.0f), 1.0f), Vector3(0.5f, 0.0f, 0.0f), 1.0f));
    // a wall standing on the floor
    const Quaternion upright(90.0f, Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_FALSE(zFights(floor, quad(Vector3(0.0f, 0.5f, 0.0f), 1.0f, upright, Vector3(0.0f, 0.5f, 0.0f)), Vector3::ZERO, 2.0f));
    // tilted 1 degree about its centre: its corners leave the plane by more than 1 mm
    const Quaternion tilt(1.0f, Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_FALSE(zFights(floor, quad(Vector3(0.5f, 0.0f, 0.3f), 1.0f, tilt, Vector3(0.5f, 0.0f, 0.3f)), Vector3::ZERO, 2.0f));
}

TEST(FrameChecksZFighting, TiltedPlanesFarFromTheOriginFight){
    // a ramp at 37 degrees (and one at 45, where two axes tie), 800 m out: the buckets are relative to
    // the region, not the world origin
    for (float degrees : {37.0f, 45.0f}){
        const Vector3 at(500.0f, 20.0f, -800.0f);
        const Quaternion slope(degrees, Vector3(1.0f, 0.0f, 0.0f));
        const std::vector<Triangle> ramp = quad(at, 6.0f, slope, at);
        EXPECT_TRUE(zFights(ramp, quad(at + Vector3(1.0f, 0.0f, 0.5f), 0.5f, slope, at), at, 4.0f)) << degrees;
        EXPECT_FALSE(zFights(ramp, quad(at + Vector3(1.0f, 0.004f, 0.5f), 0.5f, slope, at + Vector3(0.0f, 0.004f, 0.0f)), at, 4.0f)) << degrees;
    }
}

TEST(FrameChecksZFighting, SmallFacesOnALargeFaceFight){
    // one big face and a dense grid on it (one face spanning every cell of the grid's buckets)
    const std::vector<Triangle> big = quad(Vector3::ZERO, 40.0f);
    const std::vector<Triangle> dense = frameChecksGrid(32, 8.0f, 0.0f);
    EXPECT_TRUE(zFights(big, dense, Vector3::ZERO, 6.0f));
    EXPECT_TRUE(zFights(dense, big, Vector3::ZERO, 6.0f));
    EXPECT_FALSE(zFights(dense, frameChecksGrid(32, 8.0f, 0.01f), Vector3::ZERO, 6.0f));
    // built with the dense faces, the big one spans too many cells: kept apart, still found
    std::vector<Triangle> mixed = frameChecksGrid(32, 8.0f, 0.01f);
    mixed.insert(mixed.end(), big.begin(), big.end());
    EXPECT_TRUE(zFights(mixed, quad(Vector3(15.0f, 0.0f, 15.0f), 1.0f), Vector3::ZERO, 6.0f));
}

TEST(FrameChecksZFighting, DenseMeshesStayInBudget){
    // the check's worst case: two dense parallel meshes 1 cm apart (every face has neighbours in
    // its buckets and none fights), 8192 faces each
    const std::vector<Triangle> a = frameChecksGrid(64, 64.0f, 0.0f);
    const std::vector<Triangle> b = frameChecksGrid(64, 64.0f, 0.01f);
    double best = 1e9;
    for (int run = 0; run < 5; run++){
        const auto start = std::chrono::steady_clock::now();
        EXPECT_FALSE(zFights(a, b, Vector3::ZERO, 46.0f));
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("[ FrameChecksBench ] plane buckets, 2 x 8192 faces 1 cm apart: %.3f ms (budget %.0f ms)\n", best, TARGET_MS);
    const double slack = jasperengine::editor::test::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(best, TARGET_MS * slack);
    }
}

TEST(FrameChecksLuminance, Thresholds){
    auto image = [](int bright, int dark, int mid){
        std::vector<uint8_t> pixels;
        for (int i = 0; i < bright; i++) pixels.insert(pixels.end(), {255, 255, 255});
        for (int i = 0; i < dark; i++) pixels.insert(pixels.end(), {2, 2, 2});
        for (int i = 0; i < mid; i++) pixels.insert(pixels.end(), {120, 120, 120});
        return pixels;
    };
    // 20 % at or above 0.98 is clipped; 19 % is not
    std::vector<uint8_t> clipped = image(20, 0, 80);
    Luminance l = measureLuminance(clipped.data(), 100, 1, 3);
    ASSERT_TRUE(l.valid);
    EXPECT_FLOAT_EQ(l.clippedShare, 0.20f);
    EXPECT_GE(l.clippedShare, CLIPPED_SHARE);
    std::vector<uint8_t> almost = image(19, 0, 81);
    EXPECT_LT(measureLuminance(almost.data(), 100, 1, 3).clippedShare, CLIPPED_SHARE);
    // 0.98 of 255 is 249.9: 250 counts, 249 does not
    const uint8_t edge[6] = {250, 250, 250, 249, 249, 249};
    EXPECT_FLOAT_EQ(measureLuminance(edge, 2, 1, 3).clippedShare, 0.5f);
    // dark: 90 % under 0.02 and a mean under 0.04
    std::vector<uint8_t> dark = image(0, 95, 5);
    l = measureLuminance(dark.data(), 100, 1, 3);
    EXPECT_GE(l.darkShare, DARK_SHARE);
    EXPECT_LT(l.mean, DARK_MEAN);
    std::vector<uint8_t> grey = image(0, 50, 50);
    l = measureLuminance(grey.data(), 100, 1, 3);
    EXPECT_LT(l.darkShare, DARK_SHARE);
    // a rectangle of the image (the game canvas without its bars): only it counts
    std::vector<uint8_t> barred;
    for (int y = 0; y < 4; y++){
        for (int x = 0; x < 10; x++){
            const uint8_t v = (x < 2 || x >= 8) ? 0 : 255;
            barred.insert(barred.end(), {v, v, v, 255});
        }
    }
    EXPECT_FLOAT_EQ(measureLuminance(barred.data(), 10, 4, 4, 2, 0, 6, 4).clippedShare, 1.0f);
    EXPECT_FLOAT_EQ(measureLuminance(barred.data(), 10, 4, 4).clippedShare, 0.6f);
}

TEST(FrameChecksProjection, OutsideShareAndCoverage){
    // a 90 degree, square view from z = 10 looking down -z
    const Matrix4 view = Matrix4::lookAtMatrix(Vector3(0.0f, 0.0f, 10.0f), Vector3::ZERO, Vector3(0.0f, 1.0f, 0.0f));
    const Matrix4 projection = Matrix4::perspectiveMatrix(1.57079633f, 1.0f, 0.1f, 100.0f);
    const Matrix4 vp = projection * view;
    // the view at z = 0 is 20 x 20: a 2 x 2 box in the middle covers a hundredth of it
    Projection centre = projectBounds(vp, AABB(Vector3(-1.0f, -1.0f, 0.0f), Vector3(1.0f, 1.0f, 0.0f)));
    EXPECT_TRUE(centre.inFront);
    EXPECT_NEAR(centre.outsideShare, 0.0f, 1e-4f);
    EXPECT_NEAR(centre.coverage, 0.01f, 1e-4f);
    // straddling the right edge by three quarters
    Projection edge = projectBounds(vp, AABB(Vector3(9.0f, -1.0f, 0.0f), Vector3(13.0f, 1.0f, 0.0f)));
    EXPECT_NEAR(edge.outsideShare, 0.75f, 1e-3f);
    // behind the eye: wholly out
    Projection behind = projectBounds(vp, AABB(Vector3(-1.0f, -1.0f, 20.0f), Vector3(1.0f, 1.0f, 22.0f)));
    EXPECT_FALSE(behind.inFront);
    EXPECT_FLOAT_EQ(behind.outsideShare, 1.0f);
    EXPECT_FLOAT_EQ(behind.coverage, 0.0f);
    // around the eye: mostly out, never counted as fitting the frame
    Projection around = projectBounds(vp, AABB(Vector3(-50.0f, -1.0f, -50.0f), Vector3(50.0f, 1.0f, 50.0f)));
    EXPECT_FALSE(around.inFront);
    EXPECT_GT(around.outsideShare, OUT_OF_FRAME_SHARE);
}

}  // namespace FrameChecksTest_cpp
