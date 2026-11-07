// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The absolute write route for generators (T1 R88, T2 N9, R30; AC69)

#include "TerrainTestSupport.h"

#include "terrain/TerrainLayers.h"

#include <gtest/gtest.h>

using namespace terraintest;

namespace {
    constexpr int SIZE = 8;
    constexpr float BYTE = 1.0f / 255.0f;
}

TEST(TerrainBatch, AbsoluteWritesAndOneUndo) {
    RawMapStore store(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(store, SIZE, 0.25f);
    store.addMap(density(0), SIZE, SIZE, 1, 8);
    const uint64_t heightBefore = mapHash(store, HEIGHT);

    const TexelRect rect{2, 2, 4, 4};
    const std::vector<float> heights(16, 0.75f);
    const std::vector<float> values(16, 0.5f);

    {
        Batch batch(store);
        std::vector<float> snapshot(16);
        ASSERT_TRUE(batch.readHeights(rect, snapshot));
        EXPECT_NEAR(snapshot[0], 0.25f, 1e-4f);

        ASSERT_TRUE(batch.setHeights(rect, heights));
        const int layers[] = {1};
        const std::vector<float> weights(16, 0.5f);
        ASSERT_TRUE(batch.setLayerWeights(rect, layers, weights));
        ASSERT_TRUE(batch.setDensity(rect, 0, values));

        EXPECT_NE(mapHash(batch.overlay(), HEIGHT), heightBefore);
        EXPECT_EQ(mapHash(store, HEIGHT), heightBefore) << "nothing lands before the commit";
        std::vector<float> seen(16);
        ASSERT_TRUE(batch.readHeights(rect, seen));
        EXPECT_NEAR(seen[0], 0.75f, 1e-4f) << "the batch reads its own writes";

        // one undo's worth: every map in one list of deltas
        int mapsTouched = 0;
        MapId last{MapKind::Height, -1};
        batch.overlay().visitTiles([&](MapId map, TexelRect, std::span<const uint8_t>, std::span<const uint8_t>) {
            if (map != last) mapsTouched++;
            last = map;
        });
        EXPECT_EQ(mapsTouched, 3);

        const TerrainOpResult result = batch.commit();
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_EQ(result.changed.size(), 3u);
        for (const MapChangeReport& change : result.changed) {
            if (change.map == HEIGHT) {
                EXPECT_EQ(change.rect, rect);
                EXPECT_EQ(change.texels, 16);
            }
        }
        ASSERT_EQ(result.createdMaps.size(), 1u);
        EXPECT_EQ(result.createdMaps[0], blend(0));
    }
    std::vector<float> value(1);
    store.read(HEIGHT, TexelRect{3, 3, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.75f, 1e-4f);
    store.read(HEIGHT, TexelRect{0, 0, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.25f, 1e-4f);
    store.read(density(0), TexelRect{3, 3, 1, 1}, value);
    EXPECT_NEAR(value[0], 0.5f, BYTE);
}

TEST(TerrainBatch, NoCommitChangesNothing) {
    RawMapStore store(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(store, SIZE, 0.25f);
    const uint64_t before = mapHash(store, HEIGHT);
    const std::vector<float> heights(64, 0.75f);
    {
        Batch batch(store);
        ASSERT_TRUE(batch.setHeights(TexelRect{0, 0, SIZE, SIZE}, heights));
    }
    EXPECT_EQ(mapHash(store, HEIGHT), before);

    Batch discarded(store);
    ASSERT_TRUE(discarded.setHeights(TexelRect{0, 0, SIZE, SIZE}, heights));
    discarded.discard();
    const TerrainOpResult result = discarded.commit();
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.changed.empty());
    EXPECT_EQ(mapHash(store, HEIGHT), before);
}

TEST(TerrainBatch, AFailedCallFailsTheCommit) {
    RawMapStore store(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(store, SIZE, 0.25f);
    const uint64_t before = mapHash(store, HEIGHT);
    Batch batch(store);
    const std::vector<float> heights(64, 0.75f);
    ASSERT_TRUE(batch.setHeights(TexelRect{0, 0, SIZE, SIZE}, heights));
    EXPECT_FALSE(batch.setHeights(TexelRect{4, 4, SIZE, SIZE}, heights)) << "past the map";
    const TerrainOpResult result = batch.commit();
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(mapHash(store, HEIGHT), before);
}

TEST(TerrainBatch, ClippingIsCountedNotAnError) {
    RawMapStore store(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(store, SIZE, 0.25f);
    Batch batch(store);
    const std::vector<float> values = {-0.5f, 0.5f, 1.5f, 2.0f};
    ASSERT_TRUE(batch.setHeights(TexelRect{0, 0, 2, 2}, values));
    const TerrainOpResult result = batch.commit();
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].clippedLow, 1);
    EXPECT_EQ(result.ops[0].clippedHigh, 2);
    EXPECT_TRUE(result.hasWarning("clipped_low"));
    EXPECT_TRUE(result.hasWarning("clipped_high"));
}

TEST(TerrainBatch, WeightEncoderRules) {
    float perLayer[6];
    {
        // Clamp: below 0 and above 1
        const int layers[] = {1, 2};
        const float values[] = {-1.0f, 0.5f};
        encodeLayerWeights(layers, values, perLayer);
        EXPECT_EQ(perLayer[1], 0.0f);
        EXPECT_EQ(perLayer[2], 0.5f);
        // Layer 0 takes the rest
        EXPECT_FLOAT_EQ(perLayer[0], 0.5f);
    }
    {
        // Normalize a sum above 1
        const int layers[] = {1, 2};
        const float values[] = {0.75f, 0.75f};
        encodeLayerWeights(layers, values, perLayer);
        EXPECT_FLOAT_EQ(perLayer[1], 0.5f);
        EXPECT_FLOAT_EQ(perLayer[2], 0.5f);
        EXPECT_EQ(perLayer[0], 0.0f);
    }
    {
        // Layer 0 with its own value takes no rest
        const int layers[] = {0, 1};
        const float values[] = {0.25f, 0.25f};
        encodeLayerWeights(layers, values, perLayer);
        EXPECT_FLOAT_EQ(perLayer[0], 0.25f);
        EXPECT_FLOAT_EQ(perLayer[1], 0.25f);
    }
    {
        // Unlisted layers get 0, whatever was there
        perLayer[5] = 0.9f;
        const int layers[] = {1};
        const float values[] = {1.0f};
        encodeLayerWeights(layers, values, perLayer);
        EXPECT_EQ(perLayer[5], 0.0f);
    }
}

TEST(TerrainBatch, WritesWeightsAcrossBlendMaps) {
    RawMapStore store(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(store, SIZE, 0.0f);
    const TexelRect rect{0, 0, 2, 1};
    const int layers[] = {4, 1};
    const float weights[] = {0.5f, 0.25f, 1.0f, 1.0f};
    Batch batch(store);
    ASSERT_TRUE(batch.setLayerWeights(rect, layers, weights));
    ASSERT_TRUE(batch.commit().ok);
    ASSERT_TRUE(store.format(blend(1)).exists) << "layer 4 lives in the second map";
    std::vector<float> first(8), second(8);
    store.read(blend(0), rect, first);
    store.read(blend(1), rect, second);
    // texel 0: layer 4 at 0.5, layer 1 at 0.25, layer 0 takes 0.25
    EXPECT_NEAR(second[terrainLayerChannel(4).channel], 0.5f, BYTE);
    EXPECT_NEAR(first[terrainLayerChannel(1).channel], 0.25f, BYTE);
    EXPECT_NEAR(first[0], 0.25f, BYTE);
    // texel 1: both 1, normalized to a half each, none left for layer 0
    EXPECT_NEAR(second[4 + terrainLayerChannel(4).channel], 0.5f, BYTE);
    EXPECT_NEAR(first[4 + terrainLayerChannel(1).channel], 0.5f, BYTE);
    EXPECT_NEAR(first[4], 0.0f, BYTE);
    EXPECT_EQ(first[3], 1.0f) << "alpha stays opaque";
}

TEST(TerrainBatch, ReportsAMissingMap) {
    RawMapStore noHeight(static_cast<float>(SIZE), 10.0f);
    Batch batch(noHeight);
    const float value = 0.5f;
    EXPECT_FALSE(batch.setHeights(TexelRect{0, 0, 1, 1}, std::span<const float>(&value, 1)));
    EXPECT_FALSE(batch.commit().ok);
}

TEST(TerrainBatch, PaintByRulesUsesTheSameEncoder) {
    // Two rules that together leave some for layer 0: the op and a Batch with the same numbers must
    // land on the same bytes, the rest for layer 0 included
    RawMapStore byRules(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(byRules, SIZE, 0.5f);
    Op op;
    op.type = OpType::PaintByRules;
    PaintRule first;
    first.layer = 1;
    first.opacity = 0.5f;
    PaintRule second;
    second.layer = 2;
    second.opacity = 0.25f;
    op.rules = {first, second};
    ASSERT_TRUE(apply(byRules, op).ok);

    RawMapStore byBatch(static_cast<float>(SIZE), 10.0f);
    addFlatHeight(byBatch, SIZE, 0.5f);
    // later over earlier: 0.5 fades by the second rule's 0.25, which lands 0.25
    const float layer1 = 0.5f * (1.0f - 0.25f);
    const float layer2 = 0.25f;
    std::vector<float> weights;
    for (int i = 0; i < SIZE * SIZE; i++) {
        weights.push_back(layer1);
        weights.push_back(layer2);
    }
    const int layers[] = {1, 2};
    Batch batch(byBatch);
    ASSERT_TRUE(batch.setLayerWeights(TexelRect{0, 0, SIZE, SIZE}, layers, weights));
    ASSERT_TRUE(batch.commit().ok);

    EXPECT_EQ(mapHash(byRules, blend(0)), mapHash(byBatch, blend(0)));
    std::vector<float> texel(4);
    byRules.read(blend(0), TexelRect{1, 1, 1, 1}, texel);
    EXPECT_NEAR(texel[0], 1.0f - layer1 - layer2, BYTE) << "layer 0 took the rest";
}
