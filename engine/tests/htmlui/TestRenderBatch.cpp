// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The batched replay (S16 R1-R7, AC4/AC5): consecutive geometry of one state is one draw, with the
// image of the unbatched replay; untextured geometry joins a texture's white texels; a frame like
// the last one draws from the run cache with nothing gathered, a changed translation gathers its
// chunk again; runs never cross a layer, a composite, a filter, a shader or a stencil write;
// geometry inside its scissor merges across scissor changes; large geometry draws on its own.

#include "TestRenderCommon.h"

#include "HtmlUITexture.h"

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // batching on or off for a scope (read by beginFrame)
    struct BatchingMode {
        bool previous;
        explicit BatchingMode(bool on): previous(RmlRenderer::isBatching()){
            RmlRenderer::setBatchingForTesting(on);
        }
        ~BatchingMode(){
            RmlRenderer::setBatchingForTesting(previous);
        }
    };

    // SDF primitives on or off for a scope: off, quad lists are vertex runs (what the vertex tests
    // count); on, they are instances
    struct PrimitivesMode {
        bool previous;
        explicit PrimitivesMode(bool on): previous(RmlRenderer::isSdfPrimitives()){
            RmlRenderer::setSdfPrimitivesForTesting(on);
        }
        ~PrimitivesMode(){
            RmlRenderer::setSdfPrimitivesForTesting(previous);
        }
    };

    struct Quad {
        Rml::Mesh mesh;
        Rml::CompiledGeometryHandle handle = 0;

        Quad(RmlRenderer& renderer, float x, float y, float w, float h, Rml::ColourbPremultiplied colour,
             Rml::Vector2f uv0 = Rml::Vector2f(0, 0), Rml::Vector2f uv1 = Rml::Vector2f(1, 1)){
            Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour, uv0, uv1);
            handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
        }
    };

    // frames until one is drawn (the shaders compile on the first ones); draw() records the calls
    bool frame(RenderTestContext& test, const std::function<void()>& draw){
        for (int i = 0; i < 30; i++){
            beginTestFrame();
            test.renderer.beginFrame(test.target, test.contextSize);
            draw();
            const bool drawn = test.renderer.endFrame();
            SystemRender::commit();
            if (drawn)
                return true;
        }
        return false;
    }

    ::testing::AssertionResult sameImages(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int width, int tolerance = 1){
        if (a.size() != b.size())
            return ::testing::AssertionFailure() << "sizes " << a.size() << " and " << b.size();
        for (size_t i = 0; i < a.size(); i++){
            if (std::abs((int)a[i] - (int)b[i]) > tolerance){
                const size_t pixel = i / 4;
                return ::testing::AssertionFailure() << "first difference at (" << pixel % (size_t)width << ", " << pixel / (size_t)width
                                                     << "), channel " << i % 4 << ": " << int(a[i]) << " batched, " << int(b[i]) << " unbatched";
            }
        }
        return ::testing::AssertionSuccess();
    }

    // the layer of a frame drawn by draw() batched and unbatched; the batched frame's statistics
    struct Pair {
        std::vector<uint8_t> batched, unbatched;
        HtmlUIRenderStats batchedStats, unbatchedStats;
    };

    Pair drawBoth(RenderTestContext& test, const std::function<void()>& draw){
        Pair pair;
        {
            BatchingMode mode(false);
            EXPECT_TRUE(frame(test, draw));
            pair.unbatchedStats = test.renderer.getStats();
            EXPECT_TRUE(test.readLayer(pair.unbatched));
        }
        {
            BatchingMode mode(true);
            EXPECT_TRUE(frame(test, draw));
            pair.batchedStats = test.renderer.getStats();
            EXPECT_TRUE(test.readLayer(pair.batched));
        }
        return pair;
    }

    // an 8x8 texture whose top-left 4x4 texels are white (a glyph page's reserved block), red elsewhere
    Rml::TextureHandle whiteBlockTexture(RmlRenderer& renderer){
        std::vector<Rml::byte> rgba(8 * 8 * 4);
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 8; x++){
                Rml::byte* p = &rgba[(size_t)(y * 8 + x) * 4];
                const bool white = x < 4 && y < 4;
                p[0] = 255;
                p[1] = white ? 255 : 0;
                p[2] = white ? 255 : 0;
                p[3] = 255;
            }
        }
        Rml::TextureHandle handle = renderer.GenerateTexture(rgba, Rml::Vector2i(8, 8));
        HtmlUITexture* texture = reinterpret_cast<HtmlUITexture*>(handle);
        if (texture){
            texture->hasWhiteTexel = true;
            texture->whiteU = 2.0f / 8.0f;
            texture->whiteV = 2.0f / 8.0f;
        }
        return handle;
    }

}

