// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scissor rectangle under MSAA, without the HTML UI renderer: a sokol pass into a
// multisampled colour attachment of an engine framebuffer (resolved at the end of the pass),
// an integer scissor rectangle, and one fullscreen triangle in opaque white. Every sample of a
// pixel outside the scissor rectangle must stay untouched, so after the resolve the rows and
// columns around the rectangle are exactly transparent. The draw uses the engine's HTML UI
// passthrough program (any program would do); pass, pipeline, scissor and draw are raw sokol
// calls. HTML UI clips overflow with this scissor (RmlRenderer::SetScissorRegion).
//
// Found (p3-layout): with 4 samples, Mesa's llvmpipe and lavapipe (Mesa 23.2.1, the test image's
// GL and Vulkan drivers) let 2 of the 4 samples of the pixels in the row above and in the column
// left of the rectangle through (alpha 128, the corner 64); the right and bottom edges and every
// pixel inside are exact. The scissor reaches glScissor / vkCmdSetScissor as the exact integers
// given here, so it is the drivers' rasterizer: the scissor test must keep or drop a pixel whole.
// FourSamples accepts exactly that pattern (and an exact result) and reports which it saw; the
// HTML UI page at the end shows the same pattern through RmlRenderer. The browser-parity fixtures
// that scroll content past a clip edge render with "samples": 1 for it.

#include "TestRenderCommon.h"
#include "harness/GpuEnvironment.h"
#include "harness/Readback.h"

#include <RmlUi/Core.h>

#include "pool/ShaderPool.h"
#include "render/FramebufferRender.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kSize = 32;
    // the scissor rectangle, top-left origin in image rows (row 0 is the image's first row)
    constexpr int kLeft = 8, kTop = 10, kWidth = 12, kHeight = 9;

    struct ScissorRun {
        bool ok = false;
        std::string error;
        std::vector<uint8_t> rgba; // rows top first
    };

    int alphaAt(const std::vector<uint8_t>& rgba, int x, int y){
        return rgba[(size_t(y) * kSize + x) * 4 + 3];
    }

    // Renders the fullscreen triangle into a sampleCount framebuffer through the scissor
    // rectangle, top-left origin as the HTML UI renderer gives it (CameraRender::applyScissorTopLeft:
    // sokol's origin_top_left is the backend's, GL's offscreen images are drawn Y-flipped).
    ScissorRun renderThroughScissor(int sampleCount){
        ScissorRun run;
        std::shared_ptr<ShaderRender> shader;
        const bool ready = GpuEnvironment::runFrames([&](){
            shader = ShaderPool::get(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH));
            return shader && shader->isCreated();
        });
        if (!ready){
            run.error = "the passthrough program did not compile";
            return run;
        }

        FramebufferDesc desc;
        desc.width = kSize;
        desc.height = kSize;
        desc.sampleCount = sampleCount;
        desc.depth = DepthStencilFormat::NONE;
        desc.minFilter = desc.magFilter = TextureFilter::NEAREST;
        desc.label = "msaa-scissor-test";
        FramebufferRender framebuffer;
        TextureRender white;
        const uint8_t whitePixel[4] = {255, 255, 255, 255};
        void* whiteData[1] = {const_cast<uint8_t*>(whitePixel)};
        size_t whiteSize[1] = {sizeof(whitePixel)};
        if (!framebuffer.createFramebuffer(desc) ||
            !white.createTexture("msaa-scissor-white", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, whiteData, whiteSize,
                                 TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
            run.error = "framebuffer or texture not created";
            return run;
        }
        SystemRender::commit();

        ShaderData& data = shader->shaderData;
        const int vsBlock = data.getUniformBlockIndexByName("u_vs_htmluiPost");
        const int fsBlock = data.getUniformBlockIndexByName("u_fs_htmluiPost");
        const std::pair<int, int> textureSlot = data.getTextureIndexByName("u_htmluiPostTex");

        sg_pipeline_desc pipelineDesc = {};
        pipelineDesc.shader = shader->backend.get();
        pipelineDesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
        pipelineDesc.sample_count = sampleCount;
        pipelineDesc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        pipelineDesc.depth.pixel_format = SG_PIXELFORMAT_NONE;
        pipelineDesc.cull_mode = SG_CULLMODE_NONE;
        pipelineDesc.label = "msaa-scissor-pipeline";
        const sg_pipeline pipeline = sg_make_pipeline(&pipelineDesc);
        if (sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID){
            run.error = "pipeline not valid";
            return run;
        }

        sg_pass pass = {};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = {0.0f, 0.0f, 0.0f, 0.0f};
        pass.attachments = framebuffer.backend.get(0);
        sg_begin_pass(&pass);
        const bool originTopLeft = SystemRender::getFeatures().originTopLeft;
        sg_apply_viewport(0, 0, kSize, kSize, originTopLeft);
        sg_apply_scissor_rect(kLeft, kTop, kWidth, kHeight, originTopLeft);
        sg_apply_pipeline(pipeline);
        sg_bindings bindings = {};
        bindings.views[textureSlot.first] = white.backend.getView();
        bindings.samplers[textureSlot.second] = white.backend.getSampler();
        sg_apply_bindings(&bindings);
        const float uvTransform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        sg_apply_uniforms(vsBlock, sg_range{uvTransform, sizeof(uvTransform)});
        float fsParams[32] = {};
        fsParams[0] = 1.0f; // opacity
        sg_apply_uniforms(fsBlock, sg_range{fsParams, sizeof(fsParams)});
        sg_draw(0, 3, 1);
        // sokol's GL backend resolves with glBlitFramebuffer, which honours the scissor box
        sg_apply_scissor_rect(0, 0, kSize, kSize, originTopLeft);
        sg_end_pass();
        SystemRender::commit();

        run.ok = readTexture(framebuffer.getColorTexture(), kSize, kSize, run.rgba);
        if (!run.ok)
            run.error = "readback failed";
        sg_destroy_pipeline(pipeline);
        white.destroyTexture();
        framebuffer.destroyFramebuffer();
        return run;
    }

    // what is drawn outside the scissor rectangle, and what is missing inside it
    std::vector<std::string> scissorLeaks(const std::vector<uint8_t>& rgba){
        std::vector<std::string> leaks;
        for (int y = 0; y < kSize; y++){
            for (int x = 0; x < kSize; x++){
                const bool inside = x >= kLeft && x < kLeft + kWidth && y >= kTop && y < kTop + kHeight;
                const int alpha = alphaAt(rgba, x, y);
                if (inside ? alpha != 255 : alpha != 0)
                    leaks.push_back("(" + std::to_string(x) + ", " + std::to_string(y) + ") alpha " + std::to_string(alpha) +
                                    (inside ? " inside" : " outside"));
            }
        }
        return leaks;
    }

    std::string describe(const std::vector<std::string>& leaks){
        std::string text = std::to_string(leaks.size()) + " pixels:";
        for (size_t i = 0; i < leaks.size() && i < 12; i++)
            text += " " + leaks[i];
        return text;
    }

    // The pixels that differ from an exact scissor, other than Mesa's pattern at the left and top
    // edges; `mesaPattern` tells whether that pattern was there.
    std::vector<std::string> unexpectedLeaks(const std::vector<uint8_t>& rgba, int size, int left, int top, int width, int height,
                                             bool& mesaPattern){
        auto alpha = [&](int x, int y){ return int(rgba[(size_t(y) * size + x) * 4 + 3]); };
        std::vector<std::string> leaks;
        int halfCovered = 0;
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                const bool inside = x >= left && x < left + width && y >= top && y < top + height;
                const bool topRow = (y == top - 1 && x >= left && x < left + width);
                const bool leftColumn = (x == left - 1 && y >= top && y < top + height);
                const bool corner = (x == left - 1 && y == top - 1);
                const int a = alpha(x, y);
                bool ok = inside ? a == 255 : a == 0;
                if (!ok && (topRow || leftColumn) && std::abs(a - 128) <= 1){
                    ok = true;
                    halfCovered++;
                }
                if (!ok && corner && std::abs(a - 64) <= 1)
                    ok = true;
                if (!ok)
                    leaks.push_back("(" + std::to_string(x) + ", " + std::to_string(y) + ") alpha " + std::to_string(a));
            }
        }
        mesaPattern = halfCovered > 0;
        if (mesaPattern && halfCovered != width + height)
            leaks.push_back("a partial pattern: " + std::to_string(halfCovered) + " of " + std::to_string(width + height) +
                            " edge pixels half covered");
        return leaks;
    }

}

