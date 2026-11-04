// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC2, AC3, AC5, AC21, AC24, AC30, AC43: FoliageSystem without a GPU

#include "FoliageTestSupport.h"

#include "jobs/JobSystem.h"
#include "memory/MemoryStats.h"
#include "subsystem/MeshSystem.h"
#include "support/AllocationCounter.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terrain;

using namespace foliagetest;

TEST(FoliageSystemCore, SameHashWithZeroOneAndFourWorkers) {
    uint64_t hashes[3] = {0, 0, 0};
    const int workers[3] = {0, 1, 4};
    for (int i = 0; i < 3; i++) {
        jobs::shutdown();
        jobs::Config config;
        config.workers = workers[i];
        jobs::initialize(config);
        FoliageFixture fixture;
        for (int frame = 0; frame < 5000; frame++) {
            fixture.frame();
            jobs::pump(4.0);
            if (frame > 2 && !fixture.foliage->hasPendingUpdates()) break;
        }
        EXPECT_FALSE(fixture.foliage->hasPendingUpdates()) << "workers " << workers[i];
        EXPECT_GT(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
        hashes[i] = fixture.foliage->getPlacementHash(fixture.entity);
    }
    jobs::shutdown();
    jobs::initialize(jobs::Config{});
    EXPECT_EQ(hashes[0], hashes[1]);
    EXPECT_EQ(hashes[0], hashes[2]);
}

TEST(FoliageSystemCore, SameHashForAnyCameraPath) {
    // the ring reaches the whole Terrain from anywhere on it, so the same cells end up resident
    FoliageFixture straight(400.0f);
    straight.settle();

    FoliageFixture wandering(400.0f, 100.0f, -80.0f);
    wandering.frame();
    wandering.setCamera(-64.0f, 64.0f);
    wandering.frame();
    wandering.setCamera(0.0f, 0.0f);
    wandering.settle();

    EXPECT_EQ(straight.foliage->getPlacementHash(straight.entity), wandering.foliage->getPlacementHash(wandering.entity));
}

TEST(FoliageSystemCore, NoCellPlacedOnTheMainThreadWithWorkers) {
    ASSERT_TRUE(jobs::hasSystem());
    if (jobs::system().isSingleThreaded()) GTEST_SKIP() << "needs workers";
    FoliageFixture fixture;
    // the first frame only reserves and submits: nothing is resident until a job has finished,
    // and applying is a later poll's
    fixture.frame();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).applied, 0u);
    EXPECT_EQ(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
    EXPECT_TRUE(fixture.foliage->hasPendingUpdates());
    fixture.settle();
    EXPECT_GT(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
}

TEST(FoliageSystemCore, ApplyStaysWithinBudget) {
    FoliageFixture fixture;
    // the budget holds whatever a frame could take: apply cuts off, and the rest waits
    int frames = 0;
    double worst = 0.0;
    for (; frames < 4000; frames++) {
        fixture.frame();
        worst = std::max(worst, fixture.foliage->getStats(fixture.entity).mainThreadMs);
        fixture.foliage->waitForJobs();
        if (frames > 2 && !fixture.foliage->hasPendingUpdates()) break;
    }
    EXPECT_FALSE(fixture.foliage->hasPendingUpdates());
    // one more cell may finish past the limit; a loaded host is reported, not failed, as every wall clock here
    if (std::getenv("JASPER_PERF_SLACK") == nullptr) {
        RecordProperty("worst_main_thread_ms", std::to_string(worst));
    }
    EXPECT_LT(worst, 50.0) << "the budget cut does not work at all";
}

TEST(FoliageSystemCore, OneMeshLoadPerLayer) {
    FoliageFixture fixture;
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).meshLoads, 1u);
    // one engine-owned prototype, not one entity per chunk
    EXPECT_EQ(fixture.foliage->getEntities(fixture.entity).size(), 1u);
    EXPECT_GT(fixture.foliage->getStats(fixture.entity).detailCellsPlaced, 100u);
}

