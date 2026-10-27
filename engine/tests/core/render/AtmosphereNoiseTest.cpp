// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The volumetric clouds' noise bake (render/AtmosphereNoise): deterministic bytes, tileable
// noise, every channel stretched to the full range, and the threaded bake's wall time with the
// main thread free (docs/specs/S12-atmosphere.md AC9, R20).

#include "render/AtmosphereNoise.h"
#include "htmlui/compat/Hash.h"
#include "jobs/JobSystem.h"

#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    std::string_view view(const std::vector<uint8_t>& bytes){
        return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // What the test's thread (the main thread of the bake) does inside the job system while the
    // bake runs, counted by the jobs profiler hooks: a job it runs, or a wait it enters
    thread_local bool onBakeCaller = false;
    std::atomic<int> callerJobs{0};
    std::atomic<int> callerWaits{0};
    void countCallerJob(const char*, jobs::Lane, int){ if (onBakeCaller) callerJobs++; }
    void countCallerWait(jobs::WaitKind){ if (onBakeCaller) callerWaits++; }

    double nowMs(){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // each channel's smallest and largest byte
    void channelRange(const std::vector<uint8_t>& bytes, int channel, int& low, int& high){
        low = 255;
        high = 0;
        for (size_t i = channel; i < bytes.size(); i += 4){
            low = std::min(low, (int)bytes[i]);
            high = std::max(high, (int)bytes[i]);
        }
    }

    // the bake every test of the binary shares (once per process, as the engine's)
    const AtmosphereNoise& baked(){
        static AtmosphereNoise noise;
        static bool done = false;
        if (!done){
            noise.bakeNow();
            done = true;
        }
        return noise;
    }

}

TEST(AtmosphereNoise, HasTheSpecSizes){
    const AtmosphereNoise& noise = baked();
    ASSERT_TRUE(noise.isReady());
    EXPECT_EQ(noise.getShape().size(), (size_t)64 * 64 * 64 * 4);
    EXPECT_EQ(noise.getDetail().size(), (size_t)32 * 32 * 32 * 4);
    EXPECT_EQ(noise.getWeather().size(), (size_t)256 * 256 * 4);
}

TEST(AtmosphereNoise, IsDeterministic){
    const AtmosphereNoise& first = baked();
    AtmosphereNoise second;
    second.bakeNow();
    EXPECT_TRUE(first.getShape() == second.getShape());
    EXPECT_TRUE(first.getDetail() == second.getDetail());
    EXPECT_TRUE(first.getWeather() == second.getWeather());
#if defined(__linux__) && defined(__x86_64__)
    // the bytes of this build's bake (GCC/Clang x86-64; other toolchains may round a libm
    // call differently, the two bakes above still match)
    EXPECT_EQ(htmlui::compat::sha256Hex(view(first.getShape())), "c9c9692b530e96de12ad969fe4e443aba8966c885a450056f100224655f7be32");
    EXPECT_EQ(htmlui::compat::sha256Hex(view(first.getDetail())), "7f669e915af58d59437290dc133fb6a577aaab7be9780290f7254cb37a30b787");
    EXPECT_EQ(htmlui::compat::sha256Hex(view(first.getWeather())), "b3e2981fb59d834c659c09e87f3d9ae878fde684209dcd3aaf43b4763f5961f2");
#endif
}

TEST(AtmosphereNoise, Tiles){
    // the noise functions have a period of 1 on every axis: opposite faces of the textures meet
    for (int i = 0; i < 64; i++){
        const float a = (float)i / 64.0f;
        const float b = (float)((i * 37) % 64) / 64.0f;
        const Vector4 x0 = AtmosphereNoise::sampleShape(0.0f, a, b);
        const Vector4 x1 = AtmosphereNoise::sampleShape(1.0f, a, b);
        const Vector4 y0 = AtmosphereNoise::sampleDetail(a, 0.0f, b);
        const Vector4 y1 = AtmosphereNoise::sampleDetail(a, 1.0f, b);
        const Vector4 w0 = AtmosphereNoise::sampleWeather(a, 0.0f);
        const Vector4 w1 = AtmosphereNoise::sampleWeather(a, 1.0f);
        for (int c = 0; c < 4; c++){
            EXPECT_NEAR(x0[c], x1[c], 1.0f / 255.0f) << "shape channel " << c << " at " << a << ", " << b;
            EXPECT_NEAR(y0[c], y1[c], 1.0f / 255.0f) << "detail channel " << c;
            EXPECT_NEAR(w0[c], w1[c], 1.0f / 255.0f) << "weather channel " << c;
        }
    }
    // and the baked bytes wrap smoothly: the step across the edge is like a step inside
    const std::vector<uint8_t>& shape = baked().getShape();
    auto texel = [&shape](int x, int y, int z, int c){ return (int)shape[(((size_t)z * 64 + y) * 64 + x) * 4 + c]; };
    int edge = 0;
    int inside = 0;
    for (int z = 0; z < 64; z += 7){
        for (int y = 0; y < 64; y += 5){
            edge = std::max(edge, std::abs(texel(63, y, z, 0) - texel(0, y, z, 0)));
            inside = std::max(inside, std::abs(texel(31, y, z, 0) - texel(32, y, z, 0)));
        }
    }
    EXPECT_LE(edge, std::max(inside * 2, 16));
}

