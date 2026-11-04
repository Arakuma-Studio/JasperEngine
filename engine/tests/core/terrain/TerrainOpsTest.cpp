// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The engine around its kernels: all-or-nothing, results, quantization, seams (T1 R4, R7-R11, R40,
// R89; AC2, AC3, AC6, AC7, AC63, AC70)

#include "TerrainTestSupport.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <random>

using namespace terraintest;

TEST(TerrainOps, FailedListChangesNothing) {
    RawMapStore store(16.0f, 8.0f);
    addFlatHeight(store, 16, 0.5f);
    const uint64_t before = mapHash(store, HEIGHT);

    Op good = circleOp(OpType::Raise, 0.0f, 0.0f, 4.0f);
    good.delta = 1.0f;
    Op bad = circleOp(OpType::Raise, 0.0f, 0.0f, -1.0f);   // third op invalid
    bad.delta = 1.0f;
    const Op list[] = {good, good, bad};

    const TerrainOpResult result = applyOps(store, list);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(mapHash(store, HEIGHT), before);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].op, 2);
    EXPECT_EQ(result.errors[0].field, "ops[2].shape.radius");
    EXPECT_NE(result.error.find("ops[2].shape.radius"), std::string::npos) << result.error;
}

TEST(TerrainOps, ApplyFailureMidListChangesNothing) {
    // A registered op that fails while running, after earlier ops have written to the overlay
    registerOp("test_fail", nullptr, [](OpContext& context, const Op&, OpEntry&) { context.fail("no"); });
    RawMapStore store(16.0f, 8.0f);
    addFlatHeight(store, 16, 0.5f);
    const uint64_t before = mapHash(store, HEIGHT);
    Op good = circleOp(OpType::Raise, 0.0f, 0.0f, 4.0f);
    good.delta = 1.0f;
    Op failing;
    failing.type = OpType::Registered;
    failing.registeredName = "test_fail";
    const Op list[] = {good, failing};
    const TerrainOpResult result = applyOps(store, list);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(mapHash(store, HEIGHT), before);
    EXPECT_NE(result.error.find("ops[1]"), std::string::npos) << result.error;
    unregisterOp("test_fail");
}

TEST(TerrainOps, DryRunReturnsTheSameNumbers) {
    RawMapStore store(16.0f, 8.0f);
    addFlatHeight(store, 16, 0.5f);
    const uint64_t before = mapHash(store, HEIGHT);
    Op op = circleOp(OpType::Raise, 2.0f, 2.0f, 5.0f);
    op.delta = 1.0f;

    OpOptions dry;
    dry.dryRun = true;
    const TerrainOpResult preview = apply(store, op, dry);
    ASSERT_TRUE(preview.ok);
    EXPECT_TRUE(preview.dryRun);
    EXPECT_EQ(mapHash(store, HEIGHT), before);

    const TerrainOpResult real = apply(store, op);
    ASSERT_TRUE(real.ok);
    EXPECT_NE(mapHash(store, HEIGHT), before);

    ASSERT_EQ(preview.changed.size(), real.changed.size());
    ASSERT_FALSE(real.changed.empty());
    EXPECT_EQ(preview.changed[0].rect, real.changed[0].rect);
    EXPECT_EQ(preview.changed[0].texels, real.changed[0].texels);
    EXPECT_EQ(preview.ops[0].changedTexels, real.ops[0].changedTexels);
    EXPECT_FLOAT_EQ(preview.stats.maxDelta, real.stats.maxDelta);
    EXPECT_FLOAT_EQ(preview.stats.minAfter, real.stats.minAfter);
}

