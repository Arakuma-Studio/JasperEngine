// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Filters on any op (T1 R6, AC4)

#include "TerrainTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace terraintest;

namespace {

    constexpr int SIZE = 32;
    constexpr float MAX_HEIGHT = 32.0f;

    // World-unit texels on a 32 map: x below 16 is a flat floor at height 4; x from 16 climbs one
    // unit per texel (45 degrees), so slope and height tell the texels apart exactly
    std::unique_ptr<RawMapStore> rampStore() {
        auto store = std::make_unique<RawMapStore>(static_cast<float>(SIZE), MAX_HEIGHT);
        store->addMap(HEIGHT, SIZE, SIZE, 1, 16);
        fillHeight(*store, SIZE, SIZE, [](int x, int) {
            return (x < 16 ? 4.0f : 4.0f + static_cast<float>(x - 15)) / MAX_HEIGHT;
        });
        store->addMap(blend(0), SIZE, SIZE, 4, 8);
        std::vector<float> weights;
        for (int y = 0; y < SIZE; y++) {
            for (int x = 0; x < SIZE; x++) {
                weights.insert(weights.end(), {0.0f, y < 16 ? 1.0f : 0.0f, 0.0f, 1.0f});
            }
        }
        store->write(blend(0), TexelRect{0, 0, SIZE, SIZE}, weights);
        return store;
    }

    // The columns the op changed on a row
    std::vector<int> changedColumns(const TerrainMapStore& store, const std::vector<float>& before, int row = 8) {
        const std::vector<float> after = readHeights(store);
        std::vector<int> columns;
        for (int x = 0; x < SIZE; x++) {
            const size_t at = static_cast<size_t>(row) * SIZE + x;
            if (after[at] != before[at]) columns.push_back(x);
        }
        return columns;
    }

    Op raiseAll() {
        Op op;
        op.type = OpType::Raise;
        op.delta = 1.0f;
        return op;
    }

    std::vector<int> range(int first, int last) {
        std::vector<int> values;
        for (int i = first; i <= last; i++) {
            values.push_back(i);
        }
        return values;
    }

}

TEST(TerrainFilter, SlopeHeightLayerAndInvert) {
    // slope: the forward difference at x sees the next texel, so column 15 is the first steep one,
    // and the map's last column, whose difference clamps flat, counts as flat
    {
        auto store = rampStore();
        const std::vector<float> before = readHeights(*store);
        Op op = raiseAll();
        op.filter.slope = FilterRange{0.0f, 10.0f};
        const TerrainOpResult result = apply(*store, op);
        ASSERT_TRUE(result.ok);
        std::vector<int> flat = range(0, 14);
        flat.push_back(31);
        EXPECT_EQ(changedColumns(*store, before), flat);
        EXPECT_EQ(result.ops[0].filteredOut, static_cast<int64_t>(SIZE) * 16);
        EXPECT_EQ(result.ops[0].changedTexels, static_cast<int64_t>(SIZE) * 16);
    }
    {
        auto store = rampStore();
        const std::vector<float> before = readHeights(*store);
        Op op = raiseAll();
        op.filter.slope = FilterRange{40.0f, 50.0f};
        ASSERT_TRUE(apply(*store, op).ok);
        EXPECT_EQ(changedColumns(*store, before), range(15, 30)) << "the 45-degree climb";
    }
    // height, in the op's units: 4.5 to 8.5 holds columns 16 to 19 (5, 6, 7, 8)
    {
        auto store = rampStore();
        const std::vector<float> before = readHeights(*store);
        Op op = raiseAll();
        op.filter.height = FilterRange{4.5f, 8.5f};
        ASSERT_TRUE(apply(*store, op).ok);
        EXPECT_EQ(changedColumns(*store, before), range(16, 19));
    }
    // layer weight: row 8 is in layer 1's half, row 24 is not
    {
        auto store = rampStore();
        const std::vector<float> before = readHeights(*store);
        Op op = raiseAll();
        op.filter.layer = LayerRange{1, 0.5f, 1.0f};
        ASSERT_TRUE(apply(*store, op).ok);
        EXPECT_EQ(changedColumns(*store, before, 8).size(), static_cast<size_t>(SIZE));
        EXPECT_TRUE(changedColumns(*store, before, 24).empty());
    }
    // invert is the complement, whichever way the filter cut
    {
        auto plain = rampStore();
        auto inverted = rampStore();
        const std::vector<float> before = readHeights(*plain);
        Op op = raiseAll();
        op.filter.height = FilterRange{4.5f, 8.5f};
        ASSERT_TRUE(apply(*plain, op).ok);
        op.filter.invert = true;
        ASSERT_TRUE(apply(*inverted, op).ok);
        std::vector<int> both = changedColumns(*plain, before);
        const std::vector<int> other = changedColumns(*inverted, before);
        both.insert(both.end(), other.begin(), other.end());
        std::sort(both.begin(), both.end());
        EXPECT_EQ(both, range(0, SIZE - 1));
    }
}

