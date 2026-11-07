// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Runtime areas and links of docs/specs/S55-navigation-gameplay-api.md (R11-R18, R48; AC11-AC18)
// on the nav_doors fixture: runtime modifier volumes and script volumes over baked polygons, link
// Activated and Occupied, their survival through every tile rebuild, and their zero cost.

#include "NavTestUtil.h"

#include "TestPaths.h"

#include "AllocationCounter.h"
#include "LogCapture.h"
#include "component/NavMeshObstacleComponent.h"
#include "navigation/NavBakeCollector.h"
#include "object/navigation/NavMeshAgent.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavAreaOverrideTest : public NavSceneTest {};

    // bakes are bit-exact per platform and compiler only (S19 R40b): the hashes are recorded per tag
    std::string doorsHashTag() {
        std::string tag;
#if defined(__EMSCRIPTEN__)
        tag = "wasm";
#elif defined(_WIN32)
        tag = "windows";
#elif defined(__APPLE__)
        tag = "apple";
#elif defined(__ANDROID__)
        tag = "android";
#else
        tag = "linux";
#endif
#if defined(__clang__)
        tag += "-clang";
#elif defined(__GNUC__)
        tag += "-gcc";
#elif defined(_MSC_VER)
        tag += "-msvc";
#endif
#if defined(__x86_64__) || defined(_M_X64)
        tag += "-x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        tag += "-arm64";
#endif
        return tag;
    }

}

// AC13 (R48): nav_doors without its runtime volume collects the tile hashes recorded on the tree
// before S55 (JASPER_TEST_UPDATE_FIXTURES=1 records them): scenes without runtime volumes need no
// rebake after the upgrade
TEST_F(NavAreaOverrideTest, HashesUnchangedWithoutRuntimeVolumes){
    Doors fixture = doors(*scene);
    std::string error;
    std::shared_ptr<nav::NavBakeInput> input = nav::NavBakeCollector::collect(scene.get(), fixture.surface, error);
    ASSERT_TRUE(input) << error;
    ASSERT_FALSE(input->nonEmptyTiles.empty());

    std::map<std::string, uint64_t> hashes;
    for (uint32_t index : input->nonEmptyTiles) {
        const nav::NavTileCoord coord = input->tileCoord(index);
        hashes[std::to_string(coord.x) + "," + std::to_string(coord.y)] = input->tileHashes[index];
    }

    const std::string path = test::testPaths().assetDir + "/navigation/nav_doors_hashes.txt";
    const std::string tag = doorsHashTag();
    // lines: tag tile hash
    std::map<std::string, std::map<std::string, uint64_t>> recorded;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string lineTag, tile, hex;
            if (fields >> lineTag >> tile >> hex)
                recorded[lineTag][tile] = std::stoull(hex, nullptr, 16);
        }
    }
    if (test::envFlag("JASPER_TEST_UPDATE_FIXTURES")) {
        recorded[tag] = hashes;
        std::ofstream out(path);
        for (const auto& platform : recorded) {
            for (const auto& tile : platform.second) {
                char hex[17];
                std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)tile.second);
                out << platform.first << " " << tile.first << " " << hex << "\n";
            }
        }
        return;
    }
    ASSERT_TRUE(recorded.count(tag)) << "no nav_doors hashes recorded for " << tag << " in " << path
                                     << " (JASPER_TEST_UPDATE_FIXTURES=1 records them)";
    EXPECT_EQ(hashes, recorded[tag]);
}

namespace {

    // the storeTileState bytes of every tile, by (x, y, layer)
    std::map<std::tuple<int, int, int>, std::vector<uint8_t>> tileStates(const nav::NavMesh& mesh) {
        std::map<std::tuple<int, int, int>, std::vector<uint8_t>> out;
        std::vector<nav::NavTileInfo> tiles;
        mesh.getTileInfos(tiles);
        for (const nav::NavTileInfo& tile : tiles)
            out[{tile.x, tile.y, tile.layer}] = mesh.getTileStateBytes(tile.x, tile.y, tile.layer);
        return out;
    }

