// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The bake report (docs/specs/S75-navigation-diagnostics.md R16, R17; AC11): what a bake
// collected, its islands on first read, its error codes and hints, and the per-tile build times
// kept across incremental bakes and forgotten on a reload.

#include "NavTestUtil.h"

#include "component/NavMeshObstacleComponent.h"

#include <map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::navtest;
using nav::NavBakeError;

namespace {
    class NavBakeReportTest : public NavSceneTest {};
}

TEST_F(NavBakeReportTest, SourcesAndIslands){
    // 2 meshes, 1 terrain, 1 excluded (an obstacle's mesh)
    plane(*scene, Vector3(), 30.0f, 30.0f);
    box(*scene, Vector3(4.0f, 0.5f, 4.0f), Vector3(2.0f, 1.0f, 2.0f));
    const Heightmap heights = hills(33, 5);
    terrain(*scene, Vector3(60.0f, 0.0f, 0.0f), 32.0f, 2.0f, heights);
    const Entity crate = box(*scene, Vector3(-4.0f, 0.5f, -4.0f), Vector3(1.0f, 1.0f, 1.0f));
    scene->addComponent<NavMeshObstacleComponent>(crate, NavMeshObstacleComponent());
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    addSurface(*scene, surface, 0.25f, 32);
    const NavBakeResult landed = bake(surface);
    EXPECT_EQ(landed.sources.meshes, 2);
    EXPECT_EQ(landed.sources.terrains, 1);
    EXPECT_EQ(landed.sources.excluded, 1);
    EXPECT_EQ(landed.sources.instances, 0);
    EXPECT_GT(landed.sources.triangles, 12u);
    EXPECT_EQ(landed.errorCode, NavBakeError::None);
    EXPECT_TRUE(landed.hint.empty());
    // without recordSources nothing names the excluded GameObjects
    EXPECT_TRUE(landed.sources.excludedEntities.empty());
    // the tiles it built, each with its time and size
    ASSERT_EQ((int)landed.tiles.size(), landed.tilesBuilt);
    for (const nav::NavTileReport& tile : landed.tiles) {
        EXPECT_GE(tile.milliseconds, 0.0);
        if (tile.polys > 0)
            EXPECT_GT(tile.bytes, 0u);
    }
    // the bake's landing labels nothing: the first read does
    EXPECT_EQ(landed.islandCount, -1);
    EXPECT_FALSE(nav->getNavMesh(surface)->areIslandsCurrent());
    const NavBakeResult* read = nav->getLastBakeResult(surface);
    ASSERT_NE(read, nullptr);
    // the floor (with its box top) and the terrain: two apart
    EXPECT_GE(read->islandCount, 2);
    EXPECT_EQ(read->islandCount, nav->getCoverage(surface).islandCount);
    EXPECT_NEAR(read->walkableArea, nav->getCoverage(surface).walkableArea, 1e-3f);
    ASSERT_FALSE(read->islands.empty());
    EXPECT_LE(read->islands.size(), 16u);
    for (size_t i = 1; i < read->islands.size(); i++)
        EXPECT_GE(read->islands[i - 1].area, read->islands[i].area);
}

