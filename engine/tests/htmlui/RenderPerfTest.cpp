// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTML UI rendering benchmark (S16 §7, AC9, AC14, AC15), Release builds only. Every document
// runs twice in the same process: with S16 off (no batching, no SDF primitives, no
// transform-aware text: the baseline) and with the defaults on; 30 warm-up frames, then 120
// measured frames on a moving UI clock.
//
// - B1: the ten example documents (docs/mcp/guide/html-ui-examples): draws <= 30 % of the
//   baseline and <= 40; the engine's CPU time <= baseline + 5 %.
// - B2 list-500 (fixtures/bench): 500 cards with rounded borders, box shadows, an icon and two
//   labels in a rounded scrolling list, one card animating: <= 16 draws, no stencil draw, no
//   box-shadow texture; the engine's CPU time <= 50 % of the baseline; the gather stream and the
//   run cache within twice the frame's gathered bytes.
// - B3 text-wall: 5000 glyphs at 12, 16 and 24 px: <= 4 draws.
// - B4 scale-popin: 40 buttons popping in over 300 ms: no frame over 2 ms of glyph rasterization
//   or 256 bitmap glyphs, one regeneration per label after the settle, then a static context.
// - B5 text-effects: 50 labels with text-shadow, outline and glow: no layer pushed for them once
//   their distance glyphs exist.
// - B6: B2 at 1024 x 1024, a texture-mode document's own context: B2's draw budgets.
// - Gathering: <= 0.2 ms per 10 000 gathered vertices (the first frame of B2, everything new).
// - The 1-sample report (AC14): every benchmark at one sample with the defaults, printed, not gated.
//
// CPU time is the engine's own work, the frame's wall time less the graphics backend's share
// (RenderBackendTime: llvmpipe and lavapipe do on this thread what a GPU driver does not), the
// median of the measured frames; gates take the least of up to 3 runs and the
// JASPER_HTMLUI_PERF_SLACK factor (compat/PerfFixtures.h). Results go to
// build/<platform>/htmlui-render-bench[-vulkan].json and the table to docs/html-ui/render-benchmark.md.

#include "FixtureRunner.h"
#include "TestRenderCommon.h"

#include "compat/PerfFixtures.h"

#include "HtmlUIClock.h"
#include "RmlFontEngine.h"
#include "htmlui/compat/ImportManifest.h"
#include "render/RenderBackendTime.h"

#include "json.hpp"

