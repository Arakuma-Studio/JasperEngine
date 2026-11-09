// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R2-R8 (AC2-AC7): encoding, uniform collapse, the pyramid, copy-on-write, budgets, bounds

#include "VoxelTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace voxeltest;

TEST(VoxelBlock, EncodingAndClamp) {
    EXPECT_EQ(encodeSdf(0.0), 0);
    EXPECT_EQ(encodeSdf(1.0), 4096);
    EXPECT_EQ(encodeSdf(-1.0), -4096);
    EXPECT_EQ(encodeSdf(8.0), 32767);
    EXPECT_EQ(encodeSdf(-8.0), -32767);
    EXPECT_EQ(encodeSdf(100.0), 32767);
    EXPECT_EQ(encodeSdf(-100.0), -32767);
    // floor(x + 0.5): halves go up on both sides of zero
    EXPECT_EQ(encodeSdf(0.5 / 4096.0), 1);
    EXPECT_EQ(encodeSdf(-0.5 / 4096.0), 0);
    EXPECT_NEAR(decodeSdf(2048), 0.5, 1e-12);
}

TEST(VoxelBlock, MaterialPacking) {
    const uint16_t value = packMaterial(3, 12, 200);
    EXPECT_EQ(value, 3 | (12 << 4) | (200 << 8));
    EXPECT_EQ(materialSlotA(value), 3);
    EXPECT_EQ(materialSlotB(value), 12);
    EXPECT_EQ(materialWeightB(value), 200);
    EXPECT_EQ(packMaterial(15, 15, 255), 0xFFFF);
}

TEST(VoxelBlock, UniformCollapse) {
    Store store(desc(64, 64, 64));
    const Shape box = [] {
        Shape shape;
        shape.kind = ShapeKind::Box;
        shape.center = Vec3d{24.0, 24.0, 24.0};
        shape.halfExtents = Vec3d{30.0, 30.0, 30.0};
        return shape;
    }();
    // Paint leaves the block's SDF alone; filling writes materials too, so both channels get used
    const EditOp fill = addOp(sphereShape(16.0, 24.0, 24.0, 6.0), 1);
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&fill, 1)).ok);
    EXPECT_EQ(store.slot(BlockKey{0, 1, 1, 1}).kind(), Slot::Hot);
    const int64_t bytes = store.memory().hotBytes;
    EXPECT_GT(bytes, 0);
    EXPECT_GE(store.slot(BlockKey{0, 1, 1, 1}).hotBlock()->payloadBytes(), 8192u);
    EXPECT_EQ(store.slot(BlockKey{0, 1, 1, 1}).hotBlock()->payloadBytes(), 16384u);

    // Dig the whole region away, then paint back slot 0, and nothing non-uniform is left
    EditOp dig = digOp(box);
    dig.shape.halfExtents = Vec3d{31.0, 31.0, 31.0};
    EditOp paint;
    paint.kind = EditKind::Paint;
    paint.shape = box;
    paint.shape.center = Vec3d{32.0, 32.0, 32.0};
    paint.shape.halfExtents = Vec3d{40.0, 40.0, 40.0};
    paint.material = 0;
    const EditOp ops[] = {dig, paint};
    const EditResult result = applyEdit(store, ops);
    ASSERT_TRUE(result.ok);
    EXPECT_GT(result.blocksCollapsed, 0);
    EXPECT_EQ(store.slot(BlockKey{0, 1, 1, 1}).kind(), Slot::Uniform);
    EXPECT_LT(store.memory().hotBytes, bytes);
    EXPECT_EQ(result.memoryDelta, store.memory().hotBytes - bytes);
}

TEST(VoxelStore, SlotTableSize) {
    // R4: 1024x512x1024 has 131,072 level-0 slots: 1 MiB, plus 1/7 for the levels
    Store store(desc(1024, 512, 1024, 4));
    EXPECT_EQ(store.slotCount(0), 131072u);
    const double expected = 1024.0 * 1024.0;
    const double levels = static_cast<double>(store.memory().tableBytes);
    EXPECT_GE(levels, expected);
    EXPECT_LE(levels, expected * (8.0 / 7.0) * 1.05);
    EXPECT_EQ(store.memory().hotBytes, 0);
    EXPECT_EQ(sizeof(Slot), 8u);
}

