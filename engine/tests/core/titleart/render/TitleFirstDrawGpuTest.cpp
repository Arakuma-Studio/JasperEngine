// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TitleFirstDraws (TitleGpu.h): the first draw of a program in a pipeline state makes the driver
// compile its variant on the render thread (llvmpipe's LLVM JIT: the "editor not responding" of
// the Title Designer and of MCP renders). A render meeting several new programs lets one draw a
// frame and runs the rest on later frames, so no frame holds more than one of those compiles (timed:
// the longest frame is the slowest pass and little more; on a GPU, under the editor's hang
// threshold); the image is the same. Red control: unbounded, one frame lets every new program
// through. A frame also ends at a main-loop
// heartbeat: the editor commits only when a view redraws, and a render waiting for commits alone
// waited forever (an MCP render stuck at "rendering main"; red control: no heartbeat, no grant).

#include "titleart/render/TitleRenderTestSupport.h"

#include "titleart/support/TitleStandardTitle.h"

#include "diag/HangWatchdog.h"

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    // what a frame of this render spends besides its one compile (its cheap passes), at most
    constexpr double ONE_FRAME_REST_MS = 1000.0;

    struct TimedRender {
        Pixels image;
        double longestFrameMs = 0.0;
        int frames = 0;
        TitleGpuStats gpu;
    };

    // the render frame by frame as the editor pumps it, timing each frame's step
    TimedRender renderWithEffects() {
        static TestAssetResolver assets;
        TitleArtRenderer renderer;
        renderer.setContentProvider(makeGeometryTitleContentProvider());
        RenderRequest r;
        r.doc = std::make_shared<const TitleArtDoc>(standardTitleDocument(
            {standardEffect("00000201", "outer_glow"), standardEffect("00000202", "drop_shadow"), standardEffect("00000203", "inner_glow"),
             standardEffect("00000204", "outline")}));
        r.pxScale = 0.5f;
        r.assets = &assets;
        TimedRender out;
        std::unique_ptr<TitleRenderJob> job = renderer.begin(r);
        if (!job) {
            ADD_FAILURE() << "no job";
            return out;
        }
        const auto start = std::chrono::steady_clock::now();
        bool done = false;
        while (!done && !job->failed() && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            const auto frame = std::chrono::steady_clock::now();
            done = job->step(StepBudget{});
            out.longestFrameMs = std::max(out.longestFrameMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame).count());
            SystemRender::commit();
            out.frames++;
        }
        if (!done) ADD_FAILURE() << "the render did not finish: " << job->error();
        else out.image = readImage(job->image());
        out.gpu = renderer.gpuStats();
        return out;
    }

    class TitleFirstDrawGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
        void TearDown() override { TitleFirstDraws::resetForTesting(false); }
    };

}

TEST(TitleFirstDraws, AHeartbeatEndsTheFrame) {
    TitleFirstDraws::resetForTesting(false);
    EXPECT_TRUE(TitleFirstDraws::allow(0x51ull)) << "the frame's first new key goes";
    EXPECT_FALSE(TitleFirstDraws::allow(0x52ull)) << "a second one waits";
    EXPECT_TRUE(TitleFirstDraws::allow(0x51ull)) << "a key that drew before always goes";
    // the editor's main loop beats without committing (no view redrew): the next frame
    diag::heartbeat();
    EXPECT_TRUE(TitleFirstDraws::allow(0x52ull)) << "a heartbeat starts a frame";
    EXPECT_FALSE(TitleFirstDraws::allow(0x53ull));
    // a commit starts one as well (games, tests)
    SystemRender::commit();
    EXPECT_TRUE(TitleFirstDraws::allow(0x53ull));
    TitleFirstDraws::resetForTesting(false);
}

// A synchronous render (the editor's view_asset and bake step one job to its end, no frame in
// between) lets every new key through while its scope lives; the frame rule is back after it.
// Red control: without the scope view_asset waited 60 s for a frame and failed
// (test_title_art.py::test_asset_info_and_view on the wave-2 merge).
TEST(TitleFirstDraws, ASynchronousRenderIsNotSpread) {
    TitleFirstDraws::resetForTesting(false);
    EXPECT_TRUE(TitleFirstDraws::allow(0x61ull));
    {
        TitleFirstDraws::SynchronousScope synchronous;
        EXPECT_TRUE(TitleFirstDraws::allow(0x62ull)) << "a second new key in the same frame goes";
        EXPECT_TRUE(TitleFirstDraws::allow(0x63ull));
        EXPECT_TRUE(TitleFirstDraws::available());
    }
    EXPECT_FALSE(TitleFirstDraws::allow(0x64ull)) << "after the scope a new key waits for the next frame again";
    TitleFirstDraws::resetForTesting(false);
}

TEST_F(TitleFirstDrawGpu, NewProgramsDrawOneAFrame) {
    TitleFirstDraws::resetForTesting(false);
    const TimedRender spread = renderWithEffects();
    const TitleFirstDrawStats bounded = TitleFirstDraws::stats();
    EXPECT_GT(bounded.granted, 3u) << "the render met new programs";
    EXPECT_EQ(bounded.maxPerFrame, 1u) << "one new program's first draw a frame";
    EXPECT_GT(bounded.deferred, 0u) << "the others waited for later frames";
    // a frame holds at most one compile: the longest is the slowest pass and the frame's cheap
    // rest. On a GPU (milliseconds a compile) no frame reaches the editor's hang threshold; on a
    // software rasteriser one compile is up to about 1 s (an ELEMENT variant, TitleElementVariant.h;
    // the whole program was 4.3-5.3 s), timed under the box's load, so not enforced there
    const auto describe = [](const TimedRender& r) {
        return std::string("longest frame ") + std::to_string(r.longestFrameMs) + " ms of " + std::to_string(r.frames) + "; the slowest pass " +
               titleRenderProgramName(r.gpu.slowestProgram) + " (" + (r.gpu.slowestWasFirstDraw ? "a first draw" : "drawn before") + ") " +
               std::to_string(r.gpu.slowestPassMs) + " ms";
    };
    ::testing::Test::RecordProperty("bounded", describe(spread));
    EXPECT_LE(spread.longestFrameMs, spread.gpu.slowestPassMs + ONE_FRAME_REST_MS) << describe(spread);
    const std::string device = SystemRender::getDeviceName();
    const bool software = device.find("llvmpipe") != std::string::npos || device.find("softpipe") != std::string::npos ||
                          device.find("SwiftShader") != std::string::npos || device.find("Software Rasterizer") != std::string::npos;
    if (!software) { EXPECT_LT(spread.longestFrameMs, (double)diag::WatchdogConfig().thresholdMs) << describe(spread); }
    // red control: unbounded, the first frame draws every new program
    TitleFirstDraws::resetForTesting(true);
    const TimedRender together = renderWithEffects();
    EXPECT_GT(TitleFirstDraws::stats().maxPerFrame, 1u);
    // (timed, recorded only: on llvmpipe one compile is most of either longest frame)
    ::testing::Test::RecordProperty("unbounded", describe(together));
    // the same image either way
    const Difference d = compare(spread.image, together.image, 2.0);
    EXPECT_LE(d.mean, 0.5);
    EXPECT_LE(d.overFraction, 0.001);
}
