// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The scheduler: stealing, lanes, waits, dependencies, cancellation, shutdown, single-thread
// mode and allocation-free steady state (S06 AC1, AC2, AC4, AC5, AC6, AC10). Every suite runs
// at the worker counts it applies to, from {0, 1, 2, 8}.

#include "jobs/JobSystem.h"
#include "thread/ThreadUtil.h"

#include "AllocationCounter.h"
#include "JobsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__linux__)
    #include <pthread.h>
    #include <time.h>
#endif

using namespace jasperengine;
using namespace jasperengine::jobs;
using testsupport::configFor;
using testsupport::WorkerBlocker;

// One fixture per suite, instantiated at the worker counts the suite applies to: the test
// names read "Workers/JobsWaitHelp.ClaimsPendingTaskInline/workers0".
#define JE_WORKER_SUITE(Suite, ...) \
    class Suite : public ::testing::TestWithParam<int> {}; \
    INSTANTIATE_TEST_SUITE_P(Workers, Suite, ::testing::Values(__VA_ARGS__), testsupport::workersName)

JE_WORKER_SUITE(JobsBasics, 0, 1, 2, 8);
JE_WORKER_SUITE(JobsSteal, 2, 8);
JE_WORKER_SUITE(JobsPriorityLanes, 0, 1);
JE_WORKER_SUITE(JobsWaitHelp, 0, 1);
JE_WORKER_SUITE(JobsDeps, 0, 1, 2, 8);
JE_WORKER_SUITE(JobsCancel, 0, 1, 2, 8);
JE_WORKER_SUITE(JobsSerial, 0, 1, 2, 8);
JE_WORKER_SUITE(JobsChannelCap, 2, 8);
JE_WORKER_SUITE(JobsAlloc, 0, 1, 2, 8);

// ---- basics

TEST_P(JobsBasics, SubmitReturnsTheResult){
    JobSystem system(configFor(GetParam()));
    Task<int> task = system.submit([]{ return 41 + 1; });
    EXPECT_EQ(task.get(), 42);
    EXPECT_EQ(task.status(), TaskStatus::Completed);
    EXPECT_TRUE(task.isReady());
}

TEST_P(JobsBasics, GetRethrowsTheJobsException){
    JobSystem system(configFor(GetParam()));
    Task<int> task = system.submit([]() -> int { throw std::runtime_error("decode failed"); });
    try{
        task.get();
        FAIL() << "get() did not rethrow";
    }catch(const std::runtime_error& e){
        EXPECT_STREQ(e.what(), "decode failed");
    }
    EXPECT_EQ(task.status(), TaskStatus::Failed);
}

TEST_P(JobsBasics, DestroyingAHandleNeitherCancelsNorBlocks){
    JobSystem system(configFor(GetParam()));
    std::atomic<bool> ran{false};
    Event release;
    {
        Task<void> task = system.submit([&]{ release.wait(); ran = true; });
    }
    release.set();
    if (system.isSingleThreaded())
        system.pump(10.0);
    EXPECT_TRUE(testsupport::spinUntil([&]{ return ran.load(); }, 5000.0));
}

TEST_P(JobsBasics, ThenPassesTheResult){
    JobSystem system(configFor(GetParam()));
    Task<int> doubled = system.submit([]{ return 21; }).then([](int& value){ return value * 2; });
    EXPECT_EQ(doubled.get(), 42);
}

// ---- AC1 stealing

TEST_P(JobsSteal, IdleWorkersStealFromBusyLocal){
    JobSystem system(configFor(GetParam()));
    constexpr int kChildren = 1000;
    std::atomic<int> done{0};
    std::atomic<int> onOtherThreads{0};
    Task<void> parent = system.submit([&]{
        const std::thread::id self = std::this_thread::get_id();
        std::vector<Task<void>> children;
        children.reserve(kChildren);
        for (int i = 0; i < kChildren; i++){
            children.push_back(system.submit([&, self]{
                if (std::this_thread::get_id() != self)
                    onOtherThreads++;
                done++;
            }));
        }
        // busy, not waiting (a wait would help run them here)
        while (done.load() < kChildren)
            std::this_thread::yield();
    });
    // polled: a wait would run the parent on this thread, outside the pool
    ASSERT_TRUE(testsupport::spinUntil([&]{ return parent.isReady(); }, 10000.0));
    parent.get();
    EXPECT_GE(onOtherThreads.load(), kChildren * 9 / 10);
    EXPECT_GT(system.stats().totals.stolen, 0u);
}

