// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Holes and layer addressing (T1 R12, R13, R92, DC3; AC8, AC9)

#include "TerrainTestSupport.h"

#include "terrain/TerrainHoles.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainSurface.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

using namespace terraintest;

namespace {

    constexpr int SIZE = 16;

    Op cut(float x, float z, float radius) {
        return circleOp(OpType::CutHoles, x, z, radius);
    }

    // A store that keeps its maps: T3 R29's holeMapHolds, T4's per-tile maps
    class KeepingStore final : public RawMapStore {
    public:
        using RawMapStore::RawMapStore;
        bool canRemoveMap(MapId) const override { return false; }
    };

}

TEST(TerrainHoles, IsHoleTexelRule) {
    // 2 x 1 map: left a hole, right solid
    const uint8_t texels[] = {0, 255};
    const HoleMapView view{texels, 2, 1};
    EXPECT_TRUE(isHole(view, 0.0f, 0.5f));
    EXPECT_TRUE(isHole(view, 0.49f, 0.5f));
    EXPECT_FALSE(isHole(view, 0.5f, 0.5f)) << "a point on a texel edge belongs to the higher texel";
    EXPECT_FALSE(isHole(view, 1.0f, 0.5f)) << "the last edge belongs to the last texel";
    EXPECT_FALSE(isHole(view, -0.01f, 0.5f)) << "outside is not a hole";
    EXPECT_FALSE(isHole(view, 0.25f, 1.01f));
    EXPECT_FALSE(isHole(view, std::numeric_limits<float>::quiet_NaN(), 0.5f));
    EXPECT_FALSE(isHole(HoleMapView{}, 0.25f, 0.5f)) << "absent means no holes";
}

TEST(TerrainHoles, CutFillAndQuery) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    const uint64_t heightBefore = mapHash(store, HEIGHT);
    EXPECT_FALSE(store.format(HOLE).exists);

    // The first cut creates the map at the height map's resolution
    const TerrainOpResult first = apply(store, cut(0.0f, 0.0f, 3.0f));
    ASSERT_TRUE(first.ok) << first.error;
    ASSERT_TRUE(store.format(HOLE).exists);
    EXPECT_EQ(store.format(HOLE).bounds.w, SIZE);
    EXPECT_EQ(store.format(HOLE).channels, 1);
    EXPECT_EQ(store.format(HOLE).bitsPerChannel, 8);
    const OpEntry& entry = first.ops[0];
    EXPECT_TRUE(entry.mapCreated);
    EXPECT_FALSE(entry.mapRemoved);
    EXPECT_GT(entry.changedTexels, 0);
    EXPECT_EQ(entry.holeTexels, entry.changedTexels);
    EXPECT_FLOAT_EQ(entry.holeFraction, static_cast<float>(entry.holeTexels) / (SIZE * SIZE));
    EXPECT_TRUE(first.hasWarning("holes_not_rendered"));
    ASSERT_EQ(first.createdMaps.size(), 1u);
    EXPECT_EQ(first.createdMaps[0], HOLE);
    ASSERT_EQ(first.changed.size(), 1u);
    EXPECT_EQ(first.changed[0].map, HOLE);
    EXPECT_EQ(first.changed[0].texels, entry.changedTexels);

    // 0 and 255 only; height bytes untouched
    for (uint8_t byte : readAllBytes(store, HOLE)) {
        EXPECT_TRUE(byte == 0 || byte == 255) << static_cast<int>(byte);
    }
    EXPECT_EQ(mapHash(store, HEIGHT), heightBefore);

    // The texel-centre rule: isHole at a centre answers what the cut wrote
    const std::vector<uint8_t> holes = readAllBytes(store, HOLE);
    const HoleMapView view{holes.data(), SIZE, SIZE};
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            const float localX = static_cast<float>(x) + 0.5f - 8.0f;
            const float localZ = static_cast<float>(y) + 0.5f - 8.0f;
            const bool expected = localX * localX + localZ * localZ <= 9.0f;
            EXPECT_EQ(isHole(view, (localX + 8.0f) / SIZE, (localZ + 8.0f) / SIZE), expected) << x << ", " << y;
        }
    }

    // A second identical cut changes 0 texels
    const TerrainOpResult second = apply(store, cut(0.0f, 0.0f, 3.0f));
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ops[0].changedTexels, 0);
    EXPECT_EQ(second.ops[0].holeTexels, entry.holeTexels);
    EXPECT_FALSE(second.ops[0].mapCreated);

    // A partial fill keeps the map
    Op partial = circleOp(OpType::FillHoles, 0.0f, 0.0f, 1.0f);
    const TerrainOpResult filled = apply(store, partial);
    ASSERT_TRUE(filled.ok);
    EXPECT_GT(filled.ops[0].changedTexels, 0);
    EXPECT_FALSE(filled.ops[0].mapRemoved);
    EXPECT_TRUE(store.format(HOLE).exists);

    // fill_holes with no shape fills all, and the last fill removes the map
    Op all;
    all.type = OpType::FillHoles;
    const TerrainOpResult removed = apply(store, all);
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_TRUE(removed.ops[0].mapRemoved);
    EXPECT_EQ(removed.ops[0].holeTexels, 0);
    ASSERT_EQ(removed.removedMaps.size(), 1u);
    EXPECT_FALSE(store.format(HOLE).exists);
    EXPECT_EQ(mapHash(store, HEIGHT), heightBefore) << "filling restores the surface exactly";
}

