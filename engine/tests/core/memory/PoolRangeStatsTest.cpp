// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// ObjectPool, RangeAllocator, TrackedBytes and the stats registry (S06 AC9). Ported from
// OpenEngine's Modules/Memory/Tests/{ObjectPoolTests,RangeAllocatorTests,TrackedBytesTests}.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "memory/MemoryStats.h"
#include "memory/ObjectPool.h"
#include "memory/RangeAllocator.h"

#include "AllocationCounter.h"
#include "JobsTestUtil.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::memory;

namespace{
    bool findStatsEntry(const std::string& name, StatsEntry& out){
        for (StatsEntry& entry : snapshot()){
            if (entry.name == name){
                out = entry;
                return true;
            }
        }
        return false;
    }

    struct Counted {
        static int live;
        static int destroyed;
        int value;
        explicit Counted(int v): value(v){ live++; }
        ~Counted(){ live--; destroyed++; }
    };
    int Counted::live = 0;
    int Counted::destroyed = 0;

    struct Throwing {
        explicit Throwing(bool fail){ if (fail) throw std::runtime_error("ctor"); }
    };
}

// ---- ObjectPool

TEST(ObjectPoolTests, CreateReturnsStableAddressesAcrossGrowth){
    ObjectPool<Counted> pool("Tests.Pool.Stable");
    std::vector<Counted*> objects;
    for (int i = 0; i < 5000; i++)
        objects.push_back(pool.create(i));
    for (int i = 0; i < 5000; i++)
        ASSERT_EQ(objects[(size_t)i]->value, i);
    EXPECT_EQ(std::set<Counted*>(objects.begin(), objects.end()).size(), objects.size());
    EXPECT_EQ(pool.getLiveCount(), 5000u);
    for (Counted* object : objects)
        pool.destroy(object);
    EXPECT_EQ(pool.getLiveCount(), 0u);
}

TEST(ObjectPoolTests, DestroyRunsTheDestructorAndReusesTheSlotLifo){
    Counted::live = 0;
    ObjectPool<Counted> pool("Tests.Pool.Lifo");
    Counted* a = pool.create(1);
    Counted* b = pool.create(2);
    pool.destroy(a);
    pool.destroy(b);
    EXPECT_EQ(Counted::live, 0);
    EXPECT_EQ(pool.create(3), b) << "the last freed slot is reused first";
    EXPECT_EQ(pool.create(4), a);
    EXPECT_EQ(pool.getLiveCount(), 2u);
    pool.destroy(a);
    pool.destroy(b);
}

TEST(ObjectPoolTests, DestroyOfNullDoesNothing){
    ObjectPool<Counted> pool("Tests.Pool.Null");
    pool.destroy(nullptr);
    EXPECT_EQ(pool.getLiveCount(), 0u);
}

TEST(ObjectPoolTests, PoolDestructionDestroysEveryLiveObjectAcrossManyBlocks){
    Counted::live = 0;
    Counted::destroyed = 0;
    {
        ObjectPool<Counted> pool("Tests.Pool.Destruction", false);
        std::vector<Counted*> objects;
        for (int i = 0; i < 3000; i++)
            objects.push_back(pool.create(i));
        for (size_t i = 0; i < objects.size(); i += 3)
            pool.destroy(objects[i]);
    }
    EXPECT_EQ(Counted::live, 0);
    EXPECT_EQ(Counted::destroyed, 3000);
}

namespace{
    struct Sibling;
    ObjectPool<Sibling>* siblingPool = nullptr;
    int siblingDestroyed = 0;
    struct Sibling {
        Sibling* partner = nullptr;
        ~Sibling(){
            siblingDestroyed++;
            if (partner){
                Sibling* other = partner;
                partner = nullptr;
                other->partner = nullptr;
                siblingPool->destroy(other);
            }
        }
    };
}

