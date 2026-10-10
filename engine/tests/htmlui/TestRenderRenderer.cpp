// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlRenderer: scissor conversion, geometry and texture lifetimes, deferred releases, pooled
// geometry buffers, waiting for shaders, GPU objects after a context is destroyed and after
// view loss.

#include "TestRenderCommon.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    ::testing::AssertionResult rectIs(const Rect& rect, float x, float y, float w, float h){
        if (rect.getX() == x && rect.getY() == y && rect.getWidth() == w && rect.getHeight() == h)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << "rect (" << rect.getX() << ", " << rect.getY() << ", " << rect.getWidth() << ", "
                                             << rect.getHeight() << "), expected (" << x << ", " << y << ", " << w << ", " << h << ")";
    }

    Rml::Rectanglei region(int left, int top, int right, int bottom){
        return Rml::Rectanglei::FromCorners(Rml::Vector2i(left, top), Rml::Vector2i(right, bottom));
    }

    // a quad at (x, y) document px; the mesh stays alive with the struct, as RmlUi keeps it
    struct Quad {
        Rml::Mesh mesh;
        Rml::CompiledGeometryHandle handle = 0;

        Quad(RmlRenderer& renderer, float x, float y, float w, float h, Rml::ColourbPremultiplied colour = Rml::ColourbPremultiplied(255, 255, 255, 255)){
            Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour, Rml::Vector2f(0, 0), Rml::Vector2f(1, 1));
            handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
        }
    };

    // draws one textured quad per frame at the texture's own size, pixel-aligned, so the
    // layer holds the texture's premultiplied texels exactly
    bool drawTexture(RenderTestContext& test, Quad& quad, Rml::TextureHandle texture){
        for (int i = 0; i < 30; i++){
            beginTestFrame();
            test.renderer.beginFrame(test.target, test.contextSize);
            test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(0, 0), texture);
            const bool drawn = test.renderer.endFrame();
            SystemRender::commit();
            if (drawn)
                return true;
        }
        return false;
    }

    bool isBufferValid(uint32_t id){
        sg_buffer buffer = {id};
        return sg_query_buffer_state(buffer) == SG_RESOURCESTATE_VALID;
    }

    bool isImageValid(uint32_t id){
        sg_image image = {id};
        return sg_query_image_state(image) == SG_RESOURCESTATE_VALID;
    }

}

TEST(RmlRendererTest, ScissorConversion){
    const Rml::Vector2i context(640, 360);

    // identity, bottom-left origin unless GL renders offscreen flipped
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(10, 20, 110, 70), context, context, false), 10, 290, 100, 50));
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(10, 20, 110, 70), context, context, true), 10, 20, 100, 50));

    // a 2x target: document px scale to target px
    const Rml::Vector2i doubled(1280, 720);
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(10, 20, 110, 70), context, doubled, false), 20, 580, 200, 100));
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(10, 20, 110, 70), context, doubled, true), 20, 40, 200, 100));

    // fractional scale: outward rounding (floor of the start, ceil of the end)
    const Rml::Vector2i scaled(960, 540);   // 1.5x
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(1, 1, 3, 3), context, scaled, true), 1, 1, 4, 4));

    // clamped to the target
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(-20, -10, 700, 400), context, context, false), 0, 0, 640, 360));
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(600, 300, 700, 400), context, context, false), 600, 0, 40, 60));
    EXPECT_TRUE(rectIs(RmlRenderer::toTargetScissor(region(600, 300, 700, 400), context, context, true), 600, 300, 40, 60));

    // empty: zero width or height (the replay skips such draws)
    Rect empty = RmlRenderer::toTargetScissor(region(50, 50, 50, 80), context, context, false);
    EXPECT_EQ(empty.getWidth(), 0.0f);
    empty = RmlRenderer::toTargetScissor(region(700, 10, 800, 20), context, context, true);
    EXPECT_EQ(empty.getWidth(), 0.0f);
    empty = RmlRenderer::toTargetScissor(region(10, 400, 20, 500), context, context, false);
    EXPECT_EQ(empty.getHeight(), 0.0f);
}