TEST(RenderBatch, MergesConsecutiveGeometry){
    JASPER_REQUIRE_GPU();
    PrimitivesMode vertexRuns(false);
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    std::vector<std::unique_ptr<Quad>> quads;
    for (int i = 0; i < 200; i++){
        quads.emplace_back(new Quad(test.renderer, (float)(i % 20) * 30.0f, (float)(i / 20) * 30.0f, 24.0f, 24.0f,
                                    Rml::ColourbPremultiplied((uint8_t)(i * 5), 120, (uint8_t)(255 - i), 255)));
    }
    auto draw = [&](){
        for (size_t i = 0; i < quads.size(); i++)
            test.renderer.RenderGeometry(quads[i]->handle, Rml::Vector2f(3.0f, (float)(i % 3)), 0);
    };
    const Pair pair = drawBoth(test, draw);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    EXPECT_EQ(pair.unbatchedStats.drawCalls, 200u);
    EXPECT_EQ(pair.batchedStats.drawCalls, 1u);
    EXPECT_EQ(pair.batchedStats.runsMerged, 1u);
    EXPECT_EQ(pair.batchedStats.geometrySubmitted, 200u);
    EXPECT_EQ(pair.batchedStats.verticesGathered, 800u);

    for (std::unique_ptr<Quad>& quad : quads)
        test.renderer.ReleaseGeometry(quad->handle);
}

TEST(RenderBatch, UntexturedJoinsWhiteTexels){
    JASPER_REQUIRE_GPU();
    PrimitivesMode vertexRuns(false);
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    Rml::TextureHandle texture = whiteBlockTexture(test.renderer);
    ASSERT_NE(texture, 0u);
    // textured quads read the red part; untextured ones between them come out exactly in their colour
    Quad textured(test.renderer, 0, 0, 16, 16, Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(0.75f, 0.75f), Rml::Vector2f(0.875f, 0.875f));
    Quad plain(test.renderer, 0, 0, 16, 16, Rml::ColourbPremultiplied(40, 160, 220, 255));
    Quad translucent(test.renderer, 0, 0, 16, 16, Rml::ColourbPremultiplied(20, 80, 110, 128));
    auto draw = [&](){
        // untextured first: the run takes the texture of the first textured member
        test.renderer.RenderGeometry(plain.handle, Rml::Vector2f(2, 2), 0);
        test.renderer.RenderGeometry(textured.handle, Rml::Vector2f(20, 2), texture);
        test.renderer.RenderGeometry(translucent.handle, Rml::Vector2f(38, 2), 0);
        test.renderer.RenderGeometry(textured.handle, Rml::Vector2f(56, 2), texture);
        test.renderer.RenderGeometry(plain.handle, Rml::Vector2f(74, 2), 0);
    };
    const Pair pair = drawBoth(test, draw);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x, 0));
    EXPECT_EQ(pair.unbatchedStats.drawCalls, 5u);
    EXPECT_EQ(pair.batchedStats.drawCalls, 1u);

    PixelProbe probe{pair.batched, test.targetSize.x};
    EXPECT_TRUE(probe.is(10, 10, 40, 160, 220, 255, 0));
    EXPECT_TRUE(probe.is(28, 10, 255, 0, 0, 255, 0));
    EXPECT_TRUE(probe.is(46, 10, 20, 80, 110, 128, 0));

    // a texture without white texels keeps untextured geometry apart
    Rml::TextureHandle other = whiteBlockTexture(test.renderer);
    reinterpret_cast<HtmlUITexture*>(other)->hasWhiteTexel = false;
    auto drawOther = [&](){
        test.renderer.RenderGeometry(plain.handle, Rml::Vector2f(2, 30), 0);
        test.renderer.RenderGeometry(textured.handle, Rml::Vector2f(20, 30), other);
        test.renderer.RenderGeometry(plain.handle, Rml::Vector2f(38, 30), 0);
    };
    const Pair separate = drawBoth(test, drawOther);
    EXPECT_TRUE(sameImages(separate.batched, separate.unbatched, test.targetSize.x, 0));
    EXPECT_EQ(separate.batchedStats.drawCalls, 3u);

    test.renderer.ReleaseGeometry(textured.handle);
    test.renderer.ReleaseGeometry(plain.handle);
    test.renderer.ReleaseGeometry(translucent.handle);
    test.renderer.ReleaseTexture(texture);
    test.renderer.ReleaseTexture(other);
}

