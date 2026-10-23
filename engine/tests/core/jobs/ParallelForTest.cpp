// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// parallelFor, parallelForChunked and parallelSort (S06 AC3). The ParallelAlgorithms cases
// are ported from OpenEngine's Modules/JobSystem/Tests/ParallelAlgorithmsTests.cpp, Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "jobs/JobSystem.h"

#include "JobsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::jobs;
using testsupport::configFor;

namespace{
    class ParallelAlgorithms : public ::testing::TestWithParam<int> {};
    INSTANTIATE_TEST_SUITE_P(Workers, ParallelAlgorithms, ::testing::Values(0, 1, 2, 8), testsupport::workersName);

    class JobsParallelFor : public ::testing::TestWithParam<int> {};
    INSTANTIATE_TEST_SUITE_P(Workers, JobsParallelFor, ::testing::Values(0, 1, 2, 8), testsupport::workersName);
}

// ---- ported ParallelAlgorithmsTests

TEST_P(ParallelAlgorithms, ParallelForEmptyRange){
    JobSystem system(configFor(GetParam()));
    std::atomic<int> calls{0};
    EXPECT_TRUE(system.parallelFor(0, [&](size_t){ calls++; }));
    system.parallelForChunked(0, 0, 16, [&](size_t, size_t){ calls++; });
    EXPECT_EQ(calls.load(), 0);
}

TEST_P(ParallelAlgorithms, ParallelForSingleUnitRunsOnTheCaller){
    JobSystem system(configFor(GetParam()));
    std::thread::id ranOn;
    system.parallelFor(1, [&](size_t){ ranOn = std::this_thread::get_id(); });
    EXPECT_EQ(ranOn, std::this_thread::get_id());
}

TEST_P(ParallelAlgorithms, ParallelForLargeRangeCoversAll){
    JobSystem system(configFor(GetParam()));
    constexpr size_t kCount = 100000;
    std::vector<int> hits(kCount, 0);
    ParallelForOptions options;
    options.grain = 64;
    system.parallelFor(kCount, [&](size_t i){ hits[i]++; }, options);
    for (size_t i = 0; i < kCount; i++)
        ASSERT_EQ(hits[i], 1) << "unit " << i;
}

TEST_P(ParallelAlgorithms, ParallelForUnitsAllCompleteBeforeReturn){
    JobSystem system(configFor(GetParam()));
    for (int round = 0; round < 50; round++){
        std::atomic<int> done{0};
        system.parallelFor(32, [&](size_t){ testsupport::spinFor(20.0); done++; });
        ASSERT_EQ(done.load(), 32) << "round " << round;
    }
}

TEST_P(ParallelAlgorithms, ParallelForUnitsConcurrentFromMultipleThreads){
    JobSystem system(configFor(GetParam()));
    std::vector<std::thread> threads;
    std::atomic<int> total{0};
    for (int t = 0; t < 4; t++){
        threads.emplace_back([&]{
            for (int round = 0; round < 20; round++)
                system.parallelFor(100, [&](size_t){ total++; });
        });
    }
    for (auto& thread : threads)
        thread.join();
    EXPECT_EQ(total.load(), 4 * 20 * 100);
}

TEST_P(ParallelAlgorithms, ParallelForUnitsRepeatedCalls){
    JobSystem system(configFor(GetParam()));
    for (int round = 0; round < 500; round++){
        std::atomic<int> count{0};
        system.parallelFor(17, [&](size_t){ count++; });
        ASSERT_EQ(count.load(), 17);
    }
}

