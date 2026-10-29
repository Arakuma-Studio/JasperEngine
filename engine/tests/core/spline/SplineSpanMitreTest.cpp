// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Tests/SpanMitreTests.cpp, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// Span mitres at bare fence joins: the join measure, the quantised shape and the cut plane it
// rebuilds. OpenEngine cut a box mesh with each plane and read the cut ends back; the mesh cut
// is out of scope here (TRS instances), so a cut end is computed exactly instead: where each of
// the piece's four long edges meets its cut plane, or the piece's own end where the plane lies
// beyond it (the vertex a cut box would keep). Scenarios are OpenEngine's mirrored into our
// right-handed frame (world z negated; the pieces here are symmetric across travel).

#include "SplineSpanMitre.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr float MITRE_TEST_PI = 3.14159265358979f;
    // how far a cut end may land off its plane: half a millimetre, the bar set for the joins
    constexpr float MITRE_ON_PLANE = 5.0e-4f;

    SplinePiece mitrePiece(const Vector3& center, const Vector3& halfExtents){
        SplinePiece piece;
        piece.center = center;
        piece.halfExtents = halfExtents;
        return piece;
    }

    // the castle kit's 5 m wall: 5 x 5 x 0.5 m, laid along local X, corner-pivoted
    SplinePiece mitreCastleWall(){ return mitrePiece(Vector3(-2.5f, 2.5f, 0.0f), Vector3(2.5f, 2.5f, 0.25f)); }

    // a 3 m panel laid along local Z, centred, 0.4 m thick
    SplinePiece mitreZPanel(){ return mitrePiece(Vector3(0.0f, 1.0f, 0.0f), Vector3(0.2f, 1.0f, 1.5f)); }

    // the castle kit's 5 m battlement: 1 m tall, laid along local X like its wall
    SplinePiece mitreCastleBattlement(){ return mitrePiece(Vector3(-2.5f, 0.5f, 0.0f), Vector3(2.5f, 0.5f, 0.25f)); }

    int mitreAlongIndex(const SplinePiece& piece){ return piece.axis() == SplinePieceAxis::X ? 0 : 2; }

    // A bare knot corner: two straight legs meeting at the origin, the first heading -Z into
    // it, the second leaving it turned `degrees` to the right.
    std::vector<SplineCenterSample> mitreCorner(float leg, float degrees, float step){
        std::vector<SplineCenterSample> center;
        const uint32_t steps = static_cast<uint32_t>(std::lround(leg / step));
        for (uint32_t i = 0; i <= steps; i++){
            center.push_back({Vector3(0.0f, 0.0f, leg - static_cast<float>(i) * step), Vector3(0, 1, 0), true});
        }
        const float turn = degrees * MITRE_TEST_PI / 180.0f;
        const Vector3 out(std::sin(turn), 0.0f, -std::cos(turn));
        for (uint32_t i = 1; i <= steps; i++){
            center.push_back({out * (static_cast<float>(i) * step), Vector3(0, 1, 0), true});
        }
        return center;
    }

    SplineFenceLayoutResult mitreBareCorner(const SplinePiece& piece, float degrees){
        const std::vector<SplineCenterSample> center = mitreCorner(piece.length(), degrees, 0.05f);
        const float last = static_cast<float>(center.size() - 1u);
        const float knots[3] = {0.0f, last * 0.5f, last};
        const SplinePiece spans[1] = {piece};
        SplineFenceLayoutParams params;
        params.knotStations = knots;
        params.spanPieces = spans;
        return buildFenceLayout(center, params);
    }

    // local to world through the TRS the instancer is given
    Vector3 mitreToWorld(const SplineLayoutPose& frame, float lengthScale, const SplinePiece& piece, const Vector3& local){
        const SplinePiecePose pose = makePiecePose(frame, piece, lengthScale);
        return pose.position + pose.rotation * (pose.scale * local);
    }

    float mitreSignedDistance(const SplineMitrePlane& plane, const Vector3& p){
        return (plane.normal.dotProduct(p) - plane.offset) / plane.normal.length();
    }

    // the world join plane's normal at the corner: perpendicular to the bisector of the legs
    Vector3 mitreBisectorOf(float degrees){
        const float turn = degrees * MITRE_TEST_PI / 180.0f;
        return (Vector3(0, 0, -1) + Vector3(std::sin(turn), 0, -std::cos(turn))).normalized();
    }

    // The cut ends of `piece`'s four long edges (both sides, bottom and top) in the world: on
    // each edge, where it meets the end's cut plane, or the piece's own end where the plane lies
    // beyond it.
    std::vector<Vector3> mitreCutEnds(const SplinePiece& piece, const SplineLayoutPose& frame, float lengthScale,
                                      const SplineFenceEndPlane& start, const SplineFenceEndPlane& end, bool atStart){
        const SplineMitreShape shape = quantizeMitreShape(lengthScale, start, end);
        const SplineMitrePlane plane = mitreEndCutPlane(piece, shape, atStart);
        const unsigned along = static_cast<unsigned>(mitreAlongIndex(piece));
        const unsigned across = 2u - along;
        const Vector3 lo = piece.center - piece.halfExtents;
        const Vector3 hi = piece.center + piece.halfExtents;
        std::vector<Vector3> ends;
        for (const float side : {lo[across], hi[across]}){
            for (const float height : {lo.y, hi.y}){
                Vector3 p;
                p[across] = side;
                p.y = height;
                const float t = (plane.offset - plane.normal[across] * side - plane.normal.y * height) / plane.normal[along];
                p[along] = atStart ? std::max(t, lo[along]) : std::min(t, hi[along]);
                ends.push_back(mitreToWorld(frame, lengthScale, piece, p));
            }
        }
        return ends;
    }

    // how far those cut ends stand off the plane through `station` with `normal`: a cut end lies
    // on its plane over its whole height; an end that stops short of it does not
    float mitreCutEndOffPlane(const SplinePiece& piece, const SplineLayoutPose& frame, float lengthScale,
                              const SplineFenceEndPlane& start, const SplineFenceEndPlane& end, bool atStart,
                              const Vector3& station, const Vector3& normal){
        float worst = 0.0f;
        for (const Vector3& world : mitreCutEnds(piece, frame, lengthScale, start, end, atStart)){
            worst = std::max(worst, std::abs((world - station).dotProduct(normal)));
        }
        return worst;
    }

    // Two 10 m legs climbing at `pitchDegrees` the whole way, meeting at a bare knot that turns
    // them `turnDegrees` to the right.
    std::vector<SplineCenterSample> mitreClimbingTurn(float pitchDegrees, float turnDegrees){
        constexpr float leg = 10.0f;
        constexpr uint32_t steps = 200u;
        const float grade = std::tan(pitchDegrees * MITRE_TEST_PI / 180.0f);
        const float turn = turnDegrees * MITRE_TEST_PI / 180.0f;
        const Vector3 first(0.0f, 0.0f, -1.0f);
        const Vector3 second(std::sin(turn), 0.0f, -std::cos(turn));
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i <= 2u * steps; i++){
            const float walked = leg * static_cast<float>(i) / static_cast<float>(steps);
            const Vector3 plan = walked <= leg ? first * walked : first * leg + second * (walked - leg);
            center.push_back({Vector3(plan.x, grade * walked, plan.z), Vector3(0, 1, 0), true});
        }
        return center;
    }

}

