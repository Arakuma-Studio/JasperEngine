// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Render targets created while a pass is open. The HTML UI renderer creates a layer the first
// time a frame pushes it (a filter) and its post-process targets the first time a filter runs,
// in the middle of the document layer's pass. On Vulkan a new framebuffer gets its contents in
// a pass of its own (SokolFramebuffer::initializeAttachments): begun inside the open one, it
// nested two dynamic rendering instances, and NVIDIA's driver crashed in the open pass's
// vkCmdEndRendering (AssertionsTest.Visible). That pass now runs after the open one ends, and
// it keeps a depth-stencil the new layer borrows from layer 0, which holds the clip mask.

#include "TestRenderCommon.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // a rounded overflow clip (a stencil clip mask in layer 0's depth-stencil) around a strip,
    // a blurred strip (a layer and post-process targets, created by the first drawn frame in
    // the middle of layer 0's pass) and a strip drawn after it through the same clip mask
    const char* const kDocument = R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 8px; top: 8px; width: 60px; height: 48px; border-radius: 16px; overflow: hidden; }
.strip { width: 80px; height: 16px; background-color: #e04020; }
.blurred { width: 80px; height: 16px; background-color: #2040e0; filter: blur(2px); }
</style></head><body><div id="clip"><div class="strip"/><div class="blurred"/><div class="strip"/></div></body></rml>)";

    // pixels whose red channel is over half (the two strips)
    size_t redPixels(const std::vector<uint8_t>& rgba){
        size_t count = 0;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            count += rgba[i] > 128;
        return count;
    }

    size_t differingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size())
            return a.size() > b.size() ? a.size() : b.size();
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.size(); i += 4){
            if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
                count++;
        }
        return count;
    }

}

// No pass begins inside another, and the frame that creates the targets draws what the next
// frames draw (the clip mask survives the new layer's initialisation)
TEST(RenderPassNesting, TargetsCreatedWhileDrawing){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        SCOPED_TRACE(samples);
        const uint64_t nestedBefore = SystemRender::getNestedPassCount();
        RenderTestContext render(96, 64, 96, 64, samples);
        ASSERT_TRUE(render.isValid());
        ASSERT_NE(render.loadFromMemory(kDocument, "pass-nesting"), nullptr);

        // the first drawn frame creates layer 1 and the post-process targets
        bool drawn = false;
        for (int i = 0; i < 30 && !drawn; i++)
            drawn = render.renderFrame();
        ASSERT_TRUE(drawn);
        std::vector<uint8_t> first;
        ASSERT_TRUE(render.readLayer(first));
        EXPECT_EQ(SystemRender::getNestedPassCount(), nestedBefore) << "a render pass began inside another";

        ASSERT_TRUE(render.renderFrame());
        std::vector<uint8_t> second;
        ASSERT_TRUE(render.readLayer(second));
        EXPECT_EQ(differingPixels(first, second), 0u) << "the frame that created the targets drew differently";

        // both strips drawn, rounded by the clip: the corners outside it stay empty (the second
        // strip is drawn after the layer was created, through the clip mask it shares)
        const size_t red = redPixels(second);
        EXPECT_GT(red, size_t(2 * 60 * 16 - 2 * 200)) << "a strip is missing";
        EXPECT_LT(red, size_t(2 * 60 * 16)) << "the strips are not rounded";
        const auto alphaAt = [&second](int x, int y){ return int(second[(size_t(y) * 96 + x) * 4 + 3]); };
        EXPECT_EQ(alphaAt(8, 8), 0) << "the top-left corner of the rounded clip";
        EXPECT_EQ(alphaAt(8, 55), 0) << "the bottom-left corner of the rounded clip";
        EXPECT_GT(alphaAt(30, 50), 0) << "the second strip";
        EXPECT_EQ(SystemRender::getNestedPassCount(), nestedBefore);
    }
}
