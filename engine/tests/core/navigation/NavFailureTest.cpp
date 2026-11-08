// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Why a path is not Complete (docs/specs/S75-navigation-diagnostics.md R1-R6; AC1-AC5): the
// PathFailure contract, each failure on a small baked fixture with its status, the snapped ends,
// closest point and gap of a Partial path, the surface ranking, and the grid's reasons.

#include "NavTestUtil.h"

#include "component/NavMeshLinkComponent.h"
#include "navigation/GridPathfinder.h"

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::navtest;
using nav::PathFailure;
using nav::PathStatus;

namespace {

    constexpr float FAILURE_CELL = 0.2f;

    class NavFailureTest : public NavSceneTest {
    protected:
        // R2: the status that goes with each failure
        static void expectInvariant(const nav::NavPath& path) {
            EXPECT_EQ(path.failure == PathFailure::None, path.status == PathStatus::Complete) << nav::pathFailureName(path.failure);
            switch (path.failure) {
                case PathFailure::None:
                    EXPECT_EQ(path.status, PathStatus::Complete);
                    break;
                case PathFailure::Disconnected:
                case PathFailure::BlockedByArea:
                case PathFailure::OutOfNodes:
                    EXPECT_EQ(path.status, PathStatus::Partial) << nav::pathFailureName(path.failure);
                    break;
                default:
                    EXPECT_EQ(path.status, PathStatus::Invalid) << nav::pathFailureName(path.failure);
                    break;
            }
        }

        // a link between two world points (on a GameObject at the origin)
        Entity link(const Vector3& start, const Vector3& end, bool bidirectional = true) {
            const Entity entity = transformEntity(*scene, Vector3(), NULL_ENTITY, "Link");
            NavMeshLinkComponent component;
            component.startPoint = start;
            component.endPoint = end;
            component.bidirectional = bidirectional;
            scene->addComponent<NavMeshLinkComponent>(entity, component);
            return entity;
        }

        // two 8 x 8 m floors at x = -6 and 6 (a 4 m gap), one surface
        Entity twoFloors() {
            plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 8.0f);
            plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 8.0f);
            const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
            addSurface(*scene, surface, 0.2f, 32);
            return surface;
        }
    };

}

TEST(NavFailureNames, NamesAreTheContract){
    // the values are the contract shared with S76: appended only
    static_assert((int)PathFailure::None == 0 && (int)PathFailure::NoSurface == 1 && (int)PathFailure::NotBaked == 2 &&
                  (int)PathFailure::StartOffMesh == 3 && (int)PathFailure::EndOffMesh == 4 && (int)PathFailure::StartFiltered == 5 &&
                  (int)PathFailure::EndFiltered == 6 && (int)PathFailure::Disconnected == 7 && (int)PathFailure::BlockedByArea == 8 &&
                  (int)PathFailure::OutOfNodes == 9, "PathFailure's values");
    static_assert(sizeof(PathFailure) == 1, "PathFailure is a uint8_t");
    const char* names[] = {"none", "no_surface", "not_baked", "start_off_mesh", "end_off_mesh", "start_filtered", "end_filtered",
                           "disconnected", "blocked_by_area", "out_of_nodes"};
    for (int i = 0; i < 10; i++) {
        const PathFailure failure = (PathFailure)i;
        EXPECT_STREQ(nav::pathFailureName(failure), names[i]);
        PathFailure parsed = PathFailure::None;
        ASSERT_TRUE(nav::pathFailureFromName(names[i], parsed)) << names[i];
        EXPECT_EQ(parsed, failure);
        const std::string description = nav::pathFailureDescription(failure);
        EXPECT_FALSE(description.empty());
        EXPECT_EQ(description.back(), '.') << description;
    }
    PathFailure unchanged = PathFailure::OutOfNodes;
    EXPECT_FALSE(nav::pathFailureFromName("nowhere", unchanged));
    EXPECT_EQ(unchanged, PathFailure::OutOfNodes);
    // a default path is Invalid with no surface
    const nav::NavPath path;
    EXPECT_EQ(path.failure, PathFailure::NoSurface);
    EXPECT_EQ(path.status, PathStatus::Invalid);
}