// The plane rebuilt from a quantised end passes through the station, which a mitred span holds
// stationInset inside its modelled end, for both the piece axes a kit models on.
TEST(SplineSpanMitreTests, TheCutPlaneOfAQuantisedEndPassesThroughItsStation){
    for (const SplinePiece& piece : {mitreCastleWall(), mitreZPanel()}){
        const SplineFenceLayoutResult layout = mitreBareCorner(piece, 30.0f);
        ASSERT_EQ(layout.spans.size(), 2u);
        const SplineFenceSpan& before = layout.spans[0];
        const SplineFenceSpan& after = layout.spans[1];
        ASSERT_TRUE(before.end.mitred);
        ASSERT_TRUE(after.start.mitred);

        for (const bool atStart : {false, true}){
            const SplineFenceSpan& span = atStart ? after : before;
            const SplineMitreShape shape = quantizeMitreShape(span.lengthScale, span.start, span.end);
            const SplineMitrePlane plane = mitreEndCutPlane(piece, shape, atStart);
            // the station at the origin in the piece's local frame: on its centreline, `inset`
            // inside its end, at the station's height
            const SplineFenceEndPlane& end = atStart ? span.start : span.end;
            Vector3 station = piece.center;
            station.y = end.stationLocalY;
            station[static_cast<unsigned>(mitreAlongIndex(piece))] += (atStart ? -1.0f : 1.0f) * (0.5f * piece.length() - end.stationInset);
            EXPECT_NEAR(mitreSignedDistance(plane, station), 0.0f, 1e-4f);
            EXPECT_LT(mitreToWorld(span.pose, span.lengthScale, piece, station).length(), 1e-3f)
                << "the local station is not the world station";
        }
    }
}

