// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The stores and the store contract (T1 R2, R3, R86, R87; AC1, AC2, AC68)

#include "TerrainGoldenSequences.h"
#include "TerrainTestSupport.h"

#include <gtest/gtest.h>

using namespace terraintest;

TEST(TerrainStore, RawAndOverlayAgree) {
    RawMapStore raw(32.0f, 10.0f);
    raw.addMap(HEIGHT, 128, 96, 1, 16);
    raw.addMap(blend(0), 128, 96, 4, 8);

    // Rectangles crossing tile borders in both directions
    const TexelRect rects[] = {{0, 0, 128, 96}, {60, 60, 10, 10}, {63, 0, 2, 96}, {0, 63, 128, 2}, {127, 95, 1, 1}};

    OverlayMapStore overlay(raw);
    for (const TexelRect& rect : rects) {
        std::vector<float> values(static_cast<size_t>(rect.area()));
        for (size_t i = 0; i < values.size(); i++) {
            values[i] = static_cast<float>(i % 1000) / 1000.0f;
        }
        ASSERT_TRUE(overlay.write(HEIGHT, rect, values).ok);

        std::vector<float> back(values.size());
        ASSERT_TRUE(overlay.read(HEIGHT, rect, back));
        EXPECT_EQ(back, values) << "floats stay as written";

        std::vector<uint8_t> overlayBytes(values.size() * 2);
        ASSERT_TRUE(overlay.readBytes(HEIGHT, rect, overlayBytes));

        RawMapStore plain(32.0f, 10.0f);
        plain.addMap(HEIGHT, 128, 96, 1, 16);
        ASSERT_TRUE(plain.write(HEIGHT, rect, values).ok);
        std::vector<uint8_t> plainBytes(values.size() * 2);
        ASSERT_TRUE(plain.readBytes(HEIGHT, rect, plainBytes));
        EXPECT_EQ(overlayBytes, plainBytes) << "both stores round the same way";

        std::vector<float> plainFloats(values.size());
        ASSERT_TRUE(plain.read(HEIGHT, rect, plainFloats));
        ASSERT_TRUE(raw.write(HEIGHT, rect, plainFloats).ok);
        ASSERT_TRUE(raw.read(HEIGHT, rect, back));
        EXPECT_EQ(back, plainFloats) << "decoded bytes survive a write and a read";
    }

    // bytes round trip, and channels stay interleaved through the overlay
    const TexelRect part{62, 62, 4, 4};
    std::vector<uint8_t> bytes(static_cast<size_t>(part.area()) * 4);
    for (size_t i = 0; i < bytes.size(); i++) {
        bytes[i] = static_cast<uint8_t>(i * 3);
    }
    ASSERT_TRUE(overlay.writeBytes(blend(0), part, bytes).ok);
    std::vector<uint8_t> back(bytes.size());
    ASSERT_TRUE(overlay.readBytes(blend(0), part, back));
    EXPECT_EQ(back, bytes);
}

TEST(TerrainStore, OutOfRangeRequestsFail) {
    RawMapStore raw(8.0f, 1.0f);
    raw.addMap(HEIGHT, 8, 8, 1, 16);
    float one = 0.0f;
    EXPECT_FALSE(raw.read(HEIGHT, TexelRect{8, 0, 1, 1}, std::span<float>(&one, 1)));
    EXPECT_FALSE(raw.read(blend(0), TexelRect{0, 0, 1, 1}, std::span<float>(&one, 1)));
    EXPECT_FALSE(raw.write(HEIGHT, TexelRect{-1, 0, 1, 1}, std::span<const float>(&one, 1)).ok);
    OverlayMapStore overlay(raw);
    EXPECT_FALSE(overlay.write(HEIGHT, TexelRect{0, 7, 1, 2}, std::span<const float>(&one, 1)).ok);
}