TEST(ObjectPoolTests, ADestructorThatDestroysASiblingIsDestroyedOnce){
    siblingDestroyed = 0;
    {
        ObjectPool<Sibling> pool("Tests.Pool.Sibling", false);
        siblingPool = &pool;
        Sibling* a = pool.create();
        Sibling* b = pool.create();
        a->partner = b;
        b->partner = a;
    }
    EXPECT_EQ(siblingDestroyed, 2);
}

TEST(ObjectPoolTests, AConstructorThatThrowsReturnsItsSlot){
    ObjectPool<Throwing> pool("Tests.Pool.Throwing");
    EXPECT_THROW(pool.create(true), std::runtime_error);
    EXPECT_EQ(pool.getLiveCount(), 0u);
    Throwing* ok = pool.create(false);
    EXPECT_EQ(pool.getLiveCount(), 1u);
    pool.destroy(ok);
}

TEST(ObjectPoolTests, AnOverAlignedTypeIsServed){
    struct alignas(128) Wide {
        char bytes[3];
    };
    ObjectPool<Wide> pool("Tests.Pool.Aligned");
    std::vector<Wide*> objects;
    for (int i = 0; i < 100; i++){
        objects.push_back(pool.create());
        ASSERT_EQ(reinterpret_cast<uintptr_t>(objects.back()) % 128, 0u);
    }
    for (Wide* object : objects)
        pool.destroy(object);
}

TEST(ObjectPoolTests, ASlotLargerThanTheBlockCapGetsABlockOfItsOwn){
    struct Huge {
        char bytes[5 * 1024 * 1024];
    };
    ObjectPool<Huge> pool("Tests.Pool.Huge");
    Huge* a = pool.create();
    Huge* b = pool.create();
    EXPECT_NE(a, b);
    pool.destroy(a);
    pool.destroy(b);
}

TEST(ObjectPoolTests, TheSnapshotReportsThePoolUnderItsName){
    ObjectPool<Counted> pool("Tests.Pool.Snapshot");
    std::vector<Counted*> objects;
    for (int i = 0; i < 200; i++)
        objects.push_back(pool.create(i));
    StatsEntry entry;
    ASSERT_TRUE(findStatsEntry("Tests.Pool.Snapshot", entry));
    EXPECT_EQ(entry.kind, StatsKind::Pool);
    EXPECT_GT(entry.capacity, 0u);
    EXPECT_GT(entry.peak, 0u);
    EXPECT_LE(entry.count, 200u);
    for (Counted* object : objects)
        pool.destroy(object);
}

TEST(ObjectPoolTests, SteadyCreateDestroyAllocatesNothingAfterWarmup){
    ObjectPool<Counted> pool("Tests.Pool.Steady");
    std::vector<Counted*> objects;
    objects.reserve(1000);
    for (int i = 0; i < 1000; i++)
        objects.push_back(pool.create(i));
    for (Counted* object : objects)
        pool.destroy(object);
    objects.clear();
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this platform's test binary cannot count allocations";
    const testsupport::AllocationCountScope counting;
    for (int round = 0; round < 100; round++){
        for (int i = 0; i < 1000; i++)
            objects.push_back(pool.create(i));
        for (Counted* object : objects)
            pool.destroy(object);
        objects.clear();
    }
    EXPECT_EQ(counting.count(), 0u);
}

// ---- RangeAllocator

TEST(RangeAllocatorTests, AllocateGrantsExactlyTheRequestFromTheLowEnd){
    RangeAllocator ranges("Tests.Range.Exact", Residency::Gpu, 4, 1000, 16);
    RangeAllocator::Allocation a = ranges.allocate(100);
    RangeAllocator::Allocation b = ranges.allocate(50);
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());
    EXPECT_EQ(a.offset, 0u);
    EXPECT_EQ(a.size, 100u);
    EXPECT_EQ(b.offset, 100u);
    EXPECT_EQ(ranges.getUsed(), 150u);
    ranges.free(a);
    ranges.free(b);
    EXPECT_TRUE(ranges.isEmpty());
}

