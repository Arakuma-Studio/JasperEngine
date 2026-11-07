// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Navmesh islands (docs/specs/S54-navigation-diagnostics.md R7, R8, R20; AC6, AC7): components
// joined through links, area and bounds, relabelled when the revision changes (carving, flags),
// deterministic ids, and path queries that never label.

#include "NavTestUtil.h"

#include "AllocationCounter.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshObstacleComponent.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavIslandTest : public NavSceneTest {
    protected:
        Entity floors() {
            plane(*scene, Vector3(-6.0f, 0.0f, 0.0f), 8.0f, 8.0f);
            plane(*scene, Vector3(6.0f, 0.0f, 0.0f), 8.0f, 8.0f);
            const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
            addSurface(*scene, surface, 0.2f, 32);
            return surface;
        }
    };

}

TEST_F(NavIslandTest, LabelsThroughLinks){
    const Entity surface = floors();
    const Entity link = transformEntity(*scene, Vector3(), NULL_ENTITY, "Bridge");
    NavMeshLinkComponent component;
    component.startPoint = Vector3(-3.0f, 0.0f, 0.0f);
    component.endPoint = Vector3(3.0f, 0.0f, 0.0f);
    scene->addComponent<NavMeshLinkComponent>(link, component);
    bake(surface);
    EXPECT_EQ(nav->getIslands(surface).size(), 1u);
    // the link gone and rebaked: two floors, two islands
    scene->getComponent<NavMeshLinkComponent>(link).enabled = false;
    bake(surface);
    const std::vector<nav::NavIsland> islands = nav->getIslands(surface);
    ASSERT_EQ(islands.size(), 2u);
    // discovery order: tiles ascending (y, x): the floor at -x first
    EXPECT_LT(islands[0].boundsMax.x, 0.0f);
    EXPECT_GT(islands[1].boundsMin.x, 0.0f);
}

TEST_F(NavIslandTest, AreaAndBounds){
    const Entity surface = planeSurface();
    bake(surface);
    const std::vector<nav::NavIsland> islands = nav->getIslands(surface);
    ASSERT_EQ(islands.size(), 1u);
    // 20 x 20 m eroded by r = 0.5, which Recast rounds up to whole 0.2 m cells: about 19^2 m^2
    EXPECT_NEAR(islands[0].area, 361.0f, 361.0f * 0.05f);
    EXPECT_GT(islands[0].polys, 0);
    EXPECT_NEAR(islands[0].boundsMin.x, -9.5f, 0.4f);
    EXPECT_NEAR(islands[0].boundsMax.x, 9.5f, 0.4f);
    EXPECT_NEAR(islands[0].boundsMin.z, -9.5f, 0.4f);
    EXPECT_NEAR(islands[0].boundsMax.z, 9.5f, 0.4f);
    // a square: its area is its bounds' footprint
    const float footprint = (islands[0].boundsMax.x - islands[0].boundsMin.x) * (islands[0].boundsMax.z - islands[0].boundsMin.z);
    EXPECT_NEAR(islands[0].area, footprint, footprint * 0.01f);
    EXPECT_NEAR(nav->getNavMesh(surface)->getWalkableArea(), islands[0].area, 1e-3f);
}