TEST(AtmosphereNoise, ChannelsSpanTheFullRange){
    const AtmosphereNoise& noise = baked();
    const std::vector<uint8_t>* volumes[3] = {&noise.getShape(), &noise.getDetail(), &noise.getWeather()};
    const int channels[3] = {4, 4, 3}; // the weather map's alpha is a constant 255
    for (int v = 0; v < 3; v++){
        for (int c = 0; c < channels[v]; c++){
            int low = 0;
            int high = 0;
            channelRange(*volumes[v], c, low, high);
            EXPECT_EQ(low, 0) << "volume " << v << " channel " << c;
            EXPECT_EQ(high, 255) << "volume " << v << " channel " << c;
        }
    }
    int low = 0;
    int high = 0;
    channelRange(noise.getWeather(), 3, low, high);
    EXPECT_EQ(low, 255);
}

TEST(AtmosphereNoise, BakesOnTheJobsWithoutBlockingTheMainThread){
    ASSERT_FALSE(jobs::system().isSingleThreaded()) << "the threaded bake needs workers";
    // the behaviour: the main thread never runs a piece of the bake and never waits for one
    // (a wall-clock budget per call measures the host's load as much: it is the benchmark's)
    jobs::ProfilerHooks hooks;
    hooks.onJobBegin = &countCallerJob;
    hooks.onWaitBegin = &countCallerWait;
    callerJobs = 0;
    callerWaits = 0;
    onBakeCaller = true;
    jobs::setProfilerHooks(hooks);
    struct Uninstall {
        ~Uninstall(){
            jobs::setProfilerHooks(jobs::ProfilerHooks());
            onBakeCaller = false;
        }
    } uninstall;
    AtmosphereNoise noise;
    const double start = nowMs();
    double longestCall = 0.0;
    {
        const double callStart = nowMs();
        noise.start(true);
        longestCall = std::max(longestCall, nowMs() - callStart);
    }
    while (true){
        const double callStart = nowMs();
        const bool ready = noise.advance(1.0);
        longestCall = std::max(longestCall, nowMs() - callStart);
        if (ready)
            break;
        ASSERT_LT(nowMs() - start, 30000.0) << "the bake never finished";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double wall = nowMs() - start;
    EXPECT_EQ(callerJobs.load(), 0) << "the main thread ran part of the bake";
    EXPECT_EQ(callerWaits.load(), 0) << "the main thread waited for the bake";
    // R20's 250 ms is the reference desktop's (8 hardware threads: 4 workers run Background
    // jobs beside the 2 kept for the frame); a host granting fewer gets a proportionally longer
    // budget
    const int background = std::max(1, jobs::system().getWorkerCount() - 2);
    const double budget = 250.0 * std::max(1.0, 4.0 / (double)background);
    std::printf("[bench] AtmosphereNoise bake: %.1f ms wall (budget %.0f ms, %d Background workers), longest main-thread call %.2f ms\n",
                wall, budget, background, longestCall);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(wall, budget * slack);
        EXPECT_LE(longestCall, 2.0 * slack);
    }
    EXPECT_TRUE(noise.getShape() == baked().getShape());
}

TEST(AtmosphereNoise, TimeSlicedBakeKeepsItsBudget){
    // the web build without threads: each advance() spends about its budget
    AtmosphereNoise noise;
    noise.start(false);
    std::vector<double> durations;
    int calls = 0;
    while (true){
        const double callStart = nowMs();
        const bool ready = noise.advance(2.0);
        durations.push_back(nowMs() - callStart);
        calls++;
        if (ready)
            break;
        ASSERT_LT(calls, 100000);
    }
    std::sort(durations.begin(), durations.end());
    const double longestCall = durations.back();
    // the 99th percentile: a shared CI host may preempt the odd call
    const double p99 = durations[std::min(durations.size() - 1, (size_t)((double)durations.size() * 0.99))];
    std::printf("[bench] AtmosphereNoise time-sliced: %d calls, p99 %.2f ms, longest %.2f ms\n", calls, p99, longestCall);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(p99, 2.0 + 4.0 * slack); // the budget plus one work item
    }
    EXPECT_TRUE(noise.getDetail() == baked().getDetail());
}