// End to end at a 90 degree bare corner: every cut end of both spans lies on the one join plane,
// and the two spans' outer faces meet on one edge.
TEST(SplineSpanMitreTests, ACutCornerMeetsOnOnePlaneAndCloses){
    for (const SplinePiece& piece : {mitreCastleWall(), mitreZPanel()}){
        const SplineFenceLayoutResult layout = mitreBareCorner(piece, 90.0f);
        ASSERT_EQ(layout.spans.size(), 2u);
        const Vector3 bisector = mitreBisectorOf(90.0f);
        float outerCornerX[2] = {0.0f, 0.0f};
        for (int s = 0; s < 2; s++){
            const SplineFenceSpan& span = layout.spans[static_cast<size_t>(s)];
            const bool atStart = s == 1;
            for (const Vector3& world : mitreCutEnds(piece, span.pose, span.lengthScale, span.start, span.end, atStart)){
                EXPECT_NEAR(world.dotProduct(bisector), 0.0f, MITRE_ON_PLANE) << "span " << s << " does not end on the plane";
                // the outer (left) corner at the plane: the most negative x
                outerCornerX[s] = std::min(outerCornerX[s], world.x);
            }
        }
        // both outer faces reach the same outer corner: no notch
        EXPECT_NEAR(outerCornerX[0], outerCornerX[1], 1e-3f);
        // the first leg heads -Z into the corner and the second leaves along +X, so the outside
        // is -X and the outer faces meet at x = -halfThickness
        EXPECT_NEAR(outerCornerX[0], -piece.halfThickness(), 1e-3f);
    }
}

// A span with square ends needs no variant; the equal spans of a uniform mitred corner share one.
TEST(SplineSpanMitreTests, AnUnmitredShapeNeedsNoVariantAndEqualShapesShareOne){
    const SplineMitreShape square = quantizeMitreShape(1.1f, SplineFenceEndPlane{}, SplineFenceEndPlane{});
    EXPECT_FALSE(square.isMitred());
    EXPECT_EQ(square.lengthScaleThousandths, 1100);

    const SplineFenceLayoutResult corner = mitreBareCorner(mitreZPanel(), 30.0f);
    const SplineMitreVariantPlan plan = planMitreVariants(corner);
    ASSERT_EQ(plan.spanVariants.size(), corner.spans.size());
    ASSERT_EQ(plan.spanVariants.size(), 2u);
    // the closing and the opening end are different shapes of the same piece
    EXPECT_EQ(plan.variants.size(), 2u);
    EXPECT_NE(plan.spanVariants[0], plan.spanVariants[1]);
    EXPECT_NE(plan.spanVariants[0], SPLINE_NO_MITRE_VARIANT);

    // a straight fence keeps square ends everywhere
    std::vector<SplineCenterSample> line;
    for (uint32_t i = 0; i <= 200u; i++){
        line.push_back({Vector3(0.0f, 0.0f, -0.05f * static_cast<float>(i)), Vector3(0, 1, 0), true});
    }
    const float knots[2] = {0.0f, 200.0f};
    const SplinePiece spans[1] = {mitreZPanel()};
    SplineFenceLayoutParams params;
    params.knotStations = knots;
    params.spanPieces = spans;
    const SplineFenceLayoutResult straight = buildFenceLayout(line, params);
    const SplineMitreVariantPlan none = planMitreVariants(straight);
    EXPECT_TRUE(none.variants.empty());
    for (const uint32_t variant : none.spanVariants){
        EXPECT_EQ(variant, SPLINE_NO_MITRE_VARIANT);
    }
}