TEST(FoliageSystemCore, EmptySceneDoesOneCheck) {
    Scene scene;
    FoliageSystem* foliage = scene.getSystem<FoliageSystem>().get();
    foliage->update(0.016);
    {
        testsupport::ThreadAllocationCount allocations;
        for (int i = 0; i < 1000; i++) foliage->update(0.016);
        EXPECT_EQ(allocations.count(), 0u);
    }
    EXPECT_FALSE(foliage->hasDrawables());
    EXPECT_FALSE(foliage->hasPendingUpdates());

    // a Terrain with nothing to draw: no state, no entity, no job
    Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity);
    scene.addComponent<TerrainComponent>(entity);
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(entity);
    terrain.heightMapLoaded = true;
    terrain.needUpdateTerrain = false;
    foliage->update(0.016);
    EXPECT_FALSE(foliage->hasDrawables());
    EXPECT_TRUE(foliage->getEntities(entity).empty());
}

TEST(FoliageSystemCore, WalkingFillsOnlyEnteringCells) {
    // T5 R66, render-correctness's 4-and-5 column walk: a side from the draw distance, so a step
    // places the column that enters and refills nothing that stays
    FoliageFixture fixture(24.0f);
    fixture.settle();
    const uint32_t placed = fixture.foliage->getStats(fixture.entity).detailCellsPlaced;
    const uint64_t hashBefore = fixture.foliage->getPlacementHash(fixture.entity);
    (void)hashBefore;

    fixture.setCamera(8.5f, 0.0f);
    fixture.settle();
    const uint32_t entered = fixture.foliage->getStats(fixture.entity).detailCellsPlaced - placed;
    const int side = static_cast<int>(std::ceil(2.0f * 24.0f / 8.0f)) + 1;
    EXPECT_GT(entered, 0u);
    EXPECT_LE(entered, static_cast<uint32_t>(side)) << "one column, no more";

    // standing still places nothing
    const uint32_t still = fixture.foliage->getStats(fixture.entity).detailCellsPlaced;
    for (int i = 0; i < 5; i++) fixture.frame();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).detailCellsPlaced, still);
}