TEST(TerrainOps, ShapeWhollyOutsideIsAnErrorThatStatesTheBounds) {
    RawMapStore store(16.0f, 8.0f);
    addFlatHeight(store, 16, 0.5f);
    Op op = circleOp(OpType::Raise, 100.0f, 0.0f, 4.0f);
    op.delta = 1.0f;
    const TerrainOpResult result = apply(store, op);
    EXPECT_FALSE(result.ok);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].field, "ops[0].shape");
    EXPECT_NE(result.errors[0].message.find("-8"), std::string::npos) << result.errors[0].message;
    EXPECT_NE(result.errors[0].message.find("8.0"), std::string::npos) << result.errors[0].message;

    // a far shape must clip, not overflow a texel coordinate
    Op far = circleOp(OpType::Raise, 1.0e12f, -1.0e12f, 1.0e3f);
    far.delta = 1.0f;
    EXPECT_FALSE(apply(store, far).ok);
}

TEST(TerrainOps, PartlyOutsideWarns) {
    RawMapStore store(16.0f, 8.0f);
    addFlatHeight(store, 16, 0.5f);
    Op op = circleOp(OpType::Raise, 8.0f, 0.0f, 4.0f);
    op.delta = 1.0f;
    const TerrainOpResult result = apply(store, op);
    ASSERT_TRUE(result.ok);
    EXPECT_GT(result.ops[0].outside, 0);
    EXPECT_TRUE(result.hasWarning("outside_terrain"));
}

TEST(TerrainOps, ClampsAreCounted) {
    RawMapStore store(4.0f, 10.0f);
    addFlatHeight(store, 4, 0.5f);

    Op up;
    up.type = OpType::Raise;
    up.delta = 7.0f;   // 0.5 + 0.7 > 1 everywhere
    TerrainOpResult raised = apply(store, up);
    ASSERT_TRUE(raised.ok);
    EXPECT_EQ(raised.ops[0].clippedHigh, 16);
    EXPECT_EQ(raised.ops[0].clippedLow, 0);
    EXPECT_TRUE(raised.hasWarning("clipped_high"));
    EXPECT_FALSE(raised.hasWarning("clipped_low"));

    Op down;
    down.type = OpType::Lower;
    down.delta = 15.0f;
    TerrainOpResult lowered = apply(store, down);
    ASSERT_TRUE(lowered.ok);
    EXPECT_EQ(lowered.ops[0].clippedLow, 16);
    EXPECT_TRUE(lowered.hasWarning("clipped_low"));
    for (float value : readHeights(store)) {
        EXPECT_EQ(value, 0.0f);
    }
}

TEST(TerrainOps, UntouchedTexelsKeepTheirBytes) {
    // Odd values on both depths: a store that re-encoded what it did not change would show here
    for (int bits : {8, 16}) {
        SCOPED_TRACE(bits);
        std::vector<uint8_t> bytes;
        std::mt19937 rng(77);
        const int bytesPerValue = bits / 8;
        for (int i = 0; i < 32 * 32 * bytesPerValue; i++) {
            bytes.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }
        RawMapStore store(32.0f, 8.0f);
        store.addMap(HEIGHT, 32, 32, 1, bits, bytes);

        // A rectangle whose filter admits only a narrow band: most of it stays
        Op op;
        op.type = OpType::Raise;
        op.shape.type = ShapeType::Rect;
        op.shape.min = Vector2(-10.0f, -10.0f);
        op.shape.max = Vector2(10.0f, 10.0f);
        op.delta = 0.1f;
        op.filter.height = FilterRange{0.0f, 2.0f};
        ASSERT_TRUE(apply(store, op).ok);

        const std::vector<uint8_t> after = readAllBytes(store, HEIGHT);
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                const bool inRect = x >= 6 && x < 26 && y >= 6 && y < 26;
                const size_t at = (static_cast<size_t>(y) * 32 + x) * bytesPerValue;
                const float before = bits == 16 ? static_cast<float>(bytes[at] | (bytes[at + 1] << 8)) / 65535.0f : bytes[at] / 255.0f;
                if (!inRect || before * 8.0f > 2.0f) {
                    ASSERT_EQ(after[at], bytes[at]) << x << ", " << y;
                    if (bits == 16) {
                        ASSERT_EQ(after[at + 1], bytes[at + 1]) << x << ", " << y;
                    }
                }
            }
        }
    }
}