// ---- AC2 lanes

TEST_P(JobsPriorityLanes, HighBeforeNormalBeforeBackground){
    JobSystem system(configFor(GetParam()));
    std::mutex mutex;
    std::vector<Lane> order;
    std::vector<Task<void>> tasks;
    {
        WorkerBlocker blocker(system);
        for (int i = 0; i < 30; i++){
            const Lane lane = (Lane)(2 - i % 3);
            SubmitOptions options;
            options.lane = lane;
            tasks.push_back(system.submit([&, lane]{
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(lane);
            }, options));
        }
        if (system.isSingleThreaded()){
            blocker.unblock();
            system.pump(1000.0);
        }
    }
    // polled: a wait would run a task inline, out of lane order
    for (auto& task : tasks)
        ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 5000.0));
    ASSERT_EQ(order.size(), 30u);
    EXPECT_TRUE(std::is_sorted(order.begin(), order.end(), [](Lane a, Lane b){ return (int)a < (int)b; }));
}

TEST(JobsPriority, BackgroundProgressGuarantee){
    for (int workers : {3, 8}){
        Config config = configFor(workers);
        config.reserveForFrame = 2;
        JobSystem system(config);
        std::atomic<bool> flooding{true};
        std::atomic<int> inFlight{0};
        // a saturating Normal flood: the queue never drains while the feeders run
        std::vector<std::thread> feeders;
        for (int f = 0; f < 2; f++){
            feeders.emplace_back([&]{
                while (flooding.load()){
                    if (inFlight.load() > workers * 64){
                        std::this_thread::yield();
                        continue;
                    }
                    inFlight++;
                    system.enqueueDetached([&]{ testsupport::spinFor(50.0); inFlight--; });
                }
            });
        }
        ASSERT_TRUE(testsupport::spinUntil([&]{ return inFlight.load() > workers * 32; }, 5000.0));
        SubmitOptions options;
        options.lane = Lane::Background;
        const auto start = std::chrono::steady_clock::now();
        Task<double> background = system.submit([start]{ return testsupport::elapsedMs(start); }, options);
        // poll: a wait would run it inline
        ASSERT_TRUE(testsupport::spinUntil([&]{ return background.isReady(); }, 5000.0));
        flooding = false;
        for (auto& feeder : feeders)
            feeder.join();
        EXPECT_LE(background.get(), 100.0 * testsupport::timeScale()) << "workers " << workers;
    }
}

// ---- AC4 waits

TEST_P(JobsWaitHelp, ClaimsPendingTaskInline){
    JobSystem system(configFor(GetParam()));
    WorkerBlocker blocker(system);
    Task<std::thread::id> task = system.submit([]{ return std::this_thread::get_id(); });
    EXPECT_EQ(task.get(), std::this_thread::get_id()) << "a Pending task is run by its waiter";
}

TEST_P(JobsWaitHelp, RecursesIntoPendingDeps){
    JobSystem system(configFor(GetParam()));
    WorkerBlocker blocker(system);
    std::vector<int> order;
    std::vector<std::thread::id> threads;
    Task<void> a = system.submit([&]{ order.push_back(1); threads.push_back(std::this_thread::get_id()); });
    Task<void> b = system.submit([&]{ order.push_back(2); threads.push_back(std::this_thread::get_id()); }, {}, {a});
    Task<void> c = system.submit([&]{ order.push_back(3); threads.push_back(std::this_thread::get_id()); }, {}, {b});
    c.wait();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    for (const std::thread::id& id : threads)
        EXPECT_EQ(id, std::this_thread::get_id());
}

