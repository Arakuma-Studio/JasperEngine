// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One test per op of T1 §6 Table 1, against values worked out by hand on a small map (AC3)

#include "TerrainTestSupport.h"

#include "terrain/TerrainLayers.h"
#include "terrain/TerrainNoise.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace terraintest;

namespace {

    constexpr float QUANTUM = 1.0f / 65535.0f;
    constexpr float BYTE = 1.0f / 255.0f;
    constexpr int SIZE = 8;              // texel centres at -3.5, -2.5, ... 3.5
    constexpr float MAX_HEIGHT = 10.0f;

    std::unique_ptr<RawMapStore> flatStore(float value) {
        auto store = std::make_unique<RawMapStore>(static_cast<float>(SIZE), MAX_HEIGHT);
        addFlatHeight(*store, SIZE, value);
        return store;
    }

    float heightAt(const std::vector<float>& heights, int x, int y) {
        return heights[static_cast<size_t>(y) * SIZE + x];
    }

    void expectAll(const TerrainMapStore& store, float value, float tolerance = QUANTUM) {
        for (float height : readHeights(store)) {
            EXPECT_NEAR(height, value, tolerance);
        }
    }

    Op allOp(OpType type) {
        Op op;
        op.type = type;
        return op;
    }

    std::vector<float> uniformBlend(float r, float g, float b) {
        std::vector<float> values;
        for (int i = 0; i < SIZE * SIZE; i++) {
            values.insert(values.end(), {r, g, b, 1.0f});
        }
        return values;
    }

}

TEST(TerrainOpsKernels, Raise) {
    auto store = flatStore(0.25f);
    Op op = allOp(OpType::Raise);
    op.delta = 2.0f;   // 0.2 of the map's height
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.45f);
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.65f);
}

TEST(TerrainOpsKernels, RaiseTwiceIsDeltaTimesTwo) {
    auto once = flatStore(0.2f);
    auto twice = flatStore(0.2f);
    Op a = circleOp(OpType::Raise, 0.0f, 0.0f, 3.0f);
    a.shape.falloff = Falloff::Constant;
    a.delta = 1.0f;
    Op b = a;
    b.delta = 2.0f;
    const Op list[] = {a, a};
    ASSERT_TRUE(applyOps(*twice, list).ok);
    ASSERT_TRUE(apply(*once, b).ok);
    EXPECT_EQ(mapHash(*twice, HEIGHT), mapHash(*once, HEIGHT));
}

TEST(TerrainOpsKernels, Lower) {
    auto store = flatStore(0.5f);
    Op op = allOp(OpType::Lower);
    op.delta = 2.5f;
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.25f);
}

TEST(TerrainOpsKernels, StrengthScales) {
    auto store = flatStore(0.5f);
    Op op = allOp(OpType::Raise);
    op.delta = 2.0f;
    op.strength = 0.25f;
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.55f);
}

TEST(TerrainOpsKernels, SetHeightIsIdempotent) {
    auto store = flatStore(0.2f);
    Op op = allOp(OpType::SetHeight);
    op.height = 7.0f;
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.7f);
    const uint64_t once = mapHash(*store, HEIGHT);
    const TerrainOpResult again = apply(*store, op);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(mapHash(*store, HEIGHT), once);
    EXPECT_EQ(again.ops[0].changedTexels, 0);
}

TEST(TerrainOpsKernels, SetHeightLerpsByWeight) {
    auto store = flatStore(0.2f);
    Op op = allOp(OpType::SetHeight);
    op.height = 6.0f;   // 0.6
    op.strength = 0.5f;
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.4f);
}

TEST(TerrainOpsKernels, LiftFloorsOnlyLifts) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int) { return x < 4 ? 0.2f : 0.8f; });
    Op op = allOp(OpType::LiftFloors);
    op.height = 5.0f;
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(heightAt(after, 1, 1), 0.5f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 6, 1), 0.8f, QUANTUM);
}

