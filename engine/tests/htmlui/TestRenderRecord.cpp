// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Drawn resources stay usable until endFrame even when RmlUi releases their
// handles and source meshes first. An empty next frame clears earlier content.

#include "TestRenderCommon.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <algorithm>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // RenderTestContext frames drawn by hand
    struct Quad {
        Rml::Mesh mesh;
        Rml::CompiledGeometryHandle handle = 0;
        Quad(RmlRenderer& renderer, float x, float y, float w, float h, Rml::ColourbPremultiplied colour){
            Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour, Rml::Vector2f(0, 0), Rml::Vector2f(1, 1));
            handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
        }
        // as RmlUi: the handle goes, then the mesh, overwritten so a read of it shows
        void release(RmlRenderer& renderer){
            renderer.ReleaseGeometry(handle);
            handle = 0;
            for (Rml::Vertex& vertex : mesh.vertices){
                vertex.position = Rml::Vector2f(-12345.f, 98765.f);
                vertex.colour = Rml::ColourbPremultiplied(7, 7, 7, 7);
            }
            for (int& index : mesh.indices)
                index = 0x7fffffff;
            mesh = Rml::Mesh();
        }
    };

    // frames until the renderer's programs exist (an empty context)
    bool warmUp(RenderTestContext& test){
        return test.renderUntilDrawn();
    }

    std::vector<uint8_t> drawFrame(RenderTestContext& test, const std::function<void(RmlRenderer&)>& draw){
        beginTestFrame();
        EXPECT_TRUE(test.renderer.beginFrame(test.target, test.contextSize));
        draw(test.renderer);
        EXPECT_TRUE(test.renderer.endFrame());
        SystemRender::commit();
        std::vector<uint8_t> layer;
        EXPECT_TRUE(test.readLayer(layer));
        return layer;
    }

}