TEST_P(JobsWaitHelp, FilteredHelpOnlyCounterJobs){
    JobSystem system(configFor(GetParam()));
    WorkerBlocker blocker(system);
    std::atomic<bool> unrelatedRan{false};
    Task<void> unrelated = system.submit([&]{ unrelatedRan = true; });
    JobCounter counter(&system);
    std::atomic<int> counted{0};
    for (int i = 0; i < 5; i++)
        counter.run([&]{ counted++; });
    counter.wait();
    EXPECT_EQ(counted.load(), 5);
    EXPECT_FALSE(unrelatedRan.load()) << "a counter's waiter ran a job that is not the counter's";
    blocker.unblock();
    unrelated.wait();
}

TEST(JobsWait, CycleAsserts){
    // the scheduler's threads exist when the death test forks: re-execute instead
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({
        JobSystem system(configFor(1));
        struct Shared {
            TaskRef second;
            std::atomic<bool> ready{false};
        } shared;
        Task<void> first = system.submit([&shared]{
            while (!shared.ready.load())
                std::this_thread::yield();
            shared.second.wait();
        });
        shared.second = system.submit([]{}, {}, {first});
        shared.ready = true;
        first.wait();
    }, "wait cycle");
}

TEST(JobsWait, WorkerWaitOnSingleWorkerPool){
    JobSystem system(configFor(1));
    testsupport::runWithWatchdog(10.0, "WorkerWaitOnSingleWorkerPool", [&]{
        Task<int> outer = system.submit([&]{
            Task<int> inner = system.submit([&]{
                Task<int> innermost = system.submit([]{ return 40; });
                return innermost.get() + 1;
            });
            return inner.get() + 1;
        });
        EXPECT_EQ(outer.get(), 42);
    });
}

// ---- AC5 dependencies and cancellation

TEST_P(JobsDeps, RequireSuccessCascadesCancel){
    JobSystem system(configFor(GetParam()));
    std::atomic<bool> dependentRan{false};
    Task<void> failing = system.submit([]{ throw std::runtime_error("no"); });
    Task<void> dependent = system.submit([&]{ dependentRan = true; }, {}, {failing});
    Task<void> second = system.submit([&]{ dependentRan = true; }, {}, {dependent});
    second.wait();
    EXPECT_EQ(failing.status(), TaskStatus::Failed);
    EXPECT_EQ(dependent.status(), TaskStatus::Cancelled);
    EXPECT_EQ(second.status(), TaskStatus::Cancelled);
    EXPECT_FALSE(dependentRan.load());
    EXPECT_THROW(second.get(), TaskCancelled);
}

TEST_P(JobsDeps, AlwaysRunsAfterFailure){
    JobSystem system(configFor(GetParam()));
    Task<void> failing = system.submit([]{ throw std::runtime_error("no"); });
    SubmitOptions options;
    options.deps = DepPolicy::Always;
    Task<int> cleanup = system.submit([]{ return 7; }, options, {failing});
    EXPECT_EQ(cleanup.get(), 7);
    EXPECT_EQ(failing.status(), TaskStatus::Failed);
}

TEST_P(JobsDeps, DependentRunsAfterEveryDependency){
    JobSystem system(configFor(GetParam()));
    std::atomic<int> finished{0};
    std::vector<TaskRef> deps;
    for (int i = 0; i < 16; i++)
        deps.push_back(system.submit([&]{ testsupport::spinFor(100.0); finished++; }));
    Task<int> joined = system.submit([&]{ return finished.load(); }, {}, deps);
    EXPECT_EQ(joined.get(), 16);
}

TEST_P(JobsCancel, PendingNeverRuns){
    JobSystem system(configFor(GetParam()));
    const int iterations = testsupport::scaled(10000, 1000);
    int cancelledCount = 0;
    for (int i = 0; i < iterations; i++){
        std::atomic<int> ran{0};
        Task<void> task = system.submit([&ran]{ ran++; });
        if (i % 3 == 1)
            std::this_thread::yield();
        const bool cancelled = task.cancel();
        task.wait();
        ASSERT_TRUE(task.isReady());
        const bool wasCancelled = task.status() == TaskStatus::Cancelled;
        ASSERT_EQ(cancelled, wasCancelled);
        ASSERT_EQ(ran.load(), wasCancelled ? 0 : 1) << "exactly one of ran/cancelled";
        cancelledCount += wasCancelled ? 1 : 0;
    }
    EXPECT_GT(cancelledCount, 0);
}