    bool doorOpen(NavigationSystem* nav, const Doors& fixture) {
        return nav->calculatePath(fixture.west, fixture.east).status == nav::PathStatus::Complete;
    }

}

// AC11 (R11, R13, R15): the doorway volume closed blocks the only way east, opened lets it through,
// each change bumps the revision, and opening restores every tile's baked state byte for byte
TEST_F(NavAreaOverrideTest, DoorBlocksAndOpens){
    Doors fixture = doors(*scene);
    const Entity door = doorway(*scene, fixture, false);
    bake(fixture.surface);
    nav->syncAreaOverrides();
    const nav::NavMesh* mesh = nav->getNavMesh(fixture.surface);
    ASSERT_NE(mesh, nullptr);
    const auto pristine = tileStates(*mesh);
    EXPECT_EQ(mesh->getPolyStateTileCount(), 0u) << "an open door holds nothing (R18)";
    ASSERT_TRUE(doorOpen(nav, fixture));

    NavMeshModifierComponent& modifier = scene->getComponent<NavMeshModifierComponent>(door);
    uint64_t revision = mesh->getRevision();
    modifier.enabled = true;
    EXPECT_TRUE(nav->syncAreaOverrides());
    EXPECT_NE(mesh->getRevision(), revision);
    const nav::NavPath closed = nav->calculatePath(fixture.west, fixture.east);
    EXPECT_EQ(closed.status, nav::PathStatus::Partial);
    EXPECT_GT(nav->getModifierAppliedPolygons(door), 0);
    EXPECT_NE(tileStates(*mesh), pristine);
    EXPECT_GT(mesh->getPolyStateTileCount(), 0u);
    // the closed doorway: Not Walkable, nothing to sample there
    nav::NavHit hit;
    EXPECT_FALSE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));

    // a second sync without changes changes nothing
    revision = mesh->getRevision();
    EXPECT_FALSE(nav->syncAreaOverrides());
    EXPECT_EQ(mesh->getRevision(), revision);

    modifier.enabled = false;
    EXPECT_TRUE(nav->syncAreaOverrides());
    EXPECT_NE(mesh->getRevision(), revision);
    EXPECT_TRUE(doorOpen(nav, fixture));
    EXPECT_EQ(tileStates(*mesh), pristine);
    EXPECT_EQ(mesh->getPolyStateTileCount(), 0u);

    // its area, not Not Walkable: the doorway polygons take it, the path goes through
    modifier.enabled = true;
    modifier.area = 4;
    nav->syncAreaOverrides();
    EXPECT_TRUE(doorOpen(nav, fixture));
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));
    EXPECT_EQ(hit.area, 4);
    // removed with its component: baked again
    scene->removeComponent<NavMeshModifierComponent>(door);
    nav->syncAreaOverrides();
    EXPECT_EQ(tileStates(*mesh), pristine);
}

// AC11, R15: an agent walking to the far room replans in play when the door closes ahead, and walks
// through again once it opens
TEST_F(NavAreaOverrideTest, AgentReplansAtTheDoor){
    Doors fixture = doors(*scene);
    const Entity door = doorway(*scene, fixture, false);
    bake(fixture.surface);
    const Entity walker = agent(*scene, Vector3(-12.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(fixture.east));
    steps(30);
    ASSERT_EQ(handle.getPathStatus(), nav::PathStatus::Complete);
    scene->getComponent<NavMeshModifierComponent>(door).enabled = true;
    // the next tick: it asks again and finds the doorway shut
    int ticks = 0;
    while (handle.getPathStatus() != nav::PathStatus::Partial && ticks < 60) {
        step();
        ticks++;
    }
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Partial) << "after " << ticks << " ticks";
    steps(240);
    EXPECT_LT(position(walker).x, 0.0f) << "it stays in the west room";
    scene->getComponent<NavMeshModifierComponent>(door).enabled = false;
    ticks = 0;
    while (handle.getPathStatus() != nav::PathStatus::Complete && ticks < 60) {
        step();
        ticks++;
    }
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Complete) << "a partial path asks again when an area opens";
    steps(600);
    EXPECT_GT(position(walker).x, 10.0f) << "it walked through";
}