TEST(TerrainOpsKernels, FlattenPeaksOnlyLowers) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int) { return x < 4 ? 0.2f : 0.8f; });
    Op op = allOp(OpType::FlattenPeaks);
    op.height = 5.0f;
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(heightAt(after, 1, 1), 0.2f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 6, 1), 0.5f, QUANTUM);
}

TEST(TerrainOpsKernels, SmoothGoesTowardTheBlur) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int y) { return x == 4 && y == 4 ? 0.9f : 0.0f; });
    Op op = allOp(OpType::Smooth);
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    // step 1 on an all-map op: the 3x3 box of a single peak
    EXPECT_NEAR(heightAt(after, 4, 4), 0.1f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 3, 4), 0.1f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 3, 3), 0.1f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 2, 4), 0.0f, QUANTUM);
}

TEST(TerrainOpsKernels, SmoothStepFollowsTheRadius) {
    constexpr int WIDE = 64;
    auto store = std::make_unique<RawMapStore>(static_cast<float>(WIDE), MAX_HEIGHT);
    addFlatHeight(*store, WIDE, 0.0f);
    fillHeight(*store, WIDE, WIDE, [](int x, int y) { return x == 32 && y == 32 ? 0.9f : 0.0f; });
    // radius 16 texels: step 4, so the peak reaches the samples 4 texels away and no others
    Op op = circleOp(OpType::Smooth, 0.5f, 0.5f, 16.0f);
    op.shape.falloff = Falloff::Constant;
    ASSERT_TRUE(apply(*store, op).ok);
    std::vector<float> value(1);
    store->read(HEIGHT, TexelRect{36, 32, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.1f, QUANTUM);
    store->read(HEIGHT, TexelRect{34, 32, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.0f, QUANTUM);
}

TEST(TerrainOpsKernels, SharpenGoesAwayFromTheBlur) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int y) { return x == 4 && y == 4 ? 0.45f : 0.0f; });
    Op op = allOp(OpType::Sharpen);
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    // current + (current - blurred) * 1: 0.45 + 0.45 - 0.05
    EXPECT_NEAR(heightAt(after, 4, 4), 0.85f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 3, 4), 0.0f, QUANTUM) << "a neighbour below 0 clamps";
}

TEST(TerrainOpsKernels, AddNoiseFollowsTheField) {
    auto store = flatStore(0.5f);
    Op op = allOp(OpType::AddNoise);
    op.amplitude = 1.0f;
    op.noiseSize = 2.0f;
    op.octaves = 3;
    op.seed = 9;
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.ops[0].seed.has_value());
    EXPECT_EQ(*result.ops[0].seed, 9u) << "reported back (R8)";
    const std::vector<float> after = readHeights(*store);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            const float expected = 0.5f + TerrainNoise::fractal(static_cast<float>(x) / 2.0f, static_cast<float>(y) / 2.0f, 9, 3) * 0.1f;
            EXPECT_NEAR(heightAt(after, x, y), expected, QUANTUM);
        }
    }
}

TEST(TerrainOpsKernels, Terrace) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int) { return static_cast<float>(x) / 7.0f; });
    Op op = allOp(OpType::Terrace);
    op.steps = 3;   // levels 0, 0.5, 1
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(heightAt(after, 0, 0), 0.0f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 1, 0), 0.0f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 3, 0), 0.5f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 5, 0), 0.5f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 6, 0), 1.0f, QUANTUM);
}

TEST(TerrainOpsKernels, StampAddsTheMaskAsRelief) {
    auto store = flatStore(0.25f);
    Op op;
    op.type = OpType::Stamp;
    op.shape.type = ShapeType::Mask;
    op.shape.center = Vector2(0.0f, 0.0f);
    op.shape.size = Vector2(4.0f, 4.0f);
    op.shape.maskWidth = 2;
    op.shape.maskHeight = 1;
    op.shape.maskPixels = {0.0f, 1.0f};
    op.delta = 5.0f;
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    // u = x / 4 + 0.5 across the 4-unit mask; the height gains u * 0.5
    EXPECT_NEAR(heightAt(after, 2, 3), 0.25f + 0.0625f, QUANTUM * 2.0f);   // x = -1.5
    EXPECT_NEAR(heightAt(after, 5, 3), 0.25f + 0.4375f, QUANTUM * 2.0f);   // x = 1.5
    EXPECT_NEAR(heightAt(after, 0, 3), 0.25f, QUANTUM) << "outside the mask";
}