TEST(RangeAllocatorTests, AHoleOfTheRequestsSizeIsReusedBeforeALargerRangeIsSplit){
    RangeAllocator ranges("Tests.Range.Hole", Residency::Cpu, 1, 1000, 16);
    RangeAllocator::Allocation a = ranges.allocate(64);
    RangeAllocator::Allocation b = ranges.allocate(64);
    RangeAllocator::Allocation c = ranges.allocate(64);
    ranges.free(b);
    RangeAllocator::Allocation d = ranges.allocate(64);
    EXPECT_EQ(d.offset, 64u) << "best fit takes the exact hole";
    ranges.free(a);
    ranges.free(c);
    ranges.free(d);
}

TEST(RangeAllocatorTests, FreeMergesWithTheFreeNeighborOnEachSide){
    RangeAllocator ranges("Tests.Range.Merge", Residency::Cpu, 1, 300, 16);
    RangeAllocator::Allocation a = ranges.allocate(100);
    RangeAllocator::Allocation b = ranges.allocate(100);
    RangeAllocator::Allocation c = ranges.allocate(100);
    ranges.free(a);
    ranges.free(c);
    ranges.free(b);
    EXPECT_EQ(ranges.getStats().freeRanges, 1u);
    EXPECT_EQ(ranges.getStats().largestFreeUnits, 300u);
    RangeAllocator::Allocation whole = ranges.allocate(300);
    EXPECT_TRUE(whole.isValid());
    ranges.free(whole);
}

TEST(RangeAllocatorTests, ARequestThatDoesNotFitIsRefused){
    RangeAllocator ranges("Tests.Range.Refuse", Residency::Cpu, 1, 100, 16);
    RangeAllocator::Allocation a = ranges.allocate(60);
    EXPECT_FALSE(ranges.allocate(50).isValid());
    EXPECT_FALSE(ranges.allocate(0).isValid());
    EXPECT_FALSE(ranges.allocate(1000).isValid());
    EXPECT_EQ(ranges.getUsed(), 60u);
    ranges.free(a);
}

TEST(RangeAllocatorTests, AFitBehindTheBinsHeadIsFoundWhenNothingLargerIsFree){
    // 65 and 71 share a bin; the 65 was freed last (the bin's head) and nothing larger is free
    RangeAllocator ranges("Tests.Range.BinFallback", Residency::Cpu, 1, 138, 16);
    RangeAllocator::Allocation a = ranges.allocate(65);
    RangeAllocator::Allocation gap1 = ranges.allocate(1);
    RangeAllocator::Allocation c = ranges.allocate(71);
    RangeAllocator::Allocation gap2 = ranges.allocate(1);
    ASSERT_TRUE(gap2.isValid());
    const uint64_t cOffset = c.offset;
    ranges.free(c);
    ranges.free(a);
    RangeAllocator::Allocation d = ranges.allocate(70);
    ASSERT_TRUE(d.isValid()) << "71 units are free in one range";
    EXPECT_EQ(d.offset, cOffset);
    ranges.free(d);
    ranges.free(gap1);
    ranges.free(gap2);
    EXPECT_TRUE(ranges.isEmpty());
}

TEST(RangeAllocatorTests, FreeAfterReclaim){
    RangeAllocator ranges("Tests.Range.FreeAfter", Residency::Gpu, 1, 100, 16);
    RangeAllocator::Allocation a = ranges.allocate(100);
    ranges.freeAfter(a, 5);
    EXPECT_FALSE(ranges.allocate(10).isValid()) << "queued ranges stay in use";
    EXPECT_EQ(ranges.getStats().pendingRanges, 1u);
    ranges.reclaimThrough(4);
    EXPECT_FALSE(ranges.allocate(10).isValid()) << "not yet retired";
    ranges.reclaimThrough(5);
    EXPECT_TRUE(ranges.isEmpty());
    RangeAllocator::Allocation b = ranges.allocate(100);
    EXPECT_TRUE(b.isValid());
    // release is allocation-free
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this platform's test binary cannot count allocations";
    const testsupport::AllocationCountScope counting;
    ranges.freeAfter(b, 9);
    ranges.reclaimThrough(9);
    EXPECT_EQ(counting.count(), 0u);
}

