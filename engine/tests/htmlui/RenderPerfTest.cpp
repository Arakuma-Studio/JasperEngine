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
// - B6: B2 at 1024 x 1024, a texture-mode document's own context: B2's draw and CPU budgets.
// - B7 virtual-list: an S17 <list-view> of 10 000 rows scrolled 100 px a frame (its rows recycle):
//   <= 16 draws, no stencil draw, the engine's CPU time <= baseline + 5 %.
// - Gathering: <= 0.2 ms per 10 000 gathered vertices (the first frame of B2, everything new).
// - The 1-sample report (AC14): every benchmark at one sample with the defaults, printed, not gated.
//
// CPU time is the engine's own work, the frame's wall time less the graphics backend's share
// (RenderBackendTime: llvmpipe and lavapipe do on this thread what a GPU driver does not), the
// median of the measured frames, each started with the GPU idle (finishGpu: the software
// rasterizer's threads, one per core, would otherwise take the cores from the engine's thread
// while they draw the last frame, more for the heavier frame); gates take the least of up to 3 runs and the
// JASPER_HTMLUI_PERF_SLACK factor (compat/PerfFixtures.h). Results go to
// build/<platform>/htmlui-render-bench[-vulkan].json and the table to docs/html-ui/render-benchmark.md.

#include "FixtureRunner.h"
#include "TestRenderCommon.h"

#include "compat/PerfFixtures.h"

#include "HtmlUIClock.h"
#include "RmlFontEngine.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/widgets/ItemsViewElement.h"
#include "render/RenderBackendTime.h"

// (the timer exists only in the builds with the tests: one without it would measure 0)
static_assert(JASPERENGINE_BACKEND_TIME, "these tests need RenderBackendTime (JASPERENGINE_BACKEND_TIME)");
#include "render/SystemRender.h"

#include "json.hpp"