TEST(RenderBatch, RunCacheHitAndInvalidation){
    JASPER_REQUIRE_GPU();
    PrimitivesMode vertexRuns(false);
    RenderTestContext test;
    ASSERT_TRUE(test.isValid());

    // 500 quads: chunks of 64 members
    std::vector<std::unique_ptr<Quad>> quads;
    for (int i = 0; i < 500; i++){
        quads.emplace_back(new Quad(test.renderer, (float)(i % 25) * 25.0f, (float)(i / 25) * 17.0f, 20.0f, 12.0f,
                                    Rml::ColourbPremultiplied(200, (uint8_t)i, 90, 255)));
    }
    float moved = 0.0f;
    auto draw = [&](){
        for (size_t i = 0; i < quads.size(); i++)
            test.renderer.RenderGeometry(quads[i]->handle, Rml::Vector2f(i == 300 ? moved : 0.0f, 0.0f), 0);
    };

    BatchingMode mode(true);
    ASSERT_TRUE(frame(test, draw));
    EXPECT_EQ(test.renderer.getStats().verticesGathered, 2000u);
    const uint64_t created = test.renderer.getGpuCreateCount();

    // the same frame: every chunk from the cache, nothing gathered or created
    ASSERT_TRUE(frame(test, draw));
    const HtmlUIRenderStats same = test.renderer.getStats();
    EXPECT_EQ(same.verticesGathered, 0u);
    EXPECT_EQ(same.runCacheHits, 8u);
    EXPECT_EQ(same.drawCalls, 1u);
    EXPECT_EQ(test.renderer.getGpuCreateCount(), created);

    // one translation changed: its chunk only is gathered again (and drawn on its own)
    moved = 2.0f;
    ASSERT_TRUE(frame(test, draw));
    const HtmlUIRenderStats changed = test.renderer.getStats();
    EXPECT_EQ(changed.verticesGathered, 64u * 4u);
    EXPECT_EQ(changed.runCacheHits, 7u);
    EXPECT_LE(changed.drawCalls, 3u);

    std::vector<uint8_t> batched, unbatched;
    ASSERT_TRUE(test.readLayer(batched));
    {
        BatchingMode off(false);
        ASSERT_TRUE(frame(test, draw));
        ASSERT_TRUE(test.readLayer(unbatched));
    }
    EXPECT_TRUE(sameImages(batched, unbatched, test.targetSize.x));

    // a released and recompiled geometry (the same struct reused) is not taken from the cache
    test.renderer.ReleaseGeometry(quads[10]->handle);
    quads[10].reset(new Quad(test.renderer, 250.0f, 0.0f, 20.0f, 12.0f, Rml::ColourbPremultiplied(0, 0, 255, 255)));
    ASSERT_TRUE(frame(test, draw));
    EXPECT_EQ(test.renderer.getStats().verticesGathered, 64u * 4u);
    std::vector<uint8_t> layer;
    ASSERT_TRUE(test.readLayer(layer));
    PixelProbe probe{layer, test.targetSize.x};
    EXPECT_TRUE(probe.is(255, 5, 0, 0, 255, 255, 0));

    for (std::unique_ptr<Quad>& quad : quads)
        test.renderer.ReleaseGeometry(quad->handle);
}

