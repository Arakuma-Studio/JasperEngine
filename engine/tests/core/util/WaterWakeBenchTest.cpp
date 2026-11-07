// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Water Wakes' step budget (docs/superpowers/specs/2026-11-04-water-boats-design.md section
// 8): four hulls' wakes on a map of 256 texels (desktop) within 0.3 ms a step, of 128 (mobile and
// web) within 0.2 ms: the foam's fade over the map, the stamps of each history, the RGBA texels.
// Like every *Bench suite the budget is enforced with JASPER_TEST_PERF_SLACK, failing at 1.5x it,
// and only printed otherwise (docs/testing.md).

#include "util/WaterWakeMap.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace jasperengine;

namespace water_wake_bench_test {

    // four 6 m hulls crossing the map at 6 m/s, each with 8 s of history
    std::vector<WaterWakeTrack> tracks(){
        std::vector<WaterWakeTrack> result;
        for (int h = 0; h < 4; h++){
            WaterWakeTrack track;
            track.tracked = true;
            track.velocity = Vector2(6.0f, 0.0f);
            track.body = 1;
            const float z = -12.0f + 8.0f * (float)h;
            for (float t = 0.0f; t <= 8.0f; t += 1.0f / 15.0f){
                WaterWakeSample sample;
                sample.position = Vector2(-24.0f + 6.0f * t, z);
                sample.direction = Vector2(1.0f, 0.0f);
                sample.speed = 6.0f;
                sample.time = t;
                sample.body = 1;
                track.history.push_back(sample);
            }
            track.time = 8.0f;
            track.lastPosition = Vector3(0.0f, 0.0f, z);
            result.push_back(track);
        }
        return result;
    }

    double stepMicroseconds(int size){
        WaterWakeMap map;
        map.reset(size, 64.0f / (float)size);
        map.moveTo(-size / 2, -size / 2);
        std::vector<WaterWakeTrack> wakes = tracks();
        WaterWakeFootprint footprint;
        footprint.axisX = Vector2(0.0f, 1.0f);
        footprint.axisZ = Vector2(1.0f, 0.0f);
        footprint.radiusX = 1.0f;
        footprint.radiusZ = 3.0f;
        const float dt = 1.0f / 60.0f;
        float strongest = 0.0f;
        constexpr int steps = 60;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < steps; i++){
            map.fade(std::exp(-2.0f * dt / 8.0f));
            for (WaterWakeTrack& track : wakes)
                map.stampTrack(track, footprint, 1.0f, 1.0f, 8.0f, dt);
            map.encode(strongest);
        }
        EXPECT_GT(strongest, 0.1f);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / steps;
    }

}

using namespace water_wake_bench_test;

TEST(WaterWakeBench, Step256x4){
    const double us = stepMicroseconds(256);
    std::printf("[bench] Water Wakes step, 256 texels, 4 hulls %.1f us (budget 300 us)\n", us);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(us, 300.0 * 1.5 * testsupport::perfSlack());
    }
}

TEST(WaterWakeBench, Step128x4){
    const double us = stepMicroseconds(128);
    std::printf("[bench] Water Wakes step, 128 texels, 4 hulls %.1f us (budget 200 us)\n", us);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(us, 200.0 * 1.5 * testsupport::perfSlack());
    }
}