TEST_F(NavFailureTest, StartAndEndOffMesh){
    const Entity surface = planeSurface();
    bake(surface);
    const nav::NavPath complete = nav->calculatePath(Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f));
    expectInvariant(complete);
    EXPECT_EQ(complete.failure, PathFailure::None);
    EXPECT_EQ(complete.surface, surface);
    EXPECT_FLOAT_EQ(complete.gap, 0.0f);

    const nav::NavPath endOff = nav->calculatePath(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 40.0f));
    expectInvariant(endOff);
    EXPECT_EQ(endOff.failure, PathFailure::EndOffMesh);
    EXPECT_EQ(endOff.surface, surface);
    EXPECT_GE(endOff.startSnapDistance, 0.0f);
    EXPECT_LT(endOff.endSnapDistance, 0.0f);
    // no corners: from the snapped start to the requested end
    EXPECT_NEAR(endOff.gap, 40.0f, FAILURE_CELL);

    const nav::NavPath startOff = nav->calculatePath(Vector3(0.0f, 0.0f, 40.0f), Vector3(0.0f, 0.0f, 0.0f));
    expectInvariant(startOff);
    EXPECT_EQ(startOff.failure, PathFailure::StartOffMesh);
    EXPECT_LT(startOff.startSnapDistance, 0.0f);
    EXPECT_GE(startOff.endSnapDistance, 0.0f);
    // both off: the start is told first
    EXPECT_EQ(nav->calculatePath(Vector3(0.0f, 0.0f, 40.0f), Vector3(0.0f, 0.0f, -40.0f)).failure, PathFailure::StartOffMesh);
}

TEST_F(NavFailureTest, FilteredEnds){
    // a Jump strip across the plane at x in [-12, -8]
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(-10.0f, 0.0f, 0.0f), Vector3(4.0f, 4.0f, 12.0f), nav::AREA_JUMP);
    bake(surface);
    nav::QueryFilter noJump;
    noJump.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << nav::AREA_JUMP));
    const nav::NavPath start = nav->calculatePath(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), noJump);
    expectInvariant(start);
    EXPECT_EQ(start.failure, PathFailure::StartFiltered);
    const nav::NavPath end = nav->calculatePath(Vector3(10.0f, 0.0f, 0.0f), Vector3(-10.0f, 0.0f, 0.0f), noJump);
    expectInvariant(end);
    EXPECT_EQ(end.failure, PathFailure::EndFiltered);
    // with every area the same ends connect
    EXPECT_EQ(nav->calculatePath(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f)).failure, PathFailure::None);
}

TEST_F(NavFailureTest, NoSurfaceAndNotBaked){
    plane(*scene, Vector3(), 20.0f, 20.0f);
    resolveTransforms(*scene);
    const nav::NavPath none = nav->calculatePath(Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f));
    expectInvariant(none);
    EXPECT_EQ(none.failure, PathFailure::NoSurface);
    EXPECT_EQ(none.surface, NULL_ENTITY);
    EXPECT_NEAR(none.gap, 10.0f, 1e-4f);

    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    const nav::NavPath unbaked = nav->calculatePath(Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f));
    expectInvariant(unbaked);
    EXPECT_EQ(unbaked.failure, PathFailure::NotBaked);
    nav::PathFailure why = PathFailure::None;
    EXPECT_FALSE(nav->isReachable(Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f), {}, &why));
    EXPECT_EQ(why, PathFailure::NotBaked);

    // baked, but of another agent type, or disabled: no surface
    bake(surface);
    nav::QueryFilter other;
    other.agentType = 3;
    EXPECT_EQ(nav->calculatePath(Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f), other).failure, PathFailure::NoSurface);
    scene->getComponent<NavMeshSurfaceComponent>(surface).enabled = false;
    EXPECT_EQ(nav->calculatePath(Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f)).failure, PathFailure::NoSurface);
    EXPECT_EQ(nav->calculatePathOn(surface, Vector3(-5.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f)).failure, PathFailure::NoSurface);
}

TEST_F(NavFailureTest, DisconnectedIslands){
    const Entity surface = twoFloors();
    bake(surface);
    const Vector3 a(-6.0f, 0.0f, 0.0f), b(6.0f, 0.0f, 0.0f);
    // islands not labelled: a search finds it Partial
    const nav::NavPath path = nav->calculatePath(a, b);
    expectInvariant(path);
    EXPECT_EQ(path.failure, PathFailure::Disconnected);
    EXPECT_EQ(path.startIsland, -1);
    EXPECT_EQ(path.endIsland, -1);
    ASSERT_FALSE(path.corners.empty());

    // labelled (a diagnostic asked): isReachable rejects without a search
    ASSERT_EQ(nav->getIslands(surface).size(), 2u);
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    ASSERT_TRUE(mesh->areIslandsCurrent());
    const uint64_t searches = mesh->getSearchCount();
    nav::PathFailure why = PathFailure::None;
    EXPECT_FALSE(nav->isReachable(a, b, {}, &why));
    EXPECT_EQ(why, PathFailure::Disconnected);
    EXPECT_EQ(mesh->getSearchCount(), searches);
    // a path still searches (it leads to the closest point) and now names both islands
    const nav::NavPath labelled = nav->calculatePath(a, b);
    EXPECT_EQ(labelled.failure, PathFailure::Disconnected);
    EXPECT_GE(labelled.startIsland, 0);
    EXPECT_GE(labelled.endIsland, 0);
    EXPECT_NE(labelled.startIsland, labelled.endIsland);
    EXPECT_EQ(mesh->getSearchCount(), searches + 1);
    // reachable on one island
    EXPECT_TRUE(nav->isReachable(a, Vector3(-4.0f, 0.0f, 2.0f), {}, &why));
    EXPECT_EQ(why, PathFailure::None);
}