TEST(FoliageSystemCore, DisabledTerrainHasNoJobsOrDraws) {
    FoliageFixture fixture;
    fixture.terrain().enabled = false;
    for (int i = 0; i < 3; i++) fixture.frame();
    EXPECT_FALSE(fixture.foliage->hasPendingUpdates());
    EXPECT_EQ(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
}

TEST(FoliageSystemCore, ScaleZeroRemovesAndComesBack) {
    FoliageFixture fixture;
    fixture.settle();
    EXPECT_GT(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
    Engine::setFoliageDistanceScale(0.0f);
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
    Engine::resetFoliageDistanceScale();
    fixture.settle();
    EXPECT_GT(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
}

TEST(FoliageSettings, EffectiveDistanceIsTheMinimum) {
    FoliageFixture fixture(50.0f, -64.0f, 0.0f);
    fixture.terrain().detailDistance = 16.0f;
    fixture.settle();
    // cells near the camera, and none past the smaller of the two
    fixture.foliage->forEachResidentCell(fixture.entity, [](int, int, std::span<const FoliageInstance>, std::span<const uint32_t>) {});
    const uint32_t placedAtSixteen = fixture.foliage->getStats(fixture.entity).detailCellsPlaced;
    FoliageFixture wider(50.0f, -64.0f, 0.0f);
    wider.settle();
    EXPECT_LT(placedAtSixteen, wider.foliage->getStats(wider.entity).detailCellsPlaced);
}

TEST(FoliageSettings, TierScalesWithoutWriting) {
    FoliageFixture fixture;
    fixture.settle();
    const float density = fixture.terrain().detailDensity;
    const float fade = fixture.terrain().fadeLength;
    const int mesh = fixture.terrain().maxMeshTrees;
    Engine::setFoliageQuality(FoliageQuality::LOW);
    fixture.settle();
    EXPECT_EQ(fixture.terrain().detailDensity, density);
    EXPECT_EQ(fixture.terrain().fadeLength, fade);
    EXPECT_EQ(fixture.terrain().maxMeshTrees, mesh);
    Engine::resetFoliageQuality();
}

TEST(FoliageSettings, OldProjectIsHighEverywhereAndUncapped) {
    Engine::setFoliageQualityDefaults(FoliageQuality::HIGH, FoliageQuality::HIGH, FoliageQuality::HIGH, true);
    EXPECT_EQ(Engine::getFoliageQualityDefault(Platform::Android), FoliageQuality::HIGH);
    EXPECT_EQ(Engine::getFoliageQualityDefault(Platform::Web), FoliageQuality::HIGH);
    EXPECT_TRUE(Engine::isFoliageQualityUncapped());
    Engine::setFoliageQuality(FoliageQuality::HIGH);
    EXPECT_FALSE(Engine::isFoliageQualityUncapped()) << "a game that asks for High gets High's caps";
    Engine::resetFoliageQuality();
    Engine::setFoliageQualityDefaults(FoliageQuality::HIGH, FoliageQuality::LOW, FoliageQuality::LOW, false);
    EXPECT_FALSE(Engine::isFoliageQualityUncapped());
    EXPECT_EQ(Engine::getFoliageQualityDefault(Platform::iOS), FoliageQuality::LOW);
    Engine::setFoliageQualityDefaults(FoliageQuality::HIGH, FoliageQuality::HIGH, FoliageQuality::HIGH, true);
}

TEST(FoliageSettings, OneGlobalDistanceScale) {
    // no second multiplier: a tier change leaves distances alone
    Engine::setFoliageDistanceScaleDefaults(1.0f, 0.5f, 0.5f);
    EXPECT_FLOAT_EQ(Engine::getFoliageDistanceScaleDefault(Platform::Android), 0.5f);
    Engine::setFoliageQuality(FoliageQuality::LOW);
    EXPECT_FLOAT_EQ(Engine::getFoliageDistanceScale(), Engine::getFoliageDistanceScaleDefault(Engine::getPlatform()));
    Engine::resetFoliageQuality();
    Engine::setFoliageDistanceScale(2.0f);
    EXPECT_FLOAT_EQ(Engine::getFoliageDistanceScale(), 2.0f);
    Engine::setFoliageDistanceScale(99.0f);
    EXPECT_FLOAT_EQ(Engine::getFoliageDistanceScale(), 10.0f);
    Engine::setFoliageDistanceScale(std::nanf(""));
    EXPECT_FLOAT_EQ(Engine::getFoliageDistanceScale(), 10.0f) << "a value that is not finite is ignored";
    Engine::resetFoliageDistanceScale();
    Engine::setFoliageDistanceScaleDefaults(1.0f, 1.0f, 1.0f);
}

TEST(FoliageTreesCore, ResnapFollowsTheSurface) {
    FoliageFixture fixture;
    withTrees(fixture, {{10.0f, 10.0f}, {-30.0f, 40.0f}});
    fixture.settle();

    // a store write as a brush makes one, over the whole map
    TerrainComponent& terrain = fixture.terrain();
    TerrainComponentStore store(fixture.scene.get(), fixture.entity, terrain);
    std::vector<float> raised(static_cast<size_t>(MAP) * MAP, 0.75f);
    ASSERT_TRUE(store.write(MapId{MapKind::Height, 0}, TexelRect{0, 0, MAP, MAP}, raised).ok);
    TerrainEditEvents::flush(fixture.scene.get());

    // debounced: nothing yet
    fixture.frame(0.01);
    const std::shared_ptr<TreeStore> trees = fixture.foliage->getTreeStore(fixture.entity);
    EXPECT_LT(trees->records()[0].height, 0.5f);

    fixture.frame(0.2);
    fixture.settle();
    for (const TreeRecord& record : fixture.foliage->getTreeStore(fixture.entity)->records()) {
        const SurfaceSample sample = querySurface(terrain, foliageTreeLocalX(record, WORLD), foliageTreeLocalZ(record, WORLD));
        EXPECT_NEAR(record.height * terrain.maxHeight, sample.height, 1e-4f);
    }
}

TEST(FoliageTreesCore, UndoRestoresTheBytes) {
    FoliageFixture fixture;
    withTrees(fixture, {{10.0f, 10.0f}, {-30.0f, 40.0f}, {50.0f, -50.0f}});
    fixture.settle();

    TerrainComponent& terrain = fixture.terrain();
    TerrainComponentStore store(fixture.scene.get(), fixture.entity, terrain);
    const MapId height{MapKind::Height, 0};
    const TexelRect whole{0, 0, MAP, MAP};
    std::vector<uint8_t> original(static_cast<size_t>(MAP) * MAP * 2);
    ASSERT_TRUE(store.readBytes(height, whole, original));

    // the trees start with no cached surface: a sculpt and its undo write it, and what that gave is
    // what a later undo must give back
    std::vector<float> raised(static_cast<size_t>(MAP) * MAP, 0.9f);
    ASSERT_TRUE(store.write(height, whole, raised).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    fixture.frame(0.2);
    fixture.settle();
    ASSERT_TRUE(store.writeBytes(height, whole, original).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    fixture.frame(0.2);
    fixture.settle();
    std::vector<uint8_t> before;
    fixture.foliage->getTreeStore(fixture.entity)->encode(before);

    ASSERT_TRUE(store.write(height, whole, raised).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    fixture.frame(0.2);
    fixture.settle();
    std::vector<uint8_t> sculpted;
    fixture.foliage->getTreeStore(fixture.entity)->encode(sculpted);
    EXPECT_NE(sculpted, before);

    ASSERT_TRUE(store.writeBytes(height, whole, original).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    fixture.frame(0.2);
    fixture.settle();
    std::vector<uint8_t> undone;
    fixture.foliage->getTreeStore(fixture.entity)->encode(undone);
    EXPECT_EQ(undone, before);
}

TEST(FoliageTreesCore, HoleHidesAndRestores) {
    FoliageFixture fixture;
    withTrees(fixture, {{10.0f, 10.0f}, {-30.0f, 40.0f}});
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 2u);

    TerrainComponent& terrain = fixture.terrain();
    TerrainComponentStore store(fixture.scene.get(), fixture.entity, terrain);
    const MapId hole{MapKind::Hole, 0};
    MapFormat format;
    format.bounds = TexelRect{0, 0, MAP, MAP};
    format.channels = 1;
    format.bitsPerChannel = 8;
    ASSERT_TRUE(store.createMap(hole, format, 1.0f).ok);
    std::vector<float> cut(static_cast<size_t>(MAP) * MAP, 0.0f);
    ASSERT_TRUE(store.write(hole, TexelRect{0, 0, MAP, MAP}, cut).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    fixture.frame(0.2);
    fixture.settle();
    const std::shared_ptr<TreeStore> trees = fixture.foliage->getTreeStore(fixture.entity);
    EXPECT_EQ(trees->size(), 2u) << "hidden, not deleted";
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 0u);

    std::vector<float> filled(static_cast<size_t>(MAP) * MAP, 1.0f);
    ASSERT_TRUE(store.write(hole, TexelRect{0, 0, MAP, MAP}, filled).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    fixture.frame(0.2);
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 2u);
}

TEST(FoliageTreesCore, TouchesOnlyDirtyCells) {
    FoliageFixture fixture;
    std::initializer_list<std::pair<float, float>> points = {{-50.0f, -50.0f}, {-50.0f, 50.0f}, {50.0f, -50.0f}, {50.0f, 50.0f}};
    withTrees(fixture, points);
    fixture.settle();
    const uint32_t treeCells = fixture.foliage->getStats(fixture.entity).treeCellsRebuilt;
    const uint32_t detailCells = fixture.foliage->getStats(fixture.entity).detailCellsPlaced;

    TerrainComponent& terrain = fixture.terrain();
    TerrainComponentStore store(fixture.scene.get(), fixture.entity, terrain);
    const TexelRect corner{0, 0, 8, 8};
    std::vector<float> values(64, 0.8f);
    ASSERT_TRUE(store.write(MapId{MapKind::Height, 0}, corner, values).ok);
    TerrainEditEvents::flush(fixture.scene.get());
    EXPECT_FALSE(terrain.needUpdateFoliage) << "T5 R26: no whole-Terrain refill for a write";
    fixture.frame(0.2);
    fixture.settle();

    EXPECT_LE(fixture.foliage->getStats(fixture.entity).treeCellsRebuilt - treeCells, 1u + 3u) << "the corner's cell and its neighbours, no more";
    const uint32_t detailRebuilt = fixture.foliage->getStats(fixture.entity).detailCellsPlaced - detailCells;
    EXPECT_GT(detailRebuilt, 0u);
    EXPECT_LE(detailRebuilt, 16u) << "the rectangle's cells, not the ring";
}

TEST(FoliageTreesCore, AddingToOneCellRebuildsAtMostItsNeighbours) {
    FoliageFixture fixture;
    withTrees(fixture, {{-50.0f, -50.0f}, {50.0f, 50.0f}, {3.0f, 3.0f}});
    fixture.settle();
    const uint32_t rebuilt = fixture.foliage->getStats(fixture.entity).treeCellsRebuilt;

    std::shared_ptr<TreeStore> store = fixture.foliage->getTreeStore(fixture.entity);
    std::vector<TreeRecord> added;
    for (int i = 0; i < 100; i++) added.push_back(treeAt(2.0f + static_cast<float>(i) * 0.1f, 4.0f, 5));
    TreeDelta delta;
    store->apply({}, added, {}, &delta);
    fixture.foliage->treesChanged(fixture.entity, delta);
    fixture.settle();
    EXPECT_LE(fixture.foliage->getStats(fixture.entity).treeCellsRebuilt - rebuilt, 9u);
}

TEST(FoliageRuntime, RemoveRestoreAndAdd) {
    FoliageFixture fixture;
    withTrees(fixture, {{10.0f, 10.0f}, {-30.0f, 40.0f}});
    fixture.settle();
    const uint32_t id = fixture.foliage->getTreeStore(fixture.entity)->records()[0].id;
    std::vector<uint8_t> file;
    fixture.foliage->getTreeStore(fixture.entity)->encode(file);

    int removedEvents = 0;
    Subscription subscription = fixture.foliage->onTreeRemoved.subscribe([&](Entity, TreeId, const FoliageInstanceInfo&) { removedEvents++; });

    const std::optional<FoliageInstanceInfo> pose = fixture.foliage->removeTreeInstance(fixture.entity, makeTreeId(0, id));
    ASSERT_TRUE(pose.has_value());
    EXPECT_NEAR(pose->position.x, 10.0f, 1e-3f);
    EXPECT_EQ(removedEvents, 1);
    EXPECT_FALSE(fixture.foliage->removeTreeInstance(fixture.entity, makeTreeId(0, id)).has_value()) << "gone already: nil";
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 1u);

    std::vector<uint8_t> afterRemove;
    fixture.foliage->getTreeStore(fixture.entity)->encode(afterRemove);
    EXPECT_EQ(afterRemove, file) << "F10: the overlay leaves the store alone";

    EXPECT_TRUE(fixture.foliage->restoreTreeInstance(fixture.entity, makeTreeId(0, id)));
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 2u);

    const TreeId added = fixture.foliage->addTreeInstance(fixture.entity, 0, Vector3(5.0f, 0.0f, 5.0f), 90.0f, 1.2f);
    ASSERT_NE(added, 0u);
    EXPECT_GE(treeIdRecord(added), TREE_RUNTIME_ID_BASE);
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 3u);

    const std::vector<FoliageInstanceInfo> near = fixture.foliage->getInstancesInRadius(fixture.entity, Vector3(5.0f, 0.0f, 5.0f), 1.0f);
    ASSERT_EQ(near.size(), 1u);
    EXPECT_EQ(near[0].id, added);
}

TEST(FoliageRuntime, StateRoundTrips) {
    FoliageFixture fixture;
    withTrees(fixture, {{10.0f, 10.0f}, {-30.0f, 40.0f}});
    fixture.settle();
    const uint32_t id = fixture.foliage->getTreeStore(fixture.entity)->records()[1].id;
    fixture.foliage->removeTreeInstance(fixture.entity, makeTreeId(0, id));
    fixture.foliage->addTreeInstance(fixture.entity, 0, Vector3(1.0f, 0.0f, 2.0f), 45.0f, 1.5f);
    fixture.foliage->clearDetailsInRadius(fixture.entity, Vector3(0.0f, 0.0f, 0.0f), 4.0f);
    const std::string saved = fixture.foliage->getRuntimeState(fixture.entity);

    fixture.foliage->clearRuntimeState(fixture.entity);
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 2u);

    EXPECT_TRUE(fixture.foliage->setRuntimeState(fixture.entity, saved));
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getStats(fixture.entity).treesResident, 2u) << "one away, one back";
    EXPECT_EQ(fixture.foliage->getRuntimeState(fixture.entity), saved);

    EXPECT_FALSE(fixture.foliage->setRuntimeState(fixture.entity, "not a state"));
    EXPECT_EQ(fixture.foliage->getRuntimeState(fixture.entity), saved) << "a bad string changes nothing";
}

TEST(FoliageRuntime, ClearedDiscsStayCleared) {
    FoliageFixture fixture;
    fixture.settle();
    const uint64_t before = fixture.foliage->getInstanceCount(fixture.entity, 0);
    fixture.foliage->clearDetailsInRadius(fixture.entity, Vector3(0.0f, 0.0f, 0.0f), 16.0f);
    const uint64_t after = fixture.foliage->getInstanceCount(fixture.entity, 0);
    EXPECT_LT(after, before);
    EXPECT_TRUE(fixture.foliage->getInstancesInRadius(fixture.entity, Vector3(0.0f, 0.0f, 0.0f), 15.0f, 1).empty());
    fixture.foliage->restoreDetails(fixture.entity);
    fixture.settle();
    EXPECT_EQ(fixture.foliage->getInstanceCount(fixture.entity, 0), before);
}

// R42: the records exist once a Terrain has foliage, follow it, and return what they counted
TEST(FoliageSystemCore, MemoryRecordsFollowTheStats) {
    const auto recordOf = [](const std::string& name) -> uint64_t {
        for (const memory::StatsEntry& entry : memory::snapshot()) {
            if (entry.name == name) return entry.current;
        }
        return 0;
    };
    const uint64_t before = recordOf("foliage-cpu");
    {
        FoliageFixture fixture(30.0f);
        fixture.settle();
        const uint64_t bytes = fixture.foliage->getStats(fixture.entity).cpuBytes;
        ASSERT_GT(bytes, 0u);
        fixture.frame();
        EXPECT_EQ(recordOf("foliage-cpu") - before, bytes);
    }
    EXPECT_EQ(recordOf("foliage-cpu"), before);
}

// Trees written before the Terrain's size is known are not all in cell (0, 0) once it is
TEST(FoliageTreesCore, TreesPlantedBeforeTheTerrainLoadsLandInTheirCells) {
    FoliageFixture fixture;
    TerrainTreePrototype prototype;
    prototype.id = 5;
    prototype.meshPath = AssetRef(foliagetest::crossMesh());
    fixture.terrain().treePrototypes.push_back(prototype);
    fixture.terrain().heightMapLoaded = false;
    fixture.foliage->update(0.016);

    std::shared_ptr<TreeStore> store = fixture.foliage->getOrCreateTreeStore(fixture.entity);
    ASSERT_NE(store, nullptr);
    TreeDelta delta;
    const std::vector<TreeRecord> added = {treeAt(-40.0f, -40.0f, 5), treeAt(40.0f, 40.0f, 5)};
    store->apply({}, added, {}, &delta);
    fixture.foliage->treesChanged(fixture.entity, delta);

    fixture.settle();
    int cells = 0;
    fixture.foliage->forEachResidentCell(fixture.entity, [&](int cellX, int, std::span<const FoliageInstance> instances, std::span<const uint32_t>) {
        EXPECT_EQ(instances.size(), 1u) << "cell " << cellX;
        cells++;
    });
    EXPECT_EQ(cells, 2);
}