TEST(VoxelStore, LevelsFollowDimensions) {
    EXPECT_EQ(levelsFor(Int3{16, 16, 16}, 4), 1);
    EXPECT_EQ(levelsFor(Int3{32, 64, 64}, 4), 2);
    EXPECT_EQ(levelsFor(Int3{64, 64, 64}, 4), 3);
    EXPECT_EQ(levelsFor(Int3{128, 128, 128}, 4), 4);
    EXPECT_EQ(levelsFor(Int3{1024, 512, 1024}, 4), 4);
    EXPECT_EQ(levelsFor(Int3{1024, 512, 1024}, 2), 2);
}

TEST(VoxelStore, PyramidIsPointDecimation) {
    Store store(desc(128, 128, 128, 4));
    store.setBase(wavyBase(60.0));
    std::mt19937 random(5);
    for (int i = 0; i < 1000; i++) {
        const Shape shape = sphereShape(random() % 128, random() % 128, random() % 128, 1.0 + random() % 8);
        EditOp op = (random() % 2) ? digOp(shape) : addOp(shape, static_cast<int>(random() % 16));
        applyEdit(store, std::span<const EditOp>(&op, 1));
    }
    for (int level = 1; level < store.levels(); level++) {
        const Int3 samples = store.levelSamples(level);
        for (int z = 0; z < samples.z; z += 3) {
            for (int y = 0; y < samples.y; y += 3) {
                for (int x = 0; x < samples.x; x += 3) {
                    ASSERT_EQ(store.sdfAt(level, x, y, z), store.sdfAt(0, x << level, y << level, z << level));
                    ASSERT_EQ(store.materialAt(level, x, y, z), store.materialAt(0, x << level, y << level, z << level));
                }
            }
        }
    }
}

TEST(VoxelStore, JobReadsOneVersion) {
    // Run under SANITIZE=thread in the sanitizer preset: four readers, one writer, no race, and a held
    // view keeps its old bytes
    Store store(desc(64, 64, 64));
    store.setBase(sphereBase(32.0, 32.0, 32.0, 20.0));
    const BlockKey key{0, 1, 1, 1};
    const auto heldView = store.readView(key, 0);
    int16_t before[19 * 19 * 19];
    heldView->fetchSdf(0, Int3{15, 15, 15}, Int3{19, 19, 19}, before);

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    std::vector<std::shared_ptr<const ReadView>> views;
    for (int i = 0; i < 4; i++) views.push_back(store.readView(key, 0));
    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&, i] {
            int16_t window[19 * 19 * 19];
            int16_t first[19 * 19 * 19];
            views[static_cast<size_t>(i)]->fetchSdf(0, Int3{15, 15, 15}, Int3{19, 19, 19}, first);
            while (!stop.load()) {
                views[static_cast<size_t>(i)]->fetchSdf(0, Int3{15, 15, 15}, Int3{19, 19, 19}, window);
                EXPECT_EQ(std::memcmp(window, first, sizeof(window)), 0);
            }
        });
    }
    for (int i = 0; i < 200; i++) {
        const EditOp op = digOp(sphereShape(16.0 + i % 32, 16.0, 20.0, 5.0));
        applyEdit(store, std::span<const EditOp>(&op, 1));
    }
    stop.store(true);
    for (std::thread& thread : readers) thread.join();

    int16_t after[19 * 19 * 19];
    heldView->fetchSdf(0, Int3{15, 15, 15}, Int3{19, 19, 19}, after);
    EXPECT_EQ(std::memcmp(before, after, sizeof(before)), 0);
}

TEST(VoxelStore, BudgetRefusesBeforeWrite) {
    Store store(desc(64, 64, 64));
    auto budget = std::make_shared<MemoryBudget>(store.memory().total() + 1024);
    store.setBudget(budget);
    const uint64_t before = contentHash(store);
    const EditOp op = addOp(sphereShape(32.0, 32.0, 32.0, 10.0), 2);
    const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, EditError::BudgetExceeded);
    EXPECT_EQ(contentHash(store), before);
    EXPECT_EQ(store.memory().hotBytes, 0);
}