// AC12 (R11, D4): a runtime volume's boundary splits the polygons: every polygon (a fan of
// triangles here) lies inside the volume's footprint or outside it (one cell of slack), and the
// stored areas stay 0..15
TEST_F(NavAreaOverrideTest, BakeBoundaryAlignsPolys){
    Doors fixture = doors(*scene);
    doorway(*scene, fixture, false);
    bake(fixture.surface);
    nav::NavDebugGeometry geometry;
    ASSERT_TRUE(nav->getDebugGeometry(fixture.surface, nullptr, 1u << 20, false, geometry));
    const float slack = 0.2f;
    const float x0 = fixture.doorCenter.x - fixture.doorSize.x * 0.5f, x1 = fixture.doorCenter.x + fixture.doorSize.x * 0.5f;
    const float z0 = fixture.doorCenter.z - fixture.doorSize.z * 0.5f, z1 = fixture.doorCenter.z + fixture.doorSize.z * 0.5f;
    int inside = 0, crossing = 0;
    for (size_t t = 0; t < geometry.triangleAreas.size(); t++) {
        bool in = false, out = false;
        for (int k = 0; k < 3; k++) {
            const float* v = &geometry.triangles[t * 9 + (size_t)k * 3];
            if (v[0] > x0 + slack && v[0] < x1 - slack && v[2] > z0 + slack && v[2] < z1 - slack)
                in = true;
            else if (v[0] < x0 - slack || v[0] > x1 + slack || v[2] < z0 - slack || v[2] > z1 + slack)
                out = true;
        }
        inside += in && !out;
        crossing += in && out;
        EXPECT_LT(geometry.triangleAreas[t], nav::AREA_COUNT);
    }
    EXPECT_GT(inside, 0) << "polygons inside the doorway";
    EXPECT_EQ(crossing, 0) << "no polygon crosses the volume's boundary";
    // the doorway keeps its baked area (Walkable) while the modifier is disabled
    nav::NavHit hit;
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));
    EXPECT_EQ(hit.area, nav::AREA_WALKABLE);
}

// AC14 (R14): with the door closed, every operation that adds or rebuilds its tiles leaves it
// closed: an incremental bake, a carving rebuild, updateLink, a full bake, setNavMeshData
TEST_F(NavAreaOverrideTest, SurvivesTileSwap){
    Doors fixture = doors(*scene);
    scene->getComponent<NavMeshSurfaceComponent>(fixture.surface).runtimeCarving = true;
    const Entity door = doorway(*scene, fixture, true);
    // a link inside a door tile (west of the wall, north of the gap)
    const Entity hop = transformEntity(*scene, Vector3(-3.0f, 0.0f, 4.0f), NULL_ENTITY, "Hop");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(0.0f, 0.0f, 0.0f);
    link.endPoint = Vector3(1.0f, 0.0f, 1.0f);
    scene->addComponent<NavMeshLinkComponent>(hop, link);
    bake(fixture.surface);
    nav->syncAreaOverrides();
    ASSERT_FALSE(doorOpen(nav, fixture));

    // an incremental bake of the door's tiles
    std::string error;
    ASSERT_TRUE(nav->updateTiles(fixture.surface, AABB(Vector3(-2.0f, -1.0f, -2.0f), Vector3(2.0f, 3.0f, 2.0f)), error)) << error;
    while (nav->isBaking(fixture.surface))
        nav->processBakes(10.0);
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after an incremental bake";

    // a carving obstacle in a door tile: its rebuild
    const Entity crate = transformEntity(*scene, Vector3(-3.0f, 0.5f, -4.0f), NULL_ENTITY, "Crate");
    NavMeshObstacleComponent obstacle;
    obstacle.carve = true;
    obstacle.carveOnlyStationary = false;
    scene->addComponent<NavMeshObstacleComponent>(crate, obstacle);
    // its tile is rebuilt: a new tile reference (in the tile state's header)
    const std::vector<uint8_t> before = nav->getNavMesh(fixture.surface)->getTileStateBytes(-1, -1);
    steps(3);
    EXPECT_NE(nav->getNavMesh(fixture.surface)->getTileStateBytes(-1, -1), before) << "the obstacle carved its tile";
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after a carving rebuild";

    // the link moved: its tiles rebuilt
    scene->getComponent<NavMeshLinkComponent>(hop).endPoint = Vector3(1.5f, 0.0f, 1.0f);
    EXPECT_TRUE(nav->updateLink(hop));
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after updateLink";

    // a full bake: a new navmesh
    const nav::NavMesh* old = nav->getNavMesh(fixture.surface);
    bake(fixture.surface, NavBakeMode::FULL);
    EXPECT_NE(nav->getNavMesh(fixture.surface), old);
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after a full bake";

    // the same data loaded again
    const std::vector<uint8_t> file = nav->getNavMeshFile(fixture.surface);
    nav::NavMeshData data;
    ASSERT_TRUE(nav::NavMeshFile::read(file.data(), file.size(), data, error)) << error;
    ASSERT_TRUE(nav->setNavMeshData(fixture.surface, std::move(data), error)) << error;
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after setNavMeshData";

    // and opened, it opens
    scene->getComponent<NavMeshModifierComponent>(door).enabled = false;
    nav->syncAreaOverrides();
    EXPECT_TRUE(doorOpen(nav, fixture));
}