TEST(TerrainOps, OneQuantizationRule) {
    // lround(clamp01(v) * max), for every map: 0.5 / 255 rounds up, so truncating stores differ
    EXPECT_EQ(quantizeChannel(127.5f / 255.0f, 8), 128u);
    EXPECT_EQ(quantizeChannel(0.25f, 8), 64u);
    EXPECT_EQ(quantizeChannel(0.25f, 16), 16384u);
    EXPECT_EQ(quantizeChannel(-1.0f, 16), 0u);
    EXPECT_EQ(quantizeChannel(2.0f, 16), 65535u);
    EXPECT_EQ(quantizeChannel(std::numeric_limits<float>::quiet_NaN(), 8), 0u);

    RawMapStore store(4.0f, 1.0f);
    store.addMap(blend(0), 4, 4, 4, 8);
    const std::vector<float> values(4 * 4, 127.5f / 255.0f);
    ASSERT_TRUE(store.write(blend(0), TexelRect{0, 0, 1, 4}, values).ok);
    EXPECT_EQ(readAllBytes(store, blend(0))[0], 128);
}

TEST(TerrainOps, ResultNumbersMatchDiff) {
    // Random lists on a fixed seed: every report equals a byte diff of before and after
    std::mt19937 rng(424242);
    auto unit = [&]() { return static_cast<float>(rng() % 100000) / 100000.0f; };

    for (int round = 0; round < 40; round++) {
        SCOPED_TRACE(round);
        RawMapStore store(40.0f, 10.0f);
        store.addMap(HEIGHT, 40, 40, 1, 16);
        fillHeight(store, 40, 40, [](int x, int y) { return 0.3f + 0.01f * static_cast<float>(x) + 0.002f * static_cast<float>(y); });
        store.addMap(blend(0), 40, 40, 4, 8);
        const std::vector<uint8_t> heightBefore = readAllBytes(store, HEIGHT);
        const std::vector<uint8_t> blendBefore = readAllBytes(store, blend(0));

        std::vector<Op> ops;
        const int count = 1 + static_cast<int>(rng() % 4);
        for (int i = 0; i < count; i++) {
            Op op = circleOp(rng() % 2 ? OpType::Raise : OpType::Lower, unit() * 40.0f - 20.0f, unit() * 40.0f - 20.0f, 1.0f + unit() * 11.0f);
            op.delta = 0.01f + unit() * 3.0f;
            if (rng() % 3 == 0) {
                op.type = OpType::PaintLayer;
                op.delta.reset();
                op.layer = static_cast<int>(rng() % 3);
            } else if (rng() % 3 == 0) {
                op.type = OpType::SetHeight;
                op.delta.reset();
                op.height = 0.01f + unit() * 3.0f;
            }
            ops.push_back(op);
        }

        const TerrainOpResult result = applyOps(store, ops);
        ASSERT_TRUE(result.ok) << result.error;

        auto check = [&](MapId map, const std::vector<uint8_t>& before, int stride) {
            const std::vector<uint8_t> after = readAllBytes(store, map);
            TexelRect rect;
            int64_t texels = 0;
            for (int y = 0; y < 40; y++) {
                for (int x = 0; x < 40; x++) {
                    const size_t at = (static_cast<size_t>(y) * 40 + x) * stride;
                    if (std::memcmp(after.data() + at, before.data() + at, stride) != 0) {
                        rect = unite(rect, TexelRect{x, y, 1, 1});
                        texels++;
                    }
                }
            }
            const MapChangeReport* report = nullptr;
            for (const MapChangeReport& candidate : result.changed) {
                if (candidate.map == map) report = &candidate;
            }
            if (texels == 0) {
                EXPECT_EQ(report, nullptr);
                return;
            }
            ASSERT_NE(report, nullptr);
            EXPECT_EQ(report->texels, texels);
            EXPECT_EQ(report->rect, rect);
            EXPECT_FLOAT_EQ(report->localMin.x, static_cast<float>(rect.x) - 20.0f);
            EXPECT_FLOAT_EQ(report->localMax.y, static_cast<float>(rect.bottom()) - 20.0f);
        };
        check(HEIGHT, heightBefore, 2);
        check(blend(0), blendBefore, 4);

        if (ops.size() == 1 && !result.changed.empty()) {
            EXPECT_EQ(result.ops[0].changedTexels, result.changed[0].texels) << "one op: its count is the diff";
        }
    }
}

