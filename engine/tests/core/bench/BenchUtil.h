// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BENCH_BENCHUTIL_H
#define BENCH_BENCHUTIL_H

// Shared helpers of je-bench-jobs and je-bench-memory: timing, percentiles and the JSON
// result file tools/perf/check_bench.py reads ({"bench": name, "results": {metric: value}}).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bench {

    using Clock = std::chrono::steady_clock;

    inline double secondsSince(Clock::time_point start){
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    inline double percentile(std::vector<double> values, double fraction){
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        const size_t index = std::min(values.size() - 1, (size_t)(fraction * (double)(values.size() - 1) + 0.5));
        return values[index];
    }

    // ~`microseconds` of CPU work that the compiler cannot remove.
    inline unsigned spinWork(double microseconds){
        const auto end = Clock::now() + std::chrono::nanoseconds((long long)(microseconds * 1000.0));
        unsigned value = 1;
        while (Clock::now() < end)
            value = value * 1664525u + 1013904223u;
        return value;
    }

    class Results {
    public:
        explicit Results(std::string name): benchName(std::move(name)) {}

        void add(const std::string& metric, double value, const char* unit){
            entries.push_back({metric, value});
            std::printf("%-44s %14.3f %s\n", metric.c_str(), value, unit);
            std::fflush(stdout);
        }

        bool write(const char* path) const{
            FILE* file = std::fopen(path, "w");
            if (!file)
                return false;
            std::fprintf(file, "{\n  \"bench\": \"%s\",\n  \"hardware_concurrency\": %u,\n  \"results\": {\n",
                         benchName.c_str(), std::thread::hardware_concurrency());
            for (size_t i = 0; i < entries.size(); i++)
                std::fprintf(file, "    \"%s\": %.6g%s\n", entries[i].first.c_str(), entries[i].second,
                             i + 1 < entries.size() ? "," : "");
            std::fprintf(file, "  }\n}\n");
            std::fclose(file);
            return true;
        }

    private:
        std::string benchName;
        std::vector<std::pair<std::string, double>> entries;
    };

    // --json <path> from the command line, or the default
    inline const char* jsonPath(int argc, char** argv, const char* fallback){
        for (int i = 1; i + 1 < argc; i++){
            if (std::string(argv[i]) == "--json")
                return argv[i + 1];
        }
        return fallback;
    }

}

#endif // BENCH_BENCHUTIL_H