TEST(TerrainStore, MultiChannelHeightReadsRedAndWritesLikeTheBrush) {
    // An old 8-bit RGB(A) source: read through red, a changed texel replicated to RGB and opaque,
    // and an unchanged one left exactly as it was
    RawMapStore raw(4.0f, 10.0f);
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 16; i++) {
        bytes.insert(bytes.end(), {100, 7, 9, 200});
    }
    raw.addMap(HEIGHT, 4, 4, 4, 8, bytes);
    EXPECT_EQ(floatChannels(HEIGHT, raw.format(HEIGHT)), 1);

    std::vector<float> value(1);
    ASSERT_TRUE(raw.read(HEIGHT, TexelRect{1, 1, 1, 1}, value));
    EXPECT_NEAR(value[0], 100.0f / 255.0f, 1e-6f);

    value[0] = 1.0f;
    ASSERT_TRUE(raw.write(HEIGHT, TexelRect{1, 1, 1, 1}, value).ok);
    const std::vector<uint8_t> after = readAllBytes(raw, HEIGHT);
    EXPECT_EQ(after[(1 * 4 + 1) * 4], 255);
    EXPECT_EQ(after[(1 * 4 + 1) * 4 + 1], 255);
    EXPECT_EQ(after[(1 * 4 + 1) * 4 + 2], 255);
    EXPECT_EQ(after[(1 * 4 + 1) * 4 + 3], 255);
    EXPECT_EQ(after[1], 7) << "another texel keeps its green";
}

TEST(TerrainStore, TileGridHandlesNegativeTexels) {
    FakeTiledStore fake(65, 1.0f, 16.0f, 64);
    fake.backing().addMap(HEIGHT, 65, 65, 1, 16);
    OverlayMapStore overlay(fake);
    const TexelRect across{-3, -3, 6, 6};
    std::vector<float> values(36, 0.5f);
    ASSERT_TRUE(overlay.write(HEIGHT, across, values).ok);
    EXPECT_EQ(overlay.tileCount(), 4u) << "four tiles meet at texel 0";
    std::vector<float> back(36);
    ASSERT_TRUE(overlay.read(HEIGHT, across, back));
    EXPECT_EQ(back, values);
    ASSERT_TRUE(overlay.commit().ok);
    ASSERT_TRUE(fake.read(HEIGHT, across, back));
    for (float value : back) {
        EXPECT_NEAR(value, 0.5f, 1.0f / 65535.0f);
    }
}

TEST(TerrainOverlay, DiscardLeavesBaseBytes) {
    RawMapStore raw(8.0f, 10.0f);
    addFlatHeight(raw, 128, 0.25f);
    const std::vector<uint8_t> before = readAllBytes(raw, HEIGHT);

    OverlayMapStore overlay(raw);
    std::vector<float> values(static_cast<size_t>(128 * 40), 0.75f);
    ASSERT_TRUE(overlay.write(HEIGHT, TexelRect{0, 10, 128, 40}, values).ok);
    EXPECT_NE(readAllBytes(overlay, HEIGHT), before) << "the overlay sees its own write";
    EXPECT_EQ(readAllBytes(raw, HEIGHT), before) << "and the base does not";
    overlay.discard();
    EXPECT_EQ(readAllBytes(raw, HEIGHT), before);
    EXPECT_EQ(readAllBytes(overlay, HEIGHT), before);
    EXPECT_EQ(overlay.tileCount(), 0u);
}