TEST(TerrainOpsKernels, Erode) {
    constexpr int WIDE = 32;
    auto store = std::make_unique<RawMapStore>(static_cast<float>(WIDE), MAX_HEIGHT);
    addFlatHeight(*store, WIDE, 0.0f);
    fillHeight(*store, WIDE, WIDE, [](int x, int) { return 0.9f - static_cast<float>(x) * 0.02f; });
    const uint64_t before = mapHash(*store, HEIGHT);
    Op op = allOp(OpType::Erode);
    op.seed = 4;
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok);
    EXPECT_GT(result.ops[0].changedTexels, 0);
    EXPECT_NE(mapHash(*store, HEIGHT), before);
    for (float height : readHeights(*store)) {
        EXPECT_GE(height, 0.0f);
        EXPECT_LE(height, 1.0f);
    }
}

TEST(TerrainOpsKernels, ErodeIsSeeded) {
    auto run = [](uint32_t seed) {
        constexpr int WIDE = 32;
        RawMapStore store(static_cast<float>(WIDE), MAX_HEIGHT);
        addFlatHeight(store, WIDE, 0.0f);
        fillHeight(store, WIDE, WIDE, [](int x, int) { return 0.9f - static_cast<float>(x) * 0.02f; });
        Op op;
        op.type = OpType::Erode;
        op.seed = seed;
        apply(store, op);
        return mapHash(store, HEIGHT);
    };
    EXPECT_EQ(run(1), run(1));
    EXPECT_NE(run(1), run(2));
}

TEST(TerrainOpsKernels, RampFollowsItsEnds) {
    auto store = flatStore(0.0f);
    Op op;
    op.type = OpType::Ramp;
    op.shape.type = ShapeType::Polyline;
    op.shape.points = {Vector2(-3.5f, 0.5f), Vector2(3.5f, 0.5f)};
    op.shape.radius = 1.0f;
    op.shape.falloff = Falloff::Constant;
    op.heights = FilterRange{2.0f, 9.0f};
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    for (int x = 0; x < SIZE; x++) {
        EXPECT_NEAR(heightAt(after, x, 4), 0.2f + static_cast<float>(x) / 7.0f * 0.7f, QUANTUM * 2.0f);
    }
    EXPECT_NEAR(heightAt(after, 3, 0), 0.0f, QUANTUM) << "outside the capsule";
}

TEST(TerrainOpsKernels, RampDefaultsToTheSurfaceAtItsEnds) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int) { return x < 4 ? 0.2f : 0.6f; });
    Op op;
    op.type = OpType::Ramp;
    op.shape.type = ShapeType::Polyline;
    op.shape.points = {Vector2(-3.5f, 0.5f), Vector2(3.5f, 0.5f)};
    op.shape.radius = 1.0f;
    op.shape.falloff = Falloff::Constant;
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(heightAt(after, 0, 4), 0.2f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 7, 4), 0.6f, QUANTUM);
}

TEST(TerrainOpsKernels, RemapHeights) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int) { return x < 4 ? 0.2f : 0.6f; });
    Op op = allOp(OpType::RemapHeights);
    op.inRange = FilterRange{2.0f, 6.0f};
    op.outRange = FilterRange{4.0f, 5.0f};
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(heightAt(after, 0, 0), 0.4f, QUANTUM);
    EXPECT_NEAR(heightAt(after, 7, 0), 0.5f, QUANTUM);
}

