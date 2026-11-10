// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/render/TitleTargetPool.h"

#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    struct FakeTarget : TitleTarget {
        TargetKey key;
        int* alive;
        FakeTarget(const TargetKey& k, int* a) : key(k), alive(a) { (*alive)++; }
        ~FakeTarget() override { (*alive)--; }
    };

    struct FakeAllocator : TitleTargetAllocator {
        int alive = 0;
        int made = 0;
        bool failNext = false;
        std::unique_ptr<TitleTarget> create(const TargetKey& key, std::string* error) override {
            if (failNext) {
                failNext = false;
                if (error) *error = "the GPU said no";
                return nullptr;
            }
            made++;
            return std::make_unique<FakeTarget>(key, &alive);
        }
    };

    TargetKey colour(int w, int h) {
        TargetKey key;
        key.width = w;
        key.height = h;
        key.format = ColorFormat::RGBA16F;
        return key;
    }

    class TitleTargetPoolTest : public ::testing::Test {};

}

TEST_F(TitleTargetPoolTest, DepthAndSamplesInKey) {
    FakeAllocator allocator;
    TitleTargetPool pool(allocator, 512u << 20);
    TargetKey msaa = colour(256, 256);
    msaa.depth = TitleDepthFormat::Depth;
    msaa.samples = 4;
    {
        TargetRef plain = pool.acquire(colour(256, 256));
        ASSERT_TRUE(plain);
    }
    // the idle 1x target serves neither a 4x request nor one with depth: each its own key
    TargetKey fourX = colour(256, 256);
    fourX.samples = 4;
    {
        TargetRef four = pool.acquire(fourX);
        ASSERT_TRUE(four);
        EXPECT_EQ(four.as<FakeTarget>()->key.samples, 4);
    }
    TargetRef deep = pool.acquire(msaa);
    ASSERT_TRUE(deep);
    EXPECT_EQ(deep.as<FakeTarget>()->key.samples, 4);
    EXPECT_EQ(deep.as<FakeTarget>()->key.depth, TitleDepthFormat::Depth);
    EXPECT_EQ(allocator.made, 3);
    EXPECT_EQ(pool.stats().depthOrMsaaAcquires, 2u);
    // the float formats are counted (S86 AC9: the RGBA8 path takes none)
    EXPECT_EQ(pool.stats().floatAcquires, 3u);
    TargetKey srgb = colour(64, 64);
    srgb.format = ColorFormat::SRGB8A8;
    EXPECT_TRUE(pool.acquire(srgb));
    EXPECT_EQ(pool.stats().floatAcquires, 3u);
    EXPECT_TRUE(isFloatColorFormat(ColorFormat::R16F));
    EXPECT_FALSE(isFloatColorFormat(ColorFormat::RGBA));
    EXPECT_EQ(pool.stats().bytesLive, targetBytes(msaa));
    EXPECT_EQ(targetBytes(msaa), (size_t)256 * 256 * (8 * 4 + 8 + 4 * 4)) << "4 samples, a resolve and a depth per sample";
    deep.reset();
    // freed by the same rules as colour targets
    for (int i = 0; i < TITLE_TARGET_IDLE_RENDERS; i++) pool.endRender();
    EXPECT_EQ(allocator.alive, 0);
    EXPECT_EQ(pool.stats().targets, 0u);
}

TEST_F(TitleTargetPoolTest, IdleTargetsAreReusedThenFreed) {
    FakeAllocator allocator;
    TitleTargetPool pool(allocator, 64u << 20);
    TitleTarget* first = nullptr;
    {
        TargetRef a = pool.acquire(colour(128, 64));
        first = a.get();
    }
    TargetRef b = pool.acquire(colour(128, 64));
    EXPECT_EQ(b.get(), first) << "an idle target of the key is lent again";
    EXPECT_EQ(pool.stats().reuses, 1u);
    b.reset();
    pool.endRender();
    pool.endRender();
    EXPECT_EQ(allocator.alive, 1) << "idle for two renders: kept";
    pool.endRender();
    EXPECT_EQ(allocator.alive, 0) << "idle for three complete renders: freed";
}

TEST_F(TitleTargetPoolTest, TheLimitEvictsIdleTargetsThenFails) {
    FakeAllocator allocator;
    const size_t one = targetBytes(colour(256, 256));
    TitleTargetPool pool(allocator, one * 2);
    TargetRef a = pool.acquire(colour(256, 256));
    {
        TargetRef idle = pool.acquire(colour(256, 256));
    }
    // the idle one goes so the new key fits
    TargetRef b = pool.acquire(colour(256, 255));
    ASSERT_TRUE(b);
    EXPECT_LE(pool.stats().bytesLive + pool.stats().bytesIdle, one * 2);
    std::string error;
    TargetRef c = pool.acquire(colour(256, 256), &error);
    EXPECT_FALSE(c) << "two lent targets fill the pool: the acquire fails";
    EXPECT_EQ(error.rfind("out_of_target_memory", 0), 0u) << error;
    EXPECT_EQ(pool.stats().failures, 1u);
    allocator.failNext = true;
    a.reset();
    pool.trim();
    TargetRef d = pool.acquire(colour(32, 32), &error);
    EXPECT_FALSE(d);
    EXPECT_EQ(error, "the GPU said no");
}

TEST_F(TitleTargetPoolTest, ClearInvalidatesLentRefs) {
    FakeAllocator allocator;
    TitleTargetPool pool(allocator, 64u << 20);
    TargetRef a = pool.acquire(colour(64, 64));
    ASSERT_TRUE(a);
    pool.clear();
    EXPECT_FALSE(a) << "after a device loss a held ref reads null";
    EXPECT_EQ(allocator.alive, 0);
    a.reset();
    EXPECT_EQ(pool.stats().bytesLive, 0u);
    TargetRef b = pool.acquire(colour(64, 64));
    EXPECT_TRUE(b);
    TargetRef moved = std::move(b);
    EXPECT_FALSE(b);
    EXPECT_TRUE(moved);
    EXPECT_EQ(pool.stats().peakBytes, targetBytes(colour(64, 64)));
}