TEST_F(NavFailureTest, BlockedByArea){
    // a strip of area 4 across the plane: masked out, the far side is only reachable through it
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 12.0f), 4);
    bake(surface);
    nav::QueryFilter filter;
    filter.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << 4));
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    const uint64_t searches = mesh->getSearchCount();
    const nav::NavPath path = nav->calculatePath(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), filter);
    expectInvariant(path);
    EXPECT_EQ(path.failure, PathFailure::BlockedByArea);
    // R22: a failed path costs at most two searches
    EXPECT_EQ(mesh->getSearchCount(), searches + 2);
    nav::PathFailure why = PathFailure::None;
    EXPECT_FALSE(nav->isReachable(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), filter, &why));
    EXPECT_EQ(why, PathFailure::BlockedByArea);
}

TEST_F(NavFailureTest, OneWayLink){
    const Entity surface = twoFloors();
    link(Vector3(-3.0f, 0.0f, 0.0f), Vector3(3.0f, 0.0f, 0.0f), false);
    bake(surface);
    // one island (weakly connected), but only one way across
    ASSERT_EQ(nav->getIslands(surface).size(), 1u);
    const nav::NavPath there = nav->calculatePath(Vector3(-6.0f, 0.0f, 0.0f), Vector3(6.0f, 0.0f, 0.0f));
    EXPECT_EQ(there.failure, PathFailure::None);
    const nav::NavPath back = nav->calculatePath(Vector3(6.0f, 0.0f, 0.0f), Vector3(-6.0f, 0.0f, 0.0f));
    expectInvariant(back);
    EXPECT_EQ(back.failure, PathFailure::Disconnected);
    EXPECT_EQ(back.startIsland, back.endIsland);
}

TEST_F(NavFailureTest, OutOfNodes){
    // a 3.2 km strip in 1.6 m tiles: a path along it needs more than the query's 2048 nodes
    plane(*scene, Vector3(), 5120.0f, 3.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Strip");
    addSurface(*scene, surface, 0.2f, 8);
    bake(surface);
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    ASSERT_NE(mesh, nullptr);
    ASSERT_GE(mesh->getPolyCount(), 3000);
    const nav::NavPath path = nav->calculatePath(Vector3(-2550.0f, 0.0f, 0.0f), Vector3(2550.0f, 0.0f, 0.0f));
    expectInvariant(path);
    EXPECT_EQ(path.failure, PathFailure::OutOfNodes);
    // a short path on the same strip is Complete
    EXPECT_EQ(nav->calculatePath(Vector3(-10.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f)).failure, PathFailure::None);
}

TEST_F(NavFailureTest, PartialHasClosestPointAndGap){
    const Entity surface = planeSurface(30.0f, 10.0f);
    areaVolume(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 12.0f), 4);
    bake(surface);
    nav::QueryFilter filter;
    filter.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << 4));
    // a start 0.3 m above the floor snaps down
    const Vector3 from(-10.0f, 0.3f, 0.0f), to(10.0f, 0.0f, 0.0f);
    const nav::NavPath path = nav->calculatePath(from, to, filter);
    ASSERT_EQ(path.status, PathStatus::Partial);
    ASSERT_FALSE(path.corners.empty());
    EXPECT_EQ(path.closestPoint, path.corners.back());
    // the strip begins at x = -1: the path stops there, 11 m from the end
    EXPECT_NEAR(path.closestPoint.x, -1.0f, FAILURE_CELL);
    EXPECT_NEAR(path.gap, 11.0f, FAILURE_CELL);
    EXPECT_NEAR(path.gap, path.closestPoint.distance(path.end), 1e-4f);
    // the navmesh lies within a cell height (0.1 m) over the floor
    EXPECT_NEAR(path.startSnapDistance, 0.3f, 0.12f);
    EXPECT_NEAR(path.start.y, 0.0f, 0.12f);
    EXPECT_NEAR(path.start.x, -10.0f, 1e-3f);
    EXPECT_NEAR(path.endSnapDistance, 0.0f, 0.12f);
    // the start moved straight down onto the mesh
    EXPECT_NEAR(path.start.y + path.startSnapDistance, 0.3f, 1e-3f);
    EXPECT_NEAR(path.end.x, 10.0f, 1e-3f);
}