TEST(TerrainOpsKernels, PaintLayerNormalizesWithinItsMap) {
    RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
    addFlatHeight(store, SIZE, 0.0f);
    store.addMap(blend(0), SIZE, SIZE, 4, 8);
    store.write(blend(0), TexelRect{0, 0, SIZE, SIZE}, uniformBlend(0.4f, 0.4f, 0.2f));

    Op op = allOp(OpType::PaintLayer);
    op.layer = 1;
    op.strength = 0.5f;
    ASSERT_TRUE(apply(store, op).ok);
    std::vector<float> texel(4);
    store.read(blend(0), TexelRect{2, 2, 1, 1}, texel);
    EXPECT_NEAR(texel[0], 0.2f, BYTE);
    EXPECT_NEAR(texel[1], 0.7f, BYTE);
    EXPECT_NEAR(texel[2], 0.1f, BYTE);
    EXPECT_EQ(texel[3], 1.0f);
}

TEST(TerrainOpsKernels, PaintLayerWithoutNormalizeKeepsOtherChannels) {
    RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
    addFlatHeight(store, SIZE, 0.0f);
    store.addMap(blend(0), SIZE, SIZE, 4, 8);
    store.write(blend(0), TexelRect{0, 0, SIZE, SIZE}, uniformBlend(0.4f, 0.4f, 0.2f));
    Op op = allOp(OpType::PaintLayer);
    op.layer = 0;
    op.normalize = false;
    op.strength = 0.5f;
    ASSERT_TRUE(apply(store, op).ok);
    std::vector<float> texel(4);
    store.read(blend(0), TexelRect{2, 2, 1, 1}, texel);
    EXPECT_NEAR(texel[0], 0.7f, BYTE);
    EXPECT_NEAR(texel[1], 0.4f, BYTE);
    EXPECT_NEAR(texel[2], 0.2f, BYTE);
}

TEST(TerrainOpsKernels, FirstPaintCreatesTheBlendMap) {
    auto store = flatStore(0.0f);
    Op op = circleOp(OpType::PaintLayer, 0.0f, 0.0f, 2.0f);
    op.shape.falloff = Falloff::Constant;
    op.layer = 4;   // the second blend map
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ops[0].mapCreated);
    EXPECT_TRUE(result.hasWarning("map_created"));
    ASSERT_EQ(result.createdMaps.size(), 1u);
    EXPECT_EQ(result.createdMaps[0], blend(1));
    EXPECT_FALSE(store->format(blend(0)).exists);
    ASSERT_TRUE(store->format(blend(1)).exists);
    std::vector<float> texel(4);
    store->read(blend(1), TexelRect{4, 4, 1, 1}, texel);
    EXPECT_NEAR(texel[1], 1.0f, BYTE);
    store->read(blend(1), TexelRect{0, 0, 1, 1}, texel);
    EXPECT_EQ(texel[1], 0.0f);
    EXPECT_EQ(texel[3], 1.0f) << "a new blend map is opaque";

    // a dry run reports the same and leaves no map
    auto second = flatStore(0.0f);
    OpOptions dry;
    dry.dryRun = true;
    const TerrainOpResult preview = apply(*second, op, dry);
    ASSERT_TRUE(preview.ok);
    EXPECT_EQ(preview.createdMaps.size(), 1u);
    EXPECT_FALSE(second->format(blend(1)).exists);
}

TEST(TerrainOpsKernels, PaintBaseClearsChannels) {
    RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
    addFlatHeight(store, SIZE, 0.0f);
    store.addMap(blend(0), SIZE, SIZE, 4, 8);
    store.addMap(blend(1), SIZE, SIZE, 4, 8);
    const std::vector<float> start = uniformBlend(0.8f, 0.4f, 0.2f);
    store.write(blend(0), TexelRect{0, 0, SIZE, SIZE}, start);
    store.write(blend(1), TexelRect{0, 0, SIZE, SIZE}, start);

    Op op = allOp(OpType::PaintBase);
    op.strength = 0.5f;
    op.layer = 0;   // that layer's map only
    ASSERT_TRUE(apply(store, op).ok);
    std::vector<float> texel(4);
    store.read(blend(0), TexelRect{2, 2, 1, 1}, texel);
    EXPECT_NEAR(texel[0], 0.4f, BYTE);
    EXPECT_NEAR(texel[1], 0.2f, BYTE);
    store.read(blend(1), TexelRect{2, 2, 1, 1}, texel);
    EXPECT_NEAR(texel[0], 0.8f, BYTE) << "the other map stays";

    Op every = allOp(OpType::PaintBase);
    ASSERT_TRUE(apply(store, every).ok);
    store.read(blend(1), TexelRect{2, 2, 1, 1}, texel);
    EXPECT_EQ(texel[0], 0.0f);
}