TEST(TerrainOps, HeightStatsOverTheChangedTexels) {
    RawMapStore store(4.0f, 10.0f);
    addFlatHeight(store, 4, 0.5f);
    Op op;
    op.type = OpType::Raise;
    op.shape.type = ShapeType::Rect;
    op.shape.min = Vector2(-2.0f, -2.0f);
    op.shape.max = Vector2(0.0f, 0.0f);
    op.delta = 1.0f;
    const TerrainOpResult result = apply(store, op);
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.stats.valid);
    EXPECT_NEAR(result.stats.minBefore, 5.0f, 1e-3f);
    EXPECT_NEAR(result.stats.maxAfter, 6.0f, 1e-3f);
    EXPECT_NEAR(result.stats.meanDelta, 1.0f, 1e-3f);
    EXPECT_NEAR(result.stats.maxDelta, 1.0f, 1e-3f);
}

TEST(TerrainOps, WarningsOfTheClosedList) {
    RawMapStore store(8.0f, 10.0f);
    addFlatHeight(store, 8, 0.5f);

    Op nothing = circleOp(OpType::SetHeight, 0.0f, 0.0f, 3.0f);
    nothing.height = 5.0f;
    EXPECT_TRUE(apply(store, nothing).hasWarning("no_change"));

    Op filtered = circleOp(OpType::Raise, 0.0f, 0.0f, 3.0f);
    filtered.delta = 1.0f;
    filtered.filter.height = FilterRange{8.0f, 9.0f};
    const TerrainOpResult excluded = apply(store, filtered);
    EXPECT_TRUE(excluded.hasWarning("filter_excluded_all"));
    EXPECT_FALSE(excluded.hasWarning("no_change"));

    RawMapStore eightBit(8.0f, 10.0f);
    eightBit.addMap(HEIGHT, 8, 8, 1, 8);
    Op raise;
    raise.type = OpType::Raise;
    raise.delta = 1.0f;
    EXPECT_TRUE(apply(eightBit, raise).hasWarning("map_8bit_steps"));

    Op small = circleOp(OpType::Raise, 0.0f, 0.0f, 0.05f);
    small.delta = 1.0f;
    const TerrainOpResult tiny = apply(store, small);
    EXPECT_TRUE(tiny.hasWarning("brush_under_one_texel"));
    EXPECT_EQ(tiny.ops[0].changedTexels, 1) << "a brush smaller than a texel reaches the nearest one";
}