// AC14 (R14) on a static surface: an incremental bake and updateLink
TEST_F(NavAreaOverrideTest, SurvivesStaticTileSwap){
    Doors fixture = doors(*scene);
    const Entity door = doorway(*scene, fixture, true);
    const Entity hop = transformEntity(*scene, Vector3(-3.0f, 0.0f, 4.0f), NULL_ENTITY, "Hop");
    NavMeshLinkComponent link;
    link.startPoint = Vector3(0.0f, 0.0f, 0.0f);
    link.endPoint = Vector3(1.0f, 0.0f, 1.0f);
    scene->addComponent<NavMeshLinkComponent>(hop, link);
    bake(fixture.surface);
    nav->syncAreaOverrides();
    ASSERT_FALSE(doorOpen(nav, fixture));
    std::string error;
    ASSERT_TRUE(nav->updateTiles(fixture.surface, AABB(Vector3(-2.0f, -1.0f, -2.0f), Vector3(2.0f, 3.0f, 2.0f)), error)) << error;
    while (nav->isBaking(fixture.surface))
        nav->processBakes(10.0);
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after an incremental bake";
    scene->getComponent<NavMeshLinkComponent>(hop).endPoint = Vector3(1.5f, 0.0f, 1.0f);
    EXPECT_TRUE(nav->updateLink(hop));
    EXPECT_FALSE(doorOpen(nav, fixture)) << "after updateLink";
    // opened after the swaps: every tile is its baked self again
    scene->getComponent<NavMeshModifierComponent>(door).enabled = false;
    nav->syncAreaOverrides();
    EXPECT_TRUE(doorOpen(nav, fixture));
    EXPECT_EQ(nav->getNavMesh(fixture.surface)->getPolyStateTileCount(), 0u);
}