#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifdef NDEBUG

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // S16 off (D1) or the defaults
    struct S16Mode {
        bool batching, primitives, transformAware;
        explicit S16Mode(bool on):
            batching(RmlRenderer::isBatching()), primitives(RmlRenderer::isSdfPrimitives()), transformAware(RmlFontEngine::isTransformAwareText()){
            RmlRenderer::setBatchingForTesting(on);
            RmlRenderer::setSdfPrimitivesForTesting(on);
            RmlFontEngine::setTransformAwareTextForTesting(on);
        }
        ~S16Mode(){
            RmlRenderer::setBatchingForTesting(batching);
            RmlRenderer::setSdfPrimitivesForTesting(primitives);
            RmlFontEngine::setTransformAwareTextForTesting(transformAware);
        }
    };

    struct BenchRun {
        HtmlUIRenderStats last;              // the last measured frame
        uint32_t maxDraws = 0, maxStencilDraws = 0, maxLayers = 0, maxBoxShadowTextures = 0, maxGlyphs = 0;
        double maxGlyphMs = 0.0;
        double engineMs = 0.0;               // median: wall less the backend
        double frameMs = 0.0;                // median wall time
        double gatherMs = 0.0;               // median
        size_t gatherLive = 0, gatherCapacity = 0;
        std::array<uint32_t, (size_t)RmlRenderer::RunBreak::COUNT> runBreaks{};   // the last frame's
    };

    double median(std::vector<double> values){
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    std::filesystem::path examplesRoot(){
        return std::filesystem::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / ".." / ".." / ".." / "docs" / "mcp" / "guide" / "html-ui-examples";
    }

    const char* const examples[] = {"blank", "dialogue", "hud", "inventory", "loading", "main_menu", "pause_menu", "settings", "shop",
                                    "toasts", "video_menu"};

    // a document of the asset root (assetRoot: another directory, the example library)
    bool load(FixtureRunner& runner, const std::string& asset, const std::string& assetRoot = std::string()){
        if (!assetRoot.empty()){
            TestSystem::get().setAssetPath(assetRoot);
            htmlui::compat::ImportManifest::reload();
        }
        std::string error;
        const bool loaded = runner.load(asset, &error);
        EXPECT_TRUE(loaded) << error;
        return loaded;
    }

    // warm-up and measured frames on the moving clock
    BenchRun measure(FixtureRunner& runner, int warm = 30, int measured = 120, const std::function<void(int)>& perFrame = {}){
        double clock = runner.time();
        RenderBackendTime::setEnabled(true);
        BenchRun run;
        std::vector<double> engines, frames, gathers;
        for (int i = 0; i < warm + measured; i++){
            clock += 1.0 / 60.0;
            if (perFrame)
                perFrame(i);
            RenderBackendTime::reset();
            const auto start = std::chrono::steady_clock::now();
            runner.frameAt(clock, true);
            const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (i < warm)
                continue;
            const HtmlUIRenderStats& stats = runner.renderer()->getStats();
            run.last = stats;
            run.runBreaks = runner.renderer()->getLastRunBreaks();
            run.maxDraws = std::max(run.maxDraws, stats.drawCalls);
            run.maxStencilDraws = std::max(run.maxStencilDraws, stats.stencilDraws);
            run.maxLayers = std::max(run.maxLayers, stats.layersPushed);
            run.maxBoxShadowTextures = std::max(run.maxBoxShadowTextures, stats.boxShadowTextures);
            run.maxGlyphs = std::max(run.maxGlyphs, stats.glyphsRasterized);
            run.maxGlyphMs = std::max(run.maxGlyphMs, runner.textCounters().glyphRasterMs);
            engines.push_back(wall - RenderBackendTime::totalMs());
            frames.push_back(wall);
            gathers.push_back(runner.renderer()->getLastGatherMs());
            run.gatherLive = std::max(run.gatherLive, runner.renderer()->getGatherLiveBytes());
            run.gatherCapacity = std::max(run.gatherCapacity, runner.renderer()->getGatherCapacityBytes());
        }
        RenderBackendTime::setEnabled(false);
        run.engineMs = median(engines);
        run.frameMs = median(frames);
        run.gatherMs = median(gathers);
        return run;
    }

    // the least engine time of up to 3 runs, stopping once within target
    BenchRun gated(const std::function<BenchRun()>& once, double target){
        BenchRun least = once();
        for (int attempt = 1; attempt < 3 && target > 0.0 && least.engineMs > target; attempt++){
            const BenchRun run = once();
            if (run.engineMs < least.engineMs)
                least = run;
        }
        return least;
    }

    // the results of every benchmark of the process, written after each
    nlohmann::json& results(){
        static nlohmann::json value = nlohmann::json::object();
        return value;
    }

    std::string resultsFile(){
        // build/linux/htmlui-tests-<backend>/tests/htmlui -> build/linux
        const std::filesystem::path out = std::filesystem::path(JASPER_HTMLUI_TEST_BINARY_DIR).parent_path().parent_path().parent_path();
#if defined(SOKOL_VULKAN)
        return (out / "htmlui-render-bench-vulkan.json").string();
#else
        return (out / "htmlui-render-bench.json").string();
#endif
    }

    nlohmann::json toJson(const BenchRun& run){
        return {{"drawCalls", run.last.drawCalls}, {"maxDrawCalls", run.maxDraws}, {"stencilDraws", run.maxStencilDraws},
                {"layersPushed", run.maxLayers}, {"boxShadowTextures", run.maxBoxShadowTextures}, {"runsMerged", run.last.runsMerged},
                {"runCacheHits", run.last.runCacheHits}, {"instancesDrawn", run.last.instancesDrawn}, {"verticesGathered", run.last.verticesGathered},
                {"geometrySubmitted", run.last.geometrySubmitted}, {"atlasBytes", run.last.atlasBytes}, {"maxGlyphsRasterized", run.maxGlyphs},
                {"maxGlyphRasterMs", run.maxGlyphMs}, {"engineMs", run.engineMs}, {"frameMs", run.frameMs}, {"gatherMs", run.gatherMs},
                {"gatherLiveBytes", run.gatherLive}, {"gatherCapacityBytes", run.gatherCapacity}};
    }

    void record(const std::string& name, const BenchRun& baseline, const BenchRun& s16){
        results()[name] = {{"baseline", toJson(baseline)}, {"s16", toJson(s16)}};
        std::printf("[ ui-bench ] %-24s draws %4u -> %4u, stencil %3u -> %3u, layers %3u -> %3u, shadow textures %3u -> %3u, "
                    "engine %.3f -> %.3f ms (frame %.3f -> %.3f ms)\n",
                    name.c_str(), baseline.maxDraws, s16.maxDraws, baseline.maxStencilDraws, s16.maxStencilDraws, baseline.maxLayers,
                    s16.maxLayers, baseline.maxBoxShadowTextures, s16.maxBoxShadowTextures, baseline.engineMs, s16.engineMs, baseline.frameMs,
                    s16.frameMs);
        // what started the S16 runs (RmlRenderer::RunBreak), when it batched
        const auto& b = s16.runBreaks;
        using RB = RmlRenderer::RunBreak;
        std::printf("[ ui-bench ] %-24s runs after a command %u, kind %u, clip/layer %u, transform %u, texture %u, scissor %u\n", "",
                    b[(size_t)RB::COMMAND], b[(size_t)RB::KIND], b[(size_t)RB::CLIP], b[(size_t)RB::TRANSFORM], b[(size_t)RB::TEXTURE],
                    b[(size_t)RB::SCISSOR]);
        results()[name]["s16"]["runBreaks"] = {{"command", b[(size_t)RB::COMMAND]}, {"kind", b[(size_t)RB::KIND]}, {"clip", b[(size_t)RB::CLIP]},
                                               {"transform", b[(size_t)RB::TRANSFORM]}, {"texture", b[(size_t)RB::TEXTURE]},
                                               {"scissor", b[(size_t)RB::SCISSOR]}};
        std::ofstream out(resultsFile());
        out << results().dump(2) << "\n";
    }

    // one document both ways: the baseline, then S16 (gated on the engine time when target > 0)
    void both(const std::string& asset, int size, BenchRun& baseline, BenchRun& s16, const std::function<double(const BenchRun&)>& target = {},
              const std::string& assetRoot = std::string(), int samples = 4){
        {
            S16Mode off(false);
            FixtureRunner runner(size, size == 1024 ? 1024 : 720, samples);
            ASSERT_TRUE(runner.valid());
            ASSERT_TRUE(load(runner, asset, assetRoot));
            baseline = gated([&](){ return measure(runner); }, 0.0);
        }
        {
            S16Mode on(true);
            FixtureRunner runner(size, size == 1024 ? 1024 : 720, samples);
            ASSERT_TRUE(runner.valid());
            ASSERT_TRUE(load(runner, asset, assetRoot));
            HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
            const double limit = target ? target(baseline) : 0.0;
            s16 = gated([&](){ return measure(runner); }, limit);
        }
    }

}