TEST(RenderBatch, NoMergeAcrossLayersFiltersShadersOrStencil){
    JASPER_REQUIRE_GPU();
    PrimitivesMode vertexRuns(false);
    RenderTestContext test(320, 180);
    ASSERT_TRUE(test.isValid());

    Quad a(test.renderer, 10, 10, 40, 40, Rml::ColourbPremultiplied(255, 0, 0, 255));
    Quad b(test.renderer, 30, 30, 40, 40, Rml::ColourbPremultiplied(0, 0, 255, 200));
    Rml::CompiledFilterHandle opacity = test.renderer.CompileFilter("opacity", Rml::Dictionary{{"value", Rml::Variant(0.5f)}});
    Rml::ColorStopList stops = {Rml::ColorStop{Rml::ColourbPremultiplied(255, 255, 0, 255), Rml::NumericValue(0.0f, Rml::Unit::NUMBER)},
                                Rml::ColorStop{Rml::ColourbPremultiplied(0, 255, 255, 255), Rml::NumericValue(1.0f, Rml::Unit::NUMBER)}};
    Rml::CompiledShaderHandle gradient = test.renderer.CompileShader(
        "linear-gradient", Rml::Dictionary{{"p0", Rml::Variant(Rml::Vector2f(0, 0))}, {"p1", Rml::Variant(Rml::Vector2f(40, 0))},
                                           {"color_stop_list", Rml::Variant(stops)}});

    // a layer between two draws: the second goes into it
    auto layered = [&](){
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(0, 0), 0);
        const Rml::LayerHandle layer = test.renderer.PushLayer();
        test.renderer.RenderGeometry(b.handle, Rml::Vector2f(0, 0), 0);
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(60, 0), 0);
        test.renderer.CompositeLayers(layer, 0, Rml::BlendMode::Blend, Rml::Span<const Rml::CompiledFilterHandle>(&opacity, 1));
        test.renderer.PopLayer();
        test.renderer.RenderGeometry(b.handle, Rml::Vector2f(80, 0), 0);
    };
    Pair pair = drawBoth(test, layered);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    // a, [b + a merged in the layer], the opacity pass, the composite, b
    EXPECT_EQ(pair.batchedStats.runsMerged, 1u);
    EXPECT_EQ(pair.unbatchedStats.drawCalls - pair.batchedStats.drawCalls, 1u);

    // a gradient between two draws of one state keeps them apart
    auto shaded = [&](){
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(0, 0), 0);
        test.renderer.RenderShader(gradient, b.handle, Rml::Vector2f(100, 0), 0);
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(160, 0), 0);
    };
    pair = drawBoth(test, shaded);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    EXPECT_EQ(pair.batchedStats.drawCalls, 3u);
    EXPECT_EQ(pair.batchedStats.runsMerged, 0u);

    // a clip mask (stencil write) between two draws, and the draws under it
    auto clipped = [&](){
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(0, 100), 0);
        test.renderer.EnableClipMask(true);
        test.renderer.RenderToClipMask(Rml::ClipMaskOperation::Set, b.handle, Rml::Vector2f(0, 100));
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(0, 100), 0);
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(20, 100), 0);
        test.renderer.EnableClipMask(false);
        test.renderer.RenderGeometry(a.handle, Rml::Vector2f(200, 100), 0);
    };
    pair = drawBoth(test, clipped);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    // a | stencil clear + mask | [a, a] | a: no shape, so the mask is drawn
    EXPECT_EQ(pair.batchedStats.stencilDraws, 2u);
    EXPECT_EQ(pair.batchedStats.drawCalls, 5u);

    test.renderer.ReleaseShader(gradient);
    test.renderer.ReleaseFilter(opacity);
    test.renderer.ReleaseGeometry(a.handle);
    test.renderer.ReleaseGeometry(b.handle);
}

