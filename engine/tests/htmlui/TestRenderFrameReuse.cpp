// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 frame reuse (docs/specs/S92-htmlui-render-cost.md R1-R6, AC1-AC5): the signature of what a
// frame recorded follows every input of the draw (geometry and textures by serial and content
// version, everything else by value) and nothing else.

#include "TestRenderCommon.h"

#include "HtmlUITexture.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct ReuseQuad {
        Rml::Mesh mesh;
        Rml::CompiledGeometryHandle handle = 0;

        ReuseQuad(RmlRenderer& renderer, float x, float y, float w, float h, Rml::ColourbPremultiplied colour){
            Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour);
            handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
        }
    };

    // frames until one is drawn (the shaders compile on the first ones); draw() records the calls,
    // the signature of everything it recorded (origin: target px) is taken before the frame ends
    uint64_t recordedSignature(RenderTestContext& test, const std::function<void()>& draw, Rml::Vector2i origin, uint32_t& live){
        uint64_t signature = 0;
        for (int i = 0; i < 30; i++){
            beginTestFrame();
            test.renderer.beginFrame(test.target, test.contextSize);
            draw();
            live = 0;
            signature = test.renderer.signatureOf(0, test.renderer.getRecordedCommandCount(), origin, live);
            const bool drawn = test.renderer.endFrame();
            SystemRender::commit();
            if (drawn)
                break;
        }
        return signature;
    }

    uint64_t recordedSignature(RenderTestContext& test, const std::function<void()>& draw){
        uint32_t live = 0;
        return recordedSignature(test, draw, Rml::Vector2i(0, 0), live);
    }

}

TEST(RenderFrameReuse, SignatureFollowsWhatWasRecorded){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(160, 90);
    ASSERT_TRUE(test.isValid());
    const Rml::ColourbPremultiplied red(255, 0, 0, 255);
    ReuseQuad quad(test.renderer, 0, 0, 20, 10, red);
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("frame-reuse-dynamic", 4, 4);
    ASSERT_TRUE(owned);
    HtmlUITexture* texture = owned.release();   // the renderer's from its first draw, released below
    const Rml::TextureHandle textureHandle = reinterpret_cast<Rml::TextureHandle>(texture);
    const Rml::Matrix4f scaled = Rml::Matrix4f::Scale(2.0f, 2.0f, 1.0f);

    auto base = [&]{
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), 0);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), textureHandle);
    };
    const uint64_t first = recordedSignature(test, base);
    EXPECT_EQ(recordedSignature(test, base), first) << "the same calls, the same signature";

    EXPECT_NE(recordedSignature(test, [&]{
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 11), 0);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), textureHandle);
    }), first) << "a translation";
    EXPECT_NE(recordedSignature(test, [&]{
        test.renderer.EnableScissorRegion(true);
        test.renderer.SetScissorRegion(Rml::Rectanglei::FromSize(Rml::Vector2i(100, 50)));
        base();
    }), first) << "a scissor";
    EXPECT_NE(recordedSignature(test, [&]{
        test.renderer.SetTransform(&scaled);
        base();
        test.renderer.SetTransform(nullptr);
    }), first) << "a transform";

    // the same mesh compiled again is another geometry: a released address never matches (D2)
    {
        ReuseQuad again(test.renderer, 0, 0, 20, 10, red);
        EXPECT_NE(recordedSignature(test, [&]{
            test.renderer.RenderGeometry(again.handle, Rml::Vector2f(10, 10), 0);
            test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), textureHandle);
        }), first) << "a geometry compiled again";
        test.renderer.ReleaseGeometry(again.handle);
    }

    // texels uploaded again: the content version
    const std::vector<uint8_t> texels(4 * 4 * 4, 128);
    ASSERT_TRUE(texture->update(texels.data(), texels.size()));
    SystemRender::commit();
    const uint64_t uploaded = recordedSignature(test, base);
    EXPECT_NE(uploaded, first) << "an upload into a drawn texture";
    EXPECT_EQ(recordedSignature(test, base), uploaded);

    test.renderer.ReleaseTexture(textureHandle);
    test.renderer.ReleaseGeometry(quad.handle);
}

TEST(RenderFrameReuse, SignatureRelativeToAnOriginAndLiveSources){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(160, 90);
    ASSERT_TRUE(test.isValid());
    ReuseQuad quad(test.renderer, 0, 0, 20, 10, Rml::ColourbPremultiplied(0, 0, 255, 255));
    uint32_t live = 0;

    // the same content 30 px further, relative to an origin 30 px further: one key (an element that
    // moves keeps its layer scope's key)
    const uint64_t here = recordedSignature(test, [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), 0); },
                                            Rml::Vector2i(0, 0), live);
    const uint64_t there = recordedSignature(test, [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), 0); },
                                             Rml::Vector2i(30, 0), live);
    EXPECT_EQ(here, there);
    EXPECT_EQ(live, 0u);
    // not under a transform: the matrix holds where the element is
    const Rml::Matrix4f rotated = Rml::Matrix4f::RotateZ(0.5f);
    const uint64_t rotatedHere = recordedSignature(test, [&]{
        test.renderer.SetTransform(&rotated);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), 0);
    }, Rml::Vector2i(0, 0), live);
    const uint64_t rotatedThere = recordedSignature(test, [&]{
        test.renderer.SetTransform(&rotated);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), 0);
    }, Rml::Vector2i(30, 0), live);
    EXPECT_NE(rotatedHere, rotatedThere);

    // a texture whose texels change without an upload the renderer sees
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("frame-reuse-video", 4, 4);
    ASSERT_TRUE(owned);
    owned->liveContent = true;
    const Rml::TextureHandle video = reinterpret_cast<Rml::TextureHandle>(owned.release());
    recordedSignature(test, [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), video); }, Rml::Vector2i(0, 0), live);
    EXPECT_EQ(live, (uint32_t)RmlRenderer::LIVE_VIDEO);

    test.renderer.ReleaseTexture(video);
    test.renderer.ReleaseGeometry(quad.handle);
}