TEST_F(NavFailureTest, SurfaceRanking){
    // two surfaces of one agent type, each over its own floor (Collect Objects = Children)
    auto floorSurface = [&](float x, const char* name) {
        const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, name);
        addSurface(*scene, surface, 0.2f, 32).collectObjects = NavCollectObjects::CHILDREN;
        plane(*scene, Vector3(x, 0.0f, 0.0f), 10.0f, 10.0f, surface);
        return surface;
    };
    const Entity first = floorSurface(-20.0f, "First");
    const Entity second = floorSurface(20.0f, "Second");
    resolveTransforms(*scene);
    bake(first);
    bake(second);
    // on the second floor: the first surface finds no start, the second answers Complete
    const nav::NavPath onSecond = nav->calculatePath(Vector3(18.0f, 0.0f, 0.0f), Vector3(22.0f, 0.0f, 0.0f));
    EXPECT_EQ(onSecond.failure, PathFailure::None);
    EXPECT_EQ(onSecond.surface, second);
    // on the first: the first is Complete
    EXPECT_EQ(nav->calculatePath(Vector3(-22.0f, 0.0f, 0.0f), Vector3(-18.0f, 0.0f, 0.0f)).surface, first);
    // start on the second, end nowhere: EndOffMesh (second) ranks before StartOffMesh (first)
    const nav::NavPath nowhere = nav->calculatePath(Vector3(20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 60.0f));
    EXPECT_EQ(nowhere.failure, PathFailure::EndOffMesh);
    EXPECT_EQ(nowhere.surface, second);
    // across the two: each surface misses one end; the end is the better miss
    const nav::NavPath across = nav->calculatePath(Vector3(-20.0f, 0.0f, 0.0f), Vector3(20.0f, 0.0f, 0.0f));
    EXPECT_EQ(across.failure, PathFailure::EndOffMesh);
    EXPECT_EQ(across.surface, first);
}

TEST_F(NavFailureTest, GridReasons){
    nav::NavGridMap map;
    map.reset(16, 16);
    map.setLayout(nav::GridLayout::Square, 8);
    // a wall at x = 8 with no door, a strip of area 3 at y = 0
    map.fillRect(nav::GridCell{8, 0}, nav::GridCell{8, 15}, nav::AREA_NOT_WALKABLE);
    map.fillRect(nav::GridCell{0, 0}, nav::GridCell{7, 0}, 3);
    float costs[nav::AREA_COUNT];
    for (float& cost : costs)
        cost = 1.0f;
    nav::GridSolver solver;
    nav::GridQuery query;
    EXPECT_EQ(solver.solve(map, nav::GridCell{1, 1}, nav::GridCell{6, 6}, query, costs).failure, PathFailure::None);
    EXPECT_EQ(solver.solve(map, nav::GridCell{-1, 1}, nav::GridCell{6, 6}, query, costs).failure, PathFailure::StartOffMesh);
    EXPECT_EQ(solver.solve(map, nav::GridCell{1, 1}, nav::GridCell{8, 6}, query, costs).failure, PathFailure::EndOffMesh);
    EXPECT_EQ(solver.solve(map, nav::GridCell{1, 1}, nav::GridCell{12, 6}, query, costs).failure, PathFailure::Disconnected);
    query.allowPartial = true;
    const nav::GridPath partial = solver.solve(map, nav::GridCell{1, 1}, nav::GridCell{12, 6}, query, costs);
    EXPECT_EQ(partial.status, PathStatus::Partial);
    EXPECT_EQ(partial.failure, PathFailure::Disconnected);
    query.allowPartial = false;
    query.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << 3));
    EXPECT_EQ(solver.solve(map, nav::GridCell{2, 0}, nav::GridCell{6, 6}, query, costs).failure, PathFailure::StartFiltered);
    EXPECT_EQ(solver.solve(map, nav::GridCell{6, 6}, nav::GridCell{2, 0}, query, costs).failure, PathFailure::EndFiltered);
}
