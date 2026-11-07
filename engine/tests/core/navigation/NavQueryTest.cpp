// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navmesh queries (docs/specs/S19-navigation.md R5, R8; AC6, AC9) against hand-computed values on
// small baked fixtures: a 20 x 20 m plane eroded by the 0.5 m agent radius walks [-9.5, 9.5].

#include "NavTestUtil.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    // one cell of the 0.2 m bakes, plus the erosion's rounding up to whole cells
    constexpr float CELL = 0.2f;

    class NavQueryTest : public NavSceneTest {};

    // the points every `step` metres along a path's corners
    std::vector<Vector3> samples(const nav::NavPath& path, float step = 0.05f) {
        std::vector<Vector3> points;
        for (size_t i = 0; i + 1 < path.corners.size(); i++) {
            const Vector3 a = path.corners[i], b = path.corners[i + 1];
            const int n = std::max(1, (int)std::ceil(a.distance(b) / step));
            for (int k = 0; k <= n; k++)
                points.push_back(a.lerp(b, (float)k / (float)n));
        }
        return points;
    }

}

TEST_F(NavQueryTest, AreaCosts){
    // a 30 x 10 plane with a cost-10 strip across it but for a 2 m gap at z in [3, 5]
    nav::NavigationSettings settings = nav::NavigationSettings::defaults();
    settings.areas[3].cost = 10.0f;
    nav::setSettings(settings);
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(0.0f, 0.0f, -1.0f), Vector3(2.0f, 4.0f, 8.0f), 3);
    // a Not Walkable block in a corner leaves no polygon there
    areaVolume(Vector3(-12.0f, 0.0f, -3.0f), Vector3(4.0f, 4.0f, 3.0f), nav::AREA_NOT_WALKABLE);
    bake(surface);

    const Vector3 from(-10.0f, 0.0f, 0.0f), to(10.0f, 0.0f, 0.0f);
    const nav::NavPath path = nav->calculatePath(from, to);
    ASSERT_EQ(path.status, nav::PathStatus::Complete);
    // S54 R2: Complete exactly when nothing failed
    EXPECT_EQ(path.failure, nav::PathFailure::None);
    // the strip costs 10 per metre: the path detours through the gap (z > 3) instead
    float maxZ = -1e9f;
    for (const Vector3& corner : path.corners)
        maxZ = std::max(maxZ, corner.z);
    EXPECT_GT(maxZ, 3.0f - CELL);

    // with the strip's area masked out it never enters it
    nav::QueryFilter filter;
    filter.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << 3));
    const nav::NavPath masked = nav->calculatePath(from, to, filter);
    ASSERT_EQ(masked.status, nav::PathStatus::Complete);
    EXPECT_EQ(masked.failure, nav::PathFailure::None);
    for (const Vector3& point : samples(masked)) {
        nav::NavHit hit;
        ASSERT_TRUE(nav->samplePosition(point, 0.3f, hit, filter));
        EXPECT_NE(hit.area, 3) << point.toString();
    }

    // Not Walkable has no polygon: the nearest point to its middle is outside it
    nav::NavHit blocked;
    ASSERT_TRUE(nav->samplePosition(Vector3(-12.0f, 0.0f, -3.0f), 5.0f, blocked));
    const bool insideBlock = std::fabs(blocked.position.x + 12.0f) < 2.0f - CELL && std::fabs(blocked.position.z + 3.0f) < 1.5f - CELL;
    EXPECT_FALSE(insideBlock) << blocked.position.toString();
}

TEST_F(NavQueryTest, AreaMaskBlocksAcross){
    // a strip of area 4 across the whole plane: masked out, the far side is only Partial
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 12.0f), 4);
    bake(surface);
    nav::QueryFilter filter;
    filter.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << 4));
    const nav::NavPath path = nav->calculatePath(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), filter);
    EXPECT_EQ(path.status, nav::PathStatus::Partial);
    EXPECT_EQ(path.failure, nav::PathFailure::BlockedByArea);
    ASSERT_FALSE(path.corners.empty());
    EXPECT_LT(path.corners.back().x, -1.0f + CELL);
    EXPECT_FALSE(nav->isReachable(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), filter));
    EXPECT_TRUE(nav->isReachable(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f)));
}

