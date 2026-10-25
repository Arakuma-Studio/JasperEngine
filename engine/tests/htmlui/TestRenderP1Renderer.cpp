// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Render contracts: MSAA resolve/shared depth, sample selection, compositing,
// steady-state allocation, resource release and straight-alpha output.

#include "TestRenderCommon.h"
#include "harness/GpuAllocations.h"

#include "HtmlUIContext.h"

#include "pool/ShaderPool.h"
#include "render/PipelineState.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(RenderP1Test, FramebufferMsaaResolveAndSharedDepthStencil){
    JASPER_REQUIRE_GPU();
    const RenderFeatures& features = SystemRender::getFeatures();
    EXPECT_EQ(features.originTopLeft, !Engine::isOpenGL());
    ASSERT_TRUE(features.msaaRGBA8 && features.msaaDepthStencil) << "the test devices multisample";

    FramebufferDesc desc;
    desc.width = 64;
    desc.height = 32;
    desc.sampleCount = 4;
    desc.label = "test-msaa";
    Framebuffer first;
    ASSERT_TRUE(first.create(desc));
    FramebufferRender& render = first.getRender();
    EXPECT_EQ(render.getSampleCount(), 4);
    // the sampled texture is the 1-sample resolve image
    const sg_image resolve = render.getColorTexture().backend.get();
    EXPECT_EQ(sg_query_image_desc(resolve).sample_count, 1);
    EXPECT_TRUE(sg_query_image_desc(resolve).usage.resolve_attachment);
    sg_attachments attachments = render.backend.get(0);
    EXPECT_NE(attachments.resolves[0].id, SG_INVALID_ID);
    EXPECT_NE(attachments.depth_stencil.id, SG_INVALID_ID);
    EXPECT_EQ(sg_query_image_desc(sg_query_view_image(attachments.colors[0])).sample_count, 4);
    EXPECT_EQ(sg_query_image_desc(sg_query_view_image(attachments.depth_stencil)).pixel_format, SG_PIXELFORMAT_DEPTH_STENCIL);

    // a second one borrows the first one's depth-stencil attachment
    FramebufferDesc shared = desc;
    shared.shareDepthStencil = &render;
    Framebuffer second;
    ASSERT_TRUE(second.create(shared));
    EXPECT_EQ(second.getRender().backend.get(0).depth_stencil.id, attachments.depth_stencil.id);
    // a different sample count cannot borrow it
    TestSystem::get().clearLogs();
    FramebufferDesc mismatch = shared;
    mismatch.sampleCount = 1;
    Framebuffer third;
    EXPECT_FALSE(third.create(mismatch));
    // destroying the borrower keeps the owner's attachment
    second.destroy();
    EXPECT_TRUE(first.isCreated());
    first.destroy();

    // one sample: the colour attachment is the sampled texture, no resolve
    FramebufferDesc single = desc;
    single.sampleCount = 1;
    single.depth = DepthStencilFormat::NONE;
    Framebuffer plain;
    ASSERT_TRUE(plain.create(single));
    EXPECT_EQ(plain.getRender().backend.get(0).resolves[0].id, SG_INVALID_ID);
    EXPECT_EQ(plain.getRender().backend.get(0).depth_stencil.id, SG_INVALID_ID);
    EXPECT_EQ(plain.getVersion(), 1u);
    plain.destroy();
}

TEST(RenderP1Test, SampleCountChoice){
    JASPER_REQUIRE_GPU();
    const int initial = UIDocumentSystem::getPreferredSampleCount();
    EXPECT_EQ(initial, 4);
    HtmlUIContext context("sample-count");
    EXPECT_EQ(context.getSampleCount(), 4);

    UIDocumentSystem::setPreferredSampleCount(1);
    EXPECT_EQ(context.getSampleCount(), 1);
    UIDocumentSystem::setPreferredSampleCount(2);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 2);
    // not 1/2/4/8: the nearest below, logged
    TestSystem::get().clearLogs();
    UIDocumentSystem::setPreferredSampleCount(6);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 4);
    UIDocumentSystem::setPreferredSampleCount(16);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 8);
    UIDocumentSystem::setPreferredSampleCount(0);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] MSAA sample count"), 3u);

    UIDocumentSystem::setPreferredSampleCount(initial);
}