TEST(TerrainOpsKernels, PaintByRulesLaterOverEarlier) {
    auto store = flatStore(0.0f);
    fillHeight(*store, SIZE, SIZE, [](int x, int) { return x < 4 ? 0.2f : 0.8f; });
    Op op = allOp(OpType::PaintByRules);
    PaintRule ground;
    ground.layer = 1;
    PaintRule peaks;
    peaks.layer = 2;
    peaks.height = FilterRange{5.0f, 10.0f};
    op.rules = {ground, peaks};
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok) << result.error;
    std::vector<float> texel(4);
    store->read(blend(0), TexelRect{1, 1, 1, 1}, texel);
    EXPECT_NEAR(texel[1], 1.0f, BYTE);
    EXPECT_NEAR(texel[2], 0.0f, BYTE);
    store->read(blend(0), TexelRect{6, 1, 1, 1}, texel);
    EXPECT_NEAR(texel[1], 0.0f, BYTE);
    EXPECT_NEAR(texel[2], 1.0f, BYTE);
}

TEST(TerrainOpsKernels, PaintDensityGoesTowardOne) {
    RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
    addFlatHeight(store, SIZE, 0.0f);
    store.addMap(density(1), SIZE, SIZE, 1, 8);
    Op op = allOp(OpType::PaintDensity);
    op.layer = 1;
    op.strength = 0.5f;
    ASSERT_TRUE(apply(store, op).ok);
    std::vector<float> value(1);
    store.read(density(1), TexelRect{3, 3, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.5f, BYTE);
    ASSERT_TRUE(apply(store, op).ok);
    store.read(density(1), TexelRect{3, 3, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.75f, 1.5f * BYTE);
}

TEST(TerrainOpsKernels, EraseDensityGoesTowardZero) {
    RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
    addFlatHeight(store, SIZE, 0.0f);
    store.addMap(density(0), SIZE, SIZE, 1, 8);
    const std::vector<float> full(SIZE * SIZE, 1.0f);
    store.write(density(0), TexelRect{0, 0, SIZE, SIZE}, full);
    Op op = allOp(OpType::EraseDensity);
    op.layer = 0;
    op.strength = 0.75f;
    ASSERT_TRUE(apply(store, op).ok);
    std::vector<float> value(1);
    store.read(density(0), TexelRect{3, 3, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.25f, BYTE);
}

TEST(TerrainOpsKernels, PlaceObjects) {
    auto store = flatStore(0.0f);
    Op op = circleOp(OpType::PlaceObjects, 0.0f, 0.0f, 3.0f);
    op.count = 5;
    op.spacing = 0.1f;
    op.seed = 2;
    op.minScale = 1.0f;
    op.maxScale = 2.0f;
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].plan.attempts, 5);
    ASSERT_FALSE(result.ops[0].plan.objects.empty());
    for (const PlacedObject& object : result.ops[0].plan.objects) {
        EXPECT_LE(object.x * object.x + object.z * object.z, 9.001f);
        EXPECT_GE(object.scale, 1.0f);
        EXPECT_LE(object.scale, 2.0f);
    }
    EXPECT_EQ(*result.ops[0].seed, 2u);
    EXPECT_TRUE(result.changed.empty()) << "entities, not maps";
}

TEST(TerrainOpsKernels, EraseObjects) {
    auto store = flatStore(0.0f);
    Op op = circleOp(OpType::EraseObjects, 0.0f, 0.0f, 2.0f);
    op.existingObjects = {Vector2(0.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(3.0f, 0.0f)};
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].erasedObjects, (std::vector<int>{0, 1}));
}

