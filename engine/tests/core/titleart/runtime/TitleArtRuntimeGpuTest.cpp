// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Realtime titles on the headless device (docs/specs/S81-title-art-runtime.md AC3, AC5, AC6, AC7):
// a render sliced by the scheduler over frames equals the one-shot render (the bake's) byte for
// byte; the front image stays the old one until the new one is complete and a shown-before state
// swaps back without a step; a target that cannot be made keeps the previous image; the RGBA8
// path matches the float one within S80's tolerance. Shapes come from the tests' analytic content.

#include "../render/TitleRenderTestEffects.h"
#include "../render/TitleRenderTestSupport.h"

#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"

#include <chrono>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    std::shared_ptr<const TitleArtDoc> twoShapes(const char* color){
        return makeDoc({{"canvas", canvas(320, 160)},
                        {"layers", Json::array({shapeLayer("00000001", "rect", 100, solidFace(color)),
                                                shapeLayer("00000002", "ellipse", 60, solidFace("#ff2020"),
                                                           {{"effects", Json::array({Json{{"id", "00000003"}, {"type", "test_shadow"}}})}})})}});
    }

    // the pixels of a live's front image
    Pixels frontPixels(const TitleArtLive& live){
        const TitleDisplayImage& front = live.front();
        Pixels out;
        out.width = front.width;
        out.height = front.height;
        auto* target = front.target.as<SokolTitleTarget>();
        if (!target){
            ADD_FAILURE() << "the front image has no target";
            return out;
        }
        EXPECT_TRUE(jasperengine::test::readTexture(target->texture(), front.width, front.height, out.bytes));
        for (uint8_t b : out.bytes) out.rgba.push_back(b / 255.0f);
        return out;
    }

    // frames of the scheduler over one live until it has no work; the frames it took
    int runFrames(TitleArtLive& live, const TitleJobFactory& factory, uint32_t budgetUs, int maxFrames = 100000){
        TitleLiveLimits limits;
        double clock = 0.0;
        const auto start = std::chrono::steady_clock::now();
        int frames = 0;
        while (live.hasWork() && frames < maxFrames && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)){
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            TitleArtScheduler::schedule({&live}, factory, budgetUs, 1.0);
            SystemRender::commit();
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
            frames++;
        }
        return frames;
    }

    // a budget no step reaches: each job runs to its end in a frame (once its programs are built)
    constexpr uint32_t UNLIMITED = 1000000000u;

    TitleArtLiveState state(){
        TitleArtLiveState s;
        s.label = "TitleArt/Test";
        return s;
    }

    class TitleArtRuntimeGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            ensureTestEffects();
        }
    };

}

