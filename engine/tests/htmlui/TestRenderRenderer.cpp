// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlRenderer: scissor conversion, geometry and texture lifetimes, deferred releases, pooled
// geometry buffers, waiting for shaders, GPU objects after a context is destroyed and after
// view loss; S93's images: texture sources and regions, the asynchronous decode and the process
// image cache (R18-R20).

#include "TestGoldenCommon.h"
#include "TestRenderCommon.h"
#include "TempDir.h"
#include "compat/PerfFixtures.h"

#include "HtmlUIClock.h"
#include "HtmlUIImageCache.h"
#include "component/UIDocumentComponent.h"
#include "subsystem/UIDocumentSystem.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

#include "stb_image_write.h"

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
    // the statistics of a replayed frame: S92 R1 keeps an unchanged one (no pass, no draw)
    FrameReuseMode replayed(false);
    JASPER_REQUIRE_GPU();
    // (the boxes as meshes: SDF primitives make no geometry)
    ClassicReplay classic;
    HtmlUIImageCache::get().clear();
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
    // the statistics of a replayed frame: S92 R1 keeps an unchanged one (no pass, no draw)
    FrameReuseMode replayed(false);
    JASPER_REQUIRE_GPU();
    S16On s16;
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    // 5000 quads (400 KB of vertices, 120 KB of indices). Batched (S16, the default): instances
    // gathered into a few draws, from the run cache afterwards. Unbatched: drawn from the frame's
    // transient buffers in their first frame, from shared pages afterwards
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
    EXPECT_EQ(test.renderer.getStats().geometrySubmitted, 5000u);
    EXPECT_LE(test.renderer.getLastFrameDrawCount(), 3u);
    probeLayer();
    {
        // the next frames come from the run cache: nothing created, nothing gathered
        const uint64_t created = test.renderer.getGpuCreateCount();
        ASSERT_TRUE(drawAll());
        EXPECT_EQ(test.renderer.getGpuCreateCount(), created);
        EXPECT_EQ(test.renderer.getStats().verticesGathered, 0u);
        EXPECT_EQ(test.renderer.getStats().geometrySubmitted, 5000u);
        EXPECT_LE(test.renderer.getLastFrameDrawCount(), 3u);
        probeLayer();
    }

    // the unbatched replay: every geometry its own draw from persistent storage
    const bool batching = RmlRenderer::isBatching();
    RmlRenderer::setBatchingForTesting(false);
    ASSERT_TRUE(drawAll());
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
    EXPECT_EQ(test.renderer.getStats().geometrySubmitted, 5000u);
    probeLayer();
    RmlRenderer::setBatchingForTesting(batching);

    for (const std::unique_ptr<Quad>& quad : quads){
        test.renderer.ReleaseGeometry(quad->handle);
    }
    EXPECT_EQ(test.renderer.getStats().geometryLive, 0u);
    EXPECT_EQ(test.renderer.getStats().geometryBytes, 0u);
}

