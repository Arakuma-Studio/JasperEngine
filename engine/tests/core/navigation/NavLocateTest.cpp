// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where a point is and how much is walkable (docs/specs/S75-navigation-diagnostics.md R9, R10;
// AC8): locate on, near and far off the mesh, and the coverage of a plane.

#include "NavTestUtil.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {
    class NavLocateTest : public NavSceneTest {};
}

TEST_F(NavLocateTest, OnAndOffMesh){
    const Entity surface = planeSurface();
    bake(surface);
    // on: the search box finds a polygon, the point barely moves
    const nav::NavLocation on = nav->locate(Vector3(1.0f, 0.2f, 2.0f));
    EXPECT_TRUE(on.onMesh);
    EXPECT_TRUE(on.found);
    // the navmesh lies within a cell height (0.1 m) over the floor
    EXPECT_NEAR(on.snapDistance, 0.2f, 0.12f);
    EXPECT_NEAR(on.position.y, 0.0f, 0.12f);
    EXPECT_EQ(on.island, 0);
    EXPECT_EQ(on.area, nav::AREA_WALKABLE);
    EXPECT_EQ(on.surface, surface);

    // 1.5 m past the eroded edge (9.5): off the mesh, found within 5 m
    const nav::NavLocation nearby = nav->locate(Vector3(11.0f, 0.0f, 0.0f));
    EXPECT_FALSE(nearby.onMesh);
    EXPECT_TRUE(nearby.found);
    EXPECT_NEAR(nearby.snapDistance, 1.5f, 0.25f);
    EXPECT_NEAR(nearby.position.x, 9.5f, 0.25f);
    EXPECT_EQ(nearby.surface, surface);

    // 10 m off, 5 m search: nothing
    const nav::NavLocation distant = nav->locate(Vector3(19.5f, 0.0f, 0.0f), {}, 5.0f);
    EXPECT_FALSE(distant.onMesh);
    EXPECT_FALSE(distant.found);
    EXPECT_LT(distant.snapDistance, 0.0f);
    EXPECT_EQ(distant.surface, NULL_ENTITY);
    // with a 12 m search it is found
    EXPECT_TRUE(nav->locate(Vector3(19.5f, 0.0f, 0.0f), {}, 12.0f).found);

    // the location's surface on one surface
    EXPECT_TRUE(nav->locateOn(surface, Vector3()).onMesh);
}

TEST_F(NavLocateTest, CoverageOfPlane){
    const Entity surface = planeSurface();
    bake(surface);
    const nav::NavCoverage coverage = nav->getCoverage(surface);
    EXPECT_NEAR(coverage.walkableArea, 361.0f, 361.0f * 0.05f);
    EXPECT_EQ(coverage.islandCount, 1);
    EXPECT_FLOAT_EQ(coverage.largestIslandArea, coverage.walkableArea);
    // 6.4 m tiles over [-10, 10]: 4 x 4 tiles
    EXPECT_NEAR(coverage.tilesArea, 16.0f * 6.4f * 6.4f, 0.01f);
    EXPECT_NEAR(coverage.fraction, coverage.walkableArea / coverage.tilesArea, 1e-5f);
    // not baked: nothing
    const Entity other = transformEntity(*scene, Vector3(), NULL_ENTITY, "Other");
    addSurface(*scene, other);
    EXPECT_EQ(nav->getCoverage(other).islandCount, 0);
    EXPECT_TRUE(nav->getIslands(other).empty());
}