// AC15 (R12, R13): script volumes: Not Walkable blocks and wins over a later Walkable one, of two
// others the later area wins, removing restores, and the handles end with the system
TEST_F(NavAreaOverrideTest, ScriptVolumeHandle){
    Doors fixture = doors(*scene);
    const Entity door = doorway(*scene, fixture, false);
    bake(fixture.surface);
    const Quaternion none;
    const uint32_t shut = nav->setAreaInVolume(fixture.doorCenter, fixture.doorSize, none, nav::AREA_NOT_WALKABLE);
    ASSERT_GT(shut, 0u);
    EXPECT_FALSE(doorOpen(nav, fixture));
    const uint32_t walk = nav->setAreaInVolume(fixture.doorCenter, fixture.doorSize, none, nav::AREA_WALKABLE);
    ASSERT_GT(walk, shut);
    EXPECT_FALSE(doorOpen(nav, fixture)) << "Not Walkable always wins";
    EXPECT_TRUE(nav->removeAreaOverride(shut));
    EXPECT_FALSE(nav->hasAreaOverride(shut));
    EXPECT_TRUE(doorOpen(nav, fixture));

    // two areas: the later handle wins, then the earlier once the later goes
    const uint32_t mud = nav->setAreaInVolume(fixture.doorCenter, fixture.doorSize, none, 3);
    const uint32_t ice = nav->setAreaInVolume(fixture.doorCenter, fixture.doorSize, none, 4);
    nav::NavHit hit;
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));
    EXPECT_EQ(hit.area, 4);
    EXPECT_GT(nav->getAreaOverridePolygons(ice), 0);
    EXPECT_TRUE(nav->removeAreaOverride(ice));
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));
    EXPECT_EQ(hit.area, 3);
    // moved away and back
    EXPECT_TRUE(nav->updateAreaOverride(mud, Vector3(30.0f, 0.0f, 30.0f), fixture.doorSize, none, 3));
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));
    EXPECT_EQ(hit.area, nav::AREA_WALKABLE);
    EXPECT_TRUE(nav->updateAreaOverride(mud, fixture.doorCenter, fixture.doorSize, none, nav::AREA_NOT_WALKABLE));
    EXPECT_FALSE(doorOpen(nav, fixture));
    // a modifier over a script volume: modifiers first, so the script volume's area wins
    scene->getComponent<NavMeshModifierComponent>(door).area = 4;
    scene->getComponent<NavMeshModifierComponent>(door).enabled = true;
    EXPECT_TRUE(nav->updateAreaOverride(mud, fixture.doorCenter, fixture.doorSize, none, 3));
    nav->syncAreaOverrides();
    ASSERT_TRUE(nav->samplePosition(Vector3(0.0f, 0.0f, 0.0f), 0.3f, hit));
    EXPECT_EQ(hit.area, 3);
    // an area outside 0-15 is refused, with a warning
    test::LogCapture log;
    EXPECT_EQ(nav->setAreaInVolume(fixture.doorCenter, fixture.doorSize, none, nav::AREA_COUNT), 0u);
    EXPECT_EQ(log.count(D_LOG_WARN, "setAreaInVolume"), 1u);

    // Stop: the handles end
    nav->destroy();
    EXPECT_FALSE(nav->hasAreaOverride(mud));
    EXPECT_FALSE(nav->hasAreaOverride(walk));
}

// AC16 (R15): a corridor that crosses the door many polygons ahead (a field of pillars between)
// asks again the tick after the door closes; dtCrowd's own check looks 10 polygons ahead only
TEST_F(NavAreaOverrideTest, ReplanCrossingWholeCorridor){
    Doors fixture = doors(*scene);
    // pillars across the west room: many small polygons between the agent and the door
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 4; j++)
            box(*scene, Vector3(-17.0f + 2.2f * (float)i, 0.5f, -4.5f + 3.0f * (float)j), Vector3(0.6f, 1.0f, 0.6f));
    }
    const Entity door = doorway(*scene, fixture, false);
    bake(fixture.surface);
    const Entity walker = agent(*scene, Vector3(-19.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(fixture.east));
    steps(20);
    ASSERT_EQ(handle.getPathStatus(), nav::PathStatus::Complete);
    ASSERT_FALSE(handle.isPathPending());
    ASSERT_LT(position(walker).x, -14.0f) << "still far from the door";

    scene->getComponent<NavMeshModifierComponent>(door).enabled = true;
    step();
    EXPECT_TRUE(handle.isPathPending() || handle.getPathStatus() == nav::PathStatus::Partial)
        << "the tick after the close it has asked again";
    int ticks = 1;
    while (handle.getPathStatus() != nav::PathStatus::Partial && ticks < 60) {
        step();
        ticks++;
    }
    EXPECT_EQ(handle.getPathStatus(), nav::PathStatus::Partial) << "within " << ticks << " ticks, not after a second";
}