TEST(VoxelStore, EvictsUnmodifiedFirst) {
    Store store(desc(64, 64, 64));
    store.setBase(sphereBase(32.0, 32.0, 32.0, 20.0));
    // Resolve unmodified blocks: they are the eviction candidates
    for (int z = 0; z < 4; z++) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) store.content(BlockKey{0, x, y, z});
        }
    }
    const int64_t resolved = store.memory().hotBytes;
    ASSERT_GT(resolved, 0);
    const EditOp dig = digOp(sphereShape(32.0, 52.0, 32.0, 3.0));
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&dig, 1)).ok);
    const uint64_t hash = contentHash(store);
    const int64_t modifiedOnly = [&] {
        Store::BlockCounts counts = store.countBlocks();
        return counts.modified * 8192;
    }();

    auto budget = std::make_shared<MemoryBudget>(store.memory().tableBytes + modifiedOnly + 64 * 1024);
    store.setBudget(budget);
    const EditOp add = addOp(sphereShape(10.0, 10.0, 10.0, 1.5));
    const EditResult result = applyEdit(store, std::span<const EditOp>(&add, 1));
    EXPECT_TRUE(result.ok);
    EXPECT_LT(store.memory().hotBytes, resolved + result.memoryDelta);
    // The modified block stayed; evicted blocks come back the same from the base
    EXPECT_TRUE(store.slot(BlockKey{0, 2, 3, 2}).modified());
    EXPECT_NE(contentHash(store), hash);
    EXPECT_EQ(store.sdfAt(0, 32, 12, 32), encodeSdf(0.0 + std::fabs(12.0 - 32.0) - 20.0));
}

TEST(VoxelStore, PastTheEndReadsPerBounds) {
    Store closed(desc(32, 32, 32, 2));
    closed.setBase(std::make_shared<FunctionBase>([](double, double, double) { return -2.0; }));
    EXPECT_EQ(closed.sdfAt(0, 32, 5, 5), SDF_AIR);
    EXPECT_EQ(closed.sdfAt(1, 16, 5, 5), SDF_AIR);
    EXPECT_EQ(closed.sdfAt(0, 0, 5, 5), SDF_SHELL_AIR);
    EXPECT_EQ(closed.sdfAt(0, 31, 5, 5), SDF_SHELL_AIR);
    EXPECT_EQ(closed.sdfAt(0, 30, 5, 5), encodeSdf(-2.0));

    Store open(desc(32, 32, 32, 2, BoundsMode::Open));
    open.setBase(std::make_shared<FunctionBase>([](double x, double, double) { return x - 50.0; }));
    EXPECT_EQ(open.sdfAt(0, 32, 5, 5), open.sdfAt(0, 31, 5, 5));
    EXPECT_EQ(open.sdfAt(1, 16, 5, 5), open.sdfAt(1, 15, 5, 5));
    EXPECT_EQ(open.sdfAt(0, -3, 5, 5), open.sdfAt(0, 0, 5, 5));
}

TEST(VoxelStore, CloneSharesBlocksAndEditsApart) {
    Store store(desc(64, 64, 64));
    const EditOp fill = addOp(sphereShape(32.0, 32.0, 32.0, 10.0), 3);
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&fill, 1)).ok);
    const uint64_t editorHash = contentHash(store);
    auto play = store.clone();
    EXPECT_EQ(contentHash(*play), editorHash);
    EXPECT_EQ(play->slot(BlockKey{0, 2, 2, 2}).hotBlock(), store.slot(BlockKey{0, 2, 2, 2}).hotBlock());
    const EditOp dig = digOp(sphereShape(32.0, 32.0, 32.0, 4.0));
    ASSERT_TRUE(applyEdit(*play, std::span<const EditOp>(&dig, 1)).ok);
    EXPECT_NE(contentHash(*play), editorHash);
    EXPECT_EQ(contentHash(store), editorHash);
}

TEST(VoxelStore, UnresolvedBlocksResolveOnTheJobAndPublish) {
    Store store(desc(64, 64, 64, 3));
    store.setBase(sphereBase(32.0, 32.0, 32.0, 20.0));
    const BlockKey key{1, 1, 1, 1};
    EXPECT_EQ(store.slot(key).kind(), Slot::Base);
    const auto view = store.readView(key, 0);
    MeshRequest request;
    request.key = key;
    MeshOut mesh;
    ASSERT_TRUE(meshBlock(*view, request, mesh));
    EXPECT_EQ(store.slot(key).kind(), Slot::Base);    // nothing written from the job
    store.publish(view->takeResolved());
    EXPECT_EQ(store.slot(key).kind(), Slot::Hot);
    // R33: a coarse block needed no fine block
    EXPECT_EQ(store.slot(BlockKey{0, 2, 2, 2}).kind(), Slot::Base);
}
