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
    EXPECT_EQ(htmlui::compat::sha256Hex(view(first.getShape())), "0000000000000000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(htmlui::compat::sha256Hex(view(first.getDetail())), "0000000000000000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(htmlui::compat::sha256Hex(view(first.getWeather())), "0000000000000000000000000000000000000000000000000000000000000000");
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
    std::printf("[bench] AtmosphereNoise bake: %.1f ms wall, longest main-thread call %.2f ms\n", wall, longestCall);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(wall, 250.0 * slack);
        EXPECT_LE(longestCall, 2.0 * slack);
    }
    EXPECT_TRUE(noise.getShape() == baked().getShape());
}

TEST(AtmosphereNoise, TimeSlicedBakeKeepsItsBudget){
    // the web build without threads: each advance() spends about its budget
    AtmosphereNoise noise;
    noise.start(false);
    double longestCall = 0.0;
    int calls = 0;
    while (true){
        const double callStart = nowMs();
        const bool ready = noise.advance(2.0);
        longestCall = std::max(longestCall, nowMs() - callStart);
        calls++;
        if (ready)
            break;
        ASSERT_LT(calls, 100000);
    }
    std::printf("[bench] AtmosphereNoise time-sliced: %d calls, longest %.2f ms\n", calls, longestCall);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0)
        EXPECT_LE(longestCall, 2.0 + 4.0 * slack); // the budget plus one work item
    EXPECT_TRUE(noise.getDetail() == baked().getDetail());
}