TEST_F(NavIslandTest, RelabelOnRevision){
    // a 20 x 4 m corridor on a carving surface; a carving box across it closes it
    plane(*scene, Vector3(), 20.0f, 4.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Corridor");
    addSurface(*scene, surface, 0.2f, 32).runtimeCarving = true;
    bake(surface);
    ASSERT_EQ(nav->getIslands(surface).size(), 1u);
    const Entity box = transformEntity(*scene, Vector3(0.0f, 0.5f, 0.0f), NULL_ENTITY, "Door");
    NavMeshObstacleComponent obstacle;
    obstacle.carve = true;
    obstacle.carveOnlyStationary = false;
    obstacle.size = Vector3(1.0f, 2.0f, 6.0f);
    scene->addComponent<NavMeshObstacleComponent>(box, obstacle);
    steps(10);
    EXPECT_EQ(nav->getIslands(surface).size(), 2u);
    EXPECT_TRUE(nav->getNavMesh(surface)->areIslandsCurrent());
    scene->getComponent<NavMeshObstacleComponent>(box).enabled = false;
    steps(10);
    EXPECT_EQ(nav->getIslands(surface).size(), 1u);
}

TEST_F(NavIslandTest, ZeroFlagPolysExcluded){
    // a 20 x 4 m corridor in 6.4 m tiles: the tile over [0, 6.4) holds the corridor's middle
    plane(*scene, Vector3(), 20.0f, 4.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Corridor");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);
    nav::NavMesh* mesh = const_cast<nav::NavMesh*>(nav->getNavMesh(surface));
    ASSERT_EQ(mesh->getIslands().size(), 1u);
    const nav::NavFilter all = nav::NavFilter::fromSettings(nav::ALL_AREAS);
    const Vector3 extents(0.2f, 1.0f, 0.2f);
    // every polygon across the corridor at x = 3 off
    int disabled = 0;
    for (float z = -1.4f; z <= 1.4f; z += 0.2f) {
        const uint32_t poly = mesh->getPolyAt(Vector3(3.0f, 0.0f, z), all, extents);
        if (poly && mesh->setPolyFlags(poly, 0))
            disabled++;
    }
    ASSERT_GT(disabled, 0);
    EXPECT_FALSE(mesh->areIslandsCurrent());
    EXPECT_EQ(mesh->getIslands().size(), 2u);
    EXPECT_EQ(nav->calculatePath(Vector3(-8.0f, 0.0f, 0.0f), Vector3(8.0f, 0.0f, 0.0f)).failure, nav::PathFailure::Disconnected);
    EXPECT_FALSE(mesh->setPolyFlags(0, 1));
}

TEST_F(NavIslandTest, DeterministicIds){
    // three floors of different sizes; two bakes label them the same
    plane(*scene, Vector3(-15.0f, 0.0f, 0.0f), 6.0f, 6.0f);
    plane(*scene, Vector3(0.0f, 0.0f, 10.0f), 10.0f, 4.0f);
    plane(*scene, Vector3(12.0f, 0.0f, -8.0f), 8.0f, 8.0f);
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.2f, 32);
    bake(surface);
    const std::vector<nav::NavIsland> first = nav->getIslands(surface);
    bake(surface);
    const std::vector<nav::NavIsland> second = nav->getIslands(surface);
    ASSERT_EQ(first.size(), 3u);
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); i++) {
        EXPECT_EQ(first[i].id, (int)i);
        EXPECT_EQ(first[i].id, second[i].id);
        EXPECT_EQ(first[i].polys, second[i].polys);
        EXPECT_FLOAT_EQ(first[i].area, second[i].area);
        EXPECT_EQ(first[i].boundsMin, second[i].boundsMin);
    }
    // ascending (y, x) tiles: the floor at z = -8 comes first, the one at z = 10 last
    EXPECT_LT(first[0].boundsMax.z, 0.0f);
    EXPECT_GT(first[2].boundsMin.z, 5.0f);
}

TEST_F(NavIslandTest, QueriesNeverRelabel){
    const Entity surface = planeSurface();
    const Entity crate = box(*scene, Vector3(5.0f, 0.5f, 5.0f), Vector3(1.0f, 1.0f, 1.0f));
    bake(surface);
    const nav::NavMesh* mesh = nav->getNavMesh(surface);
    ASSERT_EQ(mesh->getIslands().size(), 1u);
    // a tile swap: the labels are stale now, and stay so
    scene->getComponent<Transform>(crate).position = Vector3(-5.0f, 0.5f, -5.0f);
    resolveTransforms(*scene);
    bake(surface, NavBakeMode::DIRTY);
    mesh = nav->getNavMesh(surface);
    ASSERT_FALSE(mesh->areIslandsCurrent());
    const uint64_t labelled = mesh->getLabelledRevision();
    const nav::NavFilter filter = nav::NavFilter::fromSettings(nav::ALL_AREAS);
    const Vector3 extents(1.0f, 2.0f, 1.0f);
    const Vector3 from(-8.0f, 0.0f, 3.0f), to(8.0f, 0.0f, -3.0f);
    // one warm query sizes the query's scratch
    ASSERT_EQ(mesh->findPath(from, to, filter, extents).status, nav::PathStatus::Complete);
    if (testsupport::countsAllocations()) {
        testsupport::AllocationCountScope scope;
        for (int i = 0; i < 1000; i++) {
            const nav::NavPath path = mesh->findPath(from, to, filter, extents);
            ASSERT_EQ(path.failure, nav::PathFailure::None);
        }
        // per path its four vectors (S19's corners; S55's corner areas, corner flags and polygon
        // corridor): nothing more (no labels, no second search)
        EXPECT_EQ(scope.count(), 4000u);
    }
    for (int i = 0; i < 100; i++)
        nav->calculatePath(from, to);
    EXPECT_FALSE(mesh->areIslandsCurrent());
    EXPECT_EQ(mesh->getLabelledRevision(), labelled);
    EXPECT_EQ(mesh->getIslandOf(1), -1);
}