TEST_P(ParallelAlgorithms, ParallelForStopsOnFalse){
    JobSystem system(configFor(0));
    std::vector<size_t> ran;
    const bool completed = system.parallelFor(10, [&](size_t i){
        ran.push_back(i);
        return i < 3;
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(ran, (std::vector<size_t>{0, 1, 2, 3})) << "inline runs in order and stops at the false body";

    JobSystem parallel(configFor(GetParam()));
    std::atomic<int> count{0};
    const bool stopped = !parallel.parallelFor(100000, [&](size_t){ return ++count < 50; });
    EXPECT_TRUE(stopped);
    EXPECT_LT(count.load(), 100000);
}

TEST_P(ParallelAlgorithms, ParallelForStopPollEndsTheRun){
    JobSystem system(configFor(GetParam()));
    std::atomic<int> count{0};
    const std::function<bool()> stop = [&]{ return count.load() >= 10; };
    ParallelForOptions options;
    options.stop = &stop;
    EXPECT_FALSE(system.parallelFor(100000, [&](size_t){ count++; testsupport::spinFor(1.0); }, options));
    EXPECT_LT(count.load(), 100000);
}

TEST_P(ParallelAlgorithms, ParallelForUnitThrowReachesTheCaller){
    JobSystem system(configFor(GetParam()));
    std::atomic<int> after{0};
    try{
        system.parallelFor(1000, [&](size_t i){
            if (i == 500)
                throw std::runtime_error("unit 500");
            after++;
        });
        FAIL() << "the exception did not reach the caller";
    }catch(const std::runtime_error& e){
        EXPECT_STREQ(e.what(), "unit 500");
    }
    EXPECT_LT(after.load(), 1000);
}

TEST_P(ParallelAlgorithms, ParallelForChunkedCoversEveryElementOnce){
    JobSystem system(configFor(GetParam()));
    for (size_t count : {1u, 7u, 63u, 64u, 65u, 1000u, 4097u}){
        std::vector<int> hits(count, 0);
        system.parallelForChunked(0, count, 64, [&](size_t begin, size_t end){
            ASSERT_LE(end, count);
            ASSERT_LT(begin, end);
            for (size_t i = begin; i < end; i++)
                hits[i]++;
        });
        for (size_t i = 0; i < count; i++)
            ASSERT_EQ(hits[i], 1) << "count " << count << " element " << i;
    }
}

TEST_P(ParallelAlgorithms, ParallelForChunkThrowReachesTheCaller){
    JobSystem system(configFor(GetParam()));
    EXPECT_THROW(system.parallelForChunked(0, 10000, 100, [](size_t begin, size_t){
        if (begin == 5000)
            throw std::logic_error("chunk");
    }), std::logic_error);
}

TEST_P(ParallelAlgorithms, ParallelSortMatchesStdSort){
    JobSystem system(configFor(GetParam()));
    std::mt19937 random(1234);
    for (size_t count : {0u, 1u, 2u, 100u, 4999u, 5000u, 100000u}){
        std::vector<int> values(count);
        for (int& value : values)
            value = (int)(random() % 1000);
        std::vector<int> expected = values;
        std::sort(expected.begin(), expected.end());
        system.parallelSort(values.begin(), values.end());
        ASSERT_EQ(values, expected) << "count " << count;
    }
}

TEST_P(ParallelAlgorithms, ParallelSortCustomComparatorAndShapes){
    JobSystem system(configFor(GetParam()));
    std::vector<int> descending(20000);
    std::iota(descending.begin(), descending.end(), 0);
    system.parallelSort(descending.begin(), descending.end(), std::greater<int>());
    EXPECT_TRUE(std::is_sorted(descending.begin(), descending.end(), std::greater<int>()));

    std::vector<int> duplicates(20000, 7);
    system.parallelSort(duplicates.begin(), duplicates.end());
    EXPECT_TRUE(std::all_of(duplicates.begin(), duplicates.end(), [](int v){ return v == 7; }));

    std::vector<int> reversed(20000);
    std::iota(reversed.rbegin(), reversed.rend(), 0);
    system.parallelSort(reversed.begin(), reversed.end());
    EXPECT_TRUE(std::is_sorted(reversed.begin(), reversed.end()));
}

// ---- AC3 nesting

TEST_P(JobsParallelFor, NestedFromWorkersNoDeadlock){
    JobSystem system(configFor(GetParam()));
    const int iterations = testsupport::scaled(1000, 40);
    testsupport::runWithWatchdog(10.0 * testsupport::timeScale(), "NestedFromWorkersNoDeadlock", [&]{
        for (int iteration = 0; iteration < iterations; iteration++){
            std::vector<Task<int>> outer;
            for (int job = 0; job < 16; job++){
                outer.push_back(system.submit([&system]{
                    std::atomic<int> units{0};
                    system.parallelFor(64, [&](size_t){
                        // the inner units run on several threads too
                        std::atomic<int> local{0};
                        system.parallelFor(64, [&](size_t){ local.fetch_add(1, std::memory_order_relaxed); });
                        units.fetch_add(local.load(), std::memory_order_relaxed);
                    });
                    return units.load();
                }));
            }
            for (auto& task : outer)
                ASSERT_EQ(task.get(), 64 * 64);
        }
    });
}

namespace{
    std::atomic<int> helperBackground{0};
    std::atomic<int> helperOther{0};
    void recordHelperLane(const char* name, Lane lane, int){
        if (std::string(name) != "parallel-for")
            return;
        if (lane == Lane::Background)
            helperBackground++;
        else
            helperOther++;
    }
}

TEST(JobsParallelForLanes, InsideBackgroundForksBackground){
    JobSystem system(configFor(8));
    helperBackground = 0;
    helperOther = 0;
    ProfilerHooks hooks;
    hooks.onJobBegin = &recordHelperLane;
    setProfilerHooks(hooks);
    SubmitOptions options;
    options.lane = Lane::Background;
    for (int round = 0; round < 20; round++){
        Task<int> task = system.submit([&system]{
            std::atomic<int> units{0};
            ParallelForOptions forkOptions;
            forkOptions.lane = Lane::High;   // clamped: a Background body forks Background
            system.parallelFor(256, [&](size_t){ testsupport::spinFor(5.0); units++; }, forkOptions);
            return units.load();
        }, options);
        // polled, so a worker runs the body (a wait would run it on this thread, outside any lane)
        ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 10000.0));
        ASSERT_EQ(task.get(), 256);
    }
    setProfilerHooks(ProfilerHooks());
    EXPECT_EQ(helperOther.load(), 0) << "a Background body forked helpers into a more urgent lane";
    EXPECT_GT(helperBackground.load(), 0);
}

TEST(JobsParallelForLanes, ANormalBodyNeverForksHigh){
    JobSystem system(configFor(8));
    helperBackground = 0;
    helperOther = 0;
    std::atomic<int> highHelpers{0};
    static std::atomic<int>* high = nullptr;
    high = &highHelpers;
    ProfilerHooks hooks;
    hooks.onJobBegin = [](const char* name, Lane lane, int){
        if (std::string(name) == "parallel-for" && lane == Lane::High)
            (*high)++;
    };
    setProfilerHooks(hooks);
    Task<void> task = system.submit([&system]{
        ParallelForOptions forkOptions;
        forkOptions.lane = Lane::High;
        system.parallelFor(256, [](size_t){ testsupport::spinFor(5.0); }, forkOptions);
    });
    ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 10000.0));
    setProfilerHooks(ProfilerHooks());
    EXPECT_EQ(highHelpers.load(), 0);
}