TEST_F(NavBakeReportTest, ErrorCodes){
    // nothing collected: the code, its hint, and the surface's last result
    const Entity empty = transformEntity(*scene, Vector3(), NULL_ENTITY, "Empty");
    addSurface(*scene, empty);
    const NavBakeResult nothing = nav->bakeNow(empty, NavBakeMode::FULL);
    EXPECT_FALSE(nothing.ok);
    EXPECT_EQ(nothing.errorCode, NavBakeError::NothingCollected);
    EXPECT_EQ(nothing.hint, nav::bakeErrorHint(NavBakeError::NothingCollected));
    EXPECT_STREQ(nav::bakeErrorName(nothing.errorCode), "nothing_collected");
    ASSERT_NE(nav->getLastBakeResult(empty), nullptr);
    EXPECT_EQ(nav->getLastBakeResult(empty)->errorCode, NavBakeError::NothingCollected);

    // S19's R6 fixture: too large
    plane(*scene, Vector3(), 2000.0f, 2000.0f);
    const Entity huge = transformEntity(*scene, Vector3(), NULL_ENTITY, "Huge");
    addSurface(*scene, huge, 0.05f, 16);
    resolveTransforms(*scene);
    std::string error;
    NavBakeError code = NavBakeError::None;
    EXPECT_FALSE(nav->bake(huge, NavBakeMode::FULL, error, &code));
    EXPECT_EQ(code, NavBakeError::TooLarge);
    EXPECT_EQ(nav->getLastBakeResult(huge)->errorCode, NavBakeError::TooLarge);
    EXPECT_FALSE(nav->getLastBakeResult(huge)->hint.empty());

    // busy: a second bake while one runs; the running one's result is not replaced
    const Entity small = transformEntity(*scene, Vector3(), NULL_ENTITY, "Small");
    addSurface(*scene, small, 0.5f, 32).collectObjects = NavCollectObjects::VOLUME;
    scene->getComponent<NavMeshSurfaceComponent>(small).volumeSize = Vector3(20.0f, 4.0f, 20.0f);
    ASSERT_TRUE(nav->bake(small, NavBakeMode::FULL, error, &code)) << error;
    EXPECT_EQ(code, NavBakeError::None);
    EXPECT_FALSE(nav->bake(small, NavBakeMode::FULL, error, &code));
    EXPECT_EQ(code, NavBakeError::Busy);
    EXPECT_EQ(nav->getLastBakeResult(small), nullptr);
    nav->cancelBake(small);
    while (nav->isBaking(small))
        nav->processBakes(0.0);
    EXPECT_EQ(nav->getLastBakeResult(small)->errorCode, NavBakeError::Cancelled);

    // no surface
    EXPECT_FALSE(nav->bake(transformEntity(*scene, Vector3()), NavBakeMode::FULL, error, &code));
    EXPECT_EQ(code, NavBakeError::NoSurface);
    for (int i = 0; i <= (int)NavBakeError::SaveFailed; i++) {
        NavBakeError parsed = NavBakeError::None;
        EXPECT_TRUE(nav::bakeErrorFromName(nav::bakeErrorName((NavBakeError)i), parsed));
        EXPECT_EQ(parsed, (NavBakeError)i);
        if (i > 0)
            EXPECT_GT(std::string(nav::bakeErrorHint((NavBakeError)i)).size(), 10u);
    }
}

TEST_F(NavBakeReportTest, TileReportsKept){
    const Entity surface = planeSurface(30.0f, 30.0f);
    const Entity crate = box(*scene, Vector3(5.0f, 0.5f, 5.0f), Vector3(1.0f, 1.0f, 1.0f));
    bake(surface);
    std::map<std::pair<int, int>, double> before;
    for (const nav::NavTileReport& tile : nav->getTileReports(surface)) {
        EXPECT_GE(tile.milliseconds, 0.0);
        EXPECT_GT(tile.polys, 0);
        EXPECT_GT(tile.bytes, 0u);
        before[{tile.x, tile.y}] = tile.milliseconds;
    }
    ASSERT_GT(before.size(), 4u);
    // the crate moves within its tile: only that tile is built again, the others keep their times
    scene->getComponent<Transform>(crate).position = Vector3(5.5f, 0.5f, 5.0f);
    resolveTransforms(*scene);
    const NavBakeResult incremental = bake(surface, NavBakeMode::DIRTY);
    ASSERT_GE(incremental.tilesBuilt, 1);
    ASSERT_LT((size_t)incremental.tilesBuilt, before.size());
    std::map<std::pair<int, int>, double> rebuilt;
    for (const nav::NavTileReport& tile : incremental.tiles)
        rebuilt[{tile.x, tile.y}] = tile.milliseconds;
    for (const nav::NavTileReport& tile : nav->getTileReports(surface)) {
        const auto key = std::make_pair(tile.x, tile.y);
        if (rebuilt.count(key))
            EXPECT_EQ(tile.milliseconds, rebuilt[key]);
        else
            EXPECT_EQ(tile.milliseconds, before[key]);
    }
    // read again from data: the times are of another session
    nav::NavMeshData data = *nav->getNavMesh(surface)->getData();
    std::string error;
    ASSERT_TRUE(nav->setNavMeshData(surface, std::move(data), error)) << error;
    for (const nav::NavTileReport& tile : nav->getTileReports(surface))
        EXPECT_EQ(tile.milliseconds, -1.0);
}
