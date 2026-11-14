// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S81's runtime bench on the test device (docs/specs/S81-title-art-runtime.md AC17, §6.8), the
// same method as the spec's (5 warm-ups, then median and p90 of the samples): a counter's text
// change as a Realtime title (S80's full render, sliced by the scheduler) and as a Glyph Atlas
// title (with every glyph cached: the compose alone; and with new cells), and the scheduler's
// frames under a 4 ms budget. The numbers are wall-clock milliseconds of the frames on whatever
// device the tests run (CPU work plus the GPU commands' submission; a software rasterizer such as
// llvmpipe executes them on the CPU): they are printed for the record, not device figures. Only
// what holds on any device is checked: a cached Glyph Atlas change costs less than a Realtime
// render. The frame bound (budget + 0.5 ms) is the scheduler's estimate (TitleArtSchedulerTest);
// on a software rasterizer the measured frames exceed it, which the bench prints.

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleTestAssets.h"

#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr const char* TEXT_LAYER = "00000001";
    constexpr int WARMUPS = 5;
    constexpr int SAMPLES = 30;

    std::shared_ptr<const TitleArtDoc> counterDoc() {
        Json text{{"id", TEXT_LAYER}, {"type", "text"}, {"name", "Score"}, {"text", "0"}, {"font", fontRef(SOURCE_SANS)}, {"size", 96},
                  {"features", {{"tnum", 1}}}, {"kerning", false},
                  {"style", {{"face", solidFace("#ffd040")}, {"rims", Json::array({Json{{"id", "00000101"}, {"width", 6}, {"material", solidFace("#802000")}}})}}}};
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(640, 160);
        root["layers"] = Json::array({shapeLayer("00000002", "rect", 140, solidFace("#203060"), {{"aspect", 4.0}}), text});
        root["exposed"] = Json::array({Json{{"name", "text"}, {"type", "text"}, {"targets", Json::array({Json{{"layer", TEXT_LAYER}, {"field", "text"}}})}}});
        return std::make_shared<const TitleArtDoc>(parseTitleArtJson(std::move(root)).doc);
    }

    struct Sample {
        double ms = 0.0;
        int frames = 0;
    };

    // the frames until the live has no work: wall time of each, the largest step estimate per frame
    Sample untilIdle(TitleArtLive& live, const TitleJobFactory& factory, uint32_t budgetUs, double* worstFrameUs = nullptr) {
        TitleLiveLimits limits;
        static double clock = 0.0;
        Sample s;
        const auto start = std::chrono::steady_clock::now();
        while (live.hasWork() && s.frames < 100000) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            const TitleFrameReport report = TitleArtScheduler::schedule({&live}, factory, budgetUs, 1.0);
            SystemRender::commit();
            if (worstFrameUs && report.steps > 1) *worstFrameUs = std::max(*worstFrameUs, report.spentUs);
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
            s.frames++;
        }
        s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return s;
    }

    double percentile(std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)std::floor(p * (double)(v.size() - 1) + 0.5))];
    }

    TitleArtLiveState state() {
        TitleArtLiveState s;
        s.label = "TitleArt/Score";
        return s;
    }

    class TitleArtRuntimeBenchGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
        TestAssetResolver assets;
        TitleArtRenderer renderer;
    };

}

TEST_F(TitleArtRuntimeBenchGpu, HostNumbers) {
    auto doc = counterDoc();
    const TitleJobFactory factory = makeRendererJobFactory(renderer, &assets);
    const uint32_t unlimited = 1000000000u;

    // a text change per sample (values never shown before: no cache hit)
    // cached: the samples whose change rendered no cell (Glyph Atlas)
    auto measure = [&](TitleArtRenderMode mode, std::vector<double>& out, std::vector<double>* cached) {
        TitleArtLive live("assets/score.titleart", mode, doc, 1);
        if (mode == TitleArtRenderMode::GLYPH_ATLAS) live.setGlyphPlan(planTitleGlyphAtlas(*doc));
        int value = 1000;
        for (int i = 0; i < WARMUPS + SAMPLES; i++) {
            const std::string text = std::to_string(value++);
            const uint64_t cells = live.glyphAtlas() ? live.glyphAtlas()->stats().cellRenders : 0;
            live.setState(state(), fnv1a64(text), {FieldOverride{TEXT_LAYER, "text", Json(text)}});
            const Sample s = untilIdle(live, factory, unlimited);
            ASSERT_TRUE(live.front().valid()) << live.status().error;
            if (i < WARMUPS) continue;
            out.push_back(s.ms);
            if (cached && live.glyphAtlas() && live.glyphAtlas()->stats().cellRenders == cells) cached->push_back(s.ms);
        }
    };
    std::vector<double> realtime, glyph, glyphCached;
    measure(TitleArtRenderMode::REALTIME, realtime, nullptr);
    measure(TitleArtRenderMode::GLYPH_ATLAS, glyph, &glyphCached);
    ASSERT_FALSE(glyphCached.empty()) << "some values reuse every cell";
    std::printf("[bench] host, %s: Realtime text change (one render, unsliced): median %.2f ms, p90 %.2f ms\n",
                SystemRender::getFeatures().renderRGBA16F ? "float path" : "rgba8 path", percentile(realtime, 0.5), percentile(realtime, 0.9));
    std::printf("[bench] host: Glyph Atlas text change, every sample (new cells on first use): median %.2f ms, p90 %.2f ms\n",
                percentile(glyph, 0.5), percentile(glyph, 0.9));
    std::printf("[bench] host: Glyph Atlas text change, glyphs cached (compose and output, %zu samples): median %.2f ms, p90 %.2f ms\n",
                glyphCached.size(), percentile(glyphCached, 0.5), percentile(glyphCached, 0.9));
    EXPECT_LT(percentile(glyphCached, 0.5), percentile(realtime, 0.5)) << "a cached Glyph Atlas change is cheaper than a render";

    // the scheduler's frames under a 4 ms budget: each frame's estimates stay within the budget
    // except the step a frame always runs (R13)
    TitleArtLive sliced("assets/score.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    sliced.setState(state(), fnv1a64("sliced"), {FieldOverride{TEXT_LAYER, "text", Json("987654")}});
    double worstUs = 0.0;
    const Sample s = untilIdle(sliced, factory, 4000, &worstUs);
    std::printf("[bench] host: Realtime render sliced at 4 ms: %d frames, %.2f ms wall, largest multi-step frame estimate %.2f ms\n", s.frames,
                s.ms, worstUs / 1000.0);
    EXPECT_GT(s.frames, 1) << "the render is sliced over frames";
}
