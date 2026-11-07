// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Placement as pure data (T1 R21, AC16) and strokes' stamps (R33-R35)

#include "TerrainTestSupport.h"

#include "terrain/TerrainPlacement.h"
#include "terrain/TerrainStroke.h"

#include <gtest/gtest.h>

using namespace terraintest;

namespace {

    PlacementPlan plan(uint32_t seed, float spacing, int stamps, std::span<const Vector2> existing = {}) {
        std::vector<PlacementStamp> list;
        for (int i = 0; i < stamps; i++) {
            list.push_back(PlacementStamp{static_cast<float>(i) * 0.5f - 10.0f, 0.0f, 4.0f, i % 2 == 0});
        }
        PlacementParams params;
        params.seed = seed;
        params.spacing = spacing;
        params.minScale = 0.5f;
        params.maxScale = 1.5f;
        params.rotationJitter = 0.5f;
        return planPlacement(list, params, existing, Vector2(-32.0f, -32.0f), Vector2(32.0f, 32.0f));
    }

}

TEST(TerrainPlacement, SameSeedSamePlan) {
    const PlacementPlan a = plan(7, 0.5f, 40);
    const PlacementPlan b = plan(7, 0.5f, 40);
    ASSERT_EQ(a.objects.size(), b.objects.size());
    ASSERT_FALSE(a.objects.empty());
    for (size_t i = 0; i < a.objects.size(); i++) {
        EXPECT_EQ(a.objects[i].x, b.objects[i].x);
        EXPECT_EQ(a.objects[i].z, b.objects[i].z);
        EXPECT_EQ(a.objects[i].yaw, b.objects[i].yaw);
        EXPECT_EQ(a.objects[i].scale, b.objects[i].scale);
    }
    const PlacementPlan other = plan(8, 0.5f, 40);
    EXPECT_NE(a.objects[0].x, other.objects[0].x) << "another seed, another plan";
    for (const PlacedObject& object : a.objects) {
        EXPECT_GE(object.scale, 0.5f);
        EXPECT_LE(object.scale, 1.5f);
        EXPECT_GE(object.yaw, 0.0f);
        EXPECT_LE(object.yaw, DET_TWO_PI * 0.5f);
    }
}

TEST(TerrainPlacement, RespectsSpacing) {
    const std::vector<Vector2> existing = {Vector2(-8.0f, 0.0f), Vector2(-6.0f, 1.0f)};
    const float spacing = 2.0f;
    const PlacementPlan result = plan(3, spacing, 60, existing);
    EXPECT_GT(result.rejectedBySpacing, 0);
    std::vector<Vector2> all = existing;
    for (const PlacedObject& object : result.objects) {
        for (const Vector2& other : all) {
            const float dx = object.x - other.x;
            const float dz = object.z - other.y;
            EXPECT_GE(dx * dx + dz * dz, spacing * spacing * 0.999f);
        }
        all.push_back(Vector2(object.x, object.z));
    }
}

TEST(TerrainPlacement, OneAttemptPerStamp) {
    const PlacementPlan result = plan(1, 0.0f, 17);
    EXPECT_EQ(result.attempts, 17);
    EXPECT_EQ(result.objects.size(), 17u) << "nothing to reject";

    // A drag: one attempt per spaced stamp, however many samples carried the same path
    RawMapStore store(64.0f, 10.0f);
    addFlatHeight(store, 64, 0.0f);
    auto stroke = [&](int samples) {
        Op op;
        op.type = OpType::PlaceObjects;
        op.shape.type = ShapeType::Stroke;
        op.shape.radius = 2.0f;
        op.shape.spacing = 1.0f;
        op.spacing = 0.0f;
        for (int i = 0; i <= samples; i++) {
            op.shape.points.push_back(Vector2(-10.0f + 20.0f * static_cast<float>(i) / static_cast<float>(samples), 0.0f));
        }
        return apply(store, op);
    };
    const TerrainOpResult coarse = stroke(4);
    const TerrainOpResult fine = stroke(40);
    ASSERT_TRUE(coarse.ok);
    ASSERT_TRUE(fine.ok);
    EXPECT_EQ(coarse.ops[0].plan.attempts, 21) << "the start, then each unit of 20";
    EXPECT_EQ(fine.ops[0].plan.attempts, coarse.ops[0].plan.attempts);
}