TEST(RmlRendererTest, GeometryLifetime){
    RmlRenderer renderer;
    EXPECT_EQ(renderer.getLiveGeometryCount(), 0u);

    Quad a(renderer, 0, 0, 10, 10);
    Quad b(renderer, 0, 0, 10, 10);
    Quad c(renderer, 0, 0, 10, 10);
    EXPECT_EQ(renderer.getLiveGeometryCount(), 3u);
    EXPECT_NE(a.handle, b.handle);

    renderer.ReleaseGeometry(b.handle);
    EXPECT_EQ(renderer.getLiveGeometryCount(), 2u);
    // a handle released twice (or unknown) is ignored
    renderer.ReleaseGeometry(b.handle);
    renderer.ReleaseGeometry(0);
    EXPECT_EQ(renderer.getLiveGeometryCount(), 2u);

    renderer.ReleaseGeometry(a.handle);
    renderer.ReleaseGeometry(c.handle);
    EXPECT_EQ(renderer.getLiveGeometryCount(), 0u);
}

TEST(RmlRendererTest, GeometryLifetimeThroughContext){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    Rml::ElementDocument* document = test.load("render/boxes.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    EXPECT_GT(test.renderer.getLiveGeometryCount(), 0u);
    EXPECT_GT(test.renderer.getLastFrameDrawCount(), 0u);

    // unloading the document gives every geometry back
    test.context->UnloadDocument(document);
    test.context->Update();
    EXPECT_EQ(test.renderer.getLiveGeometryCount(), 0u);
    ASSERT_TRUE(test.renderFrame());
    EXPECT_EQ(test.renderer.getLastFrameDrawCount(), 0u);
}

TEST(RmlRendererTest, RenderOutsideFrameIgnored){
    TestSystem::get().clearLogs();

    RmlRenderer renderer;
    Quad quad(renderer, 0, 0, 10, 10);
    renderer.RenderGeometry(quad.handle, Rml::Vector2f(0, 0), 0);
    renderer.RenderGeometry(quad.handle, Rml::Vector2f(0, 0), 0);

    // logged once per renderer, nothing recorded
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] geometry rendered outside a frame"), 1u);
    EXPECT_EQ(renderer.getLastFrameDrawCount(), 0u);
    renderer.ReleaseGeometry(quad.handle);
}

TEST(RmlRendererTest, LoadTexturePremultiplies){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(32, 32);
    ASSERT_TRUE(test.isValid());

    Rml::Vector2i dimensions(0, 0);
    Rml::TextureHandle texture = test.renderer.LoadTexture(dimensions, "common/alpha50.png");
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(dimensions, Rml::Vector2i(32, 32));
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 1u);
    ASSERT_NE(test.renderer.getTextureRender(texture), nullptr);

    // (255, 0, 0, 128) straight alpha is (128, 0, 0, 128) premultiplied
    Quad quad(test.renderer, 0, 0, 32, 32);
    ASSERT_TRUE(drawTexture(test, quad, texture));
    std::vector<uint8_t> layer;
    ASSERT_TRUE(test.readLayer(layer));
    PixelProbe probe{layer, 32};
    for (int y = 0; y < 32; y += 7){
        for (int x = 0; x < 32; x += 7){
            EXPECT_TRUE(probe.is(x, y, 128, 0, 0, 128, 1));
        }
    }

    test.renderer.ReleaseGeometry(quad.handle);
    test.renderer.ReleaseTexture(texture);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
}

