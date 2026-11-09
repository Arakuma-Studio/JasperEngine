// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5's foliage on a Terrain World (T4 R27, AC38; T5 F11): one region per tile in memory, made and
// dropped with residency, none for a world with nothing to grow, detail cells on the drawn surface and
// across seams as one map gives them, and each tile's own tree file. No GPU: tiles stop in memory.

#include "FoliageTestSupport.h"
#include "TerrainWorldTestSupport.h"

#include "component/MeshComponent.h"
#include "component/TerrainWorldComponent.h"
#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#endif
#include "jobs/JobSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/FoliageScatter.h"
#include "terrain/world/TerrainWorldData.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <thread>

using namespace terrainworldtest;
using jasperengine::Camera;
using jasperengine::Entity;
using jasperengine::FoliageInstanceInfo;
using jasperengine::FoliageSystem;
using jasperengine::Scene;
using jasperengine::TerrainComponent;
using jasperengine::TerrainFoliageLayer;
using jasperengine::TerrainTreePrototype;
using jasperengine::TerrainWorldComponent;
using jasperengine::TerrainWorldRuntime;
using jasperengine::TerrainWorldSystem;
using jasperengine::Transform;
using jasperengine::Vector3;
using jasperengine::terrain::TreeId;

namespace {

    constexpr int GROVE_N = 64;             // S = 64 m at 1 m: eight 8 m cells a tile side
    constexpr float GROVE_MAX_HEIGHT = 50.0f;
    constexpr uint16_t GROVE_PROTOTYPE = 5;

    void writeGroveFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A tile's own tree file: whole-metre world points, stored as T5 stores them over the tile's square
    // (its region is centred on the tile), at the fixture's height there, as a paint snaps them
    std::vector<uint8_t> groveTrees(TileCoord tile, const std::vector<std::pair<int, int>>& points) {
        jasperengine::terrain::TreeStore store;
        std::vector<TreeRecord> added;
        for (const auto& point : points) {
            TreeRecord record{};
            record.u = (static_cast<float>(point.first - tile.x * GROVE_N)) / static_cast<float>(GROVE_N);
            record.v = (static_cast<float>(point.second - tile.z * GROVE_N)) / static_cast<float>(GROVE_N);
            record.height = static_cast<float>(fixtureSample(point.first, point.second)) / 65535.0f;
            record.scale = 1.0f;
            record.rotation[3] = 32767;
            record.prototypeId = GROVE_PROTOTYPE;
            record.heightRatio = 128;
            record.flags = TREE_SNAP_TO_TERRAIN;
            std::memset(record.color, 255, 4);
            added.push_back(record);
        }
        store.apply({}, added, {}, nullptr);
        std::vector<uint8_t> bytes;
        store.encode(bytes);
        return bytes;
    }