TEST(TerrainOverlay, CommitWritesExactlyChangedTexels) {
    RawMapStore raw(8.0f, 10.0f);
    addFlatHeight(raw, 128, 0.25f);
    const std::vector<uint8_t> before = readAllBytes(raw, HEIGHT);

    OverlayMapStore overlay(raw);
    // writes the value already there over most of the rectangle, and changes three scattered texels
    const TexelRect rect{50, 50, 40, 40};
    std::vector<float> values(static_cast<size_t>(rect.area()));
    ASSERT_TRUE(raw.read(HEIGHT, rect, values));
    values[0] = 0.5f;
    values[17 * 40 + 13] = 0.6f;
    values[39 * 40 + 39] = 0.7f;
    ASSERT_TRUE(overlay.write(HEIGHT, rect, values).ok);

    const std::vector<OverlayMapStore::MapChange> changes = overlay.changes();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].texels, 3);
    EXPECT_EQ(changes[0].rect, rect);

    ASSERT_TRUE(overlay.commit().ok);

    const std::vector<uint8_t> after = readAllBytes(raw, HEIGHT);
    int differing = 0;
    for (size_t i = 0; i < after.size(); i += 2) {
        if (after[i] != before[i] || after[i + 1] != before[i + 1]) differing++;
    }
    EXPECT_EQ(differing, 3);

    std::vector<std::pair<MapId, TexelRect>> dirty;
    raw.takeDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0].second, rect) << "only changed texels count as dirty";
}

TEST(TerrainOverlay, GentleWritesAccumulateInFloats) {
    RawMapStore raw(8.0f, 10.0f);
    addFlatHeight(raw, 8, 0.5f);
    OverlayMapStore overlay(raw);
    // each step is a tenth of a 16-bit quantum: rounding at every step would lose all of them
    const float step = 0.1f / 65535.0f;
    std::vector<float> value(1);
    ASSERT_TRUE(overlay.read(HEIGHT, TexelRect{3, 3, 1, 1}, value));
    const float start = value[0];
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(overlay.read(HEIGHT, TexelRect{3, 3, 1, 1}, value));
        value[0] += step;
        ASSERT_TRUE(overlay.write(HEIGHT, TexelRect{3, 3, 1, 1}, value).ok);
    }
    ASSERT_TRUE(overlay.commit().ok);
    ASSERT_TRUE(raw.read(HEIGHT, TexelRect{3, 3, 1, 1}, value));
    EXPECT_GT(value[0], start) << "they added up to more than a quantum";
}

TEST(TerrainOverlay, TouchedTilesOnlyAndDeltaBytes) {
    RawMapStore raw(64.0f, 10.0f);
    addFlatHeight(raw, 1024, 0.25f);
    OverlayMapStore overlay(raw);
    const std::vector<float> value(1, 0.5f);
    ASSERT_TRUE(overlay.write(HEIGHT, TexelRect{500, 500, 1, 1}, value).ok);
    EXPECT_EQ(overlay.tileCount(), 1u);
    // R26's bound for one tile: float values plus stored bytes
    EXPECT_LE(overlay.memoryBytes(), static_cast<size_t>(64 * 64 * (4 + 2)));

    int visited = 0;
    overlay.visitTiles([&](MapId map, TexelRect rect, std::span<const uint8_t> before, std::span<const uint8_t> after) {
        visited++;
        EXPECT_EQ(map, HEIGHT);
        EXPECT_EQ(rect.w, 64);
        EXPECT_EQ(before.size(), after.size());
        size_t differing = 0;
        for (size_t i = 0; i < before.size(); i++) {
            if (before[i] != after[i]) differing++;
        }
        EXPECT_GE(differing, 1u);
        EXPECT_LE(differing, 2u);
    });
    EXPECT_EQ(visited, 1);
}