TEST_F(NavQueryTest, SamplePosition){
    const Entity surface = planeSurface();
    bake(surface);
    nav::NavHit hit;
    ASSERT_TRUE(nav->samplePosition(Vector3(1.0f, 1.0f, 2.0f), 2.0f, hit));
    EXPECT_NEAR(hit.position.x, 1.0f, CELL);
    EXPECT_NEAR(hit.position.y, 0.0f, CELL);
    EXPECT_NEAR(hit.position.z, 2.0f, CELL);
    EXPECT_NEAR(hit.distance, 1.0f, CELL);
    EXPECT_EQ(hit.area, nav::AREA_WALKABLE);
    EXPECT_NEAR(hit.normal.y, 1.0f, 1e-3f);

    // off the mesh's edge: the edge (9.5) within reach, nothing beyond it
    ASSERT_TRUE(nav->samplePosition(Vector3(12.0f, 0.0f, 0.0f), 3.0f, hit));
    EXPECT_NEAR(hit.position.x, 9.5f, CELL);
    EXPECT_NEAR(hit.distance, 2.5f, CELL);
    EXPECT_FALSE(nav->samplePosition(Vector3(12.0f, 0.0f, 0.0f), 1.0f, hit));
    EXPECT_FALSE(hit.hit);
}

TEST_F(NavQueryTest, Raycast){
    const Entity surface = planeSurface();
    bake(surface);
    nav::NavHit hit;
    // inside the mesh: not blocked
    EXPECT_FALSE(nav->raycast(Vector3(0.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 3.0f), hit));
    EXPECT_NEAR(hit.position.x, 5.0f, CELL);
    EXPECT_NEAR(hit.position.z, 3.0f, CELL);
    // past the edge: blocked at the boundary, the normal facing back
    EXPECT_TRUE(nav->raycast(Vector3(0.0f, 0.0f, 0.0f), Vector3(15.0f, 0.0f, 0.0f), hit));
    EXPECT_NEAR(hit.position.x, 9.5f, CELL);
    EXPECT_NEAR(hit.normal.x, -1.0f, 0.05f);
    EXPECT_NEAR(hit.distance, 9.5f, CELL);
}

TEST_F(NavQueryTest, ClosestEdge){
    const Entity surface = planeSurface();
    bake(surface);
    nav::NavHit hit;
    ASSERT_TRUE(nav->findClosestEdge(Vector3(8.0f, 0.0f, 1.0f), hit));
    EXPECT_NEAR(hit.distance, 1.5f, CELL);
    EXPECT_NEAR(hit.position.x, 9.5f, CELL);
    EXPECT_NEAR(hit.position.z, 1.0f, CELL);
    EXPECT_NEAR(hit.normal.x, -1.0f, 0.05f);
}

TEST_F(NavQueryTest, IsReachable){
    // two islands 4 m apart
    plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 8.0f);
    plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 8.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);
    EXPECT_TRUE(nav->isReachable(Vector3(-8.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 2.0f)));
    EXPECT_FALSE(nav->isReachable(Vector3(-6.0f, 0.0f, 0.0f), Vector3(6.0f, 0.0f, 0.0f)));
    const nav::NavPath path = nav->calculatePath(Vector3(-6.0f, 0.0f, 0.0f), Vector3(6.0f, 0.0f, 0.0f));
    EXPECT_EQ(path.status, nav::PathStatus::Partial);
    EXPECT_EQ(path.failure, nav::PathFailure::Disconnected);
    // a target far off every island has no polygon near it: Invalid
    const nav::NavPath off = nav->calculatePath(Vector3(-6.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 40.0f));
    EXPECT_EQ(off.status, nav::PathStatus::Invalid);
    EXPECT_EQ(off.failure, nav::PathFailure::EndOffMesh);
    const nav::NavPath sameIsland = nav->calculatePath(Vector3(-8.0f, 0.0f, 0.0f), Vector3(-4.0f, 0.0f, 2.0f));
    EXPECT_EQ(sameIsland.status, nav::PathStatus::Complete);
    EXPECT_EQ(sameIsland.failure, nav::PathFailure::None);
}