TEST_P(JobsCancel, RunningSeesToken){
    JobSystem system(configFor(GetParam()));
    CancelToken token;
    SubmitOptions options;
    options.cancel = &token;
    std::atomic<bool> started{false};
    Task<bool> task = system.submit([&]{
        started = true;
        const auto start = std::chrono::steady_clock::now();
        while (!cancelRequested()){
            if (testsupport::elapsedMs(start) > 5000.0)
                return false;
            std::this_thread::yield();
        }
        return true;
    }, options);
    if (system.isSingleThreaded()){
        token.cancel();
        EXPECT_TRUE(task.get());
        return;
    }
    ASSERT_TRUE(testsupport::spinUntil([&]{ return started.load(); }, 5000.0));
    EXPECT_FALSE(cancelRequested()) << "outside a job nothing is cancelled";
    token.cancel();
    EXPECT_TRUE(task.get()) << "the running job observed its token";
    EXPECT_EQ(task.status(), TaskStatus::Completed);
}

TEST_P(JobsCancel, ACancelTokenCoversAGroup){
    JobSystem system(configFor(GetParam()));
    CancelToken token;
    CancelToken copy = token;
    copy.cancel();
    EXPECT_TRUE(token.isCancelled());
    SubmitOptions options;
    options.cancel = &token;
    Task<bool> task = system.submit([]{ return cancelRequested(); }, options);
    EXPECT_TRUE(task.get());
}