TEST(TerrainOps, ValidationNamesTheField) {
    RawMapStore store(8.0f, 10.0f);
    addFlatHeight(store, 8, 0.5f);

    auto fieldOf = [&](const Op& op) {
        const TerrainOpResult result = apply(store, op);
        EXPECT_FALSE(result.ok);
        return result.errors.empty() ? std::string() : result.errors[0].field;
    };

    Op raise;
    raise.type = OpType::Raise;
    EXPECT_EQ(fieldOf(raise), "ops[0].delta");

    Op set;
    set.type = OpType::SetHeight;
    EXPECT_EQ(fieldOf(set), "ops[0].height");

    Op terrace;
    terrace.type = OpType::Terrace;
    terrace.steps = 1;
    EXPECT_EQ(fieldOf(terrace), "ops[0].steps");

    Op remap;
    remap.type = OpType::RemapLayers;
    remap.order = {0, 0};
    EXPECT_EQ(fieldOf(remap), "ops[0].order");

    Op strong;
    strong.type = OpType::Raise;
    strong.delta = 1.0f;
    strong.strength = 2.0f;
    EXPECT_EQ(fieldOf(strong), "ops[0].strength");

    Op filter;
    filter.type = OpType::Raise;
    filter.delta = 1.0f;
    filter.filter.slope = FilterRange{40.0f, 10.0f};
    EXPECT_EQ(fieldOf(filter), "ops[0].filter.slope");

    Op unknown;
    unknown.type = OpType::Registered;
    unknown.registeredName = "no_such_op";
    EXPECT_EQ(fieldOf(unknown), "ops[0].op");

    Op ramp;
    ramp.type = OpType::Ramp;
    ramp.shape.type = ShapeType::Polyline;
    ramp.shape.points = {Vector2(0.0f, 0.0f)};
    ramp.shape.radius = 1.0f;
    EXPECT_EQ(fieldOf(ramp), "ops[0].shape");

    Op holeStrength;
    holeStrength.type = OpType::CutHoles;
    holeStrength.strength = 0.5f;
    EXPECT_EQ(fieldOf(holeStrength), "ops[0].strength");

    RawMapStore empty(8.0f, 10.0f);
    Op noMap;
    noMap.type = OpType::Raise;
    noMap.delta = 1.0f;
    EXPECT_FALSE(apply(empty, noMap).ok);
}

TEST(TerrainOps, OpNamesRoundTrip) {
    int count = 0;
    for (int type = 0; type < static_cast<int>(OpType::Registered); type++) {
        const char* name = opTypeName(static_cast<OpType>(type));
        ASSERT_NE(name, nullptr) << type;
        OpType back;
        ASSERT_TRUE(opTypeFromName(name, back));
        EXPECT_EQ(static_cast<int>(back), type);
        count++;
    }
    EXPECT_EQ(count, 25) << "Table 1's 24 public ops and fill_noise";
    OpType ignored;
    EXPECT_FALSE(opTypeFromName("scatter_by_rules", ignored));
}

TEST(TerrainOps, ApplyFieldMatchesEquivalentOp) {
    RawMapStore byOp(16.0f, 10.0f);
    addFlatHeight(byOp, 16, 0.25f);
    RawMapStore byField(16.0f, 10.0f);
    addFlatHeight(byField, 16, 0.25f);

    Op op;
    op.type = OpType::SetHeight;
    op.height = 7.5f;
    op.strength = 0.5f;
    const TerrainOpResult opResult = apply(byOp, op);
    ASSERT_TRUE(opResult.ok);

    const TerrainOpResult fieldResult = applyField(byField, HEIGHT, byField.format(HEIGHT).bounds,
        [](void*, float, float, std::span<const float>) {
            FieldSample sample;
            sample.weight = 0.5f;
            sample.target[0] = 0.75f;
            return sample;
        }, nullptr);
    ASSERT_TRUE(fieldResult.ok);

    EXPECT_EQ(mapHash(byField, HEIGHT), mapHash(byOp, HEIGHT));
    EXPECT_EQ(fieldResult.ops[0].changedTexels, opResult.ops[0].changedTexels);
    ASSERT_EQ(fieldResult.changed.size(), 1u);
    EXPECT_EQ(fieldResult.changed[0].rect, opResult.changed[0].rect);
}

TEST(TerrainOps, ApplyFieldGivesPositionsAndCurrentValues) {
    RawMapStore store(4.0f, 10.0f);
    store.addMap(blend(0), 4, 4, 4, 8);
    struct Seen {
        std::vector<Vector2> positions;
        size_t channels = 0;
    } seen;
    const TerrainOpResult result = applyField(store, blend(0), TexelRect{1, 2, 2, 1},
        [](void* user, float x, float z, std::span<const float> current) {
            Seen& s = *static_cast<Seen*>(user);
            s.positions.push_back(Vector2(x, z));
            s.channels = current.size();
            return FieldSample{};
        }, &seen);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(seen.positions.size(), 2u);
    EXPECT_FLOAT_EQ(seen.positions[0].x, -0.5f);
    EXPECT_FLOAT_EQ(seen.positions[0].y, 0.5f);
    EXPECT_FLOAT_EQ(seen.positions[1].x, 0.5f);
    EXPECT_EQ(seen.channels, 4u);
    EXPECT_TRUE(result.changed.empty());
}