TEST(TerrainOpsKernels, ImportHeightmapIsADeltaOfTheSameSize) {
    auto store = flatStore(0.5f);
    Op op = allOp(OpType::ImportHeightmap);
    op.importedWidth = SIZE;
    op.importedHeight = SIZE;
    op.importedHeights.assign(SIZE * SIZE, 0.5f);
    op.importedHeights[3] = 0.75f;
    op.importedHeights[4] = 1.5f;
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.ops[0].changedTexels, 2) << "the texels that match keep their bytes";
    EXPECT_EQ(result.ops[0].clippedHigh, 1);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(heightAt(after, 3, 0), 0.75f, QUANTUM);
    EXPECT_EQ(heightAt(after, 4, 0), 1.0f);

    Op otherSize = op;
    otherSize.importedWidth = SIZE * 2;
    otherSize.importedHeight = SIZE / 2;
    const TerrainOpResult refused = apply(*store, otherSize);
    EXPECT_FALSE(refused.ok) << "another size is the structural path";
    EXPECT_EQ(refused.errors[0].field, "ops[0].resolution");

    Op shaped = circleOp(OpType::ImportHeightmap, 0.0f, 0.0f, 2.0f);
    shaped.importedWidth = SIZE;
    shaped.importedHeight = SIZE;
    shaped.importedHeights = op.importedHeights;
    EXPECT_FALSE(apply(*store, shaped).ok) << "it takes no shape";
}

TEST(TerrainOpsKernels, FlatFill) {
    auto store = flatStore(0.25f);
    Op op = allOp(OpType::FillNoise);
    op.baseHeight = 0.75f;
    ASSERT_TRUE(apply(*store, op).ok);
    expectAll(*store, 0.75f);
}

TEST(TerrainOpsKernels, FractalFillKeepsItsBytes) {
    auto store = flatStore(0.0f);
    Op op = allOp(OpType::FillNoise);
    op.fill = FillMode::FractalNoise;
    op.baseHeight = 0.5f;
    op.amplitude = 0.25f;
    op.frequency = 3.0f;
    op.octaves = 4;
    op.seed = 7;
    ASSERT_TRUE(apply(*store, op).ok);

    // set_terrain_maps' own loop (editor/mcp/TerrainMaps.cpp)
    const std::vector<uint8_t> bytes = readAllBytes(*store, HEIGHT);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float sum = 0.0f, weight = 1.0f, weightSum = 0.0f, f = 3.0f;
            for (int o = 0; o < 4; o++) {
                const float nx = (static_cast<float>(x) / static_cast<float>(SIZE - 1)) * f;
                const float ny = (static_cast<float>(y) / static_cast<float>(SIZE - 1)) * f;
                sum += (TerrainNoise::value(nx, ny, 7 + static_cast<uint32_t>(o * 1013)) * 2.0f - 1.0f) * weight;
                weightSum += weight;
                weight *= 0.5f;
                f *= 2.0f;
            }
            const float h = std::max(0.0f, std::min(1.0f, 0.5f + sum / weightSum * 0.25f));
            const unsigned int quantized = static_cast<unsigned int>(std::lround(h * 65535.0f));
            const size_t at = (static_cast<size_t>(y) * SIZE + x) * 2;
            ASSERT_EQ(static_cast<unsigned int>(bytes[at] | (bytes[at + 1] << 8)), quantized) << x << ", " << y;
        }
    }
}