TEST(RangeAllocatorTests, TryGrowInPlaceTakesOnlyTheFreeRangeAfterIt){
    RangeAllocator ranges("Tests.Range.Grow", Residency::Cpu, 1, 100, 16);
    RangeAllocator::Allocation a = ranges.allocate(10);
    RangeAllocator::Allocation b = ranges.allocate(10);
    EXPECT_FALSE(ranges.tryGrowInPlace(a, 20)) << "b follows a";
    EXPECT_TRUE(ranges.tryGrowInPlace(b, 50));
    EXPECT_EQ(b.size, 50u);
    EXPECT_EQ(ranges.getUsed(), 60u);
    ranges.free(a);
    ranges.free(b);
}

TEST(RangeAllocatorTests, RandomizedChurnKeepsRangesDisjointAndAccounted){
    RangeAllocator ranges("Tests.Range.Churn", Residency::Cpu, 1, 100000, 64);
    std::mt19937 random(99);
    std::vector<RangeAllocator::Allocation> live;
    uint64_t used = 0;
    for (int step = 0; step < 20000; step++){
        if (live.empty() || random() % 3 != 0){
            RangeAllocator::Allocation allocation = ranges.allocate(1 + random() % 500);
            if (allocation.isValid()){
                live.push_back(allocation);
                used += allocation.size;
            }
        }else{
            const size_t index = random() % live.size();
            used -= live[index].size;
            ranges.free(live[index]);
            live[index] = live.back();
            live.pop_back();
        }
        ASSERT_EQ(ranges.getUsed(), used);
    }
    std::map<uint64_t, uint64_t> byOffset;
    for (const auto& allocation : live)
        byOffset[allocation.offset] = allocation.size;
    uint64_t end = 0;
    for (const auto& entry : byOffset){
        ASSERT_GE(entry.first, end) << "ranges overlap";
        end = entry.first + entry.second;
    }
    for (const auto& allocation : live)
        ranges.free(allocation);
    EXPECT_TRUE(ranges.isEmpty());
    EXPECT_EQ(ranges.getStats().freeRanges, 1u);
}

TEST(RangeAllocatorTests, TheSnapshotReportsTheAllocatorInBytes){
    RangeAllocator ranges("Tests.Range.Snapshot", Residency::Gpu, 16, 1000, 16);
    RangeAllocator::Allocation a = ranges.allocate(100);
    StatsEntry entry;
    ASSERT_TRUE(findStatsEntry("Tests.Range.Snapshot", entry));
    EXPECT_EQ(entry.kind, StatsKind::Range);
    EXPECT_EQ(entry.residency, Residency::Gpu);
    EXPECT_EQ(entry.current, 1600u);
    EXPECT_EQ(entry.capacity, 16000u);
    EXPECT_EQ(entry.count, 1u);
    ranges.free(a);
}

// ---- TrackedBytes and the registry

TEST(TrackedBytesTests, ASnapshotReportsTheNameResidencyAndBytes){
    TrackedBytes tracked("Tests.Tracked.Basic", Residency::Mapped);
    tracked.add(1000);
    tracked.add(24, 2);
    tracked.setCapacity(4096);
    StatsEntry entry;
    ASSERT_TRUE(findStatsEntry("Tests.Tracked.Basic", entry));
    EXPECT_EQ(entry.kind, StatsKind::Tracked);
    EXPECT_EQ(entry.residency, Residency::Mapped);
    EXPECT_EQ(entry.current, 1024u);
    EXPECT_EQ(entry.count, 3u);
    EXPECT_EQ(entry.capacity, 4096u);
    EXPECT_EQ(entry.peak, 1024u);
}

TEST(TrackedBytesTests, SubtractingMoreThanWasAddedLeavesZero){
    TrackedBytes tracked("Tests.Tracked.Clamp", Residency::Cpu);
    tracked.add(10);
    tracked.subtract(100, 5);
    EXPECT_EQ(tracked.current(), 0u);
}

