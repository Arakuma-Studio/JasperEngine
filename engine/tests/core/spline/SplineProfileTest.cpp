// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineProfileTests.cpp, Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The cross-section presets (S18 R6): their point lists, winding, derived nominal half-width and
// cap triangles, the Flat, Tube and Custom shapes this port adds, and the ground authority rule.

#include "SplineGeometryTestSupport.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    SplineProfileParams profileRectangleParams(float thickness, float height){
        SplineProfileParams p;
        p.shape = SplineProfileShape::Rectangle;
        p.thickness = thickness;
        p.height = height;
        return p;
    }

    SplineProfileParams profileBevelParams(float width, float drop, float inset){
        SplineProfileParams p;
        p.shape = SplineProfileShape::Bevel;
        p.width = width;
        p.edgeDrop = drop;
        p.edgeInset = inset;
        return p;
    }

    // the winding rule the sweep rests on: outward is the edge direction rotated +90 degrees
    Vector2 profileOutwardNormal(const Vector2& a, const Vector2& b){
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        return Vector2(-dy / len, dx / len);
    }

    // every edge of a closed convex profile faces away from its centroid
    void expectProfileFacesOutward(const SplineProfile& profile){
        Vector2 centroid(0.0f, 0.0f);
        for (const SplineProfilePoint& p : profile.points){
            centroid = Vector2(centroid.x + p.position.x / static_cast<float>(profile.points.size()),
                               centroid.y + p.position.y / static_cast<float>(profile.points.size()));
        }
        for (size_t i = 0; i < profile.points.size(); i++){
            const Vector2 a = profile.points[i].position;
            const Vector2 b = profile.points[(i + 1u) % profile.points.size()].position;
            const Vector2 n = profileOutwardNormal(a, b);
            const Vector2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            EXPECT_GT(n.x * (mid.x - centroid.x) + n.y * (mid.y - centroid.y), 0.0f) << "edge " << i << " faces inward";
        }
    }

    // the cap triangles cover the outline's area, each turning like the outline
    float profileCapArea(const SplineProfile& profile){
        float area = 0.0f;
        for (size_t t = 0; t + 2u < profile.capTriangles.size(); t += 3u){
            const Vector2& a = profile.points[profile.capTriangles[t]].position;
            const Vector2& b = profile.points[profile.capTriangles[t + 1u]].position;
            const Vector2& c = profile.points[profile.capTriangles[t + 2u]].position;
            area += 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        }
        return area;
    }

}

TEST(SplineProfileTests, RectangleIsAClosedFourPointLoop){
    const SplineProfile profile = buildProfile(profileRectangleParams(1.0f, 3.0f));

    ASSERT_TRUE(profile.isValid());
    EXPECT_TRUE(profile.closed);
    ASSERT_EQ(profile.points.size(), 4u);
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 0.5f);
    // every corner of a wall is a crease, or the silhouette pillows
    for (const SplineProfilePoint& p : profile.points){
        EXPECT_NE(p.hard, 0u);
    }
    // the base stands on the ground, the top does not
    EXPECT_NE(profile.points[0].grounded, 0u);
    EXPECT_EQ(profile.points[1].grounded, 0u);
    EXPECT_EQ(profile.points[2].grounded, 0u);
    EXPECT_NE(profile.points[3].grounded, 0u);
}

TEST(SplineProfileTests, RectangleWindsSoEveryFacePointsAwayFromTheSolid){
    const SplineProfile profile = buildProfile(profileRectangleParams(2.0f, 4.0f));
    ASSERT_EQ(profile.points.size(), 4u);
    expectProfileFacesOutward(profile);
}

TEST(SplineProfileTests, BevelIsAnOpenStripOrderedLeftToRight){
    const SplineProfile profile = buildProfile(profileBevelParams(4.0f, 0.2f, 0.3f));

    ASSERT_EQ(profile.points.size(), 4u);
    EXPECT_FALSE(profile.closed);
    EXPECT_TRUE(profile.capTriangles.empty());
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 2.0f);
    for (size_t i = 1; i < profile.points.size(); i++){
        EXPECT_GT(profile.points[i].position.x, profile.points[i - 1].position.x);
    }

    // left to right is what makes the walkable span face the sky
    const Vector2 n = profileOutwardNormal(profile.points[1].position, profile.points[2].position);
    EXPECT_NEAR(n.x, 0.0f, 1e-5f);
    EXPECT_NEAR(n.y, 1.0f, 1e-5f);
}