TEST(TerrainHoles, NoMapMeansNothingToFillAndAnExcludedCutMakesNone) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    Op fill;
    fill.type = OpType::FillHoles;
    const TerrainOpResult result = apply(store, fill);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].changedTexels, 0);
    EXPECT_FALSE(result.ops[0].mapCreated);
    EXPECT_FALSE(store.format(HOLE).exists);

    Op filtered = cut(0.0f, 0.0f, 3.0f);
    filtered.filter.height = FilterRange{7.0f, 8.0f};
    const TerrainOpResult excluded = apply(store, filtered);
    ASSERT_TRUE(excluded.ok);
    EXPECT_FALSE(store.format(HOLE).exists);
    EXPECT_GT(excluded.ops[0].filteredOut, 0);
    EXPECT_TRUE(excluded.hasWarning("filter_excluded_all"));
    EXPECT_FALSE(excluded.hasWarning("holes_not_rendered"));
}

TEST(TerrainHoles, DryRunChangesNothing) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    OpOptions dry;
    dry.dryRun = true;
    const TerrainOpResult preview = apply(store, cut(0.0f, 0.0f, 3.0f), dry);
    ASSERT_TRUE(preview.ok);
    EXPECT_TRUE(preview.ops[0].mapCreated);
    EXPECT_GT(preview.ops[0].changedTexels, 0);
    EXPECT_FALSE(store.format(HOLE).exists);
}

TEST(TerrainHoles, CutThenFillInOneListLeavesNoMap) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    Op fill;
    fill.type = OpType::FillHoles;
    const Op list[] = {cut(0.0f, 0.0f, 3.0f), fill};
    const TerrainOpResult result = applyOps(store, list);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(store.format(HOLE).exists);
    EXPECT_TRUE(result.createdMaps.empty());
    EXPECT_TRUE(result.changed.empty());
}

TEST(TerrainHoles, RemoveThenCutInOneListKeepsTheMap) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    ASSERT_TRUE(apply(store, cut(0.0f, 0.0f, 3.0f)).ok);
    Op fill;
    fill.type = OpType::FillHoles;
    const Op list[] = {fill, cut(4.0f, 4.0f, 2.0f)};
    const TerrainOpResult result = applyOps(store, list);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(store.format(HOLE).exists);
    const std::vector<uint8_t> bytes = readAllBytes(store, HOLE);
    EXPECT_EQ(bytes[static_cast<size_t>(8) * SIZE + 8], 255) << "the old hole went with the fill";
    EXPECT_EQ(bytes[static_cast<size_t>(12) * SIZE + 12], 0) << "the new one is there";
}