TEST(HtmlUIRenderPerf, B1Examples){
    JASPER_REQUIRE_GPU();
    const double slack = compat::perfSlack();
    for (const char* example : examples){
        BenchRun baseline, s16;
        const std::string asset = std::string(example) + "/" + example + ".html";
        both(asset, 1280, baseline, s16, [&](const BenchRun& b){ return b.engineMs * 1.05 * slack + 0.05; }, examplesRoot().string());
        record(std::string("B1 ") + example, baseline, s16);
        EXPECT_LE(s16.maxDraws, std::max(1u, (uint32_t)(0.30 * baseline.maxDraws))) << example;
        EXPECT_LE(s16.maxDraws, 40u) << example;
        EXPECT_LE(s16.engineMs, baseline.engineMs * 1.05 * slack + 0.05) << example;
    }
}

TEST(HtmlUIRenderPerf, B2List500){
    JASPER_REQUIRE_GPU();
    const double slack = compat::perfSlack();
    BenchRun baseline, s16;
    both("fixtures/bench/list-500.html", 1280, baseline, s16, [&](const BenchRun& b){ return b.engineMs * 0.5 * slack; });
    record("B2 list-500", baseline, s16);
    EXPECT_LE(s16.maxDraws, 16u);
    EXPECT_EQ(s16.maxStencilDraws, 0u);
    EXPECT_EQ(s16.maxBoxShadowTextures, 0u);
    // the animating card regathers its chunk only, everything else comes from the run cache
    EXPECT_GT(s16.last.runCacheHits, 0u);
    EXPECT_LE(s16.engineMs, baseline.engineMs * 0.5 * slack);
    // R23: the stream and the run cache within twice what the frame gathers
    EXPECT_LE(s16.gatherCapacity, std::max<size_t>(2 * s16.gatherLive, 512 * 1024));
}

TEST(HtmlUIRenderPerf, B3TextWall){
    JASPER_REQUIRE_GPU();
    BenchRun baseline, s16;
    both("fixtures/bench/text-wall.html", 1280, baseline, s16);
    record("B3 text-wall", baseline, s16);
    EXPECT_LE(s16.maxDraws, 4u);
    // R23: R8 pages, 1 MiB each where the RGBA8 expansion took 4 (30 % at most)
    const RmlFontEngine& fonts = HtmlUI::getFontEngine();
    EXPECT_LE((double)s16.last.atlasBytes, 0.30 * (double)(fonts.getPageCount() * 4u * 1024u * 1024u) + (double)(fonts.getDistancePageCount() * 4u * 1024u * 1024u));
}