TEST(RmlRendererTest, NoLeaksAfterContextDestroy){
    JASPER_REQUIRE_GPU();
    // (the geometry pages and transient buffers of the unbatched replay)
    ClassicReplay classic;
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
    EXPECT_GE(images.size(), 1u);   // placeholder
    // S93 R19: the file images (checker, alpha50; compat rasterizes the SVG itself) are the process image
    // cache's, not the renderer's
    std::vector<uint32_t> cached;
    HtmlUIImageCache::get().getGpuHandles(cached);
    EXPECT_GE(cached.size(), 2u);
    for (uint32_t id : buffers) EXPECT_TRUE(isBufferValid(id));
    for (uint32_t id : images) EXPECT_TRUE(isImageValid(id));
    for (uint32_t id : cached) EXPECT_TRUE(isImageValid(id));

    test.destroy();

    EXPECT_EQ(test.renderer.getLiveGeometryCount(), 0u);
    EXPECT_EQ(test.renderer.getLiveTextureCount(), 0u);
    EXPECT_FALSE(test.renderer.isReady());
    for (uint32_t id : buffers) EXPECT_FALSE(isBufferValid(id));
    for (uint32_t id : images) EXPECT_FALSE(isImageValid(id));
    // released into the cache's LRU (the next context showing them uploads nothing), gone when it is cleared
    for (uint32_t id : cached) EXPECT_TRUE(isImageValid(id));
    HtmlUIImageCache::get().clear();
    for (uint32_t id : cached) EXPECT_FALSE(isImageValid(id));

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

// ---------------------------------------------------------------- S93: images (R18-R20)

namespace {

    // the cache decodes on the Background lane for a scope (the suite decodes at once, S16Switch)
    struct AsyncImageDecodes {
        AsyncImageDecodes(){ HtmlUIImageCache::setSynchronousForTesting(false); }
        ~AsyncImageDecodes(){ HtmlUIImageCache::setSynchronousForTesting(true); }
        AsyncImageDecodes(const AsyncImageDecodes&) = delete;
        AsyncImageDecodes& operator=(const AsyncImageDecodes&) = delete;
    };

    struct ImageCacheBudget {
        explicit ImageCacheBudget(int mib){ HtmlUIImageCache::setBudgetMiB(mib); }
        ~ImageCacheBudget(){ HtmlUIImageCache::setBudgetMiB(-1); }
        ImageCacheBudget(const ImageCacheBudget&) = delete;
        ImageCacheBudget& operator=(const ImageCacheBudget&) = delete;
    };

    // a 2048 x 2048 opaque PNG whose every pixel has red 200 (green and blue vary, so it does not
    // compress to nothing and its decode costs what a real one does)
    std::string writeLargePng(const test::TempDir& dir, const std::string& name){
        const int size = 2048;
        std::vector<uint8_t> rgba((size_t)size * size * 4);
        uint32_t seed = 12345u;
        for (size_t i = 0; i < (size_t)size * size; i++){
            seed = seed * 1664525u + 1013904223u;
            rgba[i * 4 + 0] = 200;
            rgba[i * 4 + 1] = (uint8_t)((seed >> 24) & 0x3F);
            rgba[i * 4 + 2] = (uint8_t)((i / 4096) & 0xFF);
            rgba[i * 4 + 3] = 255;
        }
        const std::string path = dir.file(name).generic_string();
        stbi_write_png(path.c_str(), size, size, 4, rgba.data(), size * 4);
        return path;
    }

    std::string imageDocument(const std::string& path, const std::string& attributes = ""){
        return "<rml><head><style>body { margin: 0; } img { width: 128px; height: 128px; }</style></head>"
               "<body><img src=\"" + path + "\" " + attributes + "/></body></rml>";
    }

    // the layer's centre texel shows the image (red 200, opaque)
    bool showsImage(RenderTestContext& test){
        std::vector<uint8_t> layer;
        if (!test.readLayer(layer))
            return false;
        const uint8_t* p = &layer[((size_t)64 * test.targetSize.x + 64) * 4];
        return std::abs(p[0] - 200) <= 2 && p[3] == 255;
    }

}

TEST(ImageRegion, ParsesMediaFragmentsAndSprites){
    HtmlUIImageRegion region;
    std::string error;
    EXPECT_TRUE(HtmlUIImageRegion::parse("", region, &error));
    EXPECT_TRUE(region.empty());
    ASSERT_TRUE(HtmlUIImageRegion::parse("#xywh=16,0,8,4", region, &error)) << error;
    EXPECT_EQ(region.kind, HtmlUIImageRegion::Kind::PIXELS);
    EXPECT_EQ(region.x, 16.0f);
    EXPECT_EQ(region.width, 8.0f);
    EXPECT_EQ(region.height, 4.0f);
    ASSERT_TRUE(HtmlUIImageRegion::parse("xywh=pixel:1,2,3,4", region, &error)) << error;
    EXPECT_EQ(region.kind, HtmlUIImageRegion::Kind::PIXELS);
    ASSERT_TRUE(HtmlUIImageRegion::parse("#xywh=percent:0,50,50,50", region, &error)) << error;
    EXPECT_EQ(region.kind, HtmlUIImageRegion::Kind::PERCENT);
    EXPECT_EQ(region.keyText(), "xywh=percent:0,50,50,50");
    ASSERT_TRUE(HtmlUIImageRegion::parse("#sprite=hero%203", region, &error)) << error;
    EXPECT_EQ(region.kind, HtmlUIImageRegion::Kind::SPRITE);
    EXPECT_EQ(region.sprite, "hero 3");
    // another fragment (an SVG's #id) names no region: the whole image, as Media Fragments say
    ASSERT_TRUE(HtmlUIImageRegion::parse("#icon-home", region, &error));
    EXPECT_TRUE(region.empty());
    // malformed ones are errors
    EXPECT_FALSE(HtmlUIImageRegion::parse("#xywh=1,2,3", region, &error));
    EXPECT_FALSE(HtmlUIImageRegion::parse("#xywh=1,2,0,4", region, &error));
    EXPECT_FALSE(HtmlUIImageRegion::parse("#xywh=-1,2,3,4", region, &error));
    EXPECT_FALSE(HtmlUIImageRegion::parse("#xywh=a,2,3,4", region, &error));
    EXPECT_FALSE(HtmlUIImageRegion::parse("#sprite=", region, &error));
    EXPECT_FALSE(error.empty());
}

TEST(ImageRegion, TextureSourceSplitsFragmentAndQuery){
    // compat appends its query after a url that kept its fragment
    RmlRenderer::TextureSource source = RmlRenderer::parseTextureSource("asset://ui/atlas.png#xywh=0,0,8,8?htmlui-filter=nearest&rmlui-wrap=repeat-x");
    EXPECT_EQ(source.path, "asset://ui/atlas.png");
    EXPECT_EQ(source.fragment, "#xywh=0,0,8,8");
    EXPECT_TRUE(source.nearest);
    EXPECT_TRUE(source.repeatX);
    EXPECT_FALSE(source.repeatY);
    EXPECT_FALSE(source.syncDecode);
    source = RmlRenderer::parseTextureSource("ui/a.png?htmlui-decoding=sync#sprite=coin");
    EXPECT_EQ(source.path, "ui/a.png");
    EXPECT_EQ(source.fragment, "#sprite=coin");
    EXPECT_TRUE(source.syncDecode);
    // other parameters stay, and the old entry points keep the fragment in the path
    source = RmlRenderer::parseTextureSource("ui/a.svg?svgScale=2&htmlui-decoding=async");
    EXPECT_EQ(source.path, "ui/a.svg?svgScale=2");
    EXPECT_FALSE(source.syncDecode);
    bool nearest = false;
    EXPECT_EQ(RmlRenderer::stripTextureFilter("ui/a.png#sprite=coin?htmlui-filter=nearest", nearest), "ui/a.png#sprite=coin");
    EXPECT_TRUE(nearest);
}

namespace {

    class RenderRendererGpu : public FullPathGoldenTest {
    protected:
        // a TEXTURE-mode document of a file
        static UIDocument textureDocument(SceneFixture& fixture, const std::string& file){
            UIDocument document = fixture.addDocument(file);
            UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::TEXTURE;
            component.textureWidth = 128;
            component.textureHeight = 128;
            return document;
        }

        // frames until the scene's documents are loaded; the image cache hits their contexts counted meanwhile
        static bool drawUntilLoaded(SceneFixture& fixture, Entity entity, uint32_t& hits){
            UIDocumentSystem& system = *fixture.getScene().getSystem<UIDocumentSystem>();
            for (int i = 0; i < 300; i++){
                GpuEnvironment::drawFrames(1);
                hits += system.getRenderStats(entity).imageCacheHits;
                if (system.isAllLoaded()){
                    GpuEnvironment::drawFrames(1);
                    hits += system.getRenderStats(entity).imageCacheHits;
                    return true;
                }
            }
            return false;
        }
    };

}

TEST_F(RenderRendererGpu, ImageDecodeIsAsync){
    JASPER_REQUIRE_GPU();
    test::TempDir dir;
    AsyncImageDecodes async;
    HtmlUIImageCache::get().clear();

    RenderTestContext test(128, 128);
    ASSERT_TRUE(test.isValid());
    // the UI programs first: the frames below measure the image only
    ASSERT_NE(test.loadFromMemory("<rml><body/></rml>", "warm.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    const double idleStart = compat::threadCpuMs();
    ASSERT_TRUE(test.renderFrame());
    const double idleMs = compat::threadCpuMs() - idleStart;

    // R18: the first frame reads the header (layout gets the size), draws nothing for the image
    // and costs the main thread no decode
    const std::string large = writeLargePng(dir, "large.png");
    Rml::ElementDocument* document = test.loadFromMemory(imageDocument(large), "async.rml");
    ASSERT_NE(document, nullptr);
    const double firstStart = compat::threadCpuMs();
    ASSERT_TRUE(test.renderFrame());
    const double firstMs = compat::threadCpuMs() - firstStart;
    EXPECT_FALSE(showsImage(test)) << "the 2048 x 2048 image was decoded in the frame that first laid it out";
    EXPECT_EQ(test.renderer.getStats().imagesDecoded, 1u);
    EXPECT_EQ(test.renderer.getStats().imagesPending, 1u);
    // the image keeps its laid-out size while it decodes: no relayout when it appears
    std::vector<Rml::Element*> images;
    document->GetElementsByTagName(images, "img");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0]->GetBox().GetSize().x, 128.0f);

    // drawn once decoded (the frames wait for the state, not for time; R42's 30 frames at 60 fps
    // are a budget for an idle machine, printed below)
    const auto decodeStart = std::chrono::steady_clock::now();
    int frames = 1;
    double uploadMs = 0.0;
    for (; frames < 2000 && !showsImage(test); frames++){
        const double frameStart = compat::threadCpuMs();
        ASSERT_TRUE(test.renderFrame());
        uploadMs = std::max(uploadMs, compat::threadCpuMs() - frameStart);
        if (!showsImage(test))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double visibleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decodeStart).count();
    ASSERT_TRUE(showsImage(test)) << "the image never appeared";
    EXPECT_EQ(test.renderer.getStats().imagesPending, 0u);
    std::printf("[bench] ImageDecodeIsAsync: idle frame %.2f ms, first frame %.2f ms (+%.2f), upload frame %.2f ms, visible after %d frames, %.1f ms\n",
                idleMs, firstMs, firstMs - idleMs, uploadMs, frames, visibleMs);
    // R42 on a quiet machine (JASPER_HTMLUI_PERF_SLACK set): <= 3 ms added, the upload frame <= 4 ms, visible within 0.5 s.
    // The upload frame's bound holds on a hardware device only: on the reference renderer (llvmpipe)
    // drawing is the main thread's own CPU work, and the frame that first draws the texture took
    // 80-300 ms in a fresh process (5 ms on a repeat) while the image cache's upload took 5 ms of it
    if (const double slack = compat::benchmarkPerfSlack(); slack > 0.0){
        EXPECT_LE(firstMs - idleMs, 3.0 * slack);
        if (!GpuEnvironment::isReferenceRenderer()){
            EXPECT_LE(uploadMs, 4.0 * slack);
        }
        EXPECT_LE(visibleMs, 500.0 * slack);
    }
    test.context->UnloadDocument(document);

    // decoding="sync": complete in the first frame (another file: nothing cached)
    const std::string syncFile = writeLargePng(dir, "sync.png");
    document = test.loadFromMemory(imageDocument(syncFile, "decoding=\"sync\""), "sync.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderFrame());
    EXPECT_TRUE(showsImage(test)) << "decoding=\"sync\" did not draw the image in its first frame";
    test.context->UnloadDocument(document);

    // a deterministic clock (a fixed UI time) waits for the decode in the same frame
    const std::string fixedFile = writeLargePng(dir, "fixed.png");
    HtmlUIClock::setFixedTime(1.0);
    document = test.loadFromMemory(imageDocument(fixedFile), "fixed.rml");
    ASSERT_NE(document, nullptr);
    const bool drawn = test.renderFrame();
    const bool shown = showsImage(test);
    HtmlUIClock::setFixedTime(std::nullopt);
    ASSERT_TRUE(drawn);
    EXPECT_TRUE(shown) << "a fixed UI time did not wait for the decode";
}

// R19, AC23: one decode for every context that shows a file; released images stay in the LRU
TEST_F(RenderRendererGpu, ImageCacheSharedAcrossContexts){
    JASPER_REQUIRE_GPU();
    HtmlUIImageCache& cache = HtmlUIImageCache::get();
    cache.clear();
    const HtmlUIImageCache::Counters before = cache.getCounters();
    {
        // a screen document and a texture-mode document (two contexts, two renderers)
        SceneFixture fixture;
        fixture.addDocument("s93/shared-image.html");
        textureDocument(fixture, "s93/shared-image.html");
        ASSERT_TRUE(fixture.waitUntilLoaded());
        GpuEnvironment::drawFrames(2);
        EXPECT_EQ(cache.getCounters().decodes - before.decodes, 1u);
        EXPECT_EQ(cache.getCounters().uploads - before.uploads, 1u);
        EXPECT_EQ(cache.getCounters().hits - before.hits, 1u);
    }
    // both contexts went: the image is released, not destroyed
    EXPECT_EQ(cache.getEntryCount(), 1u);
    EXPECT_GT(cache.getReleasedBytes(), 0u);
    {
        // R42: a second scene that shows it decodes and uploads nothing
        SceneFixture fixture;
        UIDocument document = fixture.addDocument("s93/shared-image.html");
        uint32_t hits = 0;
        ASSERT_TRUE(drawUntilLoaded(fixture, document.getEntity(), hits));
        EXPECT_EQ(hits, 1u) << "inspect_ui's imageCacheHits of the new context";
        EXPECT_EQ(cache.getCounters().decodes - before.decodes, 1u);
        EXPECT_EQ(cache.getCounters().uploads - before.uploads, 1u);
    }

    // graphics.html_ui_image_cache_mib = 0 keeps no released image: a new context decodes again
    ImageCacheBudget none(0);
    EXPECT_EQ(cache.getEntryCount(), 0u);
    {
        SceneFixture fixture;
        UIDocument document = fixture.addDocument("s93/shared-image.html");
        uint32_t hits = 0;
        ASSERT_TRUE(drawUntilLoaded(fixture, document.getEntity(), hits));
        EXPECT_EQ(hits, 0u);
        EXPECT_EQ(cache.getCounters().decodes - before.decodes, 2u);
    }
    EXPECT_EQ(cache.getEntryCount(), 0u);
}

// R15 (images): a prewarmed image is decoded and uploaded before a document shows it: its first
// frame draws it, from the cache, with no decode
TEST_F(RenderRendererGpu, PrewarmImageIsReadyForTheFirstFrame){
    JASPER_REQUIRE_GPU();
    test::TempDir dir;
    AsyncImageDecodes async;
    HtmlUIImageCache& cache = HtmlUIImageCache::get();
    cache.clear();
    const std::string file = writeLargePng(dir, "prewarm.png");

    std::shared_ptr<HtmlUIImageCache::PrewarmJob> job = cache.prewarm({file, dir.file("missing.png").generic_string()});
    ASSERT_NE(job, nullptr);
    EXPECT_EQ(job->total, 1u);   // the missing file is a warning, not part of the job
    bool called = false;
    job->onDone = [&called](){ called = true; };
    EXPECT_FALSE(job->done);
    for (int i = 0; i < 2000 && !job->done; i++){
        cache.update();
        if (!job->done)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(job->done);
    EXPECT_TRUE(called);
    EXPECT_EQ(job->progress(), 1.0f);
    expectedWarnings = 1;

    const HtmlUIImageCache::Counters before = cache.getCounters();
    RenderTestContext test(128, 128);
    ASSERT_TRUE(test.isValid());
    ASSERT_NE(test.loadFromMemory("<rml><body/></rml>", "warm.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    ASSERT_NE(test.loadFromMemory(imageDocument(file), "prewarmed.rml"), nullptr);
    ASSERT_TRUE(test.renderFrame());
    EXPECT_TRUE(showsImage(test)) << "the prewarmed image was not drawn in its first frame";
    EXPECT_EQ(test.renderer.getStats().imageCacheHits, 1u);
    EXPECT_EQ(test.renderer.getStats().imagesDecoded, 0u);
    EXPECT_EQ(cache.getCounters().decodes, before.decodes);
    EXPECT_EQ(cache.getCounters().uploads, before.uploads);
}