TEST(RenderBatch, ScissorElisionMergesAcrossScissors){
    JASPER_REQUIRE_GPU();
    PrimitivesMode vertexRuns(false);
    RenderTestContext test(320, 180);
    ASSERT_TRUE(test.isValid());

    Quad q(test.renderer, 0, 0, 20, 20, Rml::ColourbPremultiplied(0, 200, 100, 255));
    auto draw = [&](){
        // each quad inside its own scissor: one draw
        test.renderer.EnableScissorRegion(true);
        for (int i = 0; i < 6; i++){
            test.renderer.SetScissorRegion(Rml::Rectanglei::FromPositionSize(Rml::Vector2i(i * 30, 0), Rml::Vector2i(30, 40)));
            test.renderer.RenderGeometry(q.handle, Rml::Vector2f((float)(i * 30 + 5), 5.0f), 0);
        }
        // one cut by its scissor: drawn with it, so it starts a run of its own
        test.renderer.SetScissorRegion(Rml::Rectanglei::FromPositionSize(Rml::Vector2i(0, 60), Rml::Vector2i(10, 10)));
        test.renderer.RenderGeometry(q.handle, Rml::Vector2f(0.0f, 60.0f), 0);
        test.renderer.EnableScissorRegion(false);
    };
    const Pair pair = drawBoth(test, draw);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    EXPECT_EQ(pair.batchedStats.drawCalls, 2u);
    test.renderer.ReleaseGeometry(q.handle);
}

TEST(RenderBatch, LargeGeometryDrawsOnItsOwn){
    JASPER_REQUIRE_GPU();
    PrimitivesMode vertexRuns(false);
    RenderTestContext test(320, 180);
    ASSERT_TRUE(test.isValid());

    // 1100 quads in one mesh: 4400 vertices, above the 4096 of a gathered geometry
    Rml::Mesh big;
    for (int i = 0; i < 1100; i++)
        Rml::MeshUtilities::GenerateQuad(big, Rml::Vector2f((float)(i % 50) * 6.0f, (float)(i / 50) * 6.0f), Rml::Vector2f(5, 5),
                                         Rml::ColourbPremultiplied(90, 30, (uint8_t)i, 255));
    const Rml::CompiledGeometryHandle bigHandle = test.renderer.CompileGeometry(big.vertices, big.indices);
    Quad q(test.renderer, 0, 0, 10, 10, Rml::ColourbPremultiplied(255, 255, 255, 255));
    auto draw = [&](){
        test.renderer.RenderGeometry(q.handle, Rml::Vector2f(0, 150), 0);
        test.renderer.RenderGeometry(q.handle, Rml::Vector2f(20, 150), 0);
        test.renderer.RenderGeometry(bigHandle, Rml::Vector2f(2, 2), 0);
        test.renderer.RenderGeometry(q.handle, Rml::Vector2f(40, 150), 0);
    };
    const Pair pair = drawBoth(test, draw);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    EXPECT_EQ(pair.batchedStats.drawCalls, 3u);
    EXPECT_EQ(pair.batchedStats.verticesGathered, 12u);
    test.renderer.ReleaseGeometry(bigHandle);
    test.renderer.ReleaseGeometry(q.handle);
}

