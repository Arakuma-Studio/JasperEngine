// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The cost of crash-safe writes (docs/specs/S03-data-safety.md R35, AC19): a Cache write of
// 64 KB within 1.5x a plain ofstream write, a Document write of 1 MB adding at most 25 ms
// (fsync included). Medians of several runs: the build machines are shared.
// The Document budget is wall clock spent mostly in fsync, which a loaded disk alone misses (2.6 s
// against 69 ms plain on a busy build server): it is printed, and enforced in the benchmark run
// (perfSlack(), docs/testing.md). The Cache bound is a ratio to a plain write and stays checked.

#include "io/AtomicFile.h"
#include "support/AllocationCounter.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    template<typename Fn>
    double medianMs(int runs, Fn&& fn) {
        std::vector<double> times;
        for (int i = 0; i < runs; i++) {
            const auto start = std::chrono::steady_clock::now();
            fn(i);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    void plainWrite(const fs::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }

}

TEST(AtomicFileBench, CacheWriteNearPlainWrite) {
    TestTempDir dir("bench");
    const std::string bytes(64 * 1024, 'x');
    io::AtomicWriteOptions cache;
    cache.durability = io::Durability::Cache;
    // a plain write replaces the file in place; the atomic one a temp, a mode copy and a rename
    double plain = 0, atomic = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
#if defined(__APPLE__)
        // APFS makes a new file 2-4x slower than it overwrites one (BF4, macOS 26.2), and an
        // atomic write needs a new file however it publishes: the plain write there makes a new
        // file too, so the bound is what the atomic write adds beyond that
        plain = medianMs(41, [&](int i) { plainWrite(dir / ("plain-new" + std::to_string(attempt * 41 + i)), bytes); });
#else
        plain = medianMs(41, [&](int i) { plainWrite(dir / ("plain" + std::to_string(i % 4)), bytes); });
#endif
        atomic = medianMs(41, [&](int i) { io::writeFileAtomic(dir / ("atomic" + std::to_string(i % 4)), bytes, cache); });
        if (atomic <= plain * 1.5 + 0.05) break;
    }
    std::printf("[ bench    ] 64 KB: ofstream %.3f ms, Cache atomic %.3f ms\n", plain, atomic);
    // 0.05 ms of slack: at these sizes a single syscall is the difference
    EXPECT_LE(atomic, plain * 1.5 + 0.05);
}

TEST(AtomicFileBench, DocumentWriteOverhead) {
    TestTempDir dir("bench");
    const std::string bytes(1024 * 1024, 's');
    io::AtomicWriteOptions document;
    const double slack = testsupport::perfSlack();
    const double budget = 25.0 * (slack > 0.0 ? slack : 1.0);
    double plain = 0, durable = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        plain = medianMs(9, [&](int i) { plainWrite(dir / ("plain" + std::to_string(i % 3)), bytes); });
        durable = medianMs(9, [&](int i) { io::writeFileAtomic(dir / ("doc" + std::to_string(i % 3)), bytes, document); });
        if (durable - plain <= budget) break;
    }
    std::printf("[ bench    ] 1 MB: ofstream %.3f ms, Document atomic %.3f ms (budget: %.1f ms more)\n", plain, durable, budget);
    if (slack > 0.0) {
        EXPECT_LE(durable - plain, budget);
    }
    // what the time bought: the last write is whole
    std::ifstream in(dir / "doc2", std::ios::binary);
    const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(written == bytes) << written.size() << " bytes read back of " << bytes.size();
}