TEST_F(NavQueryTest, MovedSurfaceMovesItsNavmesh){
    // the navmesh lives in the surface's frame: moving the surface moves it
    const Entity surface = planeSurface();
    bake(surface);
    nav::NavHit hit;
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.5f, hit));
    scene->getComponent<Transform>(surface).position = Vector3(100.0f, 0.0f, 0.0f);
    resolveTransforms(*scene);
    EXPECT_FALSE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.5f, hit));
    ASSERT_TRUE(nav->samplePosition(Vector3(100.0f, 0.0f, 0.0f), 0.5f, hit));
    EXPECT_NEAR(hit.position.x, 100.0f, CELL);
}

// ---- S55 queries (docs/specs/S55-navigation-gameplay-api.md R3, R4)

// AC5: a wall 3 m away: within 2 m none (the distance says how far it looked), within 5 m found
TEST_F(NavQueryTest, ClosestEdgeMaxDistance){
    const Entity surface = planeSurface();
    bake(surface);
    const Vector3 point(6.5f, 0.0f, 0.0f);
    nav::NavHit hit;
    EXPECT_FALSE(nav->findClosestEdge(point, hit, {}, 2.0f));
    EXPECT_FALSE(hit.hit);
    EXPECT_FLOAT_EQ(hit.distance, 2.0f);
    ASSERT_TRUE(nav->findClosestEdge(point, hit, {}, 5.0f));
    EXPECT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 3.0f, CELL);
    EXPECT_NEAR(hit.position.x, 9.5f, CELL);
    // the normal points from the wall towards the point
    EXPECT_NEAR(hit.normal.x, -1.0f, 0.05f);
    EXPECT_EQ(hit.area, nav::AREA_WALKABLE);
    // on one surface the same
    EXPECT_FALSE(nav->findClosestEdgeOn(surface, point, hit, nav::QueryFilter(), 2.0f));
    EXPECT_FLOAT_EQ(hit.distance, 2.0f);
    // no limit: S19's reach
    ASSERT_TRUE(nav->findClosestEdge(point, hit));
    EXPECT_NEAR(hit.distance, 3.0f, CELL);
    // off the mesh: nothing, no distance
    EXPECT_FALSE(nav->findClosestEdge(Vector3(40.0f, 0.0f, 0.0f), hit, {}, 5.0f));
    EXPECT_FLOAT_EQ(hit.distance, 0.0f);
}

// AC6: a move into the mesh's edge stops on it; an open move reaches its end; off the mesh, none
TEST_F(NavQueryTest, MoveAlongSurfaceClampsAtWall){
    const Entity surface = planeSurface();
    box(*scene, Vector3(0.0f, 1.0f, 5.0f), Vector3(2.0f, 2.0f, 2.0f));
    bake(surface);
    nav::NavHit move;
    // through the edge at 9.5, from a point above the floor: the height is the polygon's
    ASSERT_TRUE(nav->moveAlongSurface(Vector3(8.0f, 0.4f, 0.0f), Vector3(12.0f, 0.4f, 0.0f), move));
    EXPECT_TRUE(move.hit);
    EXPECT_NEAR(move.position.x, 9.5f, CELL);
    EXPECT_NEAR(move.position.y, 0.0f, 0.15f);
    EXPECT_NEAR(move.distance, 1.5f, CELL);
    EXPECT_EQ(move.area, nav::AREA_WALKABLE);
    // into the box's footprint (eroded to 1.5 m around its centre): stopped at its side
    ASSERT_TRUE(nav->moveAlongSurface(Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 0.0f, 5.0f), move));
    EXPECT_TRUE(move.hit);
    EXPECT_NEAR(move.position.z, 3.5f, CELL);
    // an open move reaches `to`
    ASSERT_TRUE(nav->moveAlongSurface(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, -1.0f), move));
    EXPECT_FALSE(move.hit);
    EXPECT_NEAR(move.position.x, 1.0f, 1e-3f);
    EXPECT_NEAR(move.position.z, -1.0f, 1e-3f);
    EXPECT_NEAR(move.distance, std::sqrt(2.0f), 1e-3f);
    // from off the mesh: nothing moves
    EXPECT_FALSE(nav->moveAlongSurface(Vector3(30.0f, 0.0f, 0.0f), Vector3(31.0f, 0.0f, 0.0f), move));
    EXPECT_FALSE(move.hit);
    EXPECT_FLOAT_EQ(move.distance, 0.0f);
}