TEST(MsaaScissor, OneSampleIsExact){
    JASPER_REQUIRE_GPU();
    const ScissorRun run = renderThroughScissor(1);
    ASSERT_TRUE(run.ok) << run.error;
    const std::vector<std::string> leaks = scissorLeaks(run.rgba);
    EXPECT_TRUE(leaks.empty()) << describe(leaks);
}

// 4 samples: exact, or Mesa's half-covered row above and column left (and nothing else)
TEST(MsaaScissor, FourSamples){
    JASPER_REQUIRE_GPU();
    const ScissorRun run = renderThroughScissor(4);
    ASSERT_TRUE(run.ok) << run.error;
    bool mesaPattern = false;
    const std::vector<std::string> leaks = unexpectedLeaks(run.rgba, kSize, kLeft, kTop, kWidth, kHeight, mesaPattern);
    std::cout << "[ MSAA     ] raw sokol, 4 samples: " << (mesaPattern ? "the row above and the column left of the scissor are half covered (Mesa)"
                                                                       : "exact") << "\n";
    EXPECT_TRUE(leaks.empty()) << describe(leaks);
}

// The same through the HTML UI renderer: overflow: hidden scrolled past its top edge
TEST(MsaaScissor, HtmlUIClipEdge){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        SCOPED_TRACE(samples);
        RenderTestContext render(64, 48, 64, 48, samples);
        ASSERT_TRUE(render.isValid());
        Rml::ElementDocument* document = render.loadFromMemory(R"(<rml><head><style>
body { width: 64px; height: 48px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 10px; top: 10px; width: 30px; height: 20px; overflow: hidden; }
#content { height: 200px; width: 100px; margin-left: -20px; background-color: #ffffff; }
</style></head><body><div id="clip"><div id="content"/></div></body></rml>)", "msaa-scissor");
        ASSERT_NE(document, nullptr);
        ASSERT_TRUE(render.renderUntilDrawn());
        document->GetElementById("clip")->SetScrollTop(30.0f);
        ASSERT_TRUE(render.renderFrame());
        std::vector<uint8_t> rgba;
        ASSERT_TRUE(render.readLayer(rgba));
        // (the 64 x 48 layer: compare in a 64-wide square window of its rows)
        rgba.resize(size_t(64) * 64 * 4, 0);
        bool mesaPattern = false;
        const std::vector<std::string> leaks = unexpectedLeaks(rgba, 64, 10, 10, 30, 20, mesaPattern);
        EXPECT_TRUE(leaks.empty()) << leaks.size() << " pixels, first " << (leaks.empty() ? "" : leaks[0]);
        if (samples == 1)
            EXPECT_FALSE(mesaPattern) << "without MSAA the clip edge is exact";
        std::cout << "[ MSAA     ] HTML UI, " << samples << " sample(s): " << (mesaPattern ? "Mesa's half-covered edges" : "exact") << "\n";
    }
}
