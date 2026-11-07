// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LoggerBench: the logging hot path against the budgets of S09 (R28). They are enforced in the
// benchmark run (JASPER_TEST_PERF_SLACK=1 on the benchmark server, docs/testing.md); a plain unit
// test run, and a sanitizer run, only report them.

#include "log/LogTestSupport.h"
#include "support/AllocationCounter.h"
#include "log/Backtrace.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#if !defined(_WIN32)
    #include <sys/resource.h>
    #include <time.h>
#endif
#if defined(__linux__)
    #include <dirent.h>
    #include <fstream>
    #include <string>
#endif

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    JE_LOG_CATEGORY(logCategoryBench, "bench", LogLevel::Info)

    using Clock = std::chrono::steady_clock;

    void expectWithin(double measured, double budget, const char* what) {
        const double slack = testsupport::perfSlack();
        std::printf("[bench] %s: %.3f (budget %.3f)\n", what, measured, budget);
        if (slack > 0.0) {
            EXPECT_LE(measured, budget * slack) << what;
        }
    }

}

TEST(LoggerBench, Filtered) {
    Logger::setCategoryLevel(logCategoryBench(), LogLevel::Error);
    constexpr int calls = 20000000;
    int value = 0;
    const auto start = Clock::now();
    for (int i = 0; i < calls; i++) {
        JE_LOG_INFO(logCategoryBench(), "filtered %d", value);
        value += i & 1;
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / calls;
    Logger::setCategoryLevel(logCategoryBench(), LogLevel::Info);
    EXPECT_GE(value, 0);
    expectWithin(ns, 3.0, "filtered call, ns");
}

TEST(LoggerBench, AsyncEnabled4Threads) {
    FreshLogger logger(4 * 1024 * 1024);
    // a drain-side sink that does nothing: the producer's cost is measured
    auto sink = ScopedSink<CallbackSink>(std::make_unique<CallbackSink>("null", [](const LogRecord&) {}));
    constexpr int threads = 4;
    constexpr int perThread = 20000;
    static char keys[threads * perThread];
    std::vector<std::vector<double>> latencies(threads);
    std::vector<size_t> allocations(threads, 0);
    std::vector<std::thread> workers;
    const std::string text(128, 'm');
    for (int t = 0; t < threads; t++) {
        workers.emplace_back([&, t]() {
            latencies[t].reserve(perThread);
            // the thread's buffers warm up first
            Logger::write(LogLevel::Info, logCategoryBench(), text);
            testsupport::ThreadAllocationCount count;
            uint64_t pacingAllocations = 0;
            for (int i = 0; i < perThread; i++) {
                const auto start = Clock::now();
                Logger::write(LogLevel::Info, logCategoryBench(), text, nullptr, 0, &keys[t * perThread + i]);
                latencies[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
                // Drain bounded batches outside the measured write cost.
                if (i % 64 == 63) {
                    const testsupport::ThreadAllocationCount pacing;
                    Logger::flush();
                    pacingAllocations += pacing.count();
                }
            }
            allocations[t] = count.count() - pacingAllocations;
        });
    }
    for (std::thread& worker : workers) worker.join();
    std::vector<double> all;
    for (const auto& one : latencies) all.insert(all.end(), one.begin(), one.end());
    std::sort(all.begin(), all.end());
    const double p50 = all[all.size() / 2];
    const double p99 = all[all.size() * 99 / 100];
    expectWithin(p50, 1.0, "enabled async call p50, us");
    expectWithin(p99, 10.0, "enabled async call p99, us");
    for (int t = 0; t < threads; t++) EXPECT_EQ(allocations[t], 0u) << "thread " << t << " allocated";
    EXPECT_EQ(Logger::stats().dropped, 0u);
}

TEST(LoggerBench, BacktraceCapture) {
    void* frames[LogRecord::MAX_FRAMES];
    Backtrace::capture(frames, LogRecord::MAX_FRAMES);
    constexpr int captures = 2000;
    const auto start = Clock::now();
    size_t total = 0;
    for (int i = 0; i < captures; i++) total += Backtrace::capture(frames, LogRecord::MAX_FRAMES);
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / captures;
    EXPECT_GT(total, 0u);
    expectWithin(us, 20.0, "raw backtrace capture, us");
}

#if defined(__linux__)
namespace {
    // CPU time (ns) of this process's thread named `name` (/proc/self/task/<tid>/comm), from its
    // schedstat; -1 when there is no such thread
    long long threadCpuNs(const char* name) {
        DIR* tasks = opendir("/proc/self/task");
        if (!tasks) return -1;
        long long result = -1;
        while (dirent* entry = readdir(tasks)) {
            if (entry->d_name[0] == '.') continue;
            const std::string dir = std::string("/proc/self/task/") + entry->d_name;
            std::string comm;
            std::getline(std::ifstream(dir + "/comm"), comm);
            if (comm != name) continue;
            long long ns = -1;
            std::ifstream(dir + "/schedstat") >> ns;
            result = ns;
            break;
        }
        closedir(tasks);
        return result;
    }
}

TEST(LoggerBench, DrainCpuAt10kPerSecond) {
    FreshLogger logger(4 * 1024 * 1024);
    // Observe a drain receipt before installing the measured file sink.
    {
        auto ready = ScopedSink<CallbackSink>(std::make_unique<CallbackSink>("ready", [](const LogRecord&) {}));
        Logger::write(LogLevel::Info, logCategoryBench(), "drain ready");
        Logger::flush();
    }
    TempDir dir;
    RotatingFileSink::Options options;
    options.path = dir.file("cpu.log").string();
    options.maxBytes = 64 * 1024 * 1024;
    auto file = ScopedSink<RotatingFileSink>(std::make_unique<RotatingFileSink>(options));
    static char keys[10000];
    const std::string text(100, 'c');
    // the drain thread alone: the process also runs other suites' threads (the job system's
    // workers stay up between tests)
    long long drainBefore = threadCpuNs("je-log");
    ASSERT_GE(drainBefore, 0) << "no je-log thread";
    rusage before{};
    getrusage(RUSAGE_SELF, &before);
    timespec threadBefore{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &threadBefore);
    const auto start = Clock::now();
    // 10,000 records over one second, in steps of 10 per millisecond
    for (int i = 0; i < 10000; i++) {
        Logger::write(LogLevel::Info, logCategoryBench(), text, nullptr, 0, &keys[i]);
        if (i % 10 == 9) std::this_thread::sleep_until(start + std::chrono::microseconds(100 * (i + 1)));
    }
    Logger::flush();
    const double wall = std::chrono::duration<double>(Clock::now() - start).count();
    const long long drainAfter = threadCpuNs("je-log");
    rusage after{};
    getrusage(RUSAGE_SELF, &after);
    timespec threadAfter{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &threadAfter);
    auto seconds = [](const timeval& t) { return static_cast<double>(t.tv_sec) + t.tv_usec / 1e6; };
    const double process = seconds(after.ru_utime) + seconds(after.ru_stime) - seconds(before.ru_utime) - seconds(before.ru_stime);
    const double producer = (threadAfter.tv_sec - threadBefore.tv_sec) + (threadAfter.tv_nsec - threadBefore.tv_nsec) / 1e9;
    const double drain = (drainAfter - drainBefore) / 1e9;
    std::printf("[bench] the other threads of the process, %%: %.3f\n", 100.0 * (process - producer - drain) / wall);
    expectWithin(100.0 * drain / wall, 2.0, "drain thread CPU at 10k records/s, %");
}
#endif