TEST(JobsShutdown, PendingCancelledDetachedRun){
    auto system = std::make_unique<JobSystem>(configFor(1));
    std::atomic<int> tasksRan{0};
    std::atomic<int> detachedRan{0};
    std::vector<Task<void>> tasks;
    auto blocker = std::make_unique<WorkerBlocker>(*system);
    for (int i = 0; i < 20; i++){
        tasks.push_back(system->submit([&]{ tasksRan++; }));
        system->enqueueDetached([&]{ detachedRan++; });
    }
    std::thread stopper([&]{ system->shutdown(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    blocker->unblock();
    stopper.join();
    for (auto& task : tasks){
        EXPECT_TRUE(task.isReady());
        EXPECT_EQ(task.status(), TaskStatus::Cancelled);
    }
    EXPECT_EQ(tasksRan.load(), 0);
    EXPECT_EQ(detachedRan.load(), 20);

    // after shutdown: a submit is Cancelled and never runs; a detached job runs on the caller
    Task<void> late = system->submit([&]{ tasksRan++; });
    EXPECT_EQ(late.status(), TaskStatus::Cancelled);
    system->enqueueDetached([&]{ detachedRan++; });
    EXPECT_EQ(detachedRan.load(), 21);
    EXPECT_EQ(tasksRan.load(), 0);
    blocker.reset();
}

// ---- idle workers park

#if defined(__linux__)
namespace{
    // The CPU clocks of every worker of `system`: one job per worker, each holding its worker
    // until all of them have started (so no two run on the same thread)
    std::vector<clockid_t> workerCpuClocks(JobSystem& system){
        const int workers = system.getWorkerCount();
        std::mutex mutex;
        std::vector<clockid_t> clocks;
        std::atomic<int> started{0};
        Event release;
        std::vector<Task<void>> holders;
        for (int i = 0; i < workers; i++){
            holders.push_back(system.submit([&]{
                clockid_t clock;
                if (pthread_getcpuclockid(pthread_self(), &clock) == 0){
                    std::lock_guard<std::mutex> lock(mutex);
                    clocks.push_back(clock);
                }
                started++;
                release.wait();
            }, {Lane::High, GateClass::Default, "cpu-clock"}));
        }
        // polled: a wait would run a holder on this thread
        const bool allStarted = testsupport::spinUntil([&]{ return started.load() == workers; }, 10000.0);
        release.set();
        for (auto& holder : holders)
            holder.wait();
        EXPECT_TRUE(allStarted);
        return clocks;
    }

    double cpuMs(const std::vector<clockid_t>& clocks){
        double total = 0.0;
        for (clockid_t clock : clocks){
            timespec time{};
            clock_gettime(clock, &time);
            total += (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1.0e6;
        }
        return total;
    }
}

TEST(JobsIdle, IdleWorkersUseUnderOnePercentOfACore){
    JobSystem system(configFor(8));
    const std::vector<clockid_t> clocks = workerCpuClocks(system);
    ASSERT_EQ(clocks.size(), 8u);
    // busy first: idle starts the way it does between frames or tests
    for (int round = 0; round < 20; round++)
        system.parallelFor(256, [](size_t){ testsupport::spinFor(10.0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const double before = cpuMs(clocks);
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const double wallMs = testsupport::elapsedMs(start);
    const double used = cpuMs(clocks) - before;
    std::printf("[idle] 8 idle workers: %.2f ms of CPU in %.0f ms (%.3f%% of a core)\n", used, wallMs, 100.0 * used / wallMs);
    EXPECT_LT(used / wallMs, 0.01) << "idle workers used " << used << " ms of CPU in " << wallMs << " ms";
}

TEST(JobsIdle, AParkedPoolWakesForNewWorkAtOnce){
    JobSystem system(configFor(8));
    for (int round = 0; round < 6; round++){
        // long enough for every worker to have parked, and to have parked for long
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 2 ? 250 : 30));
        const auto submitted = std::chrono::steady_clock::now();
        Task<void> task = system.submit([]{}, {round % 3 == 0 ? Lane::Background : Lane::Normal, GateClass::Default, "wake"});
        // polled, so a worker has to wake for it
        ASSERT_TRUE(testsupport::spinUntil([&]{ return task.isReady(); }, 10000.0));
        const double latencyMs = testsupport::elapsedMs(submitted);
        EXPECT_LT(latencyMs, 50.0 * testsupport::timeScale()) << "round " << round << ": a parked worker took " << latencyMs << " ms to wake";
    }
}
#endif

// ---- AC6 single-thread mode

TEST(JobsSingleThread, AllJobsRunViaPump){
    JobSystem system(configFor(0));
    ASSERT_TRUE(system.isSingleThreaded());
    std::atomic<int> ran{0};
    std::vector<TaskRef> tasks;
    SerialQueue serial("tests-serial", Lane::Background, GateClass::Default, &system);
    JobChannel channel("tests-channel", 1, &system);
    TaskRef previous;
    for (int i = 0; i < 5000; i++){
        SubmitOptions options;
        options.lane = (Lane)(i % 3);
        switch (i % 5){
            case 0:
                tasks.push_back(system.submit([&]{ ran++; }, options));
                break;
            case 1:
                if (previous)
                    tasks.push_back(system.submit([&]{ ran++; }, options, {previous}));
                else
                    tasks.push_back(system.submit([&]{ ran++; }, options));
                break;
            case 2:
                tasks.push_back(serial.push([&]{ ran++; }));
                break;
            case 3:
                tasks.push_back(channel.submit([&]{ ran++; }));
                break;
            default:
                tasks.push_back(system.submit([&]{
                    std::atomic<int> units{0};
                    system.parallelFor(16, [&](size_t){
                        system.parallelFor(4, [&](size_t){ units++; });
                    });
                    if (units.load() == 64)
                        ran++;
                }, options));
                break;
        }
        previous = tasks.back();
    }
    EXPECT_EQ(ran.load(), 0) << "single-thread mode runs nothing at submit";
    for (int frame = 0; frame < 10000 && ran.load() < 5000; frame++)
        system.pump(4.0);
    EXPECT_EQ(ran.load(), 5000);
    for (const TaskRef& task : tasks)
        EXPECT_EQ(task.status(), TaskStatus::Completed);
}

TEST(JobsSingleThread, BackgroundRespectsBudget){
    JobSystem system(configFor(0));
    std::atomic<int> ran{0};
    SubmitOptions options;
    options.lane = Lane::Background;
    std::vector<Task<void>> tasks;
    for (int i = 0; i < 50; i++)
        tasks.push_back(system.submit([&]{ testsupport::spinFor(1000.0); ran++; }, options));
    system.pump(4.0);
    EXPECT_GE(ran.load(), 1);
    EXPECT_LE(ran.load(), 6);
    for (auto& task : tasks)
        task.wait();
}

TEST(JobsSingleThread, NothingRunsWithoutAPumpOrAWait){
    JobSystem system(configFor(0));
    std::atomic<int> ran{0};
    for (int i = 0; i < 10; i++)
        system.enqueueDetached([&]{ ran++; });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(ran.load(), 0);
    system.pump(4.0);
    EXPECT_EQ(ran.load(), 10);
}

// ---- strands and channels

TEST_P(JobsSerial, SerialQueueRunsInOrderOneAtATime){
    JobSystem system(configFor(GetParam()));
    SerialQueue serial("tests-strand", Lane::Normal, GateClass::Default, &system);
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::vector<int> order;
    std::vector<Task<void>> tasks;
    for (int i = 0; i < 200; i++){
        tasks.push_back(serial.push([&, i]{
            const int now = ++running;
            int seen = maxRunning.load();
            while (now > seen && !maxRunning.compare_exchange_weak(seen, now)){
            }
            order.push_back(i);
            running--;
        }));
    }
    tasks.back().wait();
    serial.waitIdle();
    EXPECT_EQ(maxRunning.load(), 1);
    ASSERT_EQ(order.size(), 200u);
    for (int i = 0; i < 200; i++)
        EXPECT_EQ(order[(size_t)i], i);
}

TEST_P(JobsSerial, SerialQueueCancelPendingDropsTheRest){
    JobSystem system(configFor(GetParam()));
    SerialQueue serial("tests-strand-cancel", Lane::Normal, GateClass::Default, &system);
    Event release;
    std::atomic<int> ran{0};
    Task<void> first = serial.push([&]{ release.wait(); ran++; });
    std::vector<Task<void>> rest;
    for (int i = 0; i < 10; i++)
        rest.push_back(serial.push([&]{ ran++; }));
    if (!system.isSingleThreaded())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    serial.cancelPending();
    release.set();
    serial.waitIdle();
    for (auto& task : rest)
        EXPECT_EQ(task.status(), TaskStatus::Cancelled);
    EXPECT_LE(ran.load(), 1);
}

TEST_P(JobsChannelCap, ChannelRespectsItsCap){
    JobSystem system(configFor(GetParam()));
    JobChannel channel("tests-cap", 2, &system);
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::vector<Task<void>> tasks;
    for (int i = 0; i < 12; i++){
        tasks.push_back(channel.submit([&]{
            const int now = ++running;
            int seen = maxRunning.load();
            while (now > seen && !maxRunning.compare_exchange_weak(seen, now)){
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            running--;
        }));
    }
    for (auto& task : tasks)
        task.wait();
    EXPECT_EQ(maxRunning.load(), 2);
    const Stats stats = system.stats();
    EXPECT_GE(stats.blockingThreads, 1u);
}

TEST(JobsChannel, BlockingThreadsNeverRunComputeJobs){
    JobSystem system(configFor(2));
    Task<std::string> blockingName = system.processChannel().submit([]{
        std::thread::id id = std::this_thread::get_id();
        (void)id;
        return std::string(isWorker() ? "worker" : "blocking");
    });
    EXPECT_EQ(blockingName.get(), "blocking");
}

// ---- postToMain and identity

TEST(JobsMain, PostToMainRunsOnDrain){
    JobSystem system(configFor(2));
    std::atomic<int> ran{0};
    system.submit([&]{ postToMain([&]{ ran++; }); }).wait();
    EXPECT_EQ(ran.load(), 0);
    EXPECT_EQ(drainMainQueue(), 1u);
    EXPECT_EQ(ran.load(), 1);
}

TEST(JobsMain, IdentityQueries){
    JobSystem system(configFor(2));
    EXPECT_TRUE(isMainThread());
    EXPECT_FALSE(isWorker());
    EXPECT_FALSE(isExecutingJob());
    Task<bool> onWorker = system.submit([]{ return isWorker() && isExecutingJob() && !isMainThread(); });
    // a wait may run it inline on the main thread; a poll lets a worker take it
    ASSERT_TRUE(testsupport::spinUntil([&]{ return onWorker.isReady(); }, 5000.0));
    EXPECT_TRUE(onWorker.get());
}

// ---- stats, hooks, occupancy

namespace{
    std::atomic<int> hookedBegins{0};
    std::atomic<int> hookedEnds{0};
    void countBegin(const char*, Lane, int){ hookedBegins++; }
    void countEnd(){ hookedEnds++; }
}

TEST(JobsHooks, JobsReportBeginAndEnd){
    JobSystem system(configFor(2));
    ProfilerHooks hooks;
    hooks.onJobBegin = &countBegin;
    hooks.onJobEnd = &countEnd;
    hookedBegins = 0;
    hookedEnds = 0;
    setProfilerHooks(hooks);
    for (int i = 0; i < 10; i++)
        system.submit([]{}).wait();
    setProfilerHooks(ProfilerHooks());
    EXPECT_EQ(hookedBegins.load(), 10);
    EXPECT_EQ(hookedEnds.load(), 10);
}

TEST(JobsStats, CountsAndOccupancy){
    JobSystem system(configFor(2));
    Event release;
    std::atomic<bool> started{false};
    SubmitOptions options;
    options.name = "occupancy-probe";
    options.lane = Lane::Background;
    Task<void> task = system.submit([&]{ started = true; release.wait(); }, options);
    ASSERT_TRUE(testsupport::spinUntil([&]{ return started.load(); }, 5000.0));
    Stats stats = system.stats();
    EXPECT_EQ(stats.workers, 2);
    EXPECT_EQ(stats.lanes[(size_t)Lane::Background].running, 1u);
    ThreadOccupancy threads[64];
    const size_t count = snapshotOccupancy(threads, 64);
    bool found = false;
    for (size_t i = 0; i < count; i++){
        if (threads[i].runningJob && std::string(threads[i].runningJob) == "occupancy-probe"){
            found = true;
            EXPECT_EQ(threads[i].kind, ThreadKind::Worker);
            EXPECT_EQ(std::string(threads[i].name).rfind("je-worker-", 0), 0u);
            EXPECT_GT(threads[i].sinceNs, 0u);
        }
    }
    EXPECT_TRUE(found);
    release.set();
    task.wait();
    system.sampleFrame();
    stats = system.stats(1);
    EXPECT_EQ(stats.perWorker.size(), 2u);
    EXPECT_GE(stats.totals.executed, 1u);
}

TEST(JobsStats, AssetJobsCountWhileQueuedOrRunning){
    JobSystem system(configFor(1));
    Event release;
    std::atomic<bool> started{false};
    SubmitOptions options;
    options.tags = Tag::Asset;
    Task<void> running = system.submit([&]{ started = true; release.wait(); }, options);
    ASSERT_TRUE(testsupport::spinUntil([&]{ return started.load(); }, 5000.0));
    Task<void> queued = system.submit([]{}, options);
    EXPECT_EQ(system.stats().assetJobs, 2u);
    release.set();
    running.wait();
    queued.wait();
    EXPECT_TRUE(testsupport::spinUntil([&]{ return system.stats().assetJobs == 0; }, 5000.0));
}

// ---- AC10 allocation-free steady state

namespace{
    struct Capture64 {
        uint64_t values[8];
    };
    static_assert(sizeof(Capture64) == 64, "a 64-byte capture");

    void steadyCycle(JobSystem& system, std::atomic<uint64_t>& sink){
        system.parallelFor(64, [&](size_t i){ sink.fetch_add(i, std::memory_order_relaxed); });
        Capture64 capture{{1, 2, 3, 4, 5, 6, 7, 8}};
        Task<void> task = system.submit([capture, &sink]{ sink.fetch_add(capture.values[7], std::memory_order_relaxed); });
        task.wait();
        JobCounter counter(&system);
        for (int i = 0; i < 4; i++)
            counter.run([&sink]{ sink.fetch_add(1, std::memory_order_relaxed); });
        counter.wait();
        // single-thread mode: drops the queue entries of the jobs the waits ran inline
        system.pump(0.0);
    }
}

TEST_P(JobsAlloc, SteadyStateZero){
    JobSystem system(configFor(GetParam()));
    std::atomic<uint64_t> sink{0};
    constexpr int kWindow = 10000;
    for (int i = 0; i < 2000; i++)
        steadyCycle(system, sink);
    // Warm-up for the worst case: a queue entry is freed when a worker dequeues it, even for
    // work a waiter already ran, so a window whose workers are preempted holds more blocks.
    // With every worker held for a whole window, the slab pools grow to that backlog.
    {
        std::vector<std::unique_ptr<WorkerBlocker>> blockers;
        for (int w = 0; w < system.getWorkerCount(); w++)
            blockers.push_back(std::make_unique<WorkerBlocker>(system));
        for (int i = 0; i < kWindow; i++)
            steadyCycle(system, sink);
    }
    ASSERT_TRUE(testsupport::spinUntil([&]{
        const Stats stats = system.stats();
        return stats.lanes[0].queued + stats.lanes[1].queued + stats.lanes[2].queued == 0;
    }, 30000.0));
    for (int i = 0; i < 2000; i++)
        steadyCycle(system, sink);

    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "this platform's test binary cannot count allocations";
    const testsupport::AllocationCountScope scope;
    for (int i = 0; i < kWindow; i++)
        steadyCycle(system, sink);
    EXPECT_EQ(scope.count(), 0u) << "the steady parallelFor + submit + JobCounter cycle reached the heap";
}

// ---- AC22 worker policy

TEST(JobsWorkerPolicy, DefaultWorkerCountTable){
    using thread::PlatformClass;
    using thread::workerCountFor;
    EXPECT_EQ(workerCountFor(PlatformClass::Desktop, 1), 1);
    EXPECT_EQ(workerCountFor(PlatformClass::Desktop, 2), 1);
    EXPECT_EQ(workerCountFor(PlatformClass::Desktop, 8), 7);
    EXPECT_EQ(workerCountFor(PlatformClass::Desktop, 64), 15);
    EXPECT_EQ(workerCountFor(PlatformClass::Android, 8), 4);
    EXPECT_EQ(workerCountFor(PlatformClass::Android, 4), 3);
    EXPECT_EQ(workerCountFor(PlatformClass::Android, 1), 1);
    EXPECT_EQ(workerCountFor(PlatformClass::IOS, 6), 4);
    EXPECT_EQ(workerCountFor(PlatformClass::IOS, 2), 1);
    // the threaded web build's pool of 8 (engine/CMakeLists.txt)
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 8, 8), 4);
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 4, 8), 2);
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 2, 8), 1);
    EXPECT_EQ(workerCountFor(PlatformClass::WebSingle, 16), 0);
}