TEST(TerrainHoles, AStoreMayKeepTheMap) {
    KeepingStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    ASSERT_TRUE(apply(store, cut(0.0f, 0.0f, 3.0f)).ok);
    ASSERT_TRUE(store.format(HOLE).exists);

    Op fill;
    fill.type = OpType::FillHoles;
    const TerrainOpResult result = apply(store, fill);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.ops[0].mapRemoved);
    EXPECT_TRUE(result.removedMaps.empty());
    EXPECT_EQ(result.ops[0].holeTexels, 0);
    ASSERT_TRUE(store.format(HOLE).exists) << "all solid, and still there";
    for (uint8_t byte : readAllBytes(store, HOLE)) {
        EXPECT_EQ(byte, 255);
    }
}

TEST(TerrainHoles, RegionKinds) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);

    // A mask's threshold is 0.5: of a 2-texel mask, only the side at or above it
    Op masked;
    masked.type = OpType::CutHoles;
    masked.shape.type = ShapeType::Mask;
    masked.shape.size = Vector2(16.0f, 16.0f);
    masked.shape.maskWidth = 1;
    masked.shape.maskHeight = 2;
    masked.shape.maskPixels = {1.0f, 0.0f};
    ASSERT_TRUE(apply(store, masked).ok);
    const std::vector<uint8_t> holes = readAllBytes(store, HOLE);
    EXPECT_EQ(holes[static_cast<size_t>(2) * SIZE + 8], 0) << "the mask's bright row";
    EXPECT_EQ(holes[static_cast<size_t>(14) * SIZE + 8], 255) << "the mask's dark row";

    // A polygon's inside
    Op fill;
    fill.type = OpType::FillHoles;
    ASSERT_TRUE(apply(store, fill).ok);
    Op polygon;
    polygon.type = OpType::CutHoles;
    polygon.shape.type = ShapeType::Polyline;
    polygon.shape.closed = true;
    polygon.shape.points = {Vector2(-2.0f, -2.0f), Vector2(2.0f, -2.0f), Vector2(2.0f, 2.0f), Vector2(-2.0f, 2.0f)};
    const TerrainOpResult result = apply(store, polygon);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].changedTexels, 16);
}

TEST(TerrainHoles, QueryStaysDefinedUnderAHole) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    ASSERT_TRUE(apply(store, cut(0.0f, 0.0f, 3.0f)).ok);
    const std::vector<uint8_t> heights = readAllBytes(store, HEIGHT);
    const std::vector<uint8_t> holes = readAllBytes(store, HOLE);
    SurfaceView view;
    view.pixels = heights.data();
    view.width = SIZE;
    view.height = SIZE;
    view.channels = 1;
    view.bytesPerChannel = 2;
    view.size = static_cast<float>(SIZE);
    view.maxHeight = 8.0f;
    view.holes = HoleMapView{holes.data(), SIZE, SIZE};
    const SurfaceSample under = sampleSurface(view, 0.5f, 0.5f);
    EXPECT_TRUE(under.hole);
    EXPECT_NEAR(under.height, 4.0f, 1e-3f);
    EXPECT_FLOAT_EQ(under.normal.y, 1.0f);
    EXPECT_FALSE(sampleSurface(view, 7.0f, 7.0f).hole);
}

