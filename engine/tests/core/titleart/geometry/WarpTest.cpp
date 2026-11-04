// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R24 (AC22): the fifteen Patchy warp styles against Pachislop's maths (an independent
// Python transcription of src/text/warp.ts writes fixtures/geometry/warp-reference.json:
// tools/titleart/gen_warp_refs.py), the re-centred frame, and the curve refit's accuracy.

#include "gtest/gtest.h"

#include "TestPaths.h"

#include "titleart/geometry/Distort.h"
#include "titleart/geometry/PathUtil.h"
#include "titleart/geometry/Warp.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    Json loadReference() {
        const std::filesystem::path path =
            std::filesystem::path(jasperengine::test::testPaths().sourceDir) / "titleart" / "fixtures" / "geometry" / "warp-reference.json";
        std::ifstream in(path);
        return Json::parse(in, nullptr, false);
    }

    // a circle of radius r at (cx, cy) as four cubics (the shape refit has to keep smooth)
    Path circle(double cx, double cy, double r) {
        const double k = 0.5522847498 * r;
        Path p;
        p.moveTo(cx + r, cy);
        p.cubicTo(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
        p.cubicTo(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
        p.cubicTo(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
        p.cubicTo(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
        p.close();
        return p;
    }

}

TEST(WarpTest, StylesMatchPachislop) {
    const Json ref = loadReference();
    ASSERT_TRUE(ref.is_object()) << "fixtures/geometry/warp-reference.json";
    const TitleRect box{ref["box"][0].get<double>(), ref["box"][1].get<double>(), ref["box"][2].get<double>() - ref["box"][0].get<double>(),
                        ref["box"][3].get<double>() - ref["box"][1].get<double>()};
    const Json& points = ref["points"];
    std::set<std::string> styles;
    for (const Json& c : ref["cases"]) {
        WarpParams warp;
        ASSERT_TRUE(warpStyleFromName(c["style"].get<std::string>(), warp.style)) << c["style"];
        styles.insert(c["style"].get<std::string>());
        warp.bend = c["bend"].get<double>();
        warp.horizontal = c["horizontal"].get<double>();
        warp.vertical = c["vertical"].get<double>();
        SCOPED_TRACE(c["style"].get<std::string>() + " bend " + std::to_string(warp.bend) + " distortion " + std::to_string(warp.horizontal));
        const std::optional<Warper> warper = makeWarper(box, warp);
        ASSERT_TRUE(warper.has_value());
        const TitleRect& f = warper->frame;
        EXPECT_NEAR(f.x, c["frame"][0].get<double>(), 1e-6);
        EXPECT_NEAR(f.y, c["frame"][1].get<double>(), 1e-6);
        EXPECT_NEAR(f.x + f.width, c["frame"][2].get<double>(), 1e-6);
        EXPECT_NEAR(f.y + f.height, c["frame"][3].get<double>(), 1e-6);
        // the frame is re-centred on the box centre: no drift as the bend changes
        EXPECT_NEAR(f.x + f.width / 2, box.x + box.width / 2, 1e-6);
        EXPECT_NEAR(f.y + f.height / 2, box.y + box.height / 2, 1e-6);
        for (size_t i = 0; i < points.size(); i++) {
            const TitleVec2 q = warper->map.map({points[i][0].get<double>(), points[i][1].get<double>()});
            EXPECT_NEAR(q.x, c["mapped"][i][0].get<double>(), 1e-6) << "point " << i;
            EXPECT_NEAR(q.y, c["mapped"][i][1].get<double>(), 1e-6) << "point " << i;
        }
    }
    EXPECT_EQ(styles.size(), 15u) << "every style of Patchy's fifteen";
}

TEST(WarpTest, IdentityWarpsLeaveOutlinesAlone) {
    const TitleRect box{-100, -50, 200, 100};
    const Path c = circle(0, 0, 40);
    for (const WarpParams& w : {WarpParams{}, WarpParams{WarpStyle::Arc, 0, 0, 0}, WarpParams{WarpStyle::None, 50, 0, 0}}) {
        EXPECT_TRUE(warpIsIdentity(w));
        EXPECT_FALSE(makeWarper(box, w).has_value());
        EXPECT_EQ(warpPath(c, box, w).pointList().size(), c.pointList().size());
    }
    // distortion alone is a warp
    EXPECT_FALSE(warpIsIdentity(WarpParams{WarpStyle::None, 0, 20, 0}));
    EXPECT_FALSE(makeWarper(TitleRect{0, 0, 0, 10}, WarpParams{WarpStyle::Arc, 50, 0, 0}).has_value()) << "an empty box";
}

TEST(WarpTest, ArcRowsFollowCircles) {
    // arch at +100: the top edge is the cubic approximation of a half circle over the chord
    const WarpMesh m = warpMesh(WarpParams{WarpStyle::Arch, 100, 0, 0}, 200, 50);
    ASSERT_EQ(m.nu, 4);
    ASSERT_EQ(m.nv, 2);
    // the circle-arc cubic: handle = 4/3 tan(theta/2) r with theta = 90 deg, r = 100
    EXPECT_NEAR(m.xs[1] - m.xs[0], 0.0, 1e-9);
    EXPECT_NEAR(m.ys[0] - m.ys[1], 4.0 / 3.0 * 100, 1e-9);
}

TEST(WarpTest, RefitKeepsCurvesWithinTolerance) {
    // the warped outline's points stay within the refit tolerance of the exact map of the input
    const TitleRect box{-120, -40, 240, 80};
    const Path source = circle(30, 0, 35);
    for (WarpStyle style : {WarpStyle::Arc, WarpStyle::Flag, WarpStyle::Fisheye, WarpStyle::Twist, WarpStyle::Squeeze}) {
        SCOPED_TRACE(warpStyleName(style));
        const WarpParams warp{style, 70, 10, -20};
        const std::optional<Warper> warper = makeWarper(box, warp);
        ASSERT_TRUE(warper.has_value());
        const Path warped = remapPath(source, warper->map);
        for (PathVerb v : warped.verbList()) EXPECT_NE(v, PathVerb::Cubic) << "the refit emits quadratics and lines";
        // every exact image point is close to the fitted outline (dense polyline of it)
        const std::vector<std::vector<TitleVec2>> fitted = warped.flatten(1e-4);
        ASSERT_EQ(fitted.size(), 1u);
        const std::vector<std::vector<TitleVec2>> input = source.flatten(1e-3);
        double worst = 0;
        for (const TitleVec2& p : input[0]) {
            const TitleVec2 q = warper->map.map(p);
            double best = 1e9;
            for (size_t i = 0; i + 1 < fitted[0].size(); i++) {
                const TitleVec2 a = fitted[0][i], b = fitted[0][i + 1];
                const double dx = b.x - a.x, dy = b.y - a.y;
                const double l2 = dx * dx + dy * dy;
                const double t = l2 > 0 ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / l2, 0.0, 1.0) : 0;
                best = std::min(best, std::hypot(a.x + t * dx - q.x, a.y + t * dy - q.y));
            }
            worst = std::max(worst, best);
        }
        // AC22's point tolerance is 1e-3 em; the refit's own is max(1.5e-4 x size, 1e-3)
        EXPECT_LT(worst, 0.06) << "about 2.5 x the 0.036 px refit tolerance of a 240 px box";
    }
}

TEST(DistortTest, CornersKeepLinesStraightAndHitTheCorners) {
    const TitleRect frame{-100, -50, 200, 100};
    DistortParams d;
    d.mode = DistortMode::Corners;
    d.corners = {TitleVec2{0.1, 0.0}, TitleVec2{-0.2, 0.3}, TitleVec2{0.0, -0.1}, TitleVec2{0.05, 0.0}};
    const std::optional<PointMap> pm = makeDistorter(frame, d);
    ASSERT_TRUE(pm.has_value());
    EXPECT_TRUE(pm->straight);
    // TL, TR, BR, BL move by their fractions of the frame (+y up)
    const auto expectAt = [&](TitleVec2 from, TitleVec2 to) {
        const TitleVec2 q = pm->map(from);
        EXPECT_NEAR(q.x, to.x, 1e-9);
        EXPECT_NEAR(q.y, to.y, 1e-9);
    };
    expectAt({-100, 50}, {-80, 50});
    expectAt({100, 50}, {60, 80});
    expectAt({100, -50}, {100, -60});
    expectAt({-100, -50}, {-90, -50});
    // a straight line stays straight: its midpoint maps onto the chord of its mapped ends
    const TitleVec2 a = pm->map({-60, 20}), b = pm->map({70, -30}), m = pm->map({5, -5});
    const double cross = (b.x - a.x) * (m.y - a.y) - (b.y - a.y) * (m.x - a.x);
    EXPECT_NEAR(cross, 0.0, 1e-6);
    // the identity
    EXPECT_FALSE(makeDistorter(frame, DistortParams{}).has_value());
    DistortParams zero;
    zero.mode = DistortMode::Mesh;
    EXPECT_TRUE(distortIsIdentity(zero));
}

TEST(DistortTest, MeshPatchAndItsLinearExtension) {
    const TitleRect frame{0, 0, 300, 300};
    DistortParams d;
    d.mode = DistortMode::Mesh;
    d.mesh[5] = {0.1, 0.0};     // an interior control point pulled right
    const std::optional<PointMap> pm = makeDistorter(frame, d);
    ASSERT_TRUE(pm.has_value());
    // corners stay, the interior bends toward the moved point
    const TitleVec2 corner = pm->map({0, 300});
    EXPECT_NEAR(corner.x, 0, 1e-9);
    EXPECT_NEAR(corner.y, 300, 1e-9);
    EXPECT_GT(pm->map({100, 200}).x, 100);
    // outside the frame the map continues linearly along the edge's derivative (C1): equal
    // steps outside give equal displacements
    const TitleVec2 p1 = pm->map({-10, 150}), p2 = pm->map({-20, 150}), p3 = pm->map({-30, 150});
    EXPECT_NEAR(p2.x - p1.x, p3.x - p2.x, 1e-9);
    EXPECT_NEAR(p2.y - p1.y, p3.y - p2.y, 1e-9);
}