TEST(RmlRendererTest, LoadTextureMissingReturnsZero){
    RmlRenderer renderer;
    TestSystem::get().clearLogs();

    Rml::Vector2i dimensions(7, 7);
    EXPECT_EQ(renderer.LoadTexture(dimensions, "common/does-not-exist.png"), 0u);
    EXPECT_EQ(dimensions, Rml::Vector2i(7, 7));
    EXPECT_EQ(renderer.getLiveTextureCount(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] could not load image 'common/does-not-exist.png'"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 1u);

    // not an image at all
    TestSystem::get().clearLogs();
    EXPECT_EQ(renderer.LoadTexture(dimensions, "common/base.css"), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] could not load image 'common/base.css'"), 1u);
    EXPECT_EQ(renderer.LoadTexture(dimensions, ""), 0u);
}

TEST(RmlRendererTest, LoadTextureSvg){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(24, 24);
    ASSERT_TRUE(test.isValid());

    Rml::Vector2i dimensions(0, 0);
    Rml::TextureHandle texture = test.renderer.LoadTexture(dimensions, "common/icon.svg");
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(dimensions, Rml::Vector2i(24, 24));

    Quad quad(test.renderer, 0, 0, 24, 24);
    ASSERT_TRUE(drawTexture(test, quad, texture));
    std::vector<uint8_t> layer;
    ASSERT_TRUE(test.readLayer(layer));
    PixelProbe probe{layer, 24};
    // white circle in the middle, #2060c0 rounded square around it, transparent corners
    EXPECT_TRUE(probe.is(12, 12, 255, 255, 255, 255, 3));
    EXPECT_TRUE(probe.is(4, 12, 0x20, 0x60, 0xc0, 255, 3));
    EXPECT_TRUE(probe.is(12, 20, 0x20, 0x60, 0xc0, 255, 3));
    EXPECT_TRUE(probe.is(0, 0, 0, 0, 0, 0, 1));
    EXPECT_TRUE(probe.is(23, 23, 0, 0, 0, 0, 1));

    test.renderer.ReleaseGeometry(quad.handle);
    test.renderer.ReleaseTexture(texture);
}

TEST(RmlRendererTest, GenerateTexture){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(4, 2);
    ASSERT_TRUE(test.isValid());

    const Rml::byte pixels[4 * 2 * 4] = {
        255, 0, 0, 255,    0, 255, 0, 255,    0, 0, 255, 255,    128, 128, 128, 128,
        0, 0, 0, 0,        64, 32, 16, 64,    255, 255, 255, 255, 10, 20, 30, 255,
    };
    Rml::TextureHandle texture = test.renderer.GenerateTexture(Rml::Span<const Rml::byte>(pixels, sizeof(pixels)), Rml::Vector2i(4, 2));
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 1u);

    Quad quad(test.renderer, 0, 0, 4, 2);
    ASSERT_TRUE(drawTexture(test, quad, texture));
    std::vector<uint8_t> layer;
    ASSERT_TRUE(test.readLayer(layer));
    // drawn as given: already premultiplied, row 0 at the top
    for (size_t i = 0; i < sizeof(pixels); i++){
        EXPECT_NEAR(layer[i], pixels[i], 1) << "byte " << i;
    }

    // a span that does not match the size is refused
    EXPECT_EQ(test.renderer.GenerateTexture(Rml::Span<const Rml::byte>(pixels, 12), Rml::Vector2i(4, 2)), 0u);
    EXPECT_EQ(test.renderer.GenerateTexture(Rml::Span<const Rml::byte>(pixels, sizeof(pixels)), Rml::Vector2i(0, 2)), 0u);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 1u);

    test.renderer.ReleaseGeometry(quad.handle);
    test.renderer.ReleaseTexture(texture);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
}

TEST(RmlRendererTest, ReleaseDuringFrameIsDeferred){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(8, 8);
    ASSERT_TRUE(test.isValid());

    std::vector<Rml::byte> pixels(8 * 8 * 4, 0);
    for (size_t i = 0; i < pixels.size(); i += 4){
        pixels[i] = 255;
        pixels[i + 3] = 255;
    }
    Rml::TextureHandle texture = test.renderer.GenerateTexture(Rml::Span<const Rml::byte>(pixels.data(), pixels.size()), Rml::Vector2i(8, 8));
    ASSERT_NE(texture, 0u);
    Quad quad(test.renderer, 0, 0, 8, 8);
    // pipelines ready
    ASSERT_TRUE(drawTexture(test, quad, texture));

    const uint32_t imageId = test.renderer.getTextureRender(texture)->backend.get().id;
    beginTestFrame();
    test.renderer.beginFrame(test.target, test.contextSize);
    test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(0, 0), texture);
    // released after a draw used it: alive until the frame is done
    test.renderer.ReleaseTexture(texture);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 1u);
    EXPECT_TRUE(isImageValid(imageId));
    ASSERT_TRUE(test.renderer.endFrame());
    SystemRender::commit();

    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
    EXPECT_FALSE(isImageValid(imageId));

    // and the frame drew it
    std::vector<uint8_t> layer;
    ASSERT_TRUE(test.readLayer(layer));
    PixelProbe probe{layer, 8};
    EXPECT_TRUE(probe.is(4, 4, 255, 0, 0, 255, 1));

    test.renderer.ReleaseGeometry(quad.handle);
}