TEST(HtmlUIRenderPerf, B4ScalePopin){
    JASPER_REQUIRE_GPU();
    S16Mode on(true);
    FixtureRunner runner(1280, 720, 4);
    ASSERT_TRUE(runner.valid());
    ASSERT_TRUE(load(runner, "fixtures/bench/scale-popin.html"));
    // the pop-in and its settle, frame by frame from the start of the animation
    uint32_t settleRegenerations = 0;
    double clock = 0.0;
    int frame = 0;
    for (; frame < 60; frame++){
        clock += 1.0 / 60.0;
        runner.frameAt(clock, true);
        const HtmlUIRenderStats& stats = runner.renderer()->getStats();
        EXPECT_LE(stats.glyphsRasterized, (uint32_t)RmlFontEngine::settleGlyphBudget) << "frame " << frame;
        if (frame > 0)
            EXPECT_LE(runner.textCounters().glyphRasterMs, RmlFontEngine::settleTimeBudgetMs * compat::perfSlack()) << "frame " << frame;
        // after the animation (300 ms and its 80 ms of delays)
        if (clock > 0.4)
            settleRegenerations += stats.textRegenerations;
        if (clock > 0.4 && !runner.renderRequested())
            break;
    }
    std::printf("[ ui-bench ] B4 scale-popin: settled %d frames after the start, %u regenerations after the animation\n", frame,
                settleRegenerations);
    EXPECT_LT(frame, 60) << "the text never settled";
    EXPECT_EQ(settleRegenerations, 40u);
    // a static context: nothing asked for, nothing made again
    runner.frameAt(clock + 1.0 / 60.0, true);
    EXPECT_FALSE(runner.renderRequested());
    EXPECT_EQ(runner.renderer()->getStats().textRegenerations, 0u);
    results()["B4 scale-popin"] = {{"settleFrames", frame}, {"settleRegenerations", settleRegenerations}};
}

TEST(HtmlUIRenderPerf, B5TextEffects){
    JASPER_REQUIRE_GPU();
    BenchRun baseline, s16;
    both("fixtures/bench/text-effects.html", 1280, baseline, s16);
    record("B5 text-effects", baseline, s16);
    EXPECT_GT(baseline.maxLayers, 0u);
    EXPECT_EQ(s16.maxLayers, 0u);
    EXPECT_EQ(s16.last.textEffectFallbacks, 0u);
}

TEST(HtmlUIRenderPerf, B6TextureList500){
    JASPER_REQUIRE_GPU();
    BenchRun baseline, s16;
    both("fixtures/bench/list-500.html", 1024, baseline, s16);
    record("B6 list-500 texture", baseline, s16);
    EXPECT_LE(s16.maxDraws, 16u);
    EXPECT_EQ(s16.maxStencilDraws, 0u);
    EXPECT_EQ(s16.maxBoxShadowTextures, 0u);
}

TEST(HtmlUIRenderPerf, GatherCost){
    JASPER_REQUIRE_GPU();
    S16Mode on(true);
    double least = -1.0;
    uint32_t vertices = 0;
    // the first frame of the list: everything gathered anew
    for (int attempt = 0; attempt < 5; attempt++){
        FixtureRunner runner(1280, 720, 4);
        ASSERT_TRUE(runner.valid());
        ASSERT_TRUE(load(runner, "fixtures/bench/list-500.html"));
        runner.frameAt(1.0 / 60.0, true);
        vertices = runner.renderer()->getStats().verticesGathered;
        const double ms = runner.renderer()->getLastGatherMs();
        least = least < 0.0 ? ms : std::min(least, ms);
    }
    ASSERT_GT(vertices, 0u);
    const double per10k = least * 10000.0 / (double)vertices;
    std::printf("[ ui-bench ] gathering: %u vertices and instances in %.3f ms (%.3f ms per 10k)\n", vertices, least, per10k);
    results()["gather"] = {{"items", vertices}, {"ms", least}, {"msPer10k", per10k}};
    EXPECT_LE(per10k, 0.2 * compat::perfSlack());
}

TEST(HtmlUIRenderPerf, OneSampleReport){
    JASPER_REQUIRE_GPU();
    // AC14: printed, not gated
    S16Mode on(true);
    for (const char* asset : {"fixtures/bench/list-500.html", "fixtures/bench/text-wall.html", "fixtures/bench/text-effects.html"}){
        FixtureRunner runner(1280, 720, 1);
        ASSERT_TRUE(runner.valid());
        ASSERT_TRUE(load(runner, asset));
        const BenchRun run = measure(runner, 10, 30);
        std::printf("[ ui-bench ] 1 sample %-32s draws %u, stencil %u, layers %u, engine %.3f ms\n", asset, run.maxDraws, run.maxStencilDraws,
                    run.maxLayers, run.engineMs);
        results()[std::string("1 sample ") + asset] = toJson(run);
    }
}

#endif