TEST(TerrainOpsKernels, RandomNoiseIsSeededAndPortable) {
    auto run = [](uint32_t seed) {
        RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
        addFlatHeight(store, SIZE, 0.0f);
        Op op;
        op.type = OpType::FillNoise;
        op.fill = FillMode::RandomNoise;
        op.baseHeight = 0.5f;
        op.amplitude = 0.25f;
        op.seed = seed;
        apply(store, op);
        return mapHash(store, HEIGHT);
    };
    EXPECT_EQ(run(5), run(5));
    EXPECT_NE(run(5), run(6));
    // The per-texel hash's own value: a standard-library distribution could not pin this
    EXPECT_EQ(detHash01(3, 4, 5), TerrainNoise::hash(3, 4, 5));
}

// ------------------------------------------------------------------ every shape, on one op

TEST(TerrainOpsKernels, ShapeCircleFalloffs) {
    auto measure = [](Falloff falloff, float feather) {
        auto store = flatStore(0.0f);
        Op op = circleOp(OpType::Raise, 0.5f, 0.5f, 4.0f);   // on texel (4, 4)'s centre
        op.shape.falloff = falloff;
        op.shape.feather = feather;
        op.delta = 10.0f;
        EXPECT_TRUE(apply(*store, op).ok);
        return readHeights(*store);
    };
    {
        const std::vector<float> h = measure(Falloff::Linear, -1.0f);
        EXPECT_NEAR(heightAt(h, 4, 4), 1.0f, QUANTUM);
        EXPECT_NEAR(heightAt(h, 6, 4), 0.5f, QUANTUM);
        EXPECT_NEAR(heightAt(h, 7, 4), 0.25f, QUANTUM);
    }
    {
        const std::vector<float> h = measure(Falloff::Smooth, -1.0f);
        EXPECT_NEAR(heightAt(h, 6, 4), 0.5f, QUANTUM);     // t = 0.5
        EXPECT_NEAR(heightAt(h, 7, 4), 0.15625f, QUANTUM); // t = 0.25: t * t * (3 - 2t)
    }
    {
        const std::vector<float> h = measure(Falloff::Constant, -1.0f);
        EXPECT_NEAR(heightAt(h, 7, 4), 1.0f, QUANTUM);
        EXPECT_NEAR(heightAt(h, 4, 0), 1.0f, QUANTUM) << "on the rim";
    }
    {
        // a quarter of the radius feathers: solid out to 3, 0 at 4
        const std::vector<float> h = measure(Falloff::Linear, 0.25f);
        EXPECT_NEAR(heightAt(h, 7, 4), 1.0f, QUANTUM);
    }
}

TEST(TerrainOpsKernels, ShapeRectRotated) {
    auto store = flatStore(0.0f);
    Op op;
    op.type = OpType::SetHeight;
    op.height = 10.0f;
    op.shape.type = ShapeType::Rect;
    op.shape.min = Vector2(-3.0f, -0.6f);
    op.shape.max = Vector2(3.0f, 0.6f);
    op.shape.rotation = 90.0f;
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> h = readHeights(*store);
    // turned a quarter: long along z now
    EXPECT_NEAR(heightAt(h, 3, 1), 1.0f, QUANTUM);   // (-0.5, -2.5)
    EXPECT_NEAR(heightAt(h, 1, 3), 0.0f, QUANTUM);   // (-2.5, -0.5)
}

TEST(TerrainOpsKernels, ShapePolylineUnion) {
    auto store = flatStore(0.0f);
    Op op;
    op.type = OpType::SetHeight;
    op.height = 10.0f;
    op.shape.type = ShapeType::Polyline;
    op.shape.points = {Vector2(-3.5f, -3.5f), Vector2(3.5f, -3.5f), Vector2(3.5f, 3.5f)};
    op.shape.radius = 0.5f;
    op.shape.falloff = Falloff::Constant;
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> h = readHeights(*store);
    EXPECT_NEAR(heightAt(h, 3, 0), 1.0f, QUANTUM);
    EXPECT_NEAR(heightAt(h, 7, 3), 1.0f, QUANTUM);
    EXPECT_NEAR(heightAt(h, 3, 3), 0.0f, QUANTUM);
    // one application: where the capsules overlap it does not count twice
    EXPECT_NEAR(heightAt(h, 7, 0), 1.0f, QUANTUM);
}