TEST(TerrainOps, SetHeightsTakesLocalUnits) {
    RawMapStore store(4.0f, 10.0f);
    addFlatHeight(store, 4, 0.0f);
    const std::vector<float> heights = {2.5f, 5.0f, 12.0f, -1.0f};
    const TerrainOpResult result = setHeights(store, TexelRect{0, 0, 2, 2}, heights);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].clippedHigh, 1);
    EXPECT_EQ(result.ops[0].clippedLow, 1);
    EXPECT_TRUE(result.hasWarning("clipped_high"));
    const std::vector<float> back = readHeights(store);
    EXPECT_NEAR(back[0], 0.25f, 1e-4f);
    EXPECT_NEAR(back[1], 0.5f, 1e-4f);
    EXPECT_EQ(back[4], 1.0f);
    EXPECT_EQ(back[5], 0.0f);

    const TerrainOpResult bad = setHeights(store, TexelRect{3, 3, 2, 2}, heights);
    EXPECT_FALSE(bad.ok) << "a rectangle past the map refuses the write";
}

TEST(TerrainOps, MapHashTracksContent) {
    RawMapStore store(4.0f, 10.0f);
    addFlatHeight(store, 4, 0.5f);
    const uint64_t a = mapHash(store, HEIGHT);
    EXPECT_EQ(a, mapHash(store, HEIGHT));
    const std::vector<float> one(1, 0.75f);
    store.write(HEIGHT, TexelRect{0, 0, 1, 1}, one);
    EXPECT_NE(a, mapHash(store, HEIGHT));
}

namespace {

    // A registered op: what T5's ops look like from here
    struct TestUndo final : TerrainStoreUndoHook {
        explicit TestUndo(int* trees): trees(trees) {}
        void undo() override { (*trees)--; }
        void redo() override { (*trees)++; }
        int* trees;
    };

    int testTrees = 0;

    bool validateTestOp(const TerrainMapStore& store, const Op& op, OpError& error) {
        if (!store.format(HEIGHT).exists) {
            error.message = "needs a height map";
            return false;
        }
        const OpParam* amount = op.params.find("amount");
        if (!amount || amount->kind != OpParam::Kind::Number) {
            error.field = "params.amount";
            error.message = "is required";
            return false;
        }
        return true;
    }

    void applyTestOp(OpContext& context, const Op& op, OpEntry& entry) {
        const float amount = static_cast<float>(op.params.find("amount")->number);
        TerrainMapStore& store = context.store();
        const MapFormat format = store.format(HEIGHT);
        const std::vector<float> values(static_cast<size_t>(format.bounds.area()), amount);
        store.write(HEIGHT, format.bounds, values);
        entry.changedTexels = format.bounds.area();
        OpParam planted;
        planted.kind = OpParam::Kind::Number;
        planted.number = 3.0;
        entry.fields.emplace_back("planted", planted);
        context.warn("no_change", "a test's warning");
        if (!context.dryRun()) {
            testTrees++;
            context.addUndoHook(std::make_unique<TestUndo>(&testTrees));
        }
    }

}