    // tiles low..high a side of the fixture's surface, each with a full density map for layer 0,
    // cooked as an export writes them; trees: tile keys with a .jetrees and its points
    void writeGroveWorld(const std::filesystem::path& folder, int low, int high, bool density,
                          const std::map<uint32_t, std::vector<std::pair<int, int>>>& trees = {}) {
        WorldManifest manifest;
        manifest.guid = "99992222333344445555666677778888";
        manifest.lattice = smallLattice(GROVE_N, 1.0f);
        manifest.maps.maxHeight = GROVE_MAX_HEIGHT;
        manifest.maps.densityMaps = density ? 1 : 0;
        WorldOverview overview;
        initOverview(overview, manifest.lattice, TileCoord(low, low), TileCoord(high, high), 0);
        for (int z = low; z <= high; z++) {
            for (int x = low; x <= high; x++) {
                auto tile = fixtureTile(manifest.lattice, TileCoord(x, z));
                if (density) {
                    tile->density.resize(1);
                    tile->density[0].allocate(manifest.lattice.densitySide(), 1, 8, 255);
                }
                const float colours[] = {0.5f, 0.5f, 0.5f, 1.0f};
                buildOverviewColour(*tile, manifest.lattice, colours, 0);
                std::vector<uint8_t> bytes;
                ASSERT_TRUE(encodeTile(*tile, zstdCodec(), TileEncodeOptions{}, bytes));
                writeGroveFile(folder / ("tiles/" + std::to_string(x) + "_" + std::to_string(z) + ".jtile"), bytes);
                setOverviewPatch(overview, *tile);
                const TileMinMax whole = tileNodeMinMax(*tile, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
                WorldTileInfo info{TileCoord(x, z), whole.min, whole.max};
                const auto withTrees = trees.find(TileCoord(x, z).key());
                if (withTrees != trees.end()) {
                    writeGroveFile(folder / ("tiles/" + std::to_string(x) + "_" + std::to_string(z) + ".jetrees"), groveTrees(TileCoord(x, z), withTrees->second));
                    info.trees = true;
                }
                manifest.tiles.push_back(info);
            }
        }
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(encodeOverview(overview, manifest.lattice, zstdCodec(), 3, bytes, nullptr));
        writeGroveFile(folder / "overview.jtile", bytes);
        encodeWorldBin(manifest, bytes);
        writeGroveFile(folder / "world.bin", bytes);
    }

    struct GroveWorld {
        jasperengine::test::TempDir files;
        std::unique_ptr<Scene> scene;
        Camera* camera = nullptr;
        Entity entity = NULL_ENTITY;
        FoliageSystem* foliage = nullptr;
        TerrainWorldRuntime* runtime = nullptr;

        GroveWorld(int low, int high, bool layer, bool density = true, const std::map<uint32_t, std::vector<std::pair<int, int>>>& trees = {}) {
            writeGroveWorld(files.path() / "Grove.terrainworld", low, high, density, trees);
            scene = std::make_unique<Scene>();
            camera = new Camera(scene.get());
            scene->setCamera(camera->getEntity());
            entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            scene->addComponent<jasperengine::MeshComponent>(entity, {});
            scene->addComponent<TerrainComponent>(entity, {});
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            if (layer) {
                TerrainFoliageLayer grass;
                grass.meshPath = jasperengine::AssetRef(foliagetest::crossMesh());
                grass.density = 0.5f;
                grass.maxSlope = 90.0f;
                grass.drawDistance = 100.0f;
                grass.seed = 7;
                terrain.foliageLayers.push_back(grass);
            }
            if (!trees.empty()) {
                TerrainTreePrototype prototype;
                prototype.id = GROVE_PROTOTYPE;
                prototype.meshPath = jasperengine::AssetRef(foliagetest::crossMesh());
                terrain.treePrototypes.push_back(prototype);
            }
            TerrainWorldComponent world;
            world.worldData.path = (files.path() / "Grove.terrainworld").generic_string();
            world.useCameraFocus = false;
            // the near band (2 S x LOD Distance): a focus at a tile's centre wants it and its neighbours
            world.lodDistance = 0.5f;
            // and collision wants no more than that
            world.collisionRadius = 1.0f;
            scene->addComponent<TerrainWorldComponent>(entity, world);
            foliage = scene->getSystem<FoliageSystem>().get();
            std::string error;
            runtime = scene->getSystem<TerrainWorldSystem>()->ensureRuntime(entity, &error);
            EXPECT_NE(runtime, nullptr) << error;
            // a player with no GPU: tiles stop in memory (R15), where foliage starts
            if (runtime) runtime->tiles().budgets().gpu = false;
        }

        ~GroveWorld() {
            scene.reset();
            delete camera;
        }

        void setCamera(float x, float z) {
            Transform& transform = scene->getComponent<Transform>(camera->getEntity());
            transform.worldPosition = Vector3(x, 10.0f, z);
        }

        void frame() {
            if (jasperengine::jobs::hasSystem() && jasperengine::jobs::system().isSingleThreaded()) jasperengine::jobs::system().pump(4.0);
            scene->getSystem<TerrainWorldSystem>()->pump(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
            foliage->update(0.016);
        }

        // frames until the condition holds and foliage has nothing pending; false at the timeout
        template<typename F>
        bool settle(F&& condition, double seconds = 60.0) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < until) {
                frame();
                if (condition() && !foliage->hasPendingUpdates()) return true;
                foliage->waitForJobs();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }

        // the tiles whose foliage may run: in memory, as a GPU-less player keeps them
        std::set<uint32_t> readyTiles(int low, int high) const {
            std::set<uint32_t> keys;
            for (int z = low; z <= high; z++) {
                for (int x = low; x <= high; x++) {
                    const TileCoord tile(x, z);
                    if (runtime->tileState(tile) == TileState::CpuReady && runtime->resident(tile)) keys.insert(tile.key());
                }
            }
            return keys;
        }

        std::vector<FoliageInstanceInfo> details() const { return foliage->getInstancesInRadius(entity, Vector3(0.0f, 0.0f, 0.0f), 2000.0f, 1, 10000000); }
        std::vector<FoliageInstanceInfo> trees() const { return foliage->getInstancesInRadius(entity, Vector3(0.0f, 0.0f, 0.0f), 2000.0f, 0, 10000000); }
    };

    uint32_t groveTileKey(const Vector3& position) {
        return TileCoord(static_cast<int>(std::floor(position.x / GROVE_N)), static_cast<int>(std::floor(position.z / GROVE_N))).key();
    }

}

TEST(TerrainWorldFoliage, RegionPerResidentTile) {
    // 6 x 6 tiles of 64 m; the near band is 64 m, so a focus at a tile's centre holds it and the tiles
    // within 64 m of the point
    GroveWorld world(-3, 2, true);
    ASSERT_TRUE(world.runtime);
    ASSERT_TRUE(world.settle([&] { return world.runtime->isReady(); }));

    world.setCamera(-160.0f, -160.0f);
    const int focus = world.runtime->addFocus(-160.0f, -160.0f);
    ASSERT_TRUE(world.settle([&] {
        const std::set<uint32_t> now = world.readyTiles(-3, 2);
        return now.count(TileCoord(-2, -2).key()) && world.foliage->getRegionCount(world.entity) == now.size();
    })) << world.readyTiles(-3, 2).size() << " tiles in memory, " << world.foliage->getRegionCount(world.entity) << " regions";
    const std::set<uint32_t> first = world.readyTiles(-3, 2);
    EXPECT_TRUE(first.count(TileCoord(-3, -3).key()));
    EXPECT_FALSE(first.count(TileCoord(1, 1).key()));

    std::vector<FoliageInstanceInfo> placed = world.details();
    ASSERT_GT(placed.size(), 100u);
    std::set<uint32_t> grown;
    for (const FoliageInstanceInfo& instance : placed) {
        // resident ground only, on the surface the world answers
        ASSERT_TRUE(first.count(groveTileKey(instance.position))) << instance.position.x << ", " << instance.position.z;
        grown.insert(groveTileKey(instance.position));
        const float surface = world.runtime->query(instance.position.x, instance.position.z, false).height;
        ASSERT_NEAR(instance.position.y, surface, 1e-3f) << instance.position.x << ", " << instance.position.z;
    }
    EXPECT_EQ(grown, first);
    EXPECT_EQ(world.foliage->getStats(world.entity).detailResident, placed.size());

    // the focus moves: the old tiles leave memory (a cap of nothing keeps only wanted ones) and their
    // regions go with them; the new tiles get theirs
    world.runtime->tiles().budgets().cpuCapBytes = 1;
    world.setCamera(96.0f, 96.0f);
    ASSERT_TRUE(world.runtime->setFocus(focus, 96.0f, 96.0f));
    ASSERT_TRUE(world.settle([&] {
        const std::set<uint32_t> now = world.readyTiles(-3, 2);
        return now.count(TileCoord(1, 1).key()) && !now.count(TileCoord(-3, -3).key()) && world.foliage->getRegionCount(world.entity) == now.size();
    })) << world.readyTiles(-3, 2).size() << " tiles in memory, " << world.foliage->getRegionCount(world.entity) << " regions";
    const std::set<uint32_t> second = world.readyTiles(-3, 2);
    placed = world.details();
    ASSERT_GT(placed.size(), 100u);
    for (const FoliageInstanceInfo& instance : placed) {
        ASSERT_TRUE(second.count(groveTileKey(instance.position))) << instance.position.x << ", " << instance.position.z;
    }
}

TEST(TerrainWorldFoliage, NoLayersMakesNoRegions) {
    // R37: a world with nothing to grow costs FoliageSystem nothing
    GroveWorld world(-1, 0, false);
    ASSERT_TRUE(world.runtime);
    world.setCamera(0.0f, 0.0f);
    world.runtime->addFocus(0.0f, 0.0f);
    ASSERT_TRUE(world.settle([&] { return world.readyTiles(-1, 0).size() == 4; }));
    for (int i = 0; i < 10; i++) world.frame();
    EXPECT_EQ(world.foliage->getRegionCount(world.entity), 0u);
    EXPECT_FALSE(world.foliage->hasDrawables());
}

TEST(TerrainWorldFoliage, SeamCellsMatchOneMap) {
    // 2 x 2 tiles round the origin, all in memory: the cells on both sides of each seam hold what one
    // map over the same samples places, by T5's hash with world-continuous seeds, on the same heights
    GroveWorld world(-1, 0, true);
    ASSERT_TRUE(world.runtime);
    world.setCamera(0.0f, 0.0f);
    world.runtime->addFocus(0.0f, 0.0f);
    ASSERT_TRUE(world.settle([&] { return world.readyTiles(-1, 0).size() == 4 && world.foliage->getRegionCount(world.entity) == 4; }));

    // one region of 128 m centred on the world's origin: its gutter-wide map of the fixture's samples
    const int side = 2 * GROVE_N + 3;
    std::vector<uint8_t> heights(static_cast<size_t>(side) * side * 2);
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            const uint16_t value = fixtureSample(-GROVE_N + i - 1, -GROVE_N + j - 1);
            heights[(static_cast<size_t>(j) * side + i) * 2] = static_cast<uint8_t>(value & 0xFF);
            heights[(static_cast<size_t>(j) * side + i) * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    const std::vector<uint8_t> density(static_cast<size_t>(2 * GROVE_N) * 2 * GROVE_N, 255);
    std::map<std::pair<int, int>, std::vector<Vector3>> expected;
    for (int cz = -GROVE_N / 4; cz < GROVE_N / 4; cz++) {
        for (int cx = -GROVE_N / 4; cx < GROVE_N / 4; cx++) {
            FoliageCellInput input;
            input.layer.seed = 7;
            input.layer.density = 0.5f;
            input.layer.maxSlope = 90.0f;
            input.cellX = cx;
            input.cellZ = cz;
            input.terrainSize = 2.0f * GROVE_N;
            input.heightMapSize = static_cast<float>(side);
            input.terrainMaxHeight = GROVE_MAX_HEIGHT;
            input.heightMap = FoliageMapPatch{heights.data(), side, side, 0, 0, side, side, 1, 2};
            input.densityMap = FoliageMapPatch{density.data(), 2 * GROVE_N, 2 * GROVE_N, 0, 0, 2 * GROVE_N, 2 * GROVE_N, 1, 1};
            std::vector<FoliageInstance> cell;
            scatterFoliageCell(input, cell);
            for (const FoliageInstance& instance : cell) expected[{cx, cz}].push_back(Vector3(instance.position[0], instance.position[1], instance.position[2]));
        }
    }

    std::map<std::pair<int, int>, std::vector<Vector3>> actual;
    for (const FoliageInstanceInfo& instance : world.details()) {
        const int cx = static_cast<int>(std::floor(instance.position.x / FOLIAGE_DETAIL_CELL_SIZE));
        const int cz = static_cast<int>(std::floor(instance.position.z / FOLIAGE_DETAIL_CELL_SIZE));
        actual[{cx, cz}].push_back(instance.position);
    }

    // every cell, the ones beside the seams at x = 0 and z = 0 among them, fully populated
    size_t compared = 0;
    for (const auto& [cell, positions] : expected) {
        const std::vector<Vector3>& got = actual[cell];
        ASSERT_EQ(got.size(), positions.size()) << "cell " << cell.first << ", " << cell.second;
        std::vector<Vector3> sortedGot = got, sortedWant = positions;
        const auto byXZ = [](const Vector3& a, const Vector3& b) { return a.x != b.x ? a.x < b.x : a.z < b.z; };
        std::sort(sortedGot.begin(), sortedGot.end(), byXZ);
        std::sort(sortedWant.begin(), sortedWant.end(), byXZ);
        for (size_t i = 0; i < sortedGot.size(); i++) {
            ASSERT_NEAR(sortedGot[i].x, sortedWant[i].x, 1e-4f);
            ASSERT_NEAR(sortedGot[i].z, sortedWant[i].z, 1e-4f);
            ASSERT_NEAR(sortedGot[i].y, sortedWant[i].y, 1e-3f) << "cell " << cell.first << ", " << cell.second;
        }
        compared += got.size();
    }
    EXPECT_GT((actual[{-1, 0}].size()), 0u);
    EXPECT_GT((actual[{0, 0}].size()), 0u);
    EXPECT_EQ(compared, world.details().size());
}

TEST(TerrainWorldFoliage, TreeFilePerTile) {
    // tile (0, 0) has its own .jetrees; tile (-1, 0) has none
    const std::map<uint32_t, std::vector<std::pair<int, int>>> trees = {{TileCoord(0, 0).key(), {{22, 37}, {44, 12}, {32, 32}}}};
    GroveWorld world(-1, 0, false, false, trees);
    ASSERT_TRUE(world.runtime);
    world.scene->getComponent<TerrainComponent>(world.entity).treeDistance = 500.0f;
    world.setCamera(0.0f, 0.0f);
    world.runtime->addFocus(0.0f, 0.0f);
    ASSERT_TRUE(world.settle([&] { return world.readyTiles(-1, 0).size() == 4 && world.trees().size() == 3; }));

    EXPECT_EQ(world.foliage->getStats(world.entity).treesTotal, 3u);
    size_t stores = 0;
    world.foliage->forEachTreeStore(world.entity, [&](const jasperengine::terrain::TreeStore& store) {
        stores++;
        EXPECT_EQ(store.size(), 3u);
    });
    EXPECT_EQ(stores, 1u);
    std::set<std::pair<int, int>> where;
    for (const FoliageInstanceInfo& tree : world.trees()) {
        EXPECT_EQ(groveTileKey(tree.position), TileCoord(0, 0).key());
        // snapped to the world's surface at the tile's own position
        EXPECT_NEAR(tree.position.y, world.runtime->query(tree.position.x, tree.position.z, false).height, 1e-3f);
        where.insert({static_cast<int>(std::lround(tree.position.x)), static_cast<int>(std::lround(tree.position.z))});
    }
    EXPECT_EQ(where, (std::set<std::pair<int, int>>{{22, 37}, {44, 12}, {32, 32}}));
}

TEST(TerrainWorldFoliage, RemovedHandlerCoversEveryTile) {
    // trees in two tiles, neither of them tile (0, 0) (key 0), and the handler set before any tile is
    // in memory: one handler for the World serves every tile, those that arrive later too
    const std::map<uint32_t, std::vector<std::pair<int, int>>> trees = {{TileCoord(-1, -1).key(), {{-40, -30}}},
                                                                        {TileCoord(0, -1).key(), {{20, -20}, {40, -50}}}};
    GroveWorld world(-1, 0, false, false, trees);
    ASSERT_TRUE(world.runtime);
    world.scene->getComponent<TerrainComponent>(world.entity).treeDistance = 500.0f;
    std::vector<TreeId> heard;
    world.foliage->setTreeRemovedHandler(world.entity, [&](TreeId id, const FoliageInstanceInfo&) { heard.push_back(id); });
    world.setCamera(0.0f, 0.0f);
    world.runtime->addFocus(0.0f, 0.0f);
    ASSERT_TRUE(world.settle([&] { return world.readyTiles(-1, 0).size() == 4 && world.trees().size() == 3; }));

    std::set<TreeId> removed;
    std::set<uint32_t> tiles;
    for (const FoliageInstanceInfo& tree : world.trees()) {
        ASSERT_TRUE(world.foliage->removeTreeInstance(world.entity, tree.id).has_value());
        removed.insert(tree.id);
        tiles.insert(jasperengine::terrain::treeIdRegion(tree.id));
    }
    EXPECT_EQ(tiles, (std::set<uint32_t>{TileCoord(-1, -1).key(), TileCoord(0, -1).key()}));
    EXPECT_EQ(std::set<TreeId>(heard.begin(), heard.end()), removed);
    EXPECT_EQ(heard.size(), 3u);
}

#ifdef JASPERENGINE_PHYSICS_3D
TEST(TerrainWorldFoliage, TreeHitHandlerOnAnyTile) {
    // a capsule tree in tile (-1, -1), struck by a ball: the World's handler hears it, and the capsule
    // stands where the tree is drawn (the tile's frame moved to the World's)
    const std::map<uint32_t, std::vector<std::pair<int, int>>> trees = {{TileCoord(-1, -1).key(), {{-32, -32}}}};
    GroveWorld world(-1, 0, false, false, trees);
    ASSERT_TRUE(world.runtime);
    TerrainComponent& terrain = world.scene->getComponent<TerrainComponent>(world.entity);
    terrain.treeDistance = 500.0f;
    terrain.treePrototypes[0].collision = jasperengine::TerrainTreeCollision::Capsule;
    terrain.treePrototypes[0].collisionRadius = 1.0f;
    terrain.treePrototypes[0].collisionHeight = 6.0f;
    int hits = 0;
    jasperengine::TreeHitEvent seen;
    world.foliage->setTreeHitHandler(world.entity, [&](const jasperengine::TreeHitEvent& hit) {
        hits++;
        seen = hit;
    });
    world.setCamera(0.0f, 0.0f);
    world.runtime->addFocus(0.0f, 0.0f);
    ASSERT_TRUE(world.settle([&] { return world.readyTiles(-1, 0).size() == 4 && world.trees().size() == 1; }));
    for (int i = 0; i < 4; i++) world.frame();

    const float ground = world.runtime->query(-32.0f, -32.0f, false).height;
    const Entity ball = world.scene->createEntity();
    Transform transform;
    transform.position = Vector3(-36.0f, ground + 3.0f, -32.0f);
    transform.worldPosition = transform.position;
    world.scene->addComponent<Transform>(ball, transform);
    jasperengine::RigidbodyComponent body;
    body.type = jasperengine::BodyType::DYNAMIC;
    body.gravityFactor = 0.0f;
    jasperengine::Collider3D collider;
    collider.type = jasperengine::Collider3DType::SPHERE;
    collider.radius = 0.5f;
    body.colliders.push_back(collider);
    body.hasPendingVelocity = true;
    body.pendingLinearVelocity = Vector3(3.0f, 0.0f, 0.0f);
    body.pendingAwake = true;
    world.scene->addComponent<jasperengine::RigidbodyComponent>(ball, body);
    for (int i = 0; i < 120; i++) world.scene->fixedUpdate(1.0f / 60.0f);

    EXPECT_EQ(hits, 1);
    EXPECT_EQ(jasperengine::terrain::treeIdRegion(seen.id), TileCoord(-1, -1).key());
    EXPECT_EQ(seen.body, ball);
    EXPECT_LT(world.scene->getComponent<Transform>(ball).worldPosition.x, -32.0f) << "the capsule stops it at the drawn tree";
}
#endif

TEST(TerrainWorldFoliage, RuntimeStateCoversEveryTile) {
    // 6 x 6 tiles; trees in (-3, -3) and (-2, -3). The state string lists every tile with an overlay in
    // key order, survives a tile leaving memory and coming back, and sets back what it read
    const std::map<uint32_t, std::vector<std::pair<int, int>>> trees = {{TileCoord(-3, -3).key(), {{-170, -170}, {-150, -180}}},
                                                                        {TileCoord(-2, -3).key(), {{-100, -170}}}};
    GroveWorld world(-3, 2, false, false, trees);
    ASSERT_TRUE(world.runtime);
    world.scene->getComponent<TerrainComponent>(world.entity).treeDistance = 2000.0f;
    world.setCamera(-160.0f, -160.0f);
    const int focus = world.runtime->addFocus(-160.0f, -160.0f);
    ASSERT_TRUE(world.settle([&] { return world.foliage->getRegionCount(world.entity) > 0 && world.trees().size() == 3; }));

    for (const FoliageInstanceInfo& tree : world.trees()) {
        if (std::lround(tree.position.x) != -150) ASSERT_TRUE(world.foliage->removeTreeInstance(world.entity, tree.id).has_value());
    }
    ASSERT_NE(world.foliage->addTreeInstance(world.entity, 0, Vector3(-130.0f, 0.0f, -130.0f), 30.0f, 1.5f), 0u);
    const std::string state = world.foliage->getRuntimeState(world.entity);
    ASSERT_EQ(state.rfind("JEFW1|", 0), 0u) << state;
    // (-3, -3), (-2, -3) and (-3, -2), in key order
    std::vector<uint32_t> keys;
    for (size_t at = state.find('|'); at != std::string::npos; at = state.find('|', at + 1)) keys.push_back(static_cast<uint32_t>(std::stoul(state.substr(at + 1))));
    EXPECT_EQ(keys, (std::vector<uint32_t>{TileCoord(-3, -3).key(), TileCoord(-3, -2).key(), TileCoord(-2, -3).key()}));
    EXPECT_FALSE(world.foliage->setRuntimeState(world.entity, "JEFS1;;;")) << "a lone Terrain's string on a World";

    // the focus leaves: the tiles go, their overlays wait, the string stays the same
    world.runtime->tiles().budgets().cpuCapBytes = 1;
    world.setCamera(96.0f, 96.0f);
    ASSERT_TRUE(world.runtime->setFocus(focus, 96.0f, 96.0f));
    ASSERT_TRUE(world.settle([&] { return !world.foliage->hasRegion(world.entity, TileCoord(-3, -3).key()); }));
    EXPECT_EQ(world.foliage->getRuntimeState(world.entity), state);

    // and back: the removed trees stay removed, the added one is there
    world.setCamera(-160.0f, -160.0f);
    ASSERT_TRUE(world.runtime->setFocus(focus, -160.0f, -160.0f));
    ASSERT_TRUE(world.settle([&] { return world.foliage->hasRegion(world.entity, TileCoord(-3, -3).key()) && world.trees().size() == 2; }));
    EXPECT_EQ(world.foliage->getRuntimeState(world.entity), state);

    // cleared, then set back from the string
    world.foliage->clearRuntimeState(world.entity);
    ASSERT_TRUE(world.settle([&] { return world.trees().size() == 3; }));
    EXPECT_EQ(world.foliage->getRuntimeState(world.entity), "JEFW1");
    ASSERT_TRUE(world.foliage->setRuntimeState(world.entity, state));
    ASSERT_TRUE(world.settle([&] { return world.trees().size() == 2; }));
    EXPECT_EQ(world.foliage->getRuntimeState(world.entity), state);
    EXPECT_FALSE(world.foliage->setRuntimeState(world.entity, "JEFW1|x;;;")) << "a bad tile key";
    EXPECT_EQ(world.foliage->getRuntimeState(world.entity), state) << "a refused string changes nothing";
}

TEST(TerrainWorldFoliage, TreeStoreUnderAPosition) {
    const std::map<uint32_t, std::vector<std::pair<int, int>>> trees = {{TileCoord(0, 0).key(), {{22, 37}, {44, 12}, {32, 32}}},
                                                                        {TileCoord(-1, -1).key(), {{-30, -30}}}};
    GroveWorld world(-1, 0, false, false, trees);
    ASSERT_TRUE(world.runtime);
    world.scene->getComponent<TerrainComponent>(world.entity).treeDistance = 500.0f;
    world.setCamera(0.0f, 0.0f);
    world.runtime->addFocus(0.0f, 0.0f);
    ASSERT_TRUE(world.settle([&] { return world.readyTiles(-1, 0).size() == 4 && world.trees().size() == 4; }));

    const auto ownTile = world.foliage->getTreeStore(world.entity, Vector3(30.0f, 0.0f, 30.0f));
    ASSERT_NE(ownTile, nullptr);
    EXPECT_EQ(ownTile->size(), 3u);
    EXPECT_EQ(ownTile, world.foliage->getRegionTreeStore(world.entity, TileCoord(0, 0).key()));
    const auto other = world.foliage->getTreeStore(world.entity, Vector3(-0.5f, 0.0f, -63.5f));
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->size(), 1u);
    // a border point is the higher tile's
    EXPECT_EQ(world.foliage->getTreeStore(world.entity, Vector3(0.0f, 0.0f, 0.0f)), ownTile);
    EXPECT_EQ(world.foliage->getTreeStore(world.entity, Vector3(-30.0f, 0.0f, 30.0f)), nullptr) << "a tile with no tree file";
    EXPECT_EQ(world.foliage->getTreeStore(world.entity, Vector3(500.0f, 0.0f, 500.0f)), nullptr) << "no tile there";
    // without a position a World names no store: tile (0, 0)'s key is the lone Terrain's
    EXPECT_EQ(world.foliage->getTreeStore(world.entity), nullptr);
    EXPECT_EQ(world.foliage->getOrCreateTreeStore(world.entity), nullptr);
}

TEST(TerrainWorldData, WorldBinCarriesTreeTiles) {
    // R27: which tiles have trees rides in world.bin's optional tail; a world without any keeps its bytes
    WorldManifest manifest;
    manifest.guid = "abc";
    manifest.lattice = smallLattice(64, 1.0f);
    manifest.tiles = {WorldTileInfo{TileCoord(0, 0), 1, 2}, WorldTileInfo{TileCoord(-3, 4), 3, 4}};
    std::vector<uint8_t> plain;
    encodeWorldBin(manifest, plain);
    manifest.tiles[1].trees = true;
    std::vector<uint8_t> withTrees;
    encodeWorldBin(manifest, withTrees);
    EXPECT_EQ(withTrees.size(), plain.size() + 8);

    WorldManifest decoded;
    std::string error;
    ASSERT_TRUE(decodeWorldBin(withTrees, decoded, &error)) << error;
    ASSERT_EQ(decoded.tiles.size(), 2u);
    EXPECT_FALSE(decoded.tiles[0].trees);
    EXPECT_TRUE(decoded.tiles[1].trees);
    ASSERT_TRUE(decodeWorldBin(plain, decoded, &error)) << error;
    EXPECT_FALSE(decoded.tiles[1].trees);
}