// R86: a test store with signed texels, an (i * h) lattice, refusals and small blocks passes the
// same content as the plain store
TEST(TerrainStore, FakeTiledStore) {
    using namespace terraingolden;
    for (const Sequence& sequence : sequences()) {
        SCOPED_TRACE(sequence.name);

        RawMapStore plain(TERRAIN_SIZE, MAX_HEIGHT);
        makeStore(plain);
        fillSeedMaps(plain);
        const TerrainOpResult plainResult = applyOps(plain, sequence.ops);
        ASSERT_TRUE(plainResult.ok) << plainResult.error;

        // erode's droplets belong to their block, so it runs with the map as one block
        FakeTiledStore fake(SAMPLES, 1.0f, MAX_HEIGHT, sequence.usesBlocksFreely ? 17 : 4096);
        makeStore(fake.backing());
        fillSeedMaps(fake);
        const TerrainOpResult fakeResult = applyOps(fake, sequence.ops);
        ASSERT_TRUE(fakeResult.ok) << fakeResult.error;

        EXPECT_EQ(stateHash(fake), stateHash(plain)) << "small blocks and signed texels change nothing";
        ASSERT_EQ(fakeResult.ops.size(), plainResult.ops.size());
        for (size_t i = 0; i < fakeResult.ops.size(); i++) {
            EXPECT_EQ(fakeResult.ops[i].changedTexels, plainResult.ops[i].changedTexels);
            EXPECT_EQ(fakeResult.ops[i].clippedHigh, plainResult.ops[i].clippedHigh);
            EXPECT_EQ(fakeResult.ops[i].clippedLow, plainResult.ops[i].clippedLow);
        }
    }
}

TEST(TerrainStore, RefusedRectangleChangesNothing) {
    FakeTiledStore fake(65, 1.0f, 16.0f, 16);
    fake.backing().addMap(HEIGHT, 65, 65, 1, 16);
    fake.refuse(TexelRect{10, 10, 4, 4});
    const uint64_t before = mapHash(fake, HEIGHT);

    Op outside = circleOp(OpType::Raise, -20.0f, -20.0f, 4.0f);
    outside.delta = 1.0f;
    Op inside = circleOp(OpType::Raise, 12.0f, 12.0f, 4.0f);
    inside.delta = 1.0f;
    const Op list[] = {outside, inside};

    const TerrainOpResult validated = validateOps(fake, list);
    EXPECT_FALSE(validated.ok);
    ASSERT_EQ(validated.errors.size(), 1u);
    EXPECT_EQ(validated.errors[0].op, 1);

    const TerrainOpResult result = applyOps(fake, list);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(mapHash(fake, HEIGHT), before) << "the first op's rectangle was fine, and it did not land either";
}

TEST(TerrainStore, RefusalAtCommitChangesNothing) {
    // A store may refuse at the commit too: each tile answers before the first byte moves
    FakeTiledStore fake(65, 1.0f, 16.0f, 16);
    fake.backing().addMap(HEIGHT, 65, 65, 1, 16);
    const uint64_t before = mapHash(fake, HEIGHT);

    OverlayMapStore overlay(fake);
    std::vector<float> values(65 * 65, 0.5f);
    ASSERT_TRUE(overlay.write(HEIGHT, fake.format(HEIGHT).bounds, values).ok);
    fake.refuse(TexelRect{31, 31, 1, 1});
    const WriteResult committed = overlay.commit();
    EXPECT_FALSE(committed.ok);
    EXPECT_FALSE(committed.error.empty());
    EXPECT_EQ(mapHash(fake, HEIGHT), before);
}

TEST(TerrainStore, ReportsTilesCreated) {
    FakeTiledStore fake(65, 1.0f, 16.0f, 16);
    fake.backing().addMap(HEIGHT, 65, 65, 1, 16);
    fake.tilesPerWrite = 2;
    Op op = circleOp(OpType::Raise, 0.0f, 0.0f, 5.0f);
    op.delta = 1.0f;
    const TerrainOpResult result = apply(fake, op);
    ASSERT_TRUE(result.ok);
    EXPECT_GT(result.tilesCreated, 0);
    EXPECT_TRUE(result.hasWarning("tile_created"));
}