TEST(JobsWorkerPolicy, ThreadedWebWorkersFitThePthreadPool){
    using thread::PlatformClass;
    using thread::workerCountFor;
    // a pool of 4 (S02's gate found hardware_concurrency() - 1 threads asked of it): one worker,
    // the rest for the blocking thread, a video decoder and a spare
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 16, 4), 1);
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 16, 6), 3);
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 16, 8), 4);
    EXPECT_EQ(workerCountFor(PlatformClass::WebThreads, 16, 32), 4);
    for (int pool = 1; pool <= 16; pool++){
        const int workers = workerCountFor(PlatformClass::WebThreads, 64, pool);
        EXPECT_GE(workers, 1);
        if (pool > thread::kWebPoolThreadsBesideWorkers){
            EXPECT_LE(workers + thread::kWebPoolThreadsBesideWorkers, pool) << "pool " << pool;
        }
    }
    // not a threaded web build: no pool
    EXPECT_EQ(thread::webThreadPoolSize(), 0);
}

TEST(JobsWorkerPolicy, ConfiguredCountWins){
    EXPECT_EQ(thread::resolveWorkerCount(3), 3);
    EXPECT_EQ(thread::resolveWorkerCount(0), 0);
    EXPECT_EQ(thread::resolveWorkerCount(40), 15);
}