TEST(RmlRendererTest, WaitsForShaders){
    JASPER_REQUIRE_GPU();

    // no renderer holds the HTML UI variants now: the next get() asks the builder
    for (const auto& variant : ShaderPool::getHtmlUIVariants()){
        ShaderPool::remove(variant.first, variant.second);
    }

    // a builder that reports "still compiling" for the first requests: the 8 variants of the
    // first frame and 4 of the second
    int requests = 0;
    ShaderPool::setShaderBuilder([&requests](ShaderKey key){
        requests++;
        if (requests <= 12)
            return ShaderBuildResult({}, ResourceLoadState::Loading);
        ShaderData data;
        if (!ShaderVariantCompiler::compile(key, ShaderPool::getShaderBackend(), nullptr, data))
            return ShaderBuildResult({}, ResourceLoadState::Failed);
        return ShaderBuildResult(data, ResourceLoadState::Finished);
    });

    {
        RenderTestContext test;
        ASSERT_TRUE(test.isValid());
        ASSERT_NE(test.load("render/boxes.rml"), nullptr);

        // frames are dropped (no crash, nothing drawn) until every variant exists
        EXPECT_FALSE(test.renderer.isReady());
        EXPECT_FALSE(test.renderFrame());
        EXPECT_FALSE(test.renderer.isReady());
        EXPECT_FALSE(test.renderFrame());
        EXPECT_EQ(test.renderer.getLastFrameDrawCount(), 0u);

        bool drawn = false;
        for (int i = 0; i < 10 && !drawn; i++){
            drawn = test.renderFrame();
        }
        EXPECT_TRUE(drawn);
        EXPECT_TRUE(test.renderer.isReady());
        EXPECT_GT(test.renderer.getLastFrameDrawCount(), 0u);
    }

    ShaderPool::setShaderBuilder(nullptr);
}

TEST(RmlRendererTest, GeometryStorage){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    // 5000 quads (400 KB of vertices, 120 KB of indices): drawn from the frame's transient
    // buffers in their first frame, from shared pages afterwards
    std::vector<std::unique_ptr<Quad>> quads;
    for (int i = 0; i < 5000; i++){
        const int x = (i % 125) * 5;
        const int y = (i / 125) * 5;
        quads.emplace_back(new Quad(test.renderer, (float)x, (float)y, 4.0f, 4.0f,
                                    Rml::ColourbPremultiplied((uint8_t)(i % 256), (uint8_t)((i / 256) * 12), 200, 255)));
    }

    auto drawAll = [&](){
        beginTestFrame();
        test.renderer.beginFrame(test.target, test.contextSize);
        for (const std::unique_ptr<Quad>& quad : quads){
            test.renderer.RenderGeometry(quad->handle, Rml::Vector2f(0, 0), 0);
        }
        const bool drawn = test.renderer.endFrame();
        SystemRender::commit();
        return drawn;
    };
    auto probeLayer = [&](){
        std::vector<uint8_t> layer;
        ASSERT_TRUE(test.readLayer(layer));
        PixelProbe probe{layer, 640};
        // first, a middle and the last quad (index 4999: column 124, row 39)
        EXPECT_TRUE(probe.is(1, 1, 0, 0, 200, 255, 1));
        EXPECT_TRUE(probe.is(2 * 5 + 1, 1, 2, 0, 200, 255, 1));
        EXPECT_TRUE(probe.is(124 * 5 + 2, 39 * 5 + 2, 4999 % 256, (4999 / 256) * 12, 200, 255, 1));
        // the gap between quads stays empty
        EXPECT_TRUE(probe.is(4, 4, 0, 0, 0, 0, 0));
    };

    bool drawn = false;
    for (int frame = 0; frame < 30 && !drawn; frame++){
        drawn = drawAll();
    }
    ASSERT_TRUE(drawn);
    EXPECT_EQ(test.renderer.getLastFrameDrawCount(), 5000u);
    probeLayer();

    // a few dozen buffers for 10000 vertex and index ranges (sokol has a fixed buffer pool)
    const size_t buffers = test.renderer.getGeometryBufferCount();
    EXPECT_GT(buffers, 0u);
    EXPECT_LT(buffers, 32u);
    EXPECT_EQ(test.renderer.getStats().geometryLive, 5000u);
    EXPECT_GE(test.renderer.getStats().geometryBytes, 5000u * (80u + 32u));

    // persistent from now on: the same image, nothing created or uploaded again
    const uint64_t created = test.renderer.getGpuCreateCount();
    for (int frame = 0; frame < 3; frame++){
        ASSERT_TRUE(drawAll());
    }
    EXPECT_EQ(test.renderer.getGpuCreateCount(), created);
    EXPECT_EQ(test.renderer.getLastFrameDrawCount(), 5000u);
    probeLayer();

    for (const std::unique_ptr<Quad>& quad : quads){
        test.renderer.ReleaseGeometry(quad->handle);
    }
    EXPECT_EQ(test.renderer.getStats().geometryLive, 0u);
    EXPECT_EQ(test.renderer.getStats().geometryBytes, 0u);
}

