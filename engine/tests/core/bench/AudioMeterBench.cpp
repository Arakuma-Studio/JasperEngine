// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je-bench-audio: the loudness budgets (docs/specs/S38-loudness.md R16, R17, AC8), in CPU time, so that
// a loaded machine's waiting does not count (its shared core's slowness still does: judge the budgets
// on an otherwise idle machine).
//
//   je-bench-audio [--json <file>]
//
// AudioMeterBench: AudioMeter on a 3-minute stereo 48 kHz signal, as a file and as a stream, with the
// bands and without. LimiterBench: one stereo bus through the limiter at its defaults and with the
// true-peak detector and 1.5 ms of lookahead, in the 1024-frame blocks of a mix. Each is the least of 5
// runs, the one a loaded machine disturbed the least. tools/perf/check_bench.py gates the JSON with tools/perf/bench-thresholds.json.

#include "BenchUtil.h"

#include "audio/AudioFilters.h"
#include "audio/AudioMeter.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr unsigned int kBenchRate = 48000;
    constexpr int kBenchRuns = 5;

    double cpuSeconds() {
        return (double)std::clock() / (double)CLOCKS_PER_SEC;
    }

    template <typename Work>
    double leastCpuSeconds(Work work) {
        double least = 1e30;
        for (int run = 0; run < kBenchRuns; run++) {
            const double start = cpuSeconds();
            work();
            least = std::min(least, cpuSeconds() - start);
        }
        return least;
    }

    // music-like: a tone and noise under a slow swell
    std::vector<float> benchSignal(double seconds, double level) {
        const size_t frames = (size_t)(seconds * kBenchRate);
        std::vector<float> samples(frames * 2);
        std::mt19937 generator(3);
        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
        const double twoPi = 2.0 * 3.14159265358979323846;
        for (size_t i = 0; i < frames; i++) {
            const double t = (double)i / kBenchRate;
            const double swell = 0.2 + 0.8 * std::fabs(std::sin(twoPi * 0.3 * t));
            const double tone = 0.3 * std::sin(twoPi * 440.0 * t);
            samples[i * 2] = (float)(level * swell * (tone + 0.1 * noise(generator)));
            samples[i * 2 + 1] = (float)(level * swell * (tone + 0.1 * noise(generator)));
        }
        return samples;
    }

    double limiterCorePercent(LimiterFilter& filter, const std::vector<float>& signal, double seconds) {
        const unsigned int block = 1024;
        const size_t frames = signal.size() / 2;
        std::vector<float> planar(block * 2);
        const double cpu = leastCpuSeconds([&]() {
            std::unique_ptr<SoLoud::FilterInstance> instance(filter.createInstance());
            for (size_t position = 0; position + block <= frames; position += block) {
                for (unsigned int i = 0; i < block; i++) {
                    planar[i] = signal[(position + i) * 2];
                    planar[block + i] = signal[(position + i) * 2 + 1];
                }
                instance->filter(planar.data(), block, block, 2, (float)kBenchRate, 0.0);
            }
        });
        return cpu / seconds * 100.0;
    }

}

int main(int argc, char** argv) {
    bench::Results results("je-bench-audio");

    const double fileSeconds = 180.0;
    const std::vector<float> file = benchSignal(fileSeconds, 1.0);
    const size_t frames = file.size() / 2;
    double sink = 0.0;
    results.add("meter_file_3min_ms", leastCpuSeconds([&]() {
        sink += AudioMeter::measure(file.data(), frames, kBenchRate, 2).truePeakDb;
    }) * 1000.0, "ms");
    results.add("meter_file_3min_no_bands_ms", leastCpuSeconds([&]() {
        sink += AudioMeter::measure(file.data(), frames, kBenchRate, 2, false).truePeakDb;
    }) * 1000.0, "ms");
    results.add("meter_stream_core_pct", leastCpuSeconds([&]() {
        AudioMeter meter(kBenchRate, 2);
        for (size_t position = 0; position + 1024 <= frames; position += 1024) {
            meter.process(file.data() + position * 2, 1024);
        }
        meter.finish();
        sink += meter.result().truePeakDb;
    }) / fileSeconds * 100.0, "%");

    const double busSeconds = 30.0;
    const std::vector<float> hot = benchSignal(busSeconds, 4.0);
    LimiterFilter defaults;
    results.add("limiter_defaults_core_pct", limiterCorePercent(defaults, hot, busSeconds), "%");
    LimiterFilter truePeak;
    truePeak.lookahead = 1.5f;
    truePeak.truePeak = true;
    truePeak.sampleRate = (float)kBenchRate;
    results.add("limiter_true_peak_core_pct", limiterCorePercent(truePeak, hot, busSeconds), "%");

    std::printf("(checksum %g)\n", sink);
    const char* path = bench::jsonPath(argc, argv, "je-bench-audio.json");
    return results.write(path) ? 0 : 1;
}