// Every cut end reaches its join plane over its full height, on every grade: a racked piece's
// end is square to its pitched chord, so an end laid out to the plane only at its base line
// stops short of it at the top; and a plane rebuilt from a rounded key has to land within a
// millimetre of the true one at the top of the piece, not just at its foot. Walls and the
// registered battlements on them, closing and opening ends, a sweep of grades that does not
// favour round angles, and turns of 1.5, 12, 30 and 90 degrees; the cap is raised so every join
// is mitred.
TEST(SplineSpanMitreTests, EveryCutEndReachesItsPlaneOverItsFullHeight){
    const SplinePiece walls[1] = {mitreCastleWall()};
    const SplinePiece crests[1] = {mitreCastleBattlement()};
    std::vector<float> pitches;
    for (float pitch = -20.0f; pitch <= 20.0f; pitch += 0.73f){
        pitches.push_back(pitch);
    }
    for (const SplineSpanGrade grade : {SplineSpanGrade::Racked, SplineSpanGrade::Stepped}){
        for (const float pitch : pitches){
            for (const float turn : {1.5f, 12.0f, 30.0f, 90.0f}){
                const std::vector<SplineCenterSample> center = mitreClimbingTurn(pitch, turn);
                const float last = static_cast<float>(center.size() - 1u);
                const float knots[3] = {0.0f, last * 0.5f, last};
                SplineFenceLayoutParams params;
                params.knotStations = knots;
                params.spanPieces = walls;
                params.crestPieces = crests;
                params.spanGrade = grade;
                params.spanMaxStretch = 2.0f;
                const SplineFenceLayoutResult result = buildFenceLayout(center, params);
                const std::string what = "grade " + std::to_string(static_cast<int>(grade)) + ", pitch " + std::to_string(pitch) +
                                         ", turn " + std::to_string(turn);
                ASSERT_EQ(result.spans.size(), 4u) << what;
                ASSERT_EQ(result.crests.size(), 4u) << what;
                const SplineFenceSpan& before = result.spans[1];
                const SplineFenceSpan& after = result.spans[2];
                ASSERT_TRUE(before.end.mitred && after.start.mitred) << what;
                const Vector3 station = result.stations[2].pose.base;
                const Vector3 a = Vector3(before.pose.forward.x, 0.0f, before.pose.forward.z).normalized();
                const Vector3 b = Vector3(after.pose.forward.x, 0.0f, after.pose.forward.z).normalized();
                const Vector3 normal = (a + b).normalized();
                EXPECT_LE(mitreCutEndOffPlane(mitreCastleWall(), before.pose, before.lengthScale, before.start, before.end, false,
                                              station, normal), 1e-3f) << what << ", closing wall";
                EXPECT_LE(mitreCutEndOffPlane(mitreCastleWall(), after.pose, after.lengthScale, after.start, after.end, true,
                                              station, normal), 1e-3f) << what << ", opening wall";
                const SplineFenceCrest& closing = result.crests[1];
                const SplineFenceCrest& opening = result.crests[2];
                EXPECT_LE(mitreCutEndOffPlane(mitreCastleBattlement(), closing.pose, closing.lengthScale, closing.start, closing.end,
                                              false, station, normal), 1e-3f) << what << ", closing crest";
                EXPECT_LE(mitreCutEndOffPlane(mitreCastleBattlement(), opening.pose, opening.lengthScale, opening.start, opening.end,
                                              true, station, normal), 1e-3f) << what << ", opening crest";
            }
        }
    }
}