TEST(TerrainLayers, ChannelMappingIsTheOnlyRoute) {
    // Three layers to a blend map, in order
    for (int layer = 0; layer < 9; layer++) {
        const LayerChannel channel = terrainLayerChannel(layer);
        EXPECT_EQ(channel.map, layer / 3);
        EXPECT_EQ(channel.channel, layer % 3);
        EXPECT_FALSE(channel.inverted);
    }
    EXPECT_EQ(terrainBlendMapCount(1), 1);
    EXPECT_EQ(terrainBlendMapCount(3), 1);
    EXPECT_EQ(terrainBlendMapCount(4), 2);

    // Nothing else in the folder does this arithmetic (T3 R15 changes it for layers 10-12)
    const std::filesystem::path folder = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path() /
                                         "engine" / "core" / "terrain";
    ASSERT_TRUE(std::filesystem::is_directory(folder)) << folder;
    for (const auto& file : std::filesystem::directory_iterator(folder)) {
        if (file.path().filename() == "TerrainLayers.h") continue;
        std::ifstream input(file.path());
        std::stringstream text;
        text << input.rdbuf();
        const std::string source = text.str();
        EXPECT_EQ(source.find("layer / 3"), std::string::npos) << file.path();
        EXPECT_EQ(source.find("layer % 3"), std::string::npos) << file.path();
        EXPECT_EQ(source.find("MAX_TERRAIN_LAYERS"), std::string::npos) << file.path();
    }
}

TEST(TerrainLayers, RemapMovesPaintAcrossBlendMaps) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    store.addMap(blend(0), SIZE, SIZE, 4, 8);
    // layer 1 holds a gradient
    std::vector<float> weights;
    for (int i = 0; i < SIZE * SIZE; i++) {
        weights.insert(weights.end(), {0.0f, static_cast<float>(i % 251) / 250.0f, 0.0f, 1.0f});
    }
    store.write(blend(0), TexelRect{0, 0, SIZE, SIZE}, weights);
    const std::vector<uint8_t> before = readAllBytes(store, blend(0));

    // Layer 1 moves to index 5; 2, 3, 4 and 5 each move down one
    Op op;
    op.type = OpType::RemapLayers;
    op.order = {0, 5, 1, 2, 3, 4};
    const TerrainOpResult result = apply(store, op);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.ops[0].layerOrder, op.order);
    ASSERT_TRUE(store.format(blend(1)).exists) << "the second map was made for it";

    const std::vector<uint8_t> first = readAllBytes(store, blend(0));
    const std::vector<uint8_t> second = readAllBytes(store, blend(1));
    for (size_t i = 0; i < static_cast<size_t>(SIZE) * SIZE; i++) {
        ASSERT_EQ(second[i * 4 + 2], before[i * 4 + 1]) << "layer 1's weights, exactly, at layer 5";
        ASSERT_EQ(first[i * 4 + 1], 0) << "the old place holds the old layer 2, which was empty";
    }
}

TEST(TerrainLayers, RemapRoundTripIsTheOriginalPaint) {
    RawMapStore store(static_cast<float>(SIZE), 8.0f);
    addFlatHeight(store, SIZE, 0.5f);
    store.addMap(blend(0), SIZE, SIZE, 4, 8);
    std::vector<float> weights;
    for (int i = 0; i < SIZE * SIZE; i++) {
        weights.insert(weights.end(), {static_cast<float>(i % 7) / 7.0f, static_cast<float>(i % 5) / 5.0f, static_cast<float>(i % 3) / 3.0f, 1.0f});
    }
    store.write(blend(0), TexelRect{0, 0, SIZE, SIZE}, weights);
    const std::vector<uint8_t> before = readAllBytes(store, blend(0));

    Op forward;
    forward.type = OpType::RemapLayers;
    forward.order = {2, 0, 1};
    ASSERT_TRUE(apply(store, forward).ok);
    EXPECT_NE(readAllBytes(store, blend(0)), before);
    Op back;
    back.type = OpType::RemapLayers;
    back.order = {1, 2, 0};
    ASSERT_TRUE(apply(store, back).ok);
    EXPECT_EQ(readAllBytes(store, blend(0)), before);
}
