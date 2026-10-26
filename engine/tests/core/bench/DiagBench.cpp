// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// je-bench-diag: the crash and hang diagnostics' budgets (S28 R26, AC18): the heartbeat, a
// 120-byte crash-ring write, the install, and the idle watchdog thread's CPU share.
//
//   je-bench-diag [--json <file>]    default file: je-bench-diag.json

#include "diag/CrashHandler.h"
#include "diag/HangWatchdog.h"

#include "BenchUtil.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace{

    double heartbeatNs(){
        constexpr int kBeats = 50'000'000;
        std::vector<double> samples;
        for (int trial = 0; trial < 9; trial++){
            const auto start = bench::Clock::now();
            for (int i = 0; i < kBeats; i++)
                diag::heartbeat();
            samples.push_back(bench::secondsSince(start) * 1e9 / kBeats);
        }
        return bench::percentile(samples, 0.5);
    }

    double ringWriteNs(){
        const std::string line(119, 'x');
        constexpr int kWrites = 5'000'000;
        std::vector<double> samples;
        for (int trial = 0; trial < 9; trial++){
            const auto start = bench::Clock::now();
            for (int i = 0; i < kWrites; i++)
                diag::crash::ringWrite(line);
            samples.push_back(bench::secondsSince(start) * 1e9 / kWrites);
        }
        return bench::percentile(samples, 0.5);
    }

    // the process's CPU while the main thread sleeps: the watchdog's polls only
    double watchdogIdleCpuPercent(){
        diag::WatchdogConfig config;
        diag::watchdog::start(config);
        diag::heartbeat();
        const std::clock_t cpuStart = std::clock();
        const auto start = bench::Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const double cpu = static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
        const double wall = bench::secondsSince(start);
        diag::watchdog::stop();
        return 100.0 * cpu / wall;
    }

}

int main(int argc, char** argv){
    bench::Results results("je-bench-diag");
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("je-bench-diag-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    diag::crash::Config config;
    config.product = "je-bench-diag";
    config.root = root;
    const auto installStart = bench::Clock::now();
    diag::crash::install(config);
    results.add("install_ms", bench::secondsSince(installStart) * 1000.0, "ms");
    results.add("heartbeat_ns", heartbeatNs(), "ns");
    results.add("ring_write_120b_ns", ringWriteNs(), "ns");
    results.add("watchdog_idle_cpu_pct", watchdogIdleCpuPercent(), "%");
    diag::crash::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const char* path = bench::jsonPath(argc, argv, "je-bench-diag.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