TEST(TerrainPlacement, TheRegionDecides) {
    RawMapStore store(64.0f, 10.0f);
    addFlatHeight(store, 64, 0.0f);
    Op op;
    op.type = OpType::PlaceObjects;
    op.shape.type = ShapeType::Rect;
    op.shape.min = Vector2(0.0f, 0.0f);
    op.shape.max = Vector2(4.0f, 8.0f);
    op.shape.rotation = 45.0f;
    op.count = 100;
    op.spacing = 0.0f;
    op.seed = 1;
    const TerrainOpResult result = apply(store, op);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].plan.attempts, 100);
    EXPECT_GT(result.ops[0].plan.rejectedByRegion, 0);
    EXPECT_FALSE(result.ops[0].plan.objects.empty());
    const ShapeEvaluator shape(op.shape);
    for (const PlacedObject& object : result.ops[0].plan.objects) {
        EXPECT_TRUE(shape.sample(object.x, object.z).inside);
    }
}

TEST(TerrainStroke, StampsAtArcLengthMultiplesHoweverSampled) {
    auto stamps = [](int perSegment) {
        StrokeResampler resampler(1.0f);
        std::vector<StrokeStamp> out;
        const Vector2 corners[] = {Vector2(0.0f, 0.0f), Vector2(5.0f, 0.0f), Vector2(5.0f, 5.0f)};
        resampler.add(StrokeSample{0.0f, corners[0].x, corners[0].y, 1.0f}, out);
        for (int segment = 0; segment < 2; segment++) {
            for (int i = 1; i <= perSegment; i++) {
                const float t = static_cast<float>(i) / static_cast<float>(perSegment);
                const float x = corners[segment].x + (corners[segment + 1].x - corners[segment].x) * t;
                const float z = corners[segment].y + (corners[segment + 1].y - corners[segment].y) * t;
                resampler.add(StrokeSample{0.0f, x, z, 1.0f}, out);
            }
        }
        return out;
    };
    const std::vector<StrokeStamp> coarse = stamps(1);
    const std::vector<StrokeStamp> fine = stamps(37);
    ASSERT_EQ(coarse.size(), 11u) << "the start, then each unit of 10";
    ASSERT_EQ(fine.size(), coarse.size());
    for (size_t i = 0; i < coarse.size(); i++) {
        EXPECT_NEAR(fine[i].x, coarse[i].x, 1e-3f);
        EXPECT_NEAR(fine[i].z, coarse[i].z, 1e-3f);
    }
}

TEST(TerrainStroke, ClickIsOneStamp) {
    StrokeResampler resampler(1.0f);
    std::vector<StrokeStamp> out;
    resampler.add(StrokeSample{0.0f, 3.0f, 4.0f, 0.5f}, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].x, 3.0f);
    EXPECT_EQ(out[0].pressure, 0.5f);
}

TEST(TerrainStroke, PressureInterpolatesAlongTheSegment) {
    StrokeResampler resampler(1.0f);
    std::vector<StrokeStamp> out;
    resampler.add(StrokeSample{0.0f, 0.0f, 0.0f, 0.0f}, out);
    resampler.add(StrokeSample{0.0f, 4.0f, 0.0f, 1.0f}, out);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_FLOAT_EQ(out[2].pressure, 0.5f);
}

TEST(TerrainStroke, HoldingStillDepositsAtTheFlowRateWithTheClamp) {
    StrokeResampler resampler(1.0f);
    std::vector<StrokeStamp> out;
    resampler.add(StrokeSample{0.0f, 0.0f, 0.0f, 1.0f}, out);
    // Half a second still, sampled at 60 Hz: 30 time stamps on top of the click
    for (int i = 1; i <= 30; i++) {
        resampler.add(StrokeSample{static_cast<float>(i) / 60.0f, 0.0f, 0.0f, 1.0f}, out);
    }
    EXPECT_NEAR(static_cast<int>(out.size()), 31, 1);

    // A hitch: at most R35's cap for one sample
    StrokeResampler hitch(1.0f);
    std::vector<StrokeStamp> hitched;
    hitch.add(StrokeSample{0.0f, 0.0f, 0.0f, 1.0f}, hitched);
    hitch.add(StrokeSample{1.0f, 0.0f, 0.0f, 1.0f}, hitched);
    EXPECT_EQ(hitched.size(), 1u + StrokeResampler::MAX_HELD_STAMPS_PER_SAMPLE);

    // Moving: only distance deposits
    StrokeResampler moving(1.0f);
    std::vector<StrokeStamp> moved;
    moving.add(StrokeSample{0.0f, 0.0f, 0.0f, 1.0f}, moved);
    moving.add(StrokeSample{1.0f / 60.0f, 2.0f, 0.0f, 1.0f}, moved);
    EXPECT_EQ(moved.size(), 3u);
}
