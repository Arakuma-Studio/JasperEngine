// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// je-bench-jobs: the scheduler's budgets (S06 R23-R25, AC11-AC13) on 8 workers.
//
//   je-bench-jobs [--json <file>] [--workers <n>]      default file: je-bench-jobs.json
//
// 8 workers by default, hardware threads - 2 on a smaller machine: the budgets assume the
// workers and the producing thread each have a core, and one more is left to the rest of the
// machine. 8 workers + 1 on 8 cores measures preemption, not the scheduler. On a shared build
// server, one busy neighbour then still leaves R25's frame its cores.
//
// The thresholds are in tools/perf/bench-thresholds.json; tools/perf/check_bench.py compares.

#include "Engine.h"
#include "jobs/JobSystem.h"

#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::jobs;

namespace{

    int kWorkers = 8;

    Config benchConfig(int workers = kWorkers){
        Config config;
        config.workers = workers;
        config.reserveForFrame = 2;
        config.detectWaitCycles = false;
        return config;
    }

    // R23: empty jobs from one producer, published and executed. `publishRate` is the
    // producer's side alone (diagnostic: which end limits the throughput)
    double submitThroughput(double& publishRate){
        JobSystem system(benchConfig());
        constexpr int kJobs = 2000000;
        double best = 0.0;
        publishRate = 0.0;
        for (int trial = 0; trial < 3; trial++){
            const uint64_t before = system.stats().totals.executed;
            const auto start = bench::Clock::now();
            for (int i = 0; i < kJobs; i++)
                system.enqueueDetached([]{});
            publishRate = std::max(publishRate, kJobs / bench::secondsSince(start));
            while (system.stats().totals.executed - before < (uint64_t)kJobs)
                std::this_thread::yield();
            best = std::max(best, kJobs / bench::secondsSince(start));
        }
        return best;
    }

    // R23: parallelFor of one unit (no helper): the call's own overhead
    double parallelForDispatchNs(){
        JobSystem system(benchConfig());
        constexpr int kCalls = 5000000;
        unsigned sink = 0;
        double best = 1e9;
        for (int trial = 0; trial < 3; trial++){
            const auto start = bench::Clock::now();
            for (int i = 0; i < kCalls; i++)
                system.parallelFor(1, [&](size_t unit){ sink += (unsigned)unit + 1; });
            best = std::min(best, bench::secondsSince(start) * 1e9 / kCalls);
        }
        if (sink == 0)
            std::printf("unreachable\n");
        return best;
    }

    // R23: submit + wait on an idle pool, the job run by a worker (polled, not helped)
    void roundTrip(double& p99Us, double& waitP99Us){
        JobSystem system(benchConfig());
        std::vector<double> samples;
        samples.reserve(20000);
        for (int i = 0; i < 20000; i++){
            const auto start = bench::Clock::now();
            Task<void> task = system.submit([]{});
            while (!task.isReady())
                std::this_thread::yield();
            samples.push_back(bench::secondsSince(start) * 1e6);
        }
        p99Us = bench::percentile(samples, 0.99);
        samples.clear();
        for (int i = 0; i < 20000; i++){
            const auto start = bench::Clock::now();
            system.submit([]{}).wait();
            samples.push_back(bench::secondsSince(start) * 1e6);
        }
        waitP99Us = bench::percentile(samples, 0.99);
    }

    // R24: 1 M units of ~1 µs
    double flatSpeedup(){
        constexpr size_t kUnits = 1000000;
        std::vector<unsigned> out(kUnits);
        const auto serialStart = bench::Clock::now();
        for (size_t i = 0; i < kUnits; i++)
            out[i] = bench::spinWork(1.0);
        const double serial = bench::secondsSince(serialStart);

        JobSystem system(benchConfig());
        ParallelForOptions options;
        options.grain = 64;
        double best = 1e9;
        for (int trial = 0; trial < 3; trial++){
            const auto start = bench::Clock::now();
            system.parallelFor(kUnits, [&](size_t i){ out[i] = bench::spinWork(1.0); }, options);
            best = std::min(best, bench::secondsSince(start));
        }
        return serial / best;
    }

    // R24: 8 concurrent worker jobs, each parallelFor(64) of parallelFor(64) units of ~1 µs
    double nestedSpeedup(int iterations, bool& completed){
        auto nestedRun = [](JobSystem& system){
            std::vector<Task<unsigned>> jobs;
            for (int job = 0; job < 8; job++){
                jobs.push_back(system.submit([&system]{
                    std::atomic<unsigned> sum{0};
                    system.parallelFor(64, [&](size_t){
                        unsigned local = 0;
                        system.parallelFor(64, [&](size_t){ local += bench::spinWork(1.0); });
                        sum.fetch_add(local, std::memory_order_relaxed);
                    });
                    return sum.load();
                }));
            }
            for (auto& job : jobs)
                job.get();
        };
        JobSystem serialSystem(benchConfig(0));
        const auto serialStart = bench::Clock::now();
        for (int i = 0; i < 20; i++)
            nestedRun(serialSystem);
        const double serial = bench::secondsSince(serialStart) / 20;

        JobSystem system(benchConfig());
        const auto start = bench::Clock::now();
        for (int i = 0; i < iterations; i++)
            nestedRun(system);
        const double parallel = bench::secondsSince(start) / iterations;
        completed = true;
        return serial / parallel;
    }

    // ---- R25: the frame's High parallelFor while Heavy Background work is queued

