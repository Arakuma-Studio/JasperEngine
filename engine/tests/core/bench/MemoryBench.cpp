// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// je-bench-memory: the allocators' budgets and the cost of the job system's stats and hook
// sites (S06 R26, AC14). The allocator loops follow OpenEngine's Modules/Memory/Bench/
// MemoryBench.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.
//
//   je-bench-memory [--json <file>]    default file: je-bench-memory.json

#include "Engine.h"
#include "jobs/JobSystem.h"
#include "memory/Arena.h"
#include "memory/ObjectPool.h"
#include "memory/RangeAllocator.h"

#include "AllocationCounter.h"
#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace{

    // keeps results observable so the loops are not optimised away
    volatile uintptr_t sink = 0;

    double arenaAllocateNs(){
        memory::Arena arena("Bench.Arena", 1 << 20);
        constexpr int kPerFrame = 8192;   // 512 KiB of 64-byte allocations
        constexpr int kFrames = 2000;
        for (int i = 0; i < kPerFrame; i++)
            arena.allocate(64);
        arena.reset();
        double best = 1e9;
        for (int trial = 0; trial < 3; trial++){
            const auto start = bench::Clock::now();
            for (int frame = 0; frame < kFrames; frame++){
                for (int i = 0; i < kPerFrame; i++)
                    sink = sink + (uintptr_t)arena.allocate(64);
                arena.reset();
            }
            best = std::min(best, bench::secondsSince(start) * 1e9 / ((double)kPerFrame * kFrames));
        }
        return best;
    }

    struct Particle {
        float position[3];
        float velocity[3];
        uint32_t flags;
    };

    double objectPoolNewDeleteNs(){
        memory::ObjectPool<Particle> pool("Bench.ObjectPool");
        std::vector<Particle*> live(256);
        for (auto& object : live)
            object = pool.create();
        constexpr int kOperations = 20000000;
        std::mt19937 random(7);
        std::vector<uint32_t> picks(4096);
        for (auto& pick : picks)
            pick = random() % 256;
        double best = 1e9;
        for (int trial = 0; trial < 3; trial++){
            const auto start = bench::Clock::now();
            for (int i = 0; i < kOperations; i++){
                Particle*& slot = live[picks[i & 4095]];
                pool.destroy(slot);
                slot = pool.create();
            }
            best = std::min(best, bench::secondsSince(start) * 1e9 / kOperations);
        }
        for (auto& object : live)
            pool.destroy(object);
        return best;
    }

    // R26: allocate+free pairs against 10 k live ranges of random sizes. One run: batches of
    // 256 pairs, each timed (the clock's resolution) as a per-pair cost, p99 over the batches.
    // Five runs, the median p99: a run that a neighbour on a shared machine disturbs moves
    // its own p99, not the result. `mean` is the median run's average.
    double rangeAllocatorRunP99Ns(uint32_t seed, double& mean){
        constexpr uint64_t kCapacity = 1u << 26;
        memory::RangeAllocator ranges("Bench.RangeAllocator", memory::Residency::Gpu, 1, kCapacity, 16384);
        std::mt19937 random(seed);
        std::vector<memory::RangeAllocator::Allocation> live;
        live.reserve(10000);
        while (live.size() < 10000)
            live.push_back(ranges.allocate(1 + random() % 2048));
        constexpr int kBatches = 5000;
        constexpr int kBatch = 256;
        std::vector<double> samples;
        samples.reserve(kBatches);
        std::vector<uint32_t> sizes(kBatch);
        double total = 0.0;
        for (int batch = 0; batch < kBatches; batch++){
            for (auto& size : sizes)
                size = 1 + random() % 2048;
            const size_t base = random() % (live.size() - kBatch);
            const auto start = bench::Clock::now();
            for (int i = 0; i < kBatch; i++){
                memory::RangeAllocator::Allocation& slot = live[base + (size_t)i];
                ranges.free(slot);
                slot = ranges.allocate(sizes[(size_t)i]);
            }
            const double perPair = bench::secondsSince(start) * 1e9 / kBatch;
            samples.push_back(perPair);
            total += perPair;
        }
        for (auto& allocation : live)
            ranges.free(allocation);
        mean = total / kBatches;
        return bench::percentile(samples, 0.99);
    }

    double rangeAllocatorP99Ns(double& mean){
        std::vector<std::pair<double, double>> runs;
        for (uint32_t run = 0; run < 5; run++){
            double runMean = 0.0;
            const double p99 = rangeAllocatorRunP99Ns(11 + run, runMean);
            runs.push_back({p99, runMean});
        }
        std::sort(runs.begin(), runs.end());
        mean = runs[runs.size() / 2].second;
        return runs[runs.size() / 2].first;
    }

    // R26: what the stats add to R23. A multi-threaded throughput run varies by about ten
    // percent between runs on a shared machine, more than the 2 % budget, so the two parts
    // are measured where they are stable and combined:
    //   - the stats' cost per job, on one thread (0 workers: publish, then the pump runs
    //     them), as the median difference of paired trials;
    //   - the CPU time R23's workers have per job: workers x R23's wall time per job.
    // Their ratio bounds the throughput R23 loses: the stats are work its workers do.
    double singleThreadNsPerJob(jobs::JobSystem& system){
        constexpr int kJobs = 200000;
        uint64_t done = 0;
        const auto start = bench::Clock::now();
        for (int i = 0; i < kJobs; i++)
            system.enqueueDetached([&done]{ done++; });
        system.pump(1e9);
        const double ns = bench::secondsSince(start) * 1e9 / kJobs;
        if (done != (uint64_t)kJobs)
            std::printf("the pump ran %llu of %d jobs\n", (unsigned long long)done, kJobs);
        return ns;
    }

    // Both systems live, short trials alternating which goes first (drift cancels), the
    // relative difference per pair, the median of the differences.
    double statsCostNs(double& withStatsNs, double& withoutStatsNs){
        jobs::Config config;
        config.workers = 0;
        config.collectStats = true;
        jobs::JobSystem statsOn(config);
        config.collectStats = false;
        jobs::JobSystem statsOff(config);
        for (int warm = 0; warm < 5; warm++){
            singleThreadNsPerJob(statsOn);
            singleThreadNsPerJob(statsOff);
        }
        std::vector<double> differences, on, off;
        for (int pair = 0; pair < 41; pair++){
            double a = 0.0, b = 0.0;
            if (pair % 2 == 0){
                a = singleThreadNsPerJob(statsOn);
                b = singleThreadNsPerJob(statsOff);
            }else{
                b = singleThreadNsPerJob(statsOff);
                a = singleThreadNsPerJob(statsOn);
            }
            on.push_back(a);
            off.push_back(b);
            differences.push_back((a - b) / b);   // relative: drift between pairs cancels
        }
        withStatsNs = bench::percentile(on, 0.5);
        withoutStatsNs = bench::percentile(off, 0.5);
        return bench::percentile(differences, 0.5) * withoutStatsNs;
    }

    // R23's run (stats on, as shipped): workers x wall time per job, median of trials
    double r23WorkerNsPerJob(int& workers){
        const int hardware = (int)std::thread::hardware_concurrency();
        jobs::Config config;
        config.workers = hardware > 0 ? std::max(1, std::min(8, hardware - 1)) : 8;
        workers = config.workers;
        jobs::JobSystem system(config);
        std::vector<double> samples;
        for (int trial = 0; trial < 9; trial++){
            // as je-bench-jobs: empty jobs, completion read from the executed count
            constexpr int kJobs = 1000000;
            const uint64_t before = system.stats().totals.executed;
            const auto start = bench::Clock::now();
            for (int i = 0; i < kJobs; i++)
                system.enqueueDetached([]{});
            while (system.stats().totals.executed - before < (uint64_t)kJobs)
                std::this_thread::yield();
            samples.push_back(bench::secondsSince(start) * 1e9 / kJobs);
        }
        return workers * bench::percentile(samples, 0.5);
    }
}

int main(int argc, char** argv){
    jasperengine::Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-memory");
    results.add("arena_allocate_64b_ns", arenaAllocateNs(), "ns");
    results.add("object_pool_new_delete_ns", objectPoolNewDeleteNs(), "ns");
    double rangeMean = 0.0;
    results.add("range_allocator_alloc_free_p99_ns", rangeAllocatorP99Ns(rangeMean), "ns");
    results.add("range_allocator_alloc_free_mean_ns", rangeMean, "ns");

    double withStatsNs = 0.0, withoutStatsNs = 0.0;
    const double statsNs = statsCostNs(withStatsNs, withoutStatsNs);
    int workers = 0;
    const double workerNs = r23WorkerNsPerJob(workers);
    results.add("job_ns_with_stats", withStatsNs, "ns");
    results.add("job_ns_without_stats", withoutStatsNs, "ns");
    results.add("stats_ns_per_job", statsNs, "ns");
    results.add("r23_worker_ns_per_job", workerNs, "ns");
    results.add("stats_and_hooks_overhead_pct", 100.0 * statsNs / workerNs, "%");

    const char* path = bench::jsonPath(argc, argv, "je-bench-memory.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    jobs::shutdown();
    return 0;
}
