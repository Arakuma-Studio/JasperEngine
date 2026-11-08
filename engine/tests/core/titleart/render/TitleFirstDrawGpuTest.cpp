// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TitleFirstDraws (TitleGpu.h): the first draw of a program in a pipeline state makes the driver
// compile its variant on the render thread (llvmpipe's LLVM JIT took 4-5 s per title art program
// here: the "editor not responding" of the Title Designer and of MCP renders). A render meeting
// several new programs lets one draw a frame and runs the rest on later frames, so no frame holds
// more than one of those compiles; the image is the same. Red control: unbounded, one frame lets
// every new program through.

#include "titleart/render/TitleRenderTestSupport.h"

#include "titleart/support/TitleStandardTitle.h"

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    Pixels renderWithEffects() {
        static TestAssetResolver assets;
        TitleArtRenderer renderer;
        renderer.setContentProvider(makeGeometryTitleContentProvider());
        RenderRequest r;
        r.doc = std::make_shared<const TitleArtDoc>(standardTitleDocument(
            {standardEffect("00000201", "outer_glow"), standardEffect("00000202", "drop_shadow"), standardEffect("00000203", "inner_glow"),
             standardEffect("00000204", "outline")}));
        r.pxScale = 0.5f;
        r.assets = &assets;
        return render(renderer, r);
    }

    class TitleFirstDrawGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
        void TearDown() override { TitleFirstDraws::resetForTesting(false); }
    };

}

TEST_F(TitleFirstDrawGpu, NewProgramsDrawOneAFrame) {
    TitleFirstDraws::resetForTesting(false);
    const Pixels spread = renderWithEffects();
    const TitleFirstDrawStats bounded = TitleFirstDraws::stats();
    EXPECT_GT(bounded.granted, 3u) << "the render met new programs";
    EXPECT_EQ(bounded.maxPerFrame, 1u) << "one new program's first draw a frame";
    EXPECT_GT(bounded.deferred, 0u) << "the others waited for later frames";
    // red control: unbounded, the first frame draws every new program
    TitleFirstDraws::resetForTesting(true);
    const Pixels together = renderWithEffects();
    EXPECT_GT(TitleFirstDraws::stats().maxPerFrame, 1u);
    // the same image either way
    const Difference d = compare(spread, together, 2.0);
    EXPECT_LE(d.mean, 0.5);
    EXPECT_LE(d.overFraction, 0.001);
}