// The reach is a stretch, so the cap governs it: a racked 20 degree climb through a 90 degree
// bare corner needs its closing wall about 35 % longer to cross the plane at its top, past the
// default 1.25x cap, so the corner keeps square ends and says why; at 3 degrees it mitres.
TEST(SplineSpanMitreTests, AReachPastTheStretchCapKeepsSquareEndsAndIsReported){
    const SplinePiece walls[1] = {mitreCastleWall()};
    const auto build = [&](float pitch){
        const std::vector<SplineCenterSample> center = mitreClimbingTurn(pitch, 90.0f);
        const float last = static_cast<float>(center.size() - 1u);
        const float knots[3] = {0.0f, last * 0.5f, last};
        SplineFenceLayoutParams params;
        params.knotStations = knots;
        params.spanPieces = walls;
        return buildFenceLayout(center, params);
    };
    const SplineFenceLayoutResult steep = build(20.0f);
    ASSERT_EQ(steep.spans.size(), 4u);
    EXPECT_FALSE(steep.spans[1].end.mitred);
    EXPECT_FALSE(steep.spans[2].start.mitred);
    EXPECT_LE(steep.spans[1].lengthScale, 1.25f);
    const auto reported = std::count_if(steep.validation.begin(), steep.validation.end(), [](const std::string& m){
        return m.find("past the 1.250x cap") != std::string::npos && m.find("knot 1") != std::string::npos;
    });
    EXPECT_EQ(reported, 1);

    const SplineFenceLayoutResult gentle = build(3.0f);
    EXPECT_TRUE(gentle.spans[1].end.mitred);
    EXPECT_TRUE(gentle.spans[2].start.mitred);
}

// Pieces of unequal thickness meet on the bisector plane, so near straight their reach stays
// within the thicker piece's halfThickness * tan(turn / 2); the plane through the crossings of
// two unequal pairs of side faces would run off along the spans as (h1 - h2) / sin(turn).
TEST(SplineSpanMitreTests, UnequalThicknessesMeetOnTheBisectorNearStraight){
    for (const float degrees : {0.5f, 2.0f, 10.0f, 30.0f}){
        const float turn = degrees * MITRE_TEST_PI / 180.0f;
        SplineSpanJoinSide before;
        before.pose.forward = Vector3(0.0f, 0.0f, -1.0f);
        before.pose.up = Vector3(0.0f, 1.0f, 0.0f);
        before.pose.right = Vector3(1.0f, 0.0f, 0.0f);
        before.halfThickness = 0.25f;
        before.top = 5.0f;
        SplineSpanJoinSide after = before;
        after.pose.forward = Vector3(std::sin(turn), 0.0f, -std::cos(turn));
        after.pose.right = Vector3(std::cos(turn), 0.0f, std::sin(turn));
        after.halfThickness = 0.05f;
        SplineSpanJoinMeasure measure;
        ASSERT_TRUE(measureSpanJoin(before, after, Vector3(0, 0, 0), measure)) << degrees;
        // a right turn measures positive
        EXPECT_NEAR(measure.turnRadians, turn, 1e-5f) << degrees;
        const float bound = 0.25f * std::tan(0.5f * turn) + 1e-5f;
        EXPECT_LE(measure.beforeOverhangMetres, bound) << degrees;
        EXPECT_LE(measure.afterOverhangMetres, bound) << degrees;
    }
}

// The signed ground yaw is positive for a right turn (right = forward x up) and ignores pitch.
TEST(SplineSpanMitreTests, SignedGroundYawIsPositiveToTheRight){
    const Vector3 north(0.0f, 0.0f, -1.0f);
    EXPECT_NEAR(signedGroundYaw(north, Vector3(1.0f, 0.0f, 0.0f)), 0.5f * MITRE_TEST_PI, 1e-6f);
    EXPECT_NEAR(signedGroundYaw(north, Vector3(-1.0f, 0.0f, 0.0f)), -0.5f * MITRE_TEST_PI, 1e-6f);
    EXPECT_NEAR(signedGroundYaw(north, north.crossProduct(Vector3(0.0f, 1.0f, 0.0f))), 0.5f * MITRE_TEST_PI, 1e-6f);
    EXPECT_NEAR(signedGroundYaw(Vector3(0.0f, 3.0f, -1.0f), Vector3(1.0f, -2.0f, -1.0f)), 0.25f * MITRE_TEST_PI, 1e-6f);
    EXPECT_EQ(signedGroundYaw(Vector3(0.0f, 1.0f, 0.0f), north), 0.0f);
    EXPECT_EQ(signedGroundYaw(Vector3(std::nanf(""), 0.0f, 1.0f), north), 0.0f);
}