TEST(SplineProfileTests, BevelWithNoDropOrInsetCollapsesToAFlatRibbon){
    // the coincident-point cleanup turns a degenerate bevel into exactly the two-point strip
    const SplineProfile profile = buildProfile(profileBevelParams(6.0f, 0.0f, 0.0f));

    ASSERT_TRUE(profile.isValid());
    ASSERT_EQ(profile.points.size(), 2u);
    EXPECT_FALSE(profile.closed);
    EXPECT_FLOAT_EQ(profile.points[0].position.x, -3.0f);
    EXPECT_FLOAT_EQ(profile.points[1].position.x, 3.0f);
    EXPECT_FLOAT_EQ(profile.points[0].position.y, 0.0f);
    EXPECT_FLOAT_EQ(profile.points[1].position.y, 0.0f);
}

TEST(SplineProfileTests, FlatIsTheTwoPointRibbonTheDegenerateBevelCollapsesTo){
    SplineProfileParams p;
    p.shape = SplineProfileShape::Flat;
    p.width = 6.0f;
    const SplineProfile flat = buildProfile(p);
    const SplineProfile bevel = buildProfile(profileBevelParams(6.0f, 0.0f, 0.0f));

    ASSERT_EQ(flat.points.size(), 2u);
    EXPECT_FALSE(flat.closed);
    EXPECT_FLOAT_EQ(flat.nominalHalfWidth, 3.0f);
    for (size_t i = 0; i < flat.points.size(); i++){
        EXPECT_EQ(flat.points[i].position.x, bevel.points[i].position.x);
        EXPECT_EQ(flat.points[i].position.y, bevel.points[i].position.y);
    }
}

TEST(SplineProfileTests, CrownNominalHalfWidthIncludesItsShoulders){
    SplineProfileParams p;
    p.shape = SplineProfileShape::Crown;
    p.width = 6.0f;
    p.crownRise = 0.15f;
    p.shoulderWidth = 1.0f;
    p.shoulderDrop = 0.3f;

    const SplineProfile profile = buildProfile(p);

    ASSERT_EQ(profile.points.size(), 5u);
    EXPECT_FALSE(profile.closed);
    // the width channel scales the profile's WIDEST extent onto the band the gizmo draws
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 4.0f);
    EXPECT_FLOAT_EQ(profile.points[2].position.y, 0.15f);
}

TEST(SplineProfileTests, BevelInsetIsClampedToHalfTheWidth){
    // past half the width the two inset points would cross; at exactly half they collapse
    const SplineProfile profile = buildProfile(profileBevelParams(2.0f, 0.3f, 5.0f));

    ASSERT_TRUE(profile.isValid());
    EXPECT_EQ(profile.points.size(), 3u);
    EXPECT_FLOAT_EQ(profile.points[1].position.x, 0.0f);
}

TEST(SplineProfileTests, CustomProfileDropsCoincidentPointsAndInheritsTheirCreases){
    const std::vector<SplineProfilePoint> authored = {
        {Vector2(-1.0f, 0.0f), 0u, 0u},
        {Vector2(-1.0f, 0.0f), 1u, 0u}, // duplicate carrying a crease
        {Vector2(0.0f, 1.0f), 0u, 0u},
        {Vector2(1.0f, 0.0f), 0u, 0u},
    };

    const SplineProfile profile = buildCustomProfile(authored, false);

    ASSERT_EQ(profile.points.size(), 3u);
    // a dropped duplicate must not take a crease with it
    EXPECT_NE(profile.points[0].hard, 0u);
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 1.0f);
}

TEST(SplineProfileTests, ACustomProfileKeepsAtMostThirtyTwoPoints){
    std::vector<SplineProfilePoint> authored;
    for (int i = 0; i < 40; i++){
        authored.push_back({Vector2(static_cast<float>(i) * 0.1f, 0.0f), 0u, 0u});
    }
    const SplineProfile profile = buildCustomProfile(authored, false);
    EXPECT_EQ(profile.points.size(), SPLINE_PROFILE_MAX_CUSTOM_POINTS);
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 3.1f);
}

TEST(SplineProfileTests, ADegenerateClosedProfileStopsClaimingToBeClosed){
    // a zero-thickness wall collapses to a line: closing it would emit each edge twice
    const SplineProfile profile = buildProfile(profileRectangleParams(0.0f, 2.0f));

    EXPECT_FALSE(profile.closed);
    EXPECT_LT(profile.points.size(), 3u);
    EXPECT_TRUE(profile.capTriangles.empty());
    // no lateral extent: reporting 1 keeps the width channel from dividing by zero
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 1.0f);
}

TEST(SplineProfileTests, NonFiniteDimensionsDoNotReachThePointList){
    const SplineProfile profile =
        buildProfile(profileRectangleParams(std::nanf(""), std::numeric_limits<float>::infinity()));

    for (const SplineProfilePoint& p : profile.points){
        EXPECT_TRUE(std::isfinite(p.position.x));
        EXPECT_TRUE(std::isfinite(p.position.y));
    }
    EXPECT_TRUE(std::isfinite(profile.nominalHalfWidth));
}