TEST_F(TitleArtRuntimeGpu, RealtimeMatchesBake) {
    auto renderer = makeRenderer();
    TestAssets assets;
    auto doc = twoShapes("#2040ff");
    // the bake: one call to the end
    RenderRequest bake;
    bake.doc = doc;
    const Pixels baked = render(*renderer, bake);
    // the same state through the scheduler with a small budget, so it slices
    TitleArtLive live("assets/test.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    live.setState(state(), 1, {});
    auto fresh = makeRenderer();
    const int frames = runFrames(live, makeRendererJobFactory(*fresh, &assets), 300);
    ASSERT_TRUE(live.front().valid()) << live.status().error;
    EXPECT_GT(frames, 1) << "the render was spread over frames";
    EXPECT_TRUE(identical(frontPixels(live), baked)) << "R10: a Realtime title shows what its bake shows";
    EXPECT_EQ(live.status().pixelPath, std::string(SystemRender::getFeatures().renderRGBA16F && SystemRender::getFeatures().filterRGBA16F ? "float" : "rgba8"));
}

TEST_F(TitleArtRuntimeGpu, DoubleBuffer) {
    auto renderer = makeRenderer();
    TestAssets assets;
    auto doc = twoShapes("#2040ff");
    const TitleJobFactory factory = makeRendererJobFactory(*renderer, &assets);
    TitleArtLive live("assets/test.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    live.setState(state(), 1, {});
    runFrames(live, factory, UNLIMITED);
    ASSERT_TRUE(live.front().valid());
    const Pixels first = frontPixels(live);

    // a colour override renders into its own target: every frame until the swap shows the old image
    live.setState(state(), 2, {FieldOverride{"00000001", "style.face.fill.color", Json("#20ff40")}});
    TitleLiveLimits limits;
    double clock = 0.0;
    int frames = 0;
    while (live.hasWork() && frames < 10000){
        SystemRender::updateGpuCreation();
        RuntimeShaderCompiler::beginFrame();
        TitleArtScheduler::schedule({&live}, factory, 200, 1.0);
        SystemRender::commit();
        if (live.front().hash == 1u) EXPECT_TRUE(identical(frontPixels(live), first)) << "R17: never a half-made image, frame " << frames;
        size_t cache = 0;
        clock += 1.0 / 60.0;
        live.endFrame(clock, 1.0 / 60.0, limits, cache);
        frames++;
    }
    ASSERT_EQ(live.front().hash, 2u);
    const Pixels second = frontPixels(live);
    EXPECT_FALSE(identical(first, second));
    RenderRequest edited;
    edited.doc = doc;
    edited.overrides = {FieldOverride{"00000001", "style.face.fill.color", Json("#20ff40")}};
    auto fresh = makeRenderer();
    EXPECT_TRUE(identical(second, render(*fresh, edited)));

    // R14: back to the first state from the cache, no step
    const uint64_t steps = live.stepsRun();
    live.setState(state(), 1, {});
    EXPECT_FALSE(live.hasWork());
    size_t cache = 0;
    live.endFrame(clock + 0.1, 0.1, limits, cache);
    EXPECT_EQ(live.front().hash, 1u);
    EXPECT_EQ(live.stepsRun(), steps);
    EXPECT_TRUE(identical(frontPixels(live), first));
}

TEST_F(TitleArtRuntimeGpu, FailedTargetsKeepThePreviousImage) {
    auto renderer = makeRenderer();
    TestAssets assets;
    auto doc = twoShapes("#2040ff");
    const TitleJobFactory factory = makeRendererJobFactory(*renderer, &assets);
    TitleArtLive live("assets/test.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    live.setState(state(), 1, {});
    runFrames(live, factory, UNLIMITED);
    ASSERT_TRUE(live.front().valid());
    const Pixels first = frontPixels(live);
    TitleArtLiveState failing = state();
    failing.testFlags = TITLE_ART_TEST_FAIL_TARGETS;
    live.setState(failing, 2, {});
    runFrames(live, factory, UNLIMITED, 5);
    const TitleArtLiveStatus s = live.status();
    EXPECT_TRUE(s.failed);
    EXPECT_NE(s.error.find("out_of_target_memory"), std::string::npos) << s.error;
    EXPECT_EQ(live.front().hash, 1u) << "R19: the previous image stays";
    EXPECT_TRUE(identical(frontPixels(live), first));
}

TEST_F(TitleArtRuntimeGpu, ForcedRgba8) {
    auto renderer = makeRenderer();
    TestAssets assets;
    auto doc = twoShapes("#2040ff");
    const TitleJobFactory factory = makeRendererJobFactory(*renderer, &assets);
    TitleArtLive floatLive("assets/test.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    floatLive.setState(state(), 1, {});
    runFrames(floatLive, factory, UNLIMITED);
    TitleArtLive rgba8("assets/test.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    TitleArtLiveState forced = state();
    forced.testFlags = TITLE_ART_TEST_NO_FLOAT_TARGETS;
    rgba8.setState(forced, 2, {});
    runFrames(rgba8, factory, UNLIMITED);
    ASSERT_TRUE(floatLive.front().valid() && rgba8.front().valid());
    EXPECT_EQ(rgba8.status().pixelPath, "rgba8") << "R21: the path is readable";
    // S80 §6.3's tolerance of the RGBA8 path against the float one
    const Difference d = compare(frontPixels(floatLive), frontPixels(rgba8));
    EXPECT_LE(d.mean, 1.0);
    EXPECT_LE(d.max, 48.0);
    EXPECT_LE(d.overFraction, 0.005);
}
