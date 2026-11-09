// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Foliage's budgets (docs/specs/T5-foliage.md R41, R42, R45), docs/perf/foliage.md:
//   je-bench-foliage [--json <file>]
// check_bench.py gates the file with tools/perf/bench-thresholds.json. Times are host times: the
// device budgets of R40 are counts, in FoliageBudget.

#include "BenchUtil.h"
#include "AllocationCounter.h"
#include "FoliageBenchScene.h"
#include "JobsTestSystem.h"
#include "Engine.h"
#include "jobs/JobSystem.h"
#include "terrain/FoliageCull.h"
#include "terrain/FoliageScatter.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terraincore;
using namespace foliagebench;

namespace {

    double scatterAttemptNs(double* cellMs) {
        constexpr int SIZE = 256;
        std::vector<uint8_t> heights(static_cast<size_t>(SIZE) * SIZE * 2, 0x40);
        std::vector<uint8_t> density(static_cast<size_t>(SIZE) * SIZE, 255);

        FoliageCellInput input;
        input.layer.density = 5.0f;
        input.layer.maxSlope = 90.0f;
        input.layer.seed = 3;
        input.terrainSize = 256.0f;
        input.terrainMaxHeight = 20.0f;
        input.heightMap.texels = heights.data();
        input.heightMap.mapWidth = input.heightMap.width = SIZE;
        input.heightMap.mapHeight = input.heightMap.height = SIZE;
        input.heightMap.bytesPerChannel = 2;
        input.densityMap.texels = density.data();
        input.densityMap.mapWidth = input.densityMap.width = SIZE;
        input.densityMap.mapHeight = input.densityMap.height = SIZE;
        const unsigned int attempts = foliageCellAttempts(input.layer);

        std::vector<FoliageInstance> out;
        std::vector<double> samples;
        for (int i = 0; i < 200; i++) {
            out.clear();
            input.cellX = i % 8;
            input.cellZ = i / 8;
            const auto start = bench::Clock::now();
            scatterFoliageCell(input, out);
            samples.push_back(bench::secondsSince(start));
        }
        const double seconds = bench::percentile(samples, 0.5);
        *cellMs = seconds * 1.0e3;
        return seconds * 1.0e9 / attempts;
    }

    double cull100k(int workers) {
        jobs::shutdown();
        jobs::Config config;
        config.workers = workers;
        jobs::initialize(config);

        constexpr size_t PER_CELL = 2000;
        constexpr size_t CELLS = 50;
        std::vector<FoliageInstance> instances(PER_CELL * CELLS);
        for (size_t i = 0; i < instances.size(); i++) {
            FoliageInstance& instance = instances[i];
            instance.position[0] = static_cast<float>(i % 300) * 0.1f - 15.0f;
            instance.position[1] = 0.0f;
            instance.position[2] = static_cast<float>(i / 300) * 0.1f + 2.0f;
            instance.scale = 1.0f;
            instance.rotation[3] = 32767;
            instance.heightRatio = 128;
            instance.densityRank = static_cast<uint8_t>(i);
        }
        FoliageDetailCull cull;
        cull.drawDistance = 1000.0f;
        cull.density = 1.0f;
        FoliageFrustum frustum;
        const float planes[6][4] = {{0.7f, 0.0f, 0.7f, 200.0f}, {-0.7f, 0.0f, 0.7f, 200.0f}, {0.0f, 0.7f, 0.7f, 200.0f},
                                    {0.0f, -0.7f, 0.7f, 200.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f, 1000.0f}};
        std::memcpy(frustum.planes, planes, sizeof(planes));

        std::vector<FoliageInstance> out(instances.size());
        std::vector<uint8_t> lods(instances.size());
        std::vector<double> samples;
        for (int run = 0; run < 40; run++) {
            const auto start = bench::Clock::now();
            jobs::parallelFor(CELLS, [&](size_t cell) {
                cullFoliageCell(instances.data() + cell * PER_CELL, PER_CELL, cull, frustum, FoliageCellVisibility::Intersecting,
                                out.data() + cell * PER_CELL, lods.data() + cell * PER_CELL);
            });
            samples.push_back(bench::secondsSince(start) * 1.0e3);
        }
        jobs::shutdown();
        jobs::initialize(jobs::Config{});
        return bench::percentile(samples, 0.5);
    }

}