TEST(TerrainFilter, OutOfFilterBytesAreUnchanged) {
    auto store = rampStore();
    const std::vector<uint8_t> before = readAllBytes(*store, HEIGHT);
    Op op = raiseAll();
    op.filter.height = FilterRange{4.5f, 8.5f};
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<uint8_t> after = readAllBytes(*store, HEIGHT);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            if (x >= 16 && x <= 19) continue;
            const size_t at = (static_cast<size_t>(y) * SIZE + x) * 2;
            ASSERT_EQ(after[at], before[at]);
            ASSERT_EQ(after[at + 1], before[at + 1]);
        }
    }
}

TEST(TerrainFilter, SoftnessBandValues) {
    // On a range of width 4 with softness 0.5: a band of 1 inside each end
    EXPECT_FLOAT_EQ(softRange(2.0f, 2.0f, 6.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(softRange(2.5f, 2.0f, 6.0f, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(softRange(3.0f, 2.0f, 6.0f, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(softRange(4.0f, 2.0f, 6.0f, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(softRange(5.5f, 2.0f, 6.0f, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(softRange(1.9f, 2.0f, 6.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(softRange(2.0f, 2.0f, 6.0f, 0.0f), 1.0f) << "hard at softness 0";

    // on the map: the op's value scales by the band
    auto store = rampStore();
    Op op = raiseAll();
    op.filter.height = FilterRange{4.5f, 8.5f};
    op.filter.softness = 1.0f;   // the whole range is band: a peak in the middle
    ASSERT_TRUE(apply(*store, op).ok);
    const std::vector<float> after = readHeights(*store);
    const float expected = softRange(6.0f, 4.5f, 8.5f, 1.0f);
    EXPECT_GT(expected, 0.0f);
    EXPECT_LT(expected, 1.0f);
    EXPECT_NEAR(after[static_cast<size_t>(8) * SIZE + 17], (6.0f + expected) / MAX_HEIGHT, 2.0f / 65535.0f);
}

TEST(TerrainFilter, SoftSlopeIsHardAtZeroAndFadesWithin) {
    const float at30 = detCos(detRadians(30.0f));
    EXPECT_FLOAT_EQ(softSlope(at30, 20.0f, 40.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(softSlope(1.0f, 20.0f, 40.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(softSlope(detCos(detRadians(50.0f)), 20.0f, 40.0f, 0.0f), 0.0f);
    const float mid = softSlope(detCos(detRadians(22.0f)), 20.0f, 40.0f, 0.5f);
    EXPECT_GT(mid, 0.0f);
    EXPECT_LT(mid, 1.0f);
    EXPECT_FLOAT_EQ(softSlope(at30, 20.0f, 40.0f, 0.5f), 1.0f);
}

TEST(TerrainFilter, ReadsTheStateFromBeforeTheOp) {
    // A height band a texel is raised out of still applies to it: it decides on what it was
    auto store = rampStore();
    Op op = raiseAll();
    op.delta = 8.0f;
    op.filter.height = FilterRange{0.0f, 5.0f};
    const TerrainOpResult result = apply(*store, op);
    ASSERT_TRUE(result.ok);
    const std::vector<float> after = readHeights(*store);
    EXPECT_NEAR(after[static_cast<size_t>(8) * SIZE], 12.0f / MAX_HEIGHT, 1.0f / 65535.0f);
}

TEST(TerrainFilter, StrokeFilterSeesEachStamp) {
    // A stroke's filter reads before each stamp: a height band a stamp lifts out of stops the rest
    RawMapStore store(static_cast<float>(SIZE), MAX_HEIGHT);
    addFlatHeight(store, SIZE, 4.0f / MAX_HEIGHT);
    Op op;
    op.type = OpType::Raise;
    op.delta = 1.0f;
    op.shape.type = ShapeType::Stroke;
    op.shape.points = {Vector2(0.5f, 0.5f), Vector2(0.5f, 1.0f), Vector2(0.5f, 1.5f)};
    op.shape.spacing = 0.5f;
    op.shape.radius = 3.0f;
    op.shape.falloff = Falloff::Constant;
    op.filter.height = FilterRange{0.0f, 4.01f};
    ASSERT_TRUE(apply(store, op).ok);
    // the first stamp passes, the rest see a raised texel and stop
    const float once = 1.0f * 1.5f / 60.0f;
    EXPECT_NEAR(readHeights(store)[static_cast<size_t>(16) * SIZE + 16], (4.0f + once) / MAX_HEIGHT, 2.0f / 65535.0f);
}

TEST(TerrainFilter, ABlendMapOfAnotherSizeSamplesTheHeightMapByPlace) {
    // Today's mask reads the surface at the texel's own centre, whatever the map sizes are
    RawMapStore store(32.0f, MAX_HEIGHT);
    store.addMap(HEIGHT, 32, 32, 1, 16);
    fillHeight(store, 32, 32, [](int x, int) { return x < 16 ? 0.1f : 0.9f; });
    store.addMap(blend(0), 8, 8, 4, 8);
    Op op;
    op.type = OpType::PaintLayer;
    op.layer = 0;
    op.filter.height = FilterRange{0.0f, 10.0f};
    ASSERT_TRUE(apply(store, op).ok);
    std::vector<float> texel(4);
    store.read(blend(0), TexelRect{1, 3, 1, 1}, texel);
    EXPECT_EQ(texel[0], 1.0f);
    store.read(blend(0), TexelRect{6, 3, 1, 1}, texel);
    EXPECT_EQ(texel[0], 0.0f);
}