TEST(StatsRegistryTests, ADestroyedTrackedBytesLeavesTheSnapshot){
    {
        TrackedBytes tracked("Tests.Tracked.Gone", Residency::Cpu);
        StatsEntry entry;
        EXPECT_TRUE(findStatsEntry("Tests.Tracked.Gone", entry));
    }
    StatsEntry entry;
    EXPECT_FALSE(findStatsEntry("Tests.Tracked.Gone", entry));
}

TEST(StatsRegistryTests, ANameFromATemporaryStringSurvivesInTheSnapshot){
    std::unique_ptr<TrackedBytes> tracked;
    {
        std::string name = "Tests.Tracked.";
        name += "Temporary";
        tracked = std::make_unique<TrackedBytes>(name, Residency::Cpu);
    }
    StatsEntry entry;
    EXPECT_TRUE(findStatsEntry("Tests.Tracked.Temporary", entry));
}

TEST(StatsRegistryTests, ConcurrentRegistrationNeverHidesALiveRecord){
    TrackedBytes keeper("Tests.Tracked.Keeper", Residency::Cpu);
    std::atomic<bool> stop{false};
    std::vector<std::thread> churn;
    auto stopAndJoin = [&](std::atomic<bool>* flag){
        *flag = true;
        for (auto& thread : churn){
            if (thread.joinable())
                thread.join();
        }
    };
    // A fatal assertion must join the writers before their thread objects die.
    std::unique_ptr<std::atomic<bool>, decltype(stopAndJoin)> cleanup(&stop, stopAndJoin);
    for (int t = 0; t < 4; t++){
        churn.emplace_back([&, t]{
            while (!stop.load()){
                TrackedBytes temporary("Tests.Tracked.Churn" + std::to_string(t), Residency::Cpu);
                temporary.add(1);
            }
        });
    }
    for (int i = 0; i < testsupport::scaled(500, 50); i++){
        StatsEntry entry;
        ASSERT_TRUE(findStatsEntry("Tests.Tracked.Keeper", entry));
    }
    stop = true;
    for (auto& thread : churn)
        thread.join();
}

TEST(MemoryStats, SnapshotConsistentUnderWriters){
    // owner-serialized records publish a consistent set: current == count * 64,
    // capacity == count * 128, peak >= current
    RangeAllocator ranges("Tests.Stats.Seqlock", Residency::Cpu, 64, 1u << 20, 1024);
    std::atomic<bool> stop{false};
    std::thread writer([&]{
        std::vector<RangeAllocator::Allocation> live;
        std::mt19937 random(5);
        while (!stop.load()){
            if (live.size() < 512 && (live.empty() || random() % 2))
                live.push_back(ranges.allocate(1));
            else{
                ranges.free(live.back());
                live.pop_back();
            }
        }
        for (auto& allocation : live)
            ranges.free(allocation);
    });
    auto stopAndJoin = [&](std::thread* thread){
        stop = true;
        if (thread->joinable())
            thread->join();
    };
    // Install each guard before constructing the next writer or checking a record.
    std::unique_ptr<std::thread, decltype(stopAndJoin)> writerCleanup(&writer, stopAndJoin);
    TrackedBytes tracked("Tests.Stats.Tracked", Residency::Cpu);
    std::thread trackedWriter([&]{
        while (!stop.load()){
            tracked.add(64);
            tracked.subtract(64);
        }
    });
    std::unique_ptr<std::thread, decltype(stopAndJoin)> trackedCleanup(&trackedWriter, stopAndJoin);
    int checked = 0;
    for (int i = 0; i < testsupport::scaled(20000, 2000); i++){
        for (const StatsEntry& entry : snapshot()){
            if (entry.name == "Tests.Stats.Seqlock"){
                ASSERT_EQ(entry.current, entry.count * 64) << "a torn record";
                ASSERT_GE(entry.peak, entry.current);
                checked++;
            }else if (entry.name == "Tests.Stats.Tracked"){
                ASSERT_GE(entry.peak, entry.current);
            }
        }
    }
    stop = true;
    writer.join();
    trackedWriter.join();
    EXPECT_GT(checked, 0);
}