int main(int argc, char** argv) {
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-foliage");

    double cellMs = 0.0;
    const double attemptNs = scatterAttemptNs(&cellMs);
    results.add("scatter_attempt_ns", attemptNs, "ns");
    results.add("placement_cell_full_density_ms", cellMs, "ms");
    results.add("stored_tree_bytes", static_cast<double>(sizeof(TreeRecord)), "bytes");

    {
        Scene empty;
        FoliageSystem* foliage = empty.getSystem<FoliageSystem>().get();
        foliage->update(FRAME_SECONDS);
        testsupport::ThreadAllocationCount allocations;
        const auto start = bench::Clock::now();
        constexpr int RUNS = 100000;
        for (int i = 0; i < RUNS; i++) foliage->update(FRAME_SECONDS);
        results.add("empty_scene_us", bench::secondsSince(start) * 1.0e6 / RUNS, "us");
        results.add("empty_scene_allocations", static_cast<double>(allocations.count()), "allocations");
    }

    Scene1 bench_scene("je-bench-foliage");
    bench_scene.settle();
    bench_scene.markLoadedWithoutDevice();

    // the frame's main-thread work on R44's path
    FoliageViewList list;
    std::vector<double> samples;
    samples.reserve(FRAMES);
    for (int i = 0; i < FRAMES; i++) {
        bench_scene.setPose(i);
        Plane planes[6];
        const FoliageViewRequest request = bench_scene.view(planes);
        const auto start = bench::Clock::now();
        bench_scene.frame(FRAME_SECONDS);
        bench_scene.foliage->buildView(list, request);
        samples.push_back(bench::secondsSince(start) * 1.0e3);
    }
    results.add("frame_moving_p50_ms", bench::percentile(samples, 0.5), "ms");
    results.add("frame_moving_p99_ms", bench::percentile(samples, 0.99), "ms");

    bench_scene.foliage->waitForJobs();
    bench_scene.settle();
    samples.clear();
    Plane still[6];
    const FoliageViewRequest stillRequest = bench_scene.view(still);
    bench_scene.foliage->buildView(list, stillRequest);
    for (int i = 0; i < 200; i++) {
        const auto start = bench::Clock::now();
        bench_scene.frame(FRAME_SECONDS);
        bench_scene.foliage->buildView(list, stillRequest);
        samples.push_back(bench::secondsSince(start) * 1.0e6);
    }
    results.add("frame_still_us", bench::percentile(samples, 0.5), "us");

    const FoliageStats& stats = bench_scene.foliage->getStats(bench_scene.entity);
    results.add("memory_total_mb", static_cast<double>(stats.cpuBytes + stats.gpuBytes + list.capacity) / (1024.0 * 1024.0), "MB");
    results.add("visible_details", static_cast<double>(list.detailVisible), "instances");
    results.add("visible_trees", static_cast<double>(list.treesVisible), "trees");

    // R45: a stamp, and what 100 trees in one cell rebuild
    {
        std::shared_ptr<TreeStore> store = bench_scene.foliage->getTreeStore(bench_scene.entity);
        const auto stampOf = [](int count, int salt) {
            std::vector<TreeRecord> added;
            for (int i = 0; i < count; i++) {
                TreeRecord record{};
                record.u = 0.51f + 0.005f * detHash01(i, salt, 91u);
                record.v = 0.51f + 0.005f * detHash01(i, salt, 92u);
                record.scale = 1.0f;
                record.rotation[3] = 32767;
                record.prototypeId = 1;
                record.heightRatio = 128;
                record.flags = TREE_SNAP_TO_TERRAIN;
                added.push_back(record);
            }
            return added;
        };
        samples.clear();
        for (int i = 0; i < 40; i++) {
            const std::vector<TreeRecord> added = stampOf(8, i);
            const auto start = bench::Clock::now();
            TreeDelta delta;
            store->apply({}, added, {}, &delta);
            bench_scene.foliage->treesChanged(bench_scene.entity, delta);
            bench_scene.frame(FRAME_SECONDS);
            samples.push_back(bench::secondsSince(start) * 1.0e3);
            bench_scene.foliage->waitForJobs();
        }
        results.add("stamp_8_trees_ms", bench::percentile(samples, 0.5), "ms");

        bench_scene.settle();
        const uint32_t before = stats.treeCellsRebuilt;
        const std::vector<TreeRecord> added = stampOf(100, 1000);
        TreeDelta delta;
        store->apply({}, added, {}, &delta);
        bench_scene.foliage->treesChanged(bench_scene.entity, delta);
        bench_scene.settle();
        results.add("cells_rebuilt_100_trees", static_cast<double>(stats.treeCellsRebuilt - before), "cells");
    }

    // last: restarting the job system cancels the asset loads it held
    results.add("cull_100k_0_workers_ms", cull100k(0), "ms");
    results.add("cull_100k_4_workers_ms", cull100k(4), "ms");

    const char* path = bench::jsonPath(argc, argv, "je-bench-foliage.json");
    if (!results.write(path)) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