TEST(RenderRecordTest, ResourcesReleasedMidFrameLiveUntilTheReplay){
    std::vector<uint8_t> layer;
    {
        RenderTestContext test(360, 240);
        if (!test.isValid())
            GTEST_SKIP() << "no GPU";
        ASSERT_TRUE(warmUp(test));
        const size_t geometryBefore = test.renderer.getLiveGeometryCount();
        const size_t texturesBefore = test.renderer.getLiveTextureCount();

        layer = drawFrame(test, [&](RmlRenderer& r){
            // a geometry drawn, then released with its mesh; one compiled after it is another
            Quad red(r, 10, 10, 40, 30, Rml::ColourbPremultiplied(255, 0, 0, 255));
            r.RenderGeometry(red.handle, Rml::Vector2f(0, 0), 0);
            red.release(r);
            Quad green(r, 60, 10, 40, 30, Rml::ColourbPremultiplied(0, 255, 0, 255));
            r.RenderGeometry(green.handle, Rml::Vector2f(0, 0), 0);
            green.release(r);

            // a texture released after its draw
            std::vector<Rml::byte> blue(8 * 8 * 4);
            for (size_t i = 0; i < blue.size(); i += 4){
                blue[i + 2] = 255;
                blue[i + 3] = 255;
            }
            const Rml::TextureHandle texture = r.GenerateTexture(blue, Rml::Vector2i(8, 8));
            ASSERT_NE(texture, 0u);
            Quad textured(r, 110, 10, 40, 30, Rml::ColourbPremultiplied(255, 255, 255, 255));
            r.RenderGeometry(textured.handle, Rml::Vector2f(0, 0), texture);
            r.ReleaseTexture(texture);
            textured.release(r);

            // a filter released after its composite (a box shadow's blur)
            Rml::Dictionary parameters;
            parameters["sigma"] = Rml::Variant(2.0f);
            const Rml::CompiledFilterHandle blur = r.CompileFilter("blur", parameters);
            ASSERT_NE(blur, 0u);
            const Rml::LayerHandle layer = r.PushLayer();
            Quad blurred(r, 160, 10, 40, 30, Rml::ColourbPremultiplied(255, 255, 0, 255));
            r.RenderGeometry(blurred.handle, Rml::Vector2f(0, 0), 0);
            blurred.release(r);
            const Rml::CompiledFilterHandle filters[] = {blur};
            r.CompositeLayers(layer, 0, Rml::BlendMode::Blend, Rml::Span<const Rml::CompiledFilterHandle>(filters, 1));
            r.PopLayer();
            r.ReleaseFilter(blur);

            // a saved layer drawn elsewhere, then released
            r.PushLayer();
            r.EnableScissorRegion(true);
            r.SetScissorRegion(Rml::Rectanglei::FromPositionSize(Rml::Vector2i(220, 10), Rml::Vector2i(40, 30)));
            Quad saved(r, 220, 10, 40, 30, Rml::ColourbPremultiplied(0, 128, 255, 255));
            r.RenderGeometry(saved.handle, Rml::Vector2f(0, 0), 0);
            saved.release(r);
            const Rml::TextureHandle savedTexture = r.SaveLayerAsTexture();
            ASSERT_NE(savedTexture, 0u);
            r.PopLayer();
            r.EnableScissorRegion(false);
            Quad copy(r, 270, 10, 40, 30, Rml::ColourbPremultiplied(255, 255, 255, 255));
            r.RenderGeometry(copy.handle, Rml::Vector2f(0, 0), savedTexture);
            copy.release(r);
            r.ReleaseTexture(savedTexture);

            // a mask image released after its composite
            const Rml::LayerHandle masked = r.PushLayer();
            Quad content(r, 10, 60, 80, 40, Rml::ColourbPremultiplied(255, 0, 255, 255));
            r.RenderGeometry(content.handle, Rml::Vector2f(0, 0), 0);
            content.release(r);
            r.PushLayer();
            Quad maskShape(r, 10, 60, 40, 40, Rml::ColourbPremultiplied(255, 255, 255, 255));
            r.RenderGeometry(maskShape.handle, Rml::Vector2f(0, 0), 0);
            maskShape.release(r);
            const Rml::CompiledFilterHandle mask = r.SaveLayerAsMaskImage();
            ASSERT_NE(mask, 0u);
            r.PopLayer();
            const Rml::CompiledFilterHandle maskFilters[] = {mask};
            r.CompositeLayers(masked, 0, Rml::BlendMode::Blend, Rml::Span<const Rml::CompiledFilterHandle>(maskFilters, 1));
            r.PopLayer();
            r.ReleaseFilter(mask);
        });

        EXPECT_EQ(test.renderer.getLiveGeometryCount(), geometryBefore);
        EXPECT_EQ(test.renderer.getLiveTextureCount(), texturesBefore);
        ASSERT_FALSE(layer.empty());
        const PixelProbe probe{layer, 360};
        EXPECT_TRUE(probe.is(30, 25, 255, 0, 0)) << "the released geometry was drawn with its data";
        EXPECT_TRUE(probe.is(80, 25, 0, 255, 0));
        EXPECT_TRUE(probe.is(130, 25, 0, 0, 255)) << "the released texture was drawn";
        EXPECT_TRUE(probe.is(180, 25, 255, 255, 0)) << "the released filtered geometry was drawn";
        EXPECT_GT(probe.at(158, 25)[3], 10) << "the released blur still spreads beyond the geometry";
        EXPECT_LT(probe.at(158, 25)[3], 150);
        EXPECT_TRUE(probe.is(290, 25, 0, 128, 255)) << "the saved layer was filled before its draw";
        EXPECT_TRUE(probe.is(20, 80, 255, 0, 255)) << "masked in";
        EXPECT_TRUE(probe.is(70, 80, 0, 0, 0, 0)) << "masked out";
    }
}

TEST(RenderRecordTest, AnEmptyFrameClearsTheLayer){
    RenderTestContext test(64, 48);
    if (!test.isValid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(warmUp(test));
    // something in layer 0 first, so the clear shows
    drawFrame(test, [](RmlRenderer& r){
        Quad quad(r, 0, 0, 64, 48, Rml::ColourbPremultiplied(255, 255, 255, 255));
        r.RenderGeometry(quad.handle, Rml::Vector2f(0, 0), 0);
        quad.release(r);
    });
    const std::vector<uint8_t> layer = drawFrame(test, [](RmlRenderer&){});
    ASSERT_EQ(layer.size(), (size_t)64 * 48 * 4);
    EXPECT_TRUE(std::all_of(layer.begin(), layer.end(), [](uint8_t v){ return v == 0; })) << "layer 0 is cleared";
}