TEST(RmlRendererTest, NoLeaksAfterContextDestroy){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());
    ASSERT_NE(test.load("render/images.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());

    EXPECT_GT(test.renderer.getLiveGeometryCount(), 0u);
    EXPECT_GT(test.renderer.getLiveTextureCount(), 0u);

    std::vector<uint32_t> buffers, images;
    test.renderer.getGpuHandles(buffers, images);
    // the composite quad's two, geometry pages and the transient buffers of the first frame
    EXPECT_GE(buffers.size(), 6u);
    EXPECT_GE(images.size(), 4u);   // placeholder, checker, alpha50, icon
    for (uint32_t id : buffers) EXPECT_TRUE(isBufferValid(id));
    for (uint32_t id : images) EXPECT_TRUE(isImageValid(id));

    test.destroy();

    EXPECT_EQ(test.renderer.getLiveGeometryCount(), 0u);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
    EXPECT_FALSE(test.renderer.isReady());
    for (uint32_t id : buffers) EXPECT_FALSE(isBufferValid(id));
    for (uint32_t id : images) EXPECT_FALSE(isImageValid(id));

    std::vector<uint32_t> after, afterImages;
    test.renderer.getGpuHandles(after, afterImages);
    EXPECT_TRUE(after.empty());
    EXPECT_TRUE(afterImages.empty());
}

TEST(RmlRendererTest, ViewLossAndRecovery){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());
    ASSERT_NE(test.load("render/images.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    CompareResult before = test.compareScreen("render_images");
    EXPECT_TRUE(before.passed) << before.message;

    // what HtmlUIContext::releaseGpuResources does when the view goes: RmlUi gives its
    // textures back, every GPU object goes
    std::vector<uint32_t> buffers, images;
    test.renderer.getGpuHandles(buffers, images);
    Rml::ReleaseTextures(&test.renderer);
    test.renderer.releaseGpuResources();
    EXPECT_FALSE(test.renderer.isReady());
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
    for (uint32_t id : buffers) EXPECT_FALSE(isBufferValid(id));
    for (uint32_t id : images) EXPECT_FALSE(isImageValid(id));

    // the next frames load everything again and draw the same image
    ASSERT_TRUE(test.renderUntilDrawn());
    EXPECT_GT(test.renderer.getLiveTextureCount(), 0u);
    CompareResult after = test.compareScreen("render_images");
    EXPECT_TRUE(after.passed) << after.message;
}

TEST(RmlRendererTest, DeadTextureHandlesAreFreed){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(8, 8);
    ASSERT_TRUE(test.isValid());

    std::vector<Rml::byte> pixels(8 * 8 * 4, 255);
    Rml::TextureHandle texture = test.renderer.GenerateTexture(Rml::Span<const Rml::byte>(pixels.data(), pixels.size()), Rml::Vector2i(8, 8));
    ASSERT_NE(texture, 0u);
    Quad quad(test.renderer, 0, 0, 8, 8);
    ASSERT_TRUE(drawTexture(test, quad, texture));

    // view lost without RmlUi releasing: the handle stays valid but dead
    test.renderer.releaseGpuResources();
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 1u);
    EXPECT_EQ(test.renderer.getTextureRender(texture), nullptr);

    // a frame skips draws with it instead of binding a destroyed texture
    ASSERT_TRUE(drawTexture(test, quad, texture));
    EXPECT_EQ(test.renderer.getLastFrameDrawCount(), 0u);

    // and releasing it later only frees it
    test.renderer.ReleaseTexture(texture);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
    test.renderer.ReleaseGeometry(quad.handle);
}
