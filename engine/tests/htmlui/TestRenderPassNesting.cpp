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

    // a rounded overflow clip (a stencil clip mask in layer 0's depth-stencil) around blurred
    // and shadowed content (a layer and post-process targets, created by the first drawn frame)
    const char* const kDocument = R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 8px; top: 8px; width: 60px; height: 40px; border-radius: 16px; overflow: hidden; }
#content { width: 80px; height: 50px; margin-left: -6px; background-color: #e04020; filter: blur(2px) drop-shadow(#0000ff 3px 3px 2px); }
#plain { position: absolute; left: 76px; top: 8px; width: 12px; height: 40px; background-color: #20c040; }
</style></head><body><div id="clip"><div id="content"/></div><div id="plain"/></body></rml>)";

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

        // something was drawn, and the clip held: the rounded corner outside it stays empty
        size_t covered = 0;
        for (size_t i = 3; i < second.size(); i += 4)
            covered += second[i] != 0;
        EXPECT_GT(covered, 1000u);
        const size_t corner = (size_t(8) * 96 + 8) * 4;
        EXPECT_EQ(int(second[corner + 3]), 0) << "the top-left corner of the rounded clip";
        EXPECT_EQ(SystemRender::getNestedPassCount(), nestedBefore);
    }
}