    void frameUnits(JobSystem& system, std::vector<double>& samples, int iterations){
        ParallelForOptions options;
        options.lane = Lane::High;
        for (int i = 0; i < iterations; i++){
            const auto start = bench::Clock::now();
            system.parallelFor(8, [](size_t){ bench::spinWork(1000.0); }, options);
            samples.push_back(bench::secondsSince(start) * 1000.0);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    // One run of R25: 500 frames of 8 x 1 ms High units while 200 Heavy cooks stay queued
    double frameProtectionRunP99Ms(){
        JobSystem system(benchConfig());
        std::atomic<int> queued{0};
        std::atomic<bool> stop{false};
        SubmitOptions heavy;
        heavy.lane = Lane::Background;
        heavy.gate = GateClass::Heavy;
        heavy.name = "bench-cook";
        auto topUp = [&]{
            while (queued.load() < 200){
                queued++;
                system.submit([&]{
                    if (!stop.load())
                        bench::spinWork(20000.0);
                    queued--;
                }, heavy);
            }
        };
        topUp();
        std::thread feeder([&]{
            while (!stop.load()){
                topUp();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::vector<double> samples;
        frameUnits(system, samples, 500);
        stop = true;
        feeder.join();
        // the cooks still queued end at once (stop), before the system shuts down
        while (queued.load() > 0)
            std::this_thread::yield();
        return bench::percentile(samples, 0.99);
    }

    // R25: the median of three runs' p99. A neighbour taking cores on a shared build server
    // during one run moves that run's p99, not the result.
    double frameProtectionP99Ms(){
        std::vector<double> runs;
        for (int run = 0; run < 3; run++)
            runs.push_back(frameProtectionRunP99Ms());
        return bench::percentile(runs, 0.5);
    }

    // The pre-S06 pool (one FIFO under a mutex, std::future), for the R25 comparison.
    class OldPool {
    public:
        explicit OldPool(size_t threads){
            for (size_t i = 0; i < threads; i++){
                workers.emplace_back([this]{
                    for (;;){
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(mutex);
                            condition.wait(lock, [this]{ return stopping || !tasks.empty(); });
                            if (stopping && tasks.empty())
                                return;
                            task = std::move(tasks.front());
                            tasks.pop();
                        }
                        task();
                    }
                });
            }
        }
        ~OldPool(){
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
            }
            condition.notify_all();
            for (auto& worker : workers)
                worker.join();
        }
        std::future<void> enqueue(std::function<void()> fn){
            auto task = std::make_shared<std::packaged_task<void()>>(std::move(fn));
            std::future<void> result = task->get_future();
            {
                std::lock_guard<std::mutex> lock(mutex);
                tasks.emplace([task]{ (*task)(); });
            }
            condition.notify_one();
            return result;
        }
        size_t queuedCount(){
            std::lock_guard<std::mutex> lock(mutex);
            return tasks.size();
        }

    private:
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        std::mutex mutex;
        std::condition_variable condition;
        bool stopping = false;
    };

    double oldPoolFrameP99Ms(){
        // what MeshSystem::parallelForIndexed did on the old pool: the frame's units queue
        // behind the cooks (fewer iterations: each one waits out the backlog)
        OldPool pool(kWorkers);
        std::atomic<bool> stop{false};
        for (int i = 0; i < 200; i++)
            pool.enqueue([&]{ if (!stop.load()) bench::spinWork(20000.0); });
        std::vector<double> samples;
        for (int iteration = 0; iteration < 5; iteration++){
            const auto start = bench::Clock::now();
            std::vector<std::future<void>> units;
            for (int unit = 0; unit < 8; unit++)
                units.push_back(pool.enqueue([]{ bench::spinWork(1000.0); }));
            for (auto& unit : units)
                unit.wait();
            samples.push_back(bench::secondsSince(start) * 1000.0);
            for (int i = 0; i < 200 && pool.queuedCount() < 200; i++)
                pool.enqueue([&]{ if (!stop.load()) bench::spinWork(20000.0); });
        }
        stop = true;
        return bench::percentile(samples, 0.99);
    }
}

int main(int argc, char** argv){
    const int hardware = (int)std::thread::hardware_concurrency();
    if (hardware > 0)
        kWorkers = std::max(1, std::min(8, hardware - 2));
    for (int i = 1; i + 1 < argc; i++){
        if (std::string(argv[i]) == "--workers")
            kWorkers = std::max(1, std::atoi(argv[i + 1]));
    }
    jasperengine::Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-jobs");
    std::printf("je-bench-jobs: %d workers, %u hardware threads\n", kWorkers, std::thread::hardware_concurrency());

    double publishRate = 0.0;
    results.add("submit_throughput_jobs_per_s", submitThroughput(publishRate), "jobs/s");
    results.add("submit_publish_jobs_per_s", publishRate, "jobs/s");
    results.add("parallel_for_dispatch_ns", parallelForDispatchNs(), "ns");
    double roundTripP99 = 0.0, waitP99 = 0.0;
    roundTrip(roundTripP99, waitP99);
    results.add("submit_round_trip_p99_us", roundTripP99, "us");
    results.add("submit_wait_p99_us", waitP99, "us");

    const double flat = flatSpeedup();
    results.add("parallel_for_speedup", flat, "x");
    results.add("parallel_for_speedup_ratio", flat / (kWorkers + 1), "of workers+1");
    bool completed = false;
    const double nested = nestedSpeedup(1000, completed);
    results.add("nested_parallel_for_speedup", nested, "x");
    results.add("nested_parallel_for_speedup_ratio", nested / (kWorkers + 1), "of workers+1");
    results.add("nested_parallel_for_deadlocks", completed ? 0.0 : 1.0, "");

    results.add("frame_protection_p99_ms", frameProtectionP99Ms(), "ms");
    results.add("frame_protection_old_pool_p99_ms", oldPoolFrameP99Ms(), "ms");

    const char* path = bench::jsonPath(argc, argv, "je-bench-jobs.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    jobs::shutdown();
    return 0;
}