TEST(TerrainOps, RegisteredOpJoinsValidationResultAndUndo) {
    ASSERT_TRUE(registerOp("test_plant", validateTestOp, applyTestOp));
    EXPECT_FALSE(registerOp("raise", validateTestOp, applyTestOp)) << "Table 1 keeps its names";
    EXPECT_TRUE(registeredOpExists("test_plant"));

    RawMapStore store(4.0f, 10.0f);
    addFlatHeight(store, 4, 0.25f);
    const uint64_t before = mapHash(store, HEIGHT);

    Op op;
    op.type = OpType::Registered;
    op.registeredName = "test_plant";

    // validation
    {
        const TerrainOpResult refused = apply(store, op);
        EXPECT_FALSE(refused.ok);
        ASSERT_EQ(refused.errors.size(), 1u);
        EXPECT_EQ(refused.errors[0].field, "ops[0].params.amount");
        EXPECT_EQ(mapHash(store, HEIGHT), before);
    }

    OpParam amount;
    amount.kind = OpParam::Kind::Number;
    amount.number = 0.5;
    op.params.kind = OpParam::Kind::Object;
    op.params.fields.emplace_back("amount", amount);

    // overlay: a dry run writes nothing, and the op is told
    {
        OpOptions dry;
        dry.dryRun = true;
        testTrees = 0;
        const TerrainOpResult preview = apply(store, op, dry);
        ASSERT_TRUE(preview.ok);
        EXPECT_EQ(testTrees, 0);
        EXPECT_TRUE(preview.undoHooks.empty());
        EXPECT_EQ(mapHash(store, HEIGHT), before);
        ASSERT_EQ(preview.changed.size(), 1u);
        EXPECT_EQ(preview.changed[0].texels, 16);
    }

    // a later op fails validation: nothing runs at all
    {
        Op raising = circleOp(OpType::Raise, 100.0f, 0.0f, 1.0f);
        raising.delta = 1.0f;
        const Op list[] = {op, raising};
        const TerrainOpResult failed = applyOps(store, list);
        EXPECT_FALSE(failed.ok);
        EXPECT_EQ(mapHash(store, HEIGHT), before);
        EXPECT_EQ(testTrees, 0);
    }

    // result and undo
    const TerrainOpResult result = apply(store, op);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NE(mapHash(store, HEIGHT), before);
    ASSERT_EQ(result.ops.size(), 1u);
    EXPECT_EQ(result.ops[0].op, "test_plant");
    ASSERT_EQ(result.ops[0].fields.size(), 1u);
    EXPECT_EQ(result.ops[0].fields[0].first, "planted");
    EXPECT_TRUE(result.hasWarning("no_change"));
    ASSERT_EQ(result.undoHooks.size(), 1u);
    EXPECT_EQ(testTrees, 1);
    result.undoHooks[0]->undo();
    EXPECT_EQ(testTrees, 0);
    result.undoHooks[0]->redo();
    EXPECT_EQ(testTrees, 1);

    unregisterOp("test_plant");
    EXPECT_FALSE(registeredOpExists("test_plant"));
}

TEST(TerrainOps, ARegisteredOpMayDeclareItSpansFrames) {
    RegisteredOpTraits traits;
    traits.spansFrames = true;
    ASSERT_TRUE(registerOp("test_bake", nullptr, [](OpContext&, const Op&, OpEntry&) {}, traits));
    ASSERT_TRUE(registerOp("test_quick", nullptr, [](OpContext&, const Op&, OpEntry&) {}));
    EXPECT_TRUE(registeredOpSpansFrames("test_bake"));
    EXPECT_FALSE(registeredOpSpansFrames("test_quick"));
    EXPECT_FALSE(registeredOpSpansFrames("no_such_op"));
    unregisterOp("test_bake");
    unregisterOp("test_quick");
}

TEST(TerrainOps, InPlaceKeepsTheCallersFloats) {
    RawMapStore store(8.0f, 10.0f);
    addFlatHeight(store, 8, 0.5f);
    const uint64_t before = mapHash(store, HEIGHT);
    OverlayMapStore overlay(store);

    // each is a tenth of a quantum: only working floats that survive between calls add up
    Op op;
    op.type = OpType::Raise;
    op.delta = 10.0f * 0.1f / 65535.0f;
    OpOptions inPlace;
    inPlace.inPlace = true;
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(apply(overlay, op, inPlace).ok);
    }
    EXPECT_EQ(mapHash(store, HEIGHT), before) << "nothing commits";
    ASSERT_TRUE(overlay.commit().ok);
    EXPECT_NE(mapHash(store, HEIGHT), before);
}
