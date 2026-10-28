// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 2D navigation (docs/specs/S19-navigation.md R21, R36; AC19): a top-down surface on the XY plane
// around static Box2D footprints, and a NavGrid rebuilt from a tilemap's rect -> area table.

#include "NavTestUtil.h"

#include "component/NavGridComponent.h"
#include "object/navigation/NavGrid.h"
#include "object/navigation/NavMeshAgent.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::navtest;
using nav::GridCell;

namespace {

    class Nav2DTest : public NavSceneTest {};

}

#ifdef JASPERENGINE_PHYSICS_2D
TEST_F(Nav2DTest, TopDownColliders){
    TopDown fixture = topDown(*scene);
    bake(fixture.surface);
    // the agent radius in points: 0.5 m at 64 points per metre
    const float radius = 0.5f * 64.0f;
    const Vector3 from(60.0f, 60.0f, 0.0f), to(1220.0f, 900.0f, 0.0f);
    const nav::NavPath path = nav->calculatePath(from, to);
    ASSERT_EQ(path.status, nav::PathStatus::Complete);
    ASSERT_GE(path.corners.size(), 2u);
    // every sampled point keeps the radius from every collider (one cell of rasterization slack)
    const float slack = radius / 3.0f;
    for (size_t i = 0; i + 1 < path.corners.size(); i++) {
        const Vector3 a = path.corners[i], b = path.corners[i + 1];
        EXPECT_NEAR(a.z, 0.0f, 1e-3f);
        const int n = std::max(1, (int)std::ceil(a.distance(b) / 4.0f));
        for (int k = 0; k <= n; k++) {
            const Vector3 p = a.lerp(b, (float)k / (float)n);
            for (const Vector3& circle : fixture.circles) {
                const float distance = Vector2(p.x - circle.x, p.y - circle.y).length() - circle.z;
                EXPECT_GE(distance, radius - slack) << p.toString();
            }
            for (const auto& box : fixture.boxes) {
                const float dx = std::max(0.0f, std::fabs(p.x - box.first.x) - box.second.x);
                const float dy = std::max(0.0f, std::fabs(p.y - box.first.y) - box.second.y);
                EXPECT_GE(std::sqrt(dx * dx + dy * dy), radius - slack) << p.toString();
            }
        }
    }
    // an agent walks it (the same component as 3D)
    const Entity walker = agent(*scene, from);
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.isOnNavMesh());
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    ASSERT_TRUE(handle.setDestination(to));
    for (int tick = 0; tick < 60 * 20 && reached == 0; tick++) {
        step();
        EXPECT_NEAR(position(walker).z, 0.0f, 1e-3f);
    }
    EXPECT_EQ(reached, 1);
}
#endif

TEST_F(Nav2DTest, TilemapGrid){
    TilemapFixture fixture = tilemap(*scene);
    NavGrid grid(scene.get(), fixture.grid);
    ASSERT_TRUE(grid.rebuild());
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 16; x++) {
            const char c = tilemapCell(x, y);
            const int expected = c == '.' ? nav::AREA_WALKABLE : (c == '~' ? 3 : nav::AREA_NOT_WALKABLE);
            EXPECT_EQ(grid.getCellArea(GridCell{x, y}), expected) << x << "," << y;
        }
    }
    // the door in the wall joins the halves
    EXPECT_TRUE(grid.isReachable(GridCell{1, 1}, GridCell{14, 10}));

    // a painted override survives a tilemap edit and a rebuild
    nav::NavGridMap* map = scene->getSystem<NavigationSystem>()->getGridMap(fixture.grid);
    ASSERT_NE(map, nullptr);
    map->setArea(GridCell{12, 3}, 5);
    map->setOverride(GridCell{12, 3}, true);
    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(fixture.tilemap);
    // the door becomes wall
    for (unsigned int t = 0; t < tilemap.numTiles; t++) {
        if (tilemap.tiles[t].position == Vector2(8.0f * 32.0f, 6.0f * 32.0f)) {
            tilemap.tiles[t].rectId = 2;
            tilemap.tiles[t].name = "wall";
        }
    }
    ASSERT_TRUE(grid.rebuild());
    EXPECT_EQ(grid.getCellArea(GridCell{8, 6}), nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(grid.getCellArea(GridCell{12, 3}), 5);
    EXPECT_FALSE(grid.isReachable(GridCell{1, 1}, GridCell{14, 10}));
    // Reset overrides: the source's area again
    map->clearOverrides();
    ASSERT_TRUE(grid.rebuild());
    EXPECT_EQ(grid.getCellArea(GridCell{12, 3}), nav::AREA_WALKABLE);
}

TEST_F(Nav2DTest, GridAgentsOnTilemap){
    // the nav_tilemap scene: an agent crosses the wall through its door
    TilemapFixture fixture = tilemap(*scene);
    NavGrid grid(scene.get(), fixture.grid);
    ASSERT_TRUE(grid.rebuild());
    const Entity walker = agent(*scene, grid.cellToWorld(GridCell{2, 2}), 4.0f);
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.isOnNavMesh());
    EXPECT_EQ(handle.getSurface(), NULL_ENTITY);
    EXPECT_EQ(scene->getSystem<NavigationSystem>()->getAgentSurface(walker), fixture.grid);
    int reached = 0;
    handle.getOnDestinationReached() = std::function<void(NavMeshAgent)>([&](NavMeshAgent) { reached++; });
    ASSERT_TRUE(handle.setDestination(grid.cellToWorld(GridCell{13, 10})));
    // speed 4 m/s on a 2D grid is 4 scene units/s: in 32-point cells, slow; give it time
    scene->getComponent<NavMeshAgentComponent>(walker).speed = 200.0f;
    scene->getComponent<NavMeshAgentComponent>(walker).acceleration = 800.0f;
    for (int tick = 0; tick < 60 * 30 && reached == 0; tick++)
        step();
    EXPECT_EQ(reached, 1);
    EXPECT_EQ(grid.worldToCell(position(walker)), (GridCell{13, 10}));
}