// A crest wider than its wall carries the wall's end planes, so the reach is laid to the crest's
// own corners too: laid to the wall's corners only, a 0.35 m half-thick coping on a 0.25 m wall
// falls 25.9 mm short of the plane at 30 degrees and 70.7 mm at 90.
TEST(SplineSpanMitreTests, ACrestWiderThanItsWallReachesThePlane){
    SplinePiece wide = mitreCastleBattlement();
    wide.halfExtents.z = 0.35f;
    const SplinePiece walls[1] = {mitreCastleWall()};
    const SplinePiece crests[1] = {wide};
    for (const float turn : {30.0f, 90.0f}){
        const std::vector<SplineCenterSample> center = mitreCorner(mitreCastleWall().length(), turn, 0.05f);
        const float last = static_cast<float>(center.size() - 1u);
        const float knots[3] = {0.0f, last * 0.5f, last};
        SplineFenceLayoutParams params;
        params.knotStations = knots;
        params.spanPieces = walls;
        params.crestPieces = crests;
        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        ASSERT_EQ(result.spans.size(), 2u) << turn;
        ASSERT_EQ(result.crests.size(), 2u) << turn;
        const Vector3 station = result.stations[1].pose.base;
        const Vector3 normal = mitreBisectorOf(turn);
        for (size_t k = 0; k < 2u; k++){
            const bool atStart = k == 1u;
            const SplineFenceCrest& crest = result.crests[k];
            ASSERT_TRUE(atStart ? crest.start.mitred : crest.end.mitred) << turn;
            EXPECT_LE(mitreCutEndOffPlane(wide, crest.pose, crest.lengthScale, crest.start, crest.end, atStart, station, normal), 1e-3f)
                << turn << " crest " << k;
            const SplineFenceSpan& span = result.spans[k];
            EXPECT_LE(mitreCutEndOffPlane(mitreCastleWall(), span.pose, span.lengthScale, span.start, span.end, atStart, station, normal),
                      1e-3f) << turn << " span " << k;
        }
    }
}

// Two 6.5 m legs of 5 m walls: one wall at 1.3x or two at 0.65x, neither within the 1.25x cap or
// its 0.8x floor, so the fill reports and is built at 1.3x. The bare 30 degree corner adds about
// 1 % of reach; the cap is not the join's to enforce again, so the join stays mitred and nothing
// further is reported.
TEST(SplineSpanMitreTests, AFillAlreadyPastTheCapKeepsItsJoinsMitredAndUnreported){
    const SplinePiece walls[1] = {mitreCastleWall()};
    const std::vector<SplineCenterSample> center = mitreCorner(6.5f, 30.0f, 0.05f);
    const float last = static_cast<float>(center.size() - 1u);
    const float knots[3] = {0.0f, last * 0.5f, last};
    SplineFenceLayoutParams params;
    params.knotStations = knots;
    params.spanPieces = walls;
    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 2u);
    EXPECT_GT(result.spans[0].lengthScale, 1.25f);
    EXPECT_TRUE(result.spans[0].end.mitred);
    EXPECT_TRUE(result.spans[1].start.mitred);
    size_t mitring = 0;
    for (const std::string& message : result.validation){
        mitring += message.find("mitring") != std::string::npos ? 1u : 0u;
    }
    EXPECT_EQ(mitring, 0u);
    EXPECT_FALSE(result.validation.empty()) << "the fill past the cap still reports";
}