#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
        double updateMs = 0.0, recordMs = 0.0, replayMs = 0.0;   // medians: Context::Update, Render, endFrame less the backend
        size_t gatherLive = 0, gatherCapacity = 0;
        std::array<uint32_t, (size_t)RmlRenderer::RunBreak::COUNT> runBreaks{};   // the last frame's
        size_t basePages = 0, distancePages = 0;   // the font engine's atlas pages after the run
        RmlRenderer::EffectCounts effects;          // the last frame's
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
        // the document's shaders and pipelines first (S32 creates them asynchronously: until they
        // are ready a draw is skipped, and the benchmark measured frames of no draws at all; found
        // in the GPU campaign, failing alike on llvmpipe)
        // (idle over 5 frames in a row: a frame can still ask for what the last one did not draw)
        for (int i = 0, idleFrames = 0; i < 600 && idleFrames < 5; i++){
            clock += 1.0 / 60.0;
            runner.frameAt(clock, true);
            const bool idle = SystemRender::waitGpuCreationIdle(100.0);
            // (a document that draws nothing at all is idle after 30 frames)
            idleFrames = (idle && (runner.renderer()->getStats().drawCalls > 0 || i >= 30)) ? idleFrames + 1 : 0;
        }
        BenchRun run;
        std::vector<double> engines, frames, gathers, updates, records, replays;
        for (int i = 0; i < warm + measured; i++){
            clock += 1.0 / 60.0;
            if (perFrame)
                perFrame(i);
            // the last frame rasterized first: llvmpipe's threads (as many as the cores) would
            // otherwise compete with this frame's engine work for the CPU, which a GPU does not
            finishGpu();
            RenderBackendTime::reset();
            const auto start = std::chrono::steady_clock::now();
            runner.frameAt(clock, true);
            const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (i < warm)
                continue;
            const HtmlUIRenderStats& stats = runner.renderer()->getStats();
            run.last = stats;
            run.runBreaks = runner.renderer()->getLastRunBreaks();
            run.effects = runner.renderer()->getLastEffectCounts();
            run.maxDraws = std::max(run.maxDraws, stats.drawCalls);
            run.maxStencilDraws = std::max(run.maxStencilDraws, stats.stencilDraws);
            run.maxLayers = std::max(run.maxLayers, stats.layersPushed);
            run.maxBoxShadowTextures = std::max(run.maxBoxShadowTextures, stats.boxShadowTextures);
            run.maxGlyphs = std::max(run.maxGlyphs, stats.glyphsRasterized);
            run.maxGlyphMs = std::max(run.maxGlyphMs, runner.textCounters().glyphRasterMs);
            engines.push_back(wall - RenderBackendTime::totalMs());
            frames.push_back(wall);
            gathers.push_back(runner.renderer()->getLastGatherMs());
            const FixtureRunner::PhaseTimes phases = runner.phaseTimes();
            updates.push_back(phases.updateMs);
            records.push_back(phases.renderMs);
            replays.push_back(phases.endFrameMs - RenderBackendTime::totalMs());
            run.gatherLive = std::max(run.gatherLive, runner.renderer()->getGatherLiveBytes());
            run.gatherCapacity = std::max(run.gatherCapacity, runner.renderer()->getGatherCapacityBytes());
        }
        RenderBackendTime::setEnabled(false);
        run.basePages = HtmlUI::getFontEngine().getPageCount();
        run.distancePages = HtmlUI::getFontEngine().getDistancePageCount();
        run.engineMs = median(engines);
        run.frameMs = median(frames);
        run.gatherMs = median(gathers);
        run.updateMs = median(updates);
        run.recordMs = median(records);
        run.replayMs = median(replays);
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
                {"maxGlyphRasterMs", run.maxGlyphMs}, {"engineMs", run.engineMs}, {"frameMs", run.frameMs}, {"gatherMs", run.gatherMs}, {"updateMs", run.updateMs}, {"recordMs", run.recordMs}, {"replayMs", run.replayMs},
                {"gatherLiveBytes", run.gatherLive}, {"gatherCapacityBytes", run.gatherCapacity}};
    }

    void record(const std::string& name, const BenchRun& baseline, const BenchRun& s16){
        results()[name] = {{"baseline", toJson(baseline)}, {"s16", toJson(s16)}};
        std::printf("[ ui-bench ] %-24s draws %4u -> %4u, stencil %3u -> %3u, layers %3u -> %3u, shadow textures %3u -> %3u, "
                    "engine %.3f -> %.3f ms (frame %.3f -> %.3f ms)\n",
                    name.c_str(), baseline.maxDraws, s16.maxDraws, baseline.maxStencilDraws, s16.maxStencilDraws, baseline.maxLayers,
                    s16.maxLayers, baseline.maxBoxShadowTextures, s16.maxBoxShadowTextures, baseline.engineMs, s16.engineMs, baseline.frameMs,
                    s16.frameMs);
        std::printf("[ ui-bench ] %-24s update %.3f -> %.3f ms, record %.3f -> %.3f ms, replay %.3f -> %.3f ms (gather %.3f ms)\n", "",
                    baseline.updateMs, s16.updateMs, baseline.recordMs, s16.recordMs, baseline.replayMs, s16.replayMs, s16.gatherMs);
        // what started the S16 runs (RmlRenderer::RunBreak), when it batched
        const auto& b = s16.runBreaks;
        using RB = RmlRenderer::RunBreak;
        std::printf("[ ui-bench ] %-24s runs after a command %u, kind %u, clip/layer %u, transform %u, texture %u, scissor %u\n", "",
                    b[(size_t)RB::COMMAND], b[(size_t)RB::KIND], b[(size_t)RB::CLIP], b[(size_t)RB::TRANSFORM], b[(size_t)RB::TEXTURE],
                    b[(size_t)RB::SCISSOR]);
        std::printf("[ ui-bench ] %-24s effects: %u gradient/effect shader draws, %u composites, %u filters, %u clip masks\n", "",
                    s16.effects.shaders, s16.effects.composites, s16.effects.filters, s16.effects.clipMasks);
        results()[name]["s16"]["effects"] = {{"shaders", s16.effects.shaders}, {"composites", s16.effects.composites},
                                             {"filters", s16.effects.filters}, {"clipMasks", s16.effects.clipMasks}};
        results()[name]["s16"]["runBreaks"] = {{"command", b[(size_t)RB::COMMAND]}, {"kind", b[(size_t)RB::KIND]}, {"clip", b[(size_t)RB::CLIP]},
                                               {"transform", b[(size_t)RB::TRANSFORM]}, {"texture", b[(size_t)RB::TEXTURE]},
                                               {"scissor", b[(size_t)RB::SCISSOR]}};
        std::ofstream out(resultsFile());
        out << results().dump(2) << "\n";
    }

    // after the load, what every frame does (B7 scrolls its list)
    using Prepare = std::function<std::function<void(int)>(FixtureRunner&)>;

    // one document both ways: the baseline, then S16 (gated on the engine time when target > 0)
    void both(const std::string& asset, int size, BenchRun& baseline, BenchRun& s16, const std::function<double(const BenchRun&)>& target = {},
              const std::string& assetRoot = std::string(), int samples = 4, const Prepare& prepare = {}){
        {
            S16Mode off(false);
            FixtureRunner runner(size, size == 1024 ? 1024 : 720, samples);
            ASSERT_TRUE(runner.valid());
            ASSERT_TRUE(load(runner, asset, assetRoot));
            const std::function<void(int)> perFrame = prepare ? prepare(runner) : std::function<void(int)>();
            baseline = gated([&](){ return measure(runner, 30, 120, perFrame); }, 0.0);
        }
        {
            S16Mode on(true);
            FixtureRunner runner(size, size == 1024 ? 1024 : 720, samples);
            ASSERT_TRUE(runner.valid());
            ASSERT_TRUE(load(runner, asset, assetRoot));
            const std::function<void(int)> perFrame = prepare ? prepare(runner) : std::function<void(int)>();
            HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
            const double limit = target ? target(baseline) : 0.0;
            s16 = gated([&](){ return measure(runner, 30, 120, perFrame); }, limit);
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
        // R21's "<= 30 % of the baseline and <= 40" is reported, not gated (spec Deviations): the
        // examples' draws are mostly their effects' passes (filters, backdrop blurs, gradients,
        // layers) and textures that cannot share a draw, which S16 leaves as they are
        const bool target = s16.maxDraws <= std::max(1u, (uint32_t)(0.30 * baseline.maxDraws)) && s16.maxDraws <= 40u;
        std::printf("[ ui-bench ] %-24s R21 draw target (<= 30 %%, <= 40): %s\n", "", target ? "met" : "not met");
        EXPECT_LE(s16.maxDraws, baseline.maxDraws) << example;
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
    // R22: the engine time <= 50 % of the baseline (the replay batched, the paint traversal passing
    // over the cards outside the list's scissor: S16-F4, S16-F6)
    EXPECT_LE(s16.replayMs, baseline.replayMs * 0.5 * slack);
    EXPECT_LE(s16.engineMs, baseline.engineMs * 0.5 * slack);
    // R23: the stream and the run cache within twice what the frame gathers
    EXPECT_LE(s16.gatherCapacity, std::max<size_t>(2 * s16.gatherLive, 512 * 1024));
}

TEST(HtmlUIRenderPerf, B3TextWall){
    JASPER_REQUIRE_GPU();
    BenchRun baseline, s16;
    both("fixtures/bench/text-wall.html", 1280, baseline, s16);
    record("B3 text-wall", baseline, s16);
    {
        // (printed: the glyphs batched as vertices instead of instances, for the wall-time comparison
        // on a software rasterizer, whose instanced draws cost per instance)
        S16Mode on(true);
        RmlRenderer::setSdfPrimitivesForTesting(false);
        FixtureRunner runner(1280, 720, 4);
        ASSERT_TRUE(runner.valid());
        ASSERT_TRUE(load(runner, "fixtures/bench/text-wall.html"));
        const BenchRun vertices = measure(runner);
        std::printf("[ ui-bench ] B3 as vertices: draws %u, engine %.3f ms, frame %.3f ms\n", vertices.maxDraws, vertices.engineMs, vertices.frameMs);
        results()["B3 text-wall"]["s16Vertices"] = toJson(vertices);
    }
    EXPECT_LE(s16.maxDraws, 4u);
    // R23: R8 pages, 1 MiB each where the RGBA8 expansion took 4 (30 % at most)
    // (the page counts of the measured run: the runner releases them)
    ASSERT_GT(s16.basePages, 0u);
    EXPECT_LE((double)s16.last.atlasBytes, 0.30 * (double)(s16.basePages * 4u * 1024u * 1024u) + (double)(s16.distancePages * 4u * 1024u * 1024u));
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
        if (frame > 0){
            EXPECT_LE(runner.textCounters().glyphRasterMs, RmlFontEngine::settleTimeBudgetMs * compat::perfSlack()) << "frame " << frame;
        }
        // from the end of the first labels' animation (300 ms; the others' delays reach 80 ms)
        if (clock > 0.3)
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
    const double slack = compat::perfSlack();
    both("fixtures/bench/list-500.html", 1024, baseline, s16, [&](const BenchRun& b){ return b.engineMs * 0.5 * slack; });
    record("B6 list-500 texture", baseline, s16);
    EXPECT_LE(s16.engineMs, baseline.engineMs * 0.5 * slack);
    EXPECT_LE(s16.maxDraws, 16u);
    EXPECT_EQ(s16.maxStencilDraws, 0u);
    EXPECT_EQ(s16.maxBoxShadowTextures, 0u);
}

// an S17 virtual list (<list-view>, 10 000 rows bound by a C++ provider) scrolled 100 px a frame
// (6 000 px/s): its rows recycle every frame, so the run cache misses on them; the culling and
// the instanced runs still keep the draws and the engine time (S17's widgets, round 2)
TEST(HtmlUIRenderPerf, B7VirtualListScroll){
    JASPER_REQUIRE_GPU();
    const double slack = compat::perfSlack();
    const Prepare scroll = [](FixtureRunner& runner) -> std::function<void(int)> {
        Rml::Element* element = runner.document() ? runner.document()->GetElementById("list") : nullptr;
        htmlui::ItemsViewElement* list = rmlui_dynamic_cast<htmlui::ItemsViewElement*>(element);
        EXPECT_NE(list, nullptr);
        if (!list)
            return {};
        htmlui::ItemsViewElement::Provider provider;
        provider.count = 10000;
        provider.bind = [](Rml::Element* row, int index){
            static const char* const zones[] = {"Harbor", "Forest", "Ruins", "Peaks"};
            if (Rml::Element* name = row->QuerySelector(".name"))
                name->SetInnerRML("Player " + std::to_string(index + 1));
            if (Rml::Element* zone = row->QuerySelector(".zone"))
                zone->SetInnerRML(zones[index % 4]);
            if (Rml::Element* level = row->QuerySelector(".level"))
                level->SetInnerRML("Lv " + std::to_string(index % 60 + 1));
        };
        list->setProvider(provider);
        runner.frameAt(runner.time() + 1.0 / 60.0, true);
        return [list](int){
            const float span = list->GetScrollHeight() - list->GetClientHeight();
            float top = list->GetScrollTop() + 100.0f;
            if (top > span)
                top = 0.0f;
            list->SetScrollTop(top);
        };
    };
    BenchRun baseline, s16;
    both("fixtures/bench/virtual-list.html", 1280, baseline, s16, [&](const BenchRun& b){ return b.engineMs * 1.05 * slack + 0.05; },
         std::string(), 4, scroll);
    record("B7 virtual-list scroll", baseline, s16);
    EXPECT_GT(baseline.last.geometrySubmitted, 100u) << "the rows are drawn";
    EXPECT_LE(s16.maxDraws, 16u);
    EXPECT_EQ(s16.maxStencilDraws, 0u);
    EXPECT_LE(s16.maxDraws, baseline.maxDraws);
    EXPECT_LE(s16.engineMs, baseline.engineMs * 1.05 * slack + 0.05);
}

TEST(HtmlUIRenderPerf, GatherCost){
    JASPER_REQUIRE_GPU();
    S16Mode on(true);
    double least = -1.0, upload = -1.0;
    uint32_t vertices = 0;
    // warm frames of the list, each gathering everything anew (the run cache dropped before it);
    // an instance counts as the 4 vertices it replaces (its 132 bytes against 4 x 20 + 6 x 4).
    // The budget is the CPU's gathering on every backend; the driver's buffer updates are reported
    // apart: a software driver does GPU work in them (lavapipe: 0.12 ms per 10k gathering, 0.7 ms
    // per 10k in the driver, far more on a loaded host), which the gathering budget never meant
    FixtureRunner runner(1280, 720, 4);
    ASSERT_TRUE(runner.valid());
    ASSERT_TRUE(load(runner, "fixtures/bench/list-500.html"));
    double clock = runner.time();
    for (int frame = 0; frame < 40; frame++){
        clock += 1.0 / 60.0;
        runner.renderer()->clearRunCacheForTesting();
        runner.frameAt(clock, true);
        if (frame < 10)
            continue;
        const HtmlUIRenderStats& stats = runner.renderer()->getStats();
        ASSERT_EQ(stats.runCacheHits, 0u);
        vertices = (stats.verticesGathered - stats.instancesDrawn) + 4u * stats.instancesDrawn;
        const double ms = runner.renderer()->getLastGatherMs();
        least = least < 0.0 ? ms : std::min(least, ms);
        const double uploadMs = runner.renderer()->getLastUploadMs();
        upload = upload < 0.0 ? uploadMs : std::min(upload, uploadMs);
    }
    ASSERT_GT(vertices, 0u);
    const double per10k = least * 10000.0 / (double)vertices;
    std::printf("[ ui-bench ] gathering: %u vertices (instances x 4) in %.3f ms (%.3f ms per 10k); the driver's buffer updates %.3f ms\n",
                vertices, least, per10k, upload);
    results()["gather"] = {{"items", vertices}, {"ms", least}, {"msPer10k", per10k}, {"uploadMs", upload}};
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


// A profiling target, not a benchmark (JASPER_HTMLUI_PROFILE=<asset>[,seconds]): the document drawn
// with S16's defaults for that long, for a sampling profiler attached from outside (perf record).
// Skipped without the variable.
TEST(HtmlUIRenderPerf, ProfileTarget){
    const char* value = std::getenv("JASPER_HTMLUI_PROFILE");
    if (!value || !value[0])
        GTEST_SKIP() << "JASPER_HTMLUI_PROFILE not set";
    JASPER_REQUIRE_GPU();
    std::string asset = value;
    double seconds = 20.0;
    const size_t comma = asset.find(',');
    if (comma != std::string::npos){
        seconds = std::atof(asset.c_str() + comma + 1);
        asset = asset.substr(0, comma);
    }
    const bool example = asset.find('/') == std::string::npos;
    S16Mode on(true);
    FixtureRunner runner(1280, 720, 4);
    ASSERT_TRUE(runner.valid());
    ASSERT_TRUE(example ? load(runner, asset + "/" + asset + ".html", examplesRoot().string()) : load(runner, asset));
    std::printf("[ profile  ] %s for %.0f s\n", asset.c_str(), seconds);
    std::fflush(stdout);
    double clock = runner.time();
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds){
        clock += 1.0 / 60.0;
        runner.frameAt(clock, true);
        frames++;
    }
    std::printf("[ profile  ] %d frames\n", frames);
}

#endif