// AC17 (R16, R17): a deactivated bridge is ignored by paths (and is no bake input), an agent already
// on it finishes, and Occupied holds exactly between the link events
TEST_F(NavAreaOverrideTest, LinkActivated){
    Doors fixture = doors(*scene);
    bake(fixture.surface);
    ASSERT_EQ(nav->calculatePath(fixture.east, fixture.island).status, nav::PathStatus::Complete);
    const int dirty = nav->countDirtyTiles(fixture.surface);
    NavMeshLinkComponent& bridge = scene->getComponent<NavMeshLinkComponent>(fixture.bridge);
    bridge.activated = false;
    nav->syncAreaOverrides();
    EXPECT_FALSE(nav->getNavMesh(fixture.surface)->isLinkActive((uint32_t)fixture.bridge));
    EXPECT_EQ(nav->calculatePath(fixture.east, fixture.island).status, nav::PathStatus::Partial);
    EXPECT_EQ(nav->countDirtyTiles(fixture.surface), dirty) << "Activated is not a bake input";
    bridge.activated = true;
    nav->syncAreaOverrides();
    EXPECT_EQ(nav->calculatePath(fixture.east, fixture.island).status, nav::PathStatus::Complete);
    EXPECT_EQ(nav->getNavMesh(fixture.surface)->getPolyStateTileCount(), 0u);

    // an agent crossing: Occupied follows its link events; deactivated mid-way, it finishes
    const Entity walker = agent(*scene, Vector3(16.0f, 0.0f, 0.0f));
    step();
    NavMeshAgent handle(scene.get(), walker);
    ASSERT_TRUE(handle.setDestination(fixture.island));
    bool crossed = false;
    for (int tick = 0; tick < 900; tick++) {
        step();
        EXPECT_EQ(nav->isLinkOccupied(fixture.bridge), handle.isOnOffMeshLink()) << "tick " << tick;
        if (handle.isOnOffMeshLink() && !crossed) {
            crossed = true;
            scene->getComponent<NavMeshLinkComponent>(fixture.bridge).activated = false;
        }
        if (crossed && !handle.isOnOffMeshLink())
            break;
    }
    ASSERT_TRUE(crossed) << "it reached the bridge";
    steps(120);
    EXPECT_GT(position(walker).x, 23.0f) << "it finished the crossing";
    EXPECT_FALSE(nav->isLinkOccupied(fixture.bridge));
    EXPECT_EQ(nav->calculatePath(fixture.east, fixture.island).status, nav::PathStatus::Partial) << "still deactivated";
}

// AC18 (R18): without overrides and with every link activated nothing is stored, a tick allocates
// nothing for them, and a tile swap neither
TEST_F(NavAreaOverrideTest, ZeroCostWithoutOverrides){
    Doors fixture = doors(*scene);
    bake(fixture.surface);
    std::vector<Entity> walkers;
    for (int i = 0; i < 50; i++) {
        const float x = -18.0f + 0.9f * (float)(i % 20), z = -4.0f + 3.0f * (float)(i / 20);
        walkers.push_back(agent(*scene, Vector3(x, 0.0f, z)));
    }
    step();
    for (size_t i = 0; i < walkers.size(); i++)
        NavMeshAgent(scene.get(), walkers[i]).setDestination(Vector3(-10.0f + (float)(i % 5), 0.0f, -3.0f + (float)(i % 3)));
    steps(60);
    if (testsupport::countsAllocations()) {
        // the area sync's share of a tick: nothing
        testsupport::ThreadAllocationCount allocations;
        for (int i = 0; i < 600; i++)
            nav->syncAreaOverrides();
        EXPECT_EQ(allocations.count(), 0u);
    }
    steps(600);
    const nav::NavMesh* mesh = nav->getNavMesh(fixture.surface);
    EXPECT_EQ(mesh->getPolyStateTileCount(), 0u);
    std::string error;
    ASSERT_TRUE(nav->updateTiles(fixture.surface, AABB(Vector3(-2.0f, -1.0f, -2.0f), Vector3(2.0f, 3.0f, 2.0f)), error)) << error;
    while (nav->isBaking(fixture.surface))
        nav->processBakes(10.0);
    EXPECT_EQ(nav->getNavMesh(fixture.surface)->getPolyStateTileCount(), 0u);
    EXPECT_FALSE(nav->syncAreaOverrides());
}