// with SDF primitives: quad lists (glyphs, images, plain rectangles) and box primitives are
// instances of one run, whatever textures the untextured ones sit between, and draw the
// unbatched image
TEST(RenderBatch, InstancesMergeQuadsAndBoxes){
    JASPER_REQUIRE_GPU();
    PrimitivesMode instances(true);
    RenderTestContext test(320, 180);
    ASSERT_TRUE(test.isValid());

    Rml::TextureHandle texture = whiteBlockTexture(test.renderer);
    reinterpret_cast<HtmlUITexture*>(texture)->hasWhiteTexel = false;
    Quad textured(test.renderer, 0, 0, 16, 16, Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(0.75f, 0.75f), Rml::Vector2f(0.875f, 0.875f));
    Quad plain(test.renderer, 0, 0, 16, 16, Rml::ColourbPremultiplied(40, 160, 220, 255));
    Rml::BoxPrimitive box;
    box.border_box = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(0, 0), Rml::Vector2f(40, 24));
    box.radii = {Rml::Vector2f(6, 6), Rml::Vector2f(6, 6), Rml::Vector2f(6, 6), Rml::Vector2f(6, 6)};
    box.border_widths = Rml::Vector4f(2, 2, 2, 2);
    box.fill = Rml::ColourbPremultiplied(30, 60, 90, 255);
    box.border_colors = {Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::ColourbPremultiplied(255, 255, 255, 255),
                         Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::ColourbPremultiplied(255, 255, 255, 255)};
    auto draw = [&](){
        for (int i = 0; i < 20; i++){
            const float x = (float)(i % 5) * 60.0f, y = (float)(i / 5) * 40.0f;
            test.renderer.RenderPrimitives(Rml::Span<const Rml::BoxPrimitive>(&box, 1), Rml::Vector2f(x + 4, y + 4));
            test.renderer.RenderGeometry(plain.handle, Rml::Vector2f(x + 8, y + 8), 0);
            test.renderer.RenderGeometry(textured.handle, Rml::Vector2f(x + 26, y + 8), texture);
        }
    };
    const Pair pair = drawBoth(test, draw);
    EXPECT_TRUE(sameImages(pair.batched, pair.unbatched, test.targetSize.x));
    EXPECT_EQ(pair.batchedStats.geometrySubmitted, 60u);
    EXPECT_EQ(pair.batchedStats.drawCalls, 1u);
    EXPECT_EQ(pair.batchedStats.instancesDrawn, 60u);
    EXPECT_EQ(pair.batchedStats.verticesGathered, 60u);

    test.renderer.ReleaseGeometry(textured.handle);
    test.renderer.ReleaseGeometry(plain.handle);
    test.renderer.ReleaseTexture(texture);
}

// quads of up to four textures share a run (HTMLUI_PRIM's texture slots), a fifth starts another;
// the image is the unbatched one
TEST(RenderBatch, InstancesTakeFourTextures){
    JASPER_REQUIRE_GPU();
    PrimitivesMode instances(true);
    RenderTestContext test(320, 180);
    ASSERT_TRUE(test.isValid());

    std::vector<Rml::TextureHandle> textures;
    for (int i = 0; i < 5; i++){
        std::vector<Rml::byte> rgba(4 * 4 * 4);
        for (size_t p = 0; p < rgba.size(); p += 4){
            rgba[p] = (Rml::byte)(40 * i + 60);
            rgba[p + 1] = (Rml::byte)(200 - 30 * i);
            rgba[p + 2] = (Rml::byte)(30 * i);
            rgba[p + 3] = 255;
        }
        textures.push_back(test.renderer.GenerateTexture(rgba, Rml::Vector2i(4, 4)));
        ASSERT_NE(textures.back(), 0u);
    }
    Quad textured(test.renderer, 0, 0, 16, 16, Rml::ColourbPremultiplied(255, 255, 255, 255), Rml::Vector2f(0.0f, 0.0f), Rml::Vector2f(1.0f, 1.0f));
    auto draw = [&](int count){
        for (int i = 0; i < 20; i++){
            const float x = (float)(i % 10) * 30.0f, y = (float)(i / 10) * 30.0f;
            test.renderer.RenderGeometry(textured.handle, Rml::Vector2f(x + 4, y + 4), textures[(size_t)(i % count)]);
        }
    };
    const Pair four = drawBoth(test, [&](){ draw(4); });
    EXPECT_TRUE(sameImages(four.batched, four.unbatched, test.targetSize.x));
    EXPECT_EQ(four.batchedStats.drawCalls, 1u);
    // the fifth texture does not fit the run's four slots: a run for every fifth quad and the next four
    const Pair five = drawBoth(test, [&](){ draw(5); });
    EXPECT_TRUE(sameImages(five.batched, five.unbatched, test.targetSize.x));
    EXPECT_EQ(five.batchedStats.drawCalls, 5u);

    test.renderer.ReleaseGeometry(textured.handle);
    for (Rml::TextureHandle texture : textures)
        test.renderer.ReleaseTexture(texture);
}