TEST(TerrainStore, LargeEditUsesBlocks) {
    constexpr int SAMPLES = 513;
    constexpr int BLOCK = 64;
    FakeTiledStore fake(SAMPLES, 1.0f, 16.0f, BLOCK);
    fake.backing().addMap(HEIGHT, SAMPLES, SAMPLES, 1, 16);
    fake.lendLargeEdit(true);

    Op op;
    op.type = OpType::Smooth;
    OpOptions serial;
    serial.parallel = false;

    fake.largestRead = 0;
    const TerrainOpResult result = apply(fake, op, serial);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(fake.largeEditsGiven, 1);
    ASSERT_EQ(result.largeEdits.size(), 1u) << "the token holds undo";
    // the smooth's margin is one texel each way: a read never goes past a block plus that
    EXPECT_LE(fake.largestRead, BLOCK + 2);
    EXPECT_GT(fake.largestRead, 0);
}

TEST(TerrainStore, StraightOnTheStoreGivesTheOverlaysBytes) {
    using namespace terraingolden;
    for (const Sequence& sequence : sequences()) {
        SCOPED_TRACE(sequence.name);
        // One application of one op: between applications the store holds bytes, not floats (R87's
        // deviation D-7)
        if (sequence.ops.size() != 1 || !sequence.usesBlocksFreely) continue;
        if (sequence.ops[0].iterations != 1 || sequence.ops[0].shape.type == ShapeType::Stroke) continue;

        FakeTiledStore overlayed(SAMPLES, 1.0f, MAX_HEIGHT, 17);
        makeStore(overlayed.backing());
        fillSeedMaps(overlayed);
        ASSERT_TRUE(applyOps(overlayed, sequence.ops).ok);

        FakeTiledStore straight(SAMPLES, 1.0f, MAX_HEIGHT, 17);
        makeStore(straight.backing());
        fillSeedMaps(straight);
        straight.lendLargeEdit(true);
        const TerrainOpResult result = applyOps(straight, sequence.ops);
        ASSERT_TRUE(result.ok) << result.error;

        EXPECT_EQ(stateHash(straight), stateHash(overlayed));
    }
}

TEST(TerrainStore, UndoHookRunsWithTheDelta) {
    // What the store created on write stays the caller's to take after the list (R86): here the
    // tile a write made. TerrainMapDeltaCmd keeps this with the step.
    struct TileHook final : TerrainStoreUndoHook {
        explicit TileHook(int& exists): exists(exists) {}
        void undo() override { exists = 0; }
        void redo() override { exists = 1; }
        int& exists;
    };
    int tileExists = 1;

    FakeTiledStore fake(65, 1.0f, 16.0f, 16);
    fake.backing().addMap(HEIGHT, 65, 65, 1, 16);
    fake.lendUndoHook(std::make_unique<TileHook>(tileExists));

    Op op = circleOp(OpType::Raise, 0.0f, 0.0f, 5.0f);
    op.delta = 1.0f;
    ASSERT_TRUE(apply(fake, op).ok);

    std::unique_ptr<TerrainStoreUndoHook> hook = fake.takeUndoHook();
    ASSERT_NE(hook, nullptr) << "the op did not take it";
    hook->undo();
    EXPECT_EQ(tileExists, 0);
    hook->redo();
    EXPECT_EQ(tileExists, 1);
    EXPECT_EQ(fake.takeUndoHook(), nullptr) << "handed over once";
}

TEST(TerrainStore, OverlayForwardsUndoHookAndRefusesLargeEdits) {
    struct Hook final : TerrainStoreUndoHook {
        void undo() override {}
        void redo() override {}
    };
    FakeTiledStore fake(65, 1.0f, 16.0f, 16);
    fake.backing().addMap(HEIGHT, 65, 65, 1, 16);
    fake.lendUndoHook(std::make_unique<Hook>());
    fake.lendLargeEdit(true);
    OverlayMapStore overlay(fake);
    EXPECT_EQ(overlay.beginLarge(HEIGHT, TexelRect{0, 0, 1, 1}), nullptr) << "a transaction holds its own undo";
    EXPECT_NE(overlay.takeUndoHook(), nullptr);
    EXPECT_EQ(overlay.preferredBlock(HEIGHT).w, 16);
}