TEST(SplineProfileTests, CustomShapeCarriesNoGeneratedPoints){
    SplineProfileParams p;
    p.shape = SplineProfileShape::Custom;

    EXPECT_FALSE(buildProfile(p).isValid());
}

TEST(SplineProfileTests, ATubeIsAClosedSmoothUnitCircleWoundOutward){
    SplineProfileParams p;
    p.shape = SplineProfileShape::Tube;
    p.tubeSides = 16;
    const SplineProfile profile = buildProfile(p);

    ASSERT_EQ(profile.points.size(), 16u);
    EXPECT_TRUE(profile.closed);
    EXPECT_FLOAT_EQ(profile.nominalHalfWidth, 1.0f);
    for (const SplineProfilePoint& point : profile.points){
        EXPECT_NEAR(std::hypot(point.position.x, point.position.y), 1.0f, 1e-5f);
        EXPECT_EQ(point.hard, 0u) << "a tube shades round";
        EXPECT_EQ(point.grounded, 0u);
    }
    expectProfileFacesOutward(profile);
    // the cap covers the polygon, wound clockwise like the outline
    const float polygonArea = 0.5f * 16.0f * std::sin(2.0f * geometrytest::PI / 16.0f);
    EXPECT_NEAR(profileCapArea(profile), -polygonArea, 1e-4f);
    EXPECT_EQ(profile.capTriangles.size(), 14u * 3u);
}

TEST(SplineProfileTests, TubeSidesAreClamped){
    SplineProfileParams p;
    p.shape = SplineProfileShape::Tube;
    p.tubeSides = 1;
    EXPECT_EQ(buildProfile(p).points.size(), SPLINE_PROFILE_MIN_TUBE_SIDES);
    p.tubeSides = 1000;
    EXPECT_EQ(buildProfile(p).points.size(), SPLINE_PROFILE_MAX_TUBE_SIDES);
}

TEST(SplineProfileTests, AConcaveClosedCustomProfileIsEarClippedNotFanned){
    // an L-shaped wall section, clockwise: a fan from point 0 would cover the notch
    const std::vector<SplineProfilePoint> authored = {
        {Vector2(-1.0f, 0.0f), 1u, 1u}, {Vector2(-1.0f, 2.0f), 1u, 0u}, {Vector2(0.0f, 2.0f), 1u, 0u},
        {Vector2(0.0f, 1.0f), 1u, 0u},  {Vector2(1.0f, 1.0f), 1u, 0u},  {Vector2(1.0f, 0.0f), 1u, 1u},
    };
    const SplineProfile profile = buildCustomProfile(authored, true);
    ASSERT_TRUE(profile.closed);
    ASSERT_EQ(profile.capTriangles.size(), 4u * 3u);
    // the L's area is 3, every triangle wound clockwise like the outline, none outside it
    EXPECT_NEAR(profileCapArea(profile), -3.0f, 1e-5f);
    for (size_t t = 0; t + 2u < profile.capTriangles.size(); t += 3u){
        const Vector2& a = profile.points[profile.capTriangles[t]].position;
        const Vector2& b = profile.points[profile.capTriangles[t + 1u]].position;
        const Vector2& c = profile.points[profile.capTriangles[t + 2u]].position;
        EXPECT_LT((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x), 0.0f) << "triangle " << t / 3u;
        const float cx = (a.x + b.x + c.x) / 3.0f;
        const float cy = (a.y + b.y + c.y) / 3.0f;
        EXPECT_FALSE(cx > 0.0f && cy > 1.0f) << "triangle " << t / 3u << " covers the notch";
    }
}

TEST(SplineProfileTests, ARectangleCapIsTwoTrianglesCoveringIt){
    const SplineProfile profile = buildProfile(profileRectangleParams(2.0f, 4.0f));
    ASSERT_EQ(profile.capTriangles.size(), 6u);
    EXPECT_NEAR(profileCapArea(profile), -8.0f, 1e-5f);
}

TEST(SplineProfileTests, GroundAuthorityFollowsTheCrossSection){
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Crown, false), SplineGroundAuthority::Owns);
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Bevel, false), SplineGroundAuthority::Grades);
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Flat, false), SplineGroundAuthority::Grades);
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Rectangle, false), SplineGroundAuthority::None);
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Tube, false), SplineGroundAuthority::None);
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Custom, false), SplineGroundAuthority::None);
    // water filled to its banks reads the ground and never grades it, whatever its profile
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Flat, true), SplineGroundAuthority::None);
    EXPECT_EQ(resolveGroundAuthority(SplineProfileShape::Crown, true), SplineGroundAuthority::None);
}