TEST(TerrainOpsKernels, ShapePolygonInside) {
    auto store = flatStore(0.0f);
    Op op;
    op.type = OpType::SetHeight;
    op.height = 10.0f;
    op.shape.type = ShapeType::Polyline;
    op.shape.closed = true;
    op.shape.points = {Vector2(-3.0f, -3.0f), Vector2(3.0f, -3.0f), Vector2(3.0f, 3.0f), Vector2(-3.0f, 3.0f)};
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> h = readHeights(*store);
    EXPECT_NEAR(heightAt(h, 4, 4), 1.0f, QUANTUM);
    EXPECT_NEAR(heightAt(h, 1, 1), 1.0f, QUANTUM);
    EXPECT_NEAR(heightAt(h, 0, 4), 0.0f, QUANTUM);
}

TEST(TerrainOpsKernels, ShapeMaskWeights) {
    auto store = flatStore(0.0f);
    Op op;
    op.type = OpType::SetHeight;
    op.height = 10.0f;
    op.shape.type = ShapeType::Mask;
    op.shape.size = Vector2(8.0f, 8.0f);
    op.shape.maskWidth = 2;
    op.shape.maskHeight = 1;
    op.shape.maskPixels = {0.0f, 1.0f};
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> h = readHeights(*store);
    // weight 0 at the left edge, 1 at the right; lerp from 0 to 1 is the weight
    EXPECT_NEAR(heightAt(h, 0, 3), 0.0625f, QUANTUM * 2.0f);
    EXPECT_NEAR(heightAt(h, 7, 3), 0.9375f, QUANTUM * 2.0f);
}

TEST(TerrainOpsKernels, ShapeStrokeStampsAccumulate) {
    // Each stamp deposits strength * falloff * flow * 1/60 (R34). A constant falloff makes the sum exact.
    auto store = flatStore(0.0f);
    Op op;
    op.type = OpType::Raise;
    op.shape.type = ShapeType::Stroke;
    op.shape.points = {Vector2(0.5f, 0.5f)};
    op.shape.radius = 2.0f;
    op.shape.falloff = Falloff::Constant;
    ASSERT_TRUE(apply(*store, op).ok);
    EXPECT_NEAR(heightAt(readHeights(*store), 4, 4), 1.5f / 60.0f, QUANTUM) << "a click is one stamp";

    auto dragged = flatStore(0.0f);
    Op drag = op;
    drag.shape.points = {Vector2(0.5f, 0.5f), Vector2(0.5f, 1.5f)};
    drag.shape.spacing = 0.5f;
    ASSERT_TRUE(apply(*dragged, drag).ok);
    EXPECT_NEAR(heightAt(readHeights(*dragged), 4, 4), 3.0f * 1.5f / 60.0f, QUANTUM * 2.0f) << "start, then each half unit";
}

TEST(TerrainOpsKernels, ShapeStrokeFollowsPressure) {
    auto run = [](bool strength, bool size) {
        auto store = flatStore(0.0f);
        Op op;
        op.type = OpType::Raise;
        op.shape.type = ShapeType::Stroke;
        op.shape.points = {Vector2(0.5f, 0.5f)};
        op.shape.pressures = {0.5f};
        op.shape.radius = 2.0f;
        op.shape.falloff = Falloff::Constant;
        op.shape.pressureStrength = strength;
        op.shape.pressureSize = size;
        EXPECT_TRUE(apply(*store, op).ok);
        return readHeights(*store);
    };
    EXPECT_NEAR(heightAt(run(true, false), 4, 4), 0.5f * 1.5f / 60.0f, QUANTUM);
    const std::vector<float> smaller = run(false, true);
    EXPECT_NEAR(heightAt(smaller, 4, 4), 1.5f / 60.0f, QUANTUM);
    EXPECT_NEAR(heightAt(smaller, 6, 4), 0.0f, QUANTUM);
}