// The composite inside the post-processed camera's multisampled scene target (the shader
// library's MSAA, CameraRender::setMultisample): PIP_MSAA_NODEPTH through the pipeline cache at
// the target's sample count draws the same pixels as the one-sample composite
TEST(RenderP1Test, CompositeIntoMultisampledSceneTarget){
    JASPER_REQUIRE_GPU();
    const int width = 160, height = 90;
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    const std::string rml = "<html><head><style>body { width: 100%; height: 100%; }"
                            " #a { position: absolute; left: 10px; top: 8px; width: 70px; height: 40px; background-color: #e04030;"
                            " border-radius: 12px; }"
                            " #b { position: absolute; left: 60px; top: 30px; width: 80px; height: 50px; background-color: #2080ffa0; }"
                            "</style></head><body><div id=\"a\"/><div id=\"b\"/></body></html>";
    ASSERT_NE(test.loadFromMemory(rml, "p1/msaa-scene-composite.html"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    std::vector<uint8_t> single;
    ASSERT_TRUE(test.readScreen(single));

    const int samples = FramebufferRender::getMaxMsaaSamples(4);
    ASSERT_GT(samples, 1) << "the test devices multisample";
    FramebufferRender msaa;
    ASSERT_TRUE(msaa.createMultisampleFramebuffer(width, height, samples));
    FramebufferRender scene;
    ASSERT_TRUE(scene.createFramebuffer(TextureType::TEXTURE_2D, width, height, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                        TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false));

    bool drawn = false;
    for (int frame = 0; frame < 2; frame++){
        CameraRender pass;
        const float bg = RenderTestContext::background / 255.0f;
        pass.setClearColor(Vector4(bg, bg, bg, 1.0f));
        pass.setMultisample(&msaa);
        pass.startRenderPass(&scene);
        const Rect full(0, 0, (float)width, (float)height);
        pass.applyViewport(full);
        pass.applyScissor(full);
        // an offscreen camera pass renders Y-flipped on GL (RenderSystem::isRenderingFlipped)
        const Rml::Matrix4f clipFromQuad = Engine::isOpenGL() ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity();
        drawn = test.renderer.drawComposite(test.target.getComposited()->getColorTexture(), PIP_MSAA_NODEPTH, clipFromQuad, samples);
        pass.endRenderPass();
        SystemRender::commit();
        EXPECT_TRUE(drawn) << "frame " << frame;
    }

    std::vector<uint8_t> resolved;
    ASSERT_TRUE(readTexture(scene.getColorTexture(), width, height, resolved));
    ASSERT_EQ(resolved.size(), single.size());
    int worst = 0;
    for (size_t i = 0; i < resolved.size(); i++)
        worst = std::max(worst, std::abs((int)resolved[i] - (int)single[i]));
    EXPECT_LE(worst, 1) << "the resolved composite differs from the one-sample one";

    msaa.destroyFramebuffer();
    scene.destroyFramebuffer();
}

TEST(RenderP1Test, SteadyStateNoAllocations){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    GpuAllocations allocations;
    // filters, a backdrop filter, box shadows, a mask and gradients
    ASSERT_NE(test.load("p1/filter_chain.rml"), nullptr);
    ASSERT_NE(test.load("p1/box_shadow.rml"), nullptr);
    ASSERT_NE(test.load("p1/backdrop_filter.rml"), nullptr);
    ASSERT_NE(test.load("p1/mask_image.rml"), nullptr);
    ASSERT_NE(test.load("p1/gradients_linear.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());

    const size_t buffersAndTextures = allocations.created();
    ASSERT_GT(buffersAndTextures, 0u);
    const uint64_t generation = test.target.getGeneration();
    const size_t pipelines = SystemRender::getPipelineCacheSize();
    for (int frame = 0; frame < 2; frame++){
        ASSERT_TRUE(test.renderFrame());
        EXPECT_EQ(test.renderer.getStats().pipelineCacheMisses, 0u);
    }
    EXPECT_EQ(allocations.created(), buffersAndTextures);
    EXPECT_EQ(test.target.getGeneration(), generation);
    EXPECT_EQ(SystemRender::getPipelineCacheSize(), pipelines);
}

TEST(RenderP1Test, UnloadReleasesEverything){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    ASSERT_TRUE(test.renderUntilDrawn());
    const size_t initialTextures = test.renderer.getLiveTextureCount();

    size_t pipelinesAfterFirst = 0;
    for (int cycle = 0; cycle < 2; cycle++){
        Rml::ElementDocument* a = test.load("p1/box_shadow.rml");
        Rml::ElementDocument* b = test.load("p1/mask_image.rml");
        Rml::ElementDocument* c = test.load("p1/filter_drop_shadow.rml");
        ASSERT_TRUE(a && b && c);
        ASSERT_TRUE(test.renderFrame());
        EXPECT_GT(test.renderer.getStats().geometryLive, 0u);
        test.context->UnloadDocument(a);
        test.context->UnloadDocument(b);
        test.context->UnloadDocument(c);
        test.context->Update();
        // file textures stay in RmlUi's cache until released
        Rml::ReleaseTextures(&test.renderer);
        ASSERT_TRUE(test.renderFrame());

        EXPECT_EQ(test.renderer.getLiveGeometryCount(), 0u) << "cycle " << cycle;
        EXPECT_EQ(test.renderer.getStats().geometryLive, 0u);
        EXPECT_EQ(test.renderer.getStats().geometryBytes, 0u);
        EXPECT_EQ(test.renderer.getLiveTextureCount(), initialTextures) << "cycle " << cycle;
        if (cycle == 0)
            pipelinesAfterFirst = SystemRender::getPipelineCacheSize();
        else
            EXPECT_EQ(SystemRender::getPipelineCacheSize(), pipelinesAfterFirst);
        if (::testing::Test::HasFailure())
            break;
    }
}

TEST(RenderP1Test, UnpremultipliedCopy){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(64, 64, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    const std::string rml = "<html><head><style>body { position: absolute; left: 0px; top: 0px; width: 64px; height: 64px; }"
                            " div { position: absolute; display: block; width: 32px; height: 32px; }"
                            " #red { left: 0px; top: 0px; background-color: #ff0000; }"
                            " #white { left: 32px; top: 32px; background-color: #ffffff80; }</style></head>"
                            "<body><div id=\"red\"/><div id=\"white\"/></body></html>";
    ASSERT_NE(test.loadFromMemory(rml, "p1/unpremultiply.html"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());

    FramebufferDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.depth = DepthStencilFormat::NONE;
    Framebuffer straight;
    ASSERT_TRUE(straight.create(desc));
    ASSERT_TRUE(test.renderer.drawUnpremultiplied(test.target, straight.getRender(), 64, 64));
    SystemRender::commit();

    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(straight.getRender().getColorTexture(), 64, 64, rgba));
    PixelProbe probe{rgba, 64};
    // top-left origin: the red square at texel (0, 0)
    EXPECT_TRUE(probe.is(0, 0, 255, 0, 0, 255, 1));
    // 50 % white comes back straight
    EXPECT_TRUE(probe.is(48, 48, 255, 255, 255, 128, 1));
    // nothing: transparent
    EXPECT_TRUE(probe.is(48, 8, 0, 0, 0, 0, 0));
    straight.destroy();
}
