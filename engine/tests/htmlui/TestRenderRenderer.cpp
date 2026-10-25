// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlRenderer: scissor conversion, geometry and texture lifetimes, deferred releases, pooled
// geometry buffers, waiting for shaders, GPU objects after a context is destroyed and after
// view loss.

#include "TestRenderCommon.h"
#include "harness/GpuAllocations.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

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

TEST(RmlRendererTest, WaitsForShaders){
    JASPER_REQUIRE_GPU();

    // no renderer holds the HTML UI variants now: the next get() asks the builder
    for (const auto& variant : ShaderPool::getHtmlUIVariants()){
        ShaderPool::remove(variant.first, variant.second);
    }

    // Hold compilation at the dependency boundary, then let the actual compiler
    // finish. No assumption about how many variants the renderer requests.
    bool pending = true;
    struct RestoreBuilder {
        ~RestoreBuilder(){ ShaderPool::setShaderBuilder(nullptr); }
    } restore;
    ShaderPool::setShaderBuilder([&pending](ShaderKey key){
        if (pending)
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
        pending = false;

        bool drawn = false;
        for (int i = 0; i < 10 && !drawn; i++){
            drawn = test.renderFrame();
        }
        EXPECT_TRUE(drawn);
        EXPECT_TRUE(test.renderer.isReady());
        EXPECT_GT(test.renderer.getLastFrameDrawCount(), 0u);
        std::vector<uint8_t> layer;
        ASSERT_TRUE(test.readLayer(layer));
        const PixelProbe probe{layer, 640};
        EXPECT_TRUE(probe.is(80, 60, 0x30, 0x60, 0xc0));
    }

}

TEST(RmlRendererTest, GeometryStorage){
    JASPER_REQUIRE_GPU();
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());
    GpuAllocations allocations;

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

    // The large batch must allocate geometry storage before reuse is checked.
    const size_t buffers = test.renderer.getGeometryBufferCount();
    EXPECT_GT(buffers, 0u);

    // persistent from now on: the same image, nothing created or uploaded again
    const size_t created = allocations.created();
    ASSERT_GT(created, 0u);
    for (int frame = 0; frame < 3; frame++){
        ASSERT_TRUE(drawAll());
    }
    EXPECT_EQ(allocations.created(), created);
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
    EXPECT_FALSE(buffers.empty());
    EXPECT_FALSE(images.empty());
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
