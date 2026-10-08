// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat decorators instanced from RCSS (phase-2 §3.11.3): html-layer (tile geometry,
// nearest sampling for pixelated, SVG sources rasterized at the drawn size x the context's
// pixel ratio and tinted for currentColor, rounded clip boxes through stencil clip masks,
// fixed attachment), html-shape (the shape rasterized at the box size; in mask-image on the
// GPU), html-border-image (nine-slice geometry; on the GPU), invalid arguments, no leaks.

#include "RuntimeTestCommon.h"
#include "TestRenderCommon.h"

#include "RmlFontEngine.h"
#include "htmlui/compat/runtime/HtmlShapeDecorator.h"

#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Factory.h>

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    class CompatDecoratorTest : public RuntimeContextTest {
    protected:
        // the draws whose texture was loaded from a source containing `name`
        std::vector<RecordingRenderInterface::Draw> drawsFromSource(const std::string& name) const{
            std::vector<RecordingRenderInterface::Draw> result;
            for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
                const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
                if (texture && texture->source.find(name) != std::string::npos)
                    result.push_back(draw);
            }
            return result;
        }

        // the draws of generated textures (callback textures) of a size
        std::vector<RecordingRenderInterface::Draw> drawsOfGenerated(Rml::Vector2i size) const{
            std::vector<RecordingRenderInterface::Draw> result;
            for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
                const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
                if (texture && texture->source.empty() && texture->dimensions == size)
                    result.push_back(draw);
            }
            return result;
        }

        float maxVertexX(const RecordingRenderInterface::Draw& draw) const{
            float x = -1e9f;
            for (const Rml::Vertex& vertex : recording.getGeometry(draw.geometry)->vertices)
                x = std::max(x, vertex.position.x);
            return x;
        }

        float minVertexX(const RecordingRenderInterface::Draw& draw) const{
            float x = 1e9f;
            for (const Rml::Vertex& vertex : recording.getGeometry(draw.geometry)->vertices)
                x = std::min(x, vertex.position.x);
            return x;
        }
    };

}

TEST_F(CompatDecoratorTest, InstancersAreRegistered){
    for (const char* name : {"html-layer", "html-shape", "html-border-image"})
        EXPECT_NE(Rml::Factory::GetDecoratorInstancer(name), nullptr) << name;
}

TEST_F(CompatDecoratorTest, LayerTilesFromRcss){
    // a 4 x 2 image at 10 x 5, repeated over 40 x 20: 4 x 4 tiles
    ASSERT_NE(show("<div id='box'/>", "#box { width: 40px; height: 20px; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" 10px 5px 0% 0% repeat repeat padding-box border-box scroll); }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsFromSource("wide4x2.png");
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].vertexCount, 64u);
    EXPECT_EQ(draws[0].indexCount, 96u);
    // no nearest filter unless pixelated
    for (const std::string& request : recording.getLoadRequests())
        EXPECT_EQ(request.find("htmlui-filter"), std::string::npos) << request;
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN), 0u);
}

TEST_F(CompatDecoratorTest, LayerSizeFromEmAndPercent){
    // 50% of 40 = 20, 1em = 16: 2 x 2 tiles cover 40 x 32
    ASSERT_NE(show("<div id='box'/>", "#box { width: 40px; height: 32px; font-size: 16px; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" 50% 1em 0% 0% repeat repeat padding-box border-box scroll); }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsFromSource("wide4x2.png");
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].vertexCount, 16u);
}

TEST_F(CompatDecoratorTest, PixelatedRequestsNearestSampling){
    ASSERT_NE(show("<div id='box'/>", "#box { width: 40px; height: 20px; image-rendering: pixelated; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" 40px 20px 0% 0% no-repeat no-repeat padding-box border-box scroll); }"), nullptr);
    const std::vector<std::string>& requests = recording.getLoadRequests();
    EXPECT_TRUE(std::any_of(requests.begin(), requests.end(), [](const std::string& request){
        return request.find("wide4x2.png?htmlui-filter=nearest") != std::string::npos;
    }));
}

TEST_F(CompatDecoratorTest, SvgLayerIsRasterizedAtTheDrawnSizeAndTinted){
    ASSERT_NE(show("<div id='box'/>", "#box { width: 30px; height: 30px; color: #ff0000; "
                   "decorator: html-layer(\"asset://compat/runtime/svg/mono.svg\" 30px 30px 0% 0% no-repeat no-repeat padding-box border-box scroll); }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsOfGenerated(Rml::Vector2i(30, 30));
    ASSERT_EQ(draws.size(), 1u);
    // white raster, red vertices (monochrome currentColor)
    const RecordingRenderInterface::Texture* texture = recording.getTexture(draws[0].texture);
    EXPECT_EQ(texture->pixels[(15 * 30 + 15) * 4 + 0], 255);
    EXPECT_EQ(texture->pixels[(15 * 30 + 15) * 4 + 1], 255);
    EXPECT_EQ(recording.getGeometry(draws[0].geometry)->vertices[0].colour, Rml::ColourbPremultiplied(255, 0, 0, 255));

    // a context at twice the pixel ratio rasterizes twice as many pixels
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    fonts.setRasterScale(context->GetRenderManager(), 2.0f);
    byId("box")->SetProperty("width", "31px");
    frame();
    EXPECT_EQ(drawsOfGenerated(Rml::Vector2i(30, 30)).size(), 0u);
    // the 30px tile at scale 2
    EXPECT_EQ(drawsOfGenerated(Rml::Vector2i(60, 60)).size(), 1u);
    fonts.setRasterScale(context->GetRenderManager(), 1.0f);
}

TEST_F(CompatDecoratorTest, RoundedClipBoxUsesAClipMask){
    ASSERT_NE(show("<div id='square'/>", "#square { width: 40px; height: 20px; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" 10px 5px 0% 0% repeat repeat padding-box border-box scroll); }"), nullptr);
    EXPECT_EQ(recording.clipMaskIntersects, 0);
    ASSERT_NE(show("<div id='round'/>", "#round { width: 40px; height: 20px; border-radius: 8px; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" 10px 5px 0% 0% repeat repeat padding-box border-box scroll); }"), nullptr);
    EXPECT_GE(recording.clipMaskIntersects, 1);
    EXPECT_EQ(drawsFromSource("wide4x2.png").size(), 1u);
}

TEST_F(CompatDecoratorTest, FixedAttachmentFollowsTheViewport){
    ASSERT_NE(show("<div id='box'/>", "#box { position: absolute; left: 50px; top: 40px; width: 40px; height: 40px; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" 20px 20px 50px 40px no-repeat no-repeat padding-box border-box fixed); }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsFromSource("wide4x2.png");
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_FLOAT_EQ(minVertexX(draws[0]), 0.0f);
    EXPECT_FLOAT_EQ(maxVertexX(draws[0]), 20.0f);

    // the element moves right by 10: the tile stays at viewport x 50, cut to the element
    byId("box")->SetProperty("left", "60px");
    frame();
    draws = drawsFromSource("wide4x2.png");
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_FLOAT_EQ(minVertexX(draws[0]), 0.0f);
    EXPECT_FLOAT_EQ(maxVertexX(draws[0]), 10.0f);
}

TEST_F(CompatDecoratorTest, InvalidArgumentsAreReported){
    ASSERT_NE(show("<div id='box'/>", "#box { width: 40px; height: 20px; "
                   "decorator: html-layer(\"asset://compat/runtime/wide4x2.png\" auto auto 0% 0% tile repeat padding-box border-box scroll); }"), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] html-layer(): repeat 'tile repeat'"), 1u);
    EXPECT_TRUE(drawsFromSource("wide4x2.png").empty());

    ASSERT_NE(show("<div id='shape'/>", "#shape { width: 40px; height: 20px; decorator: html-shape(\"hexagon(10px)\"); }"), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] html-shape(): 'hexagon()' is not a basic shape"), 1u);

    // a missing image: reported once, nothing drawn, no crash
    TestSystem::get().clearLogs();
    ASSERT_NE(show("<div id='a'/><div id='b'/>", "#a, #b { width: 20px; height: 20px; "
                   "decorator: html-layer(\"asset://compat/runtime/missing.svg\" auto auto 0% 0% repeat repeat padding-box border-box scroll); }"), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "image 'asset://compat/runtime/missing.svg' not found"), 1u);
}

TEST_F(CompatDecoratorTest, ShapeIsRasterizedAtTheBoxSize){
    ASSERT_NE(show("<div id='box'/>", "#box { width: 40px; height: 40px; decorator: html-shape(\"circle(50% at 50% 50%)\"); }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsOfGenerated(Rml::Vector2i(40, 40));
    ASSERT_EQ(draws.size(), 1u);
    const RecordingRenderInterface::Texture* texture = recording.getTexture(draws[0].texture);
    EXPECT_EQ(texture->pixels[(20 * 40 + 20) * 4 + 3], 255);
    EXPECT_EQ(texture->pixels[(1 * 40 + 1) * 4 + 3], 0);
    EXPECT_EQ(draws[0].vertexCount, 4u);
}

TEST_F(CompatDecoratorTest, BorderImageNineSlice){
    ASSERT_NE(show("<div id='box'/>", "#box { width: 60px; height: 40px; border-width: 4px; border-color: transparent; "
                   "decorator: html-border-image(\"asset://compat/runtime/nine3x3.png\" 1 1 1 1 nofill 1 1 1 1 0 0 0 0 stretch stretch); }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsFromSource("nine3x3.png");
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].vertexCount, 8u * 4u);   // corners and edges
    ASSERT_NE(show("<div id='box'/>", "#box { width: 60px; height: 40px; border-width: 4px; border-color: transparent; "
                   "decorator: html-border-image(\"asset://compat/runtime/nine3x3.png\" 1 1 1 1 fill 1 1 1 1 0 0 0 0 stretch stretch); }"), nullptr);
    draws = drawsFromSource("nine3x3.png");
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].vertexCount, 9u * 4u);
}

TEST(CompatBasicShape, ShapesToSvg){
    BasicShape shape;
    std::string error;
    ASSERT_TRUE(BasicShape::parse("circle(50% at 50% 50%)", shape, &error)) << error;
    EXPECT_EQ(shape.box, Rml::BoxArea::Border);
    std::string svg = shape.toSvg(40, 40);
    EXPECT_NE(svg.find("<circle cx=\"20\" cy=\"20\" r=\"20\""), std::string::npos) << svg;

    // closest-side by default, positions with keywords and edge offsets
    ASSERT_TRUE(BasicShape::parse("circle(at right 10px top)", shape, &error)) << error;
    svg = shape.toSvg(100, 40);
    EXPECT_NE(svg.find("cx=\"90\" cy=\"0\" r=\"0\""), std::string::npos) << svg;
    ASSERT_TRUE(BasicShape::parse("ellipse(farthest-side 25% at left)", shape, &error)) << error;
    svg = shape.toSvg(100, 40);
    EXPECT_NE(svg.find("<ellipse cx=\"0\" cy=\"20\" rx=\"100\" ry=\"10\""), std::string::npos) << svg;

    // inset with round, a reference box keyword
    ASSERT_TRUE(BasicShape::parse("inset(10px 20% round 5px) content-box", shape, &error)) << error;
    EXPECT_EQ(shape.box, Rml::BoxArea::Content);
    svg = shape.toSvg(100, 50);
    EXPECT_NE(svg.find("M25 10H75A5 5 0 0 1 80 15V35A5 5 0 0 1 75 40H25A5 5 0 0 1 20 35V15A5 5 0 0 1 25 10Z"), std::string::npos) << svg;

    // radii larger than the box scale down; elliptical radii with '/'
    ASSERT_TRUE(BasicShape::parse("inset(0 round 100px / 10px)", shape, &error)) << error;
    svg = shape.toSvg(100, 50);
    EXPECT_NE(svg.find("A50 5 0 0 1 100 5"), std::string::npos) << svg;

    // polygon with a fill rule, path, xywh, rect
    ASSERT_TRUE(BasicShape::parse("polygon(evenodd, 50% 0%, 100% 100%, 0 100%)", shape, &error)) << error;
    svg = shape.toSvg(20, 10);
    EXPECT_NE(svg.find("points=\"10,0 20,10 0,10\""), std::string::npos) << svg;
    EXPECT_NE(svg.find("fill-rule=\"evenodd\""), std::string::npos) << svg;
    ASSERT_TRUE(BasicShape::parse("path('M0 0 L10 0 L10 10 Z')", shape, &error)) << error;
    EXPECT_NE(shape.toSvg(10, 10).find("<path d=\"M0 0 L10 0 L10 10 Z\""), std::string::npos);
    ASSERT_TRUE(BasicShape::parse("xywh(10px 5px 50% 20px)", shape, &error)) << error;
    EXPECT_NE(shape.toSvg(100, 40).find("M10 5H60"), std::string::npos);
    ASSERT_TRUE(BasicShape::parse("rect(5px auto 30px 10%)", shape, &error)) << error;
    EXPECT_NE(shape.toSvg(100, 40).find("M10 5H100"), std::string::npos);

    // errors
    EXPECT_FALSE(BasicShape::parse("circle(10px 20px)", shape, &error));
    EXPECT_FALSE(BasicShape::parse("polygon(0 0, 10px 10px)", shape, &error));
    EXPECT_FALSE(BasicShape::parse("inset()", shape, &error));
    EXPECT_FALSE(BasicShape::parse("path(nonzero 'M0 0')", shape, &error));
    EXPECT_FALSE(BasicShape::parse("circle(10px) circle(5px)", shape, &error));
    EXPECT_FALSE(BasicShape::parse("blob(10px)", shape, &error));
    EXPECT_FALSE(BasicShape::parse("", shape, &error));
}

// ---------------------------------------------------------------- GPU

TEST(CompatDecoratorGpu, ShapeMaskAndLayersDraw){
    JASPER_REQUIRE_GPU();
    TestSystem::get().clearLogs();
    {
        RenderTestContext test(256, 128);
        ASSERT_TRUE(test.isValid());
        const std::string rml =
            "<rml><head><style>body { display: block; width: 256px; height: 128px; margin: 0; } "
            "div { display: block; position: absolute; top: 0; width: 64px; height: 64px; } "
            "#mask { left: 0; background-color: #ff0000; mask-image: html-shape(\"circle(50%)\"); } "
            "#layer { left: 64px; decorator: html-layer(\"asset://compat/runtime/pixels2x2.png\" 32px 32px 0% 0% repeat repeat padding-box border-box scroll); "
            "image-rendering: pixelated; } "
            "#frame { left: 128px; width: 48px; height: 48px; border-width: 8px; border-color: transparent; "
            "decorator: html-border-image(\"asset://compat/runtime/nine3x3.png\" 1 1 1 1 fill 1 1 1 1 0 0 0 0 stretch stretch); image-rendering: pixelated; }"
            "</style></head><body><div id='mask'/><div id='layer'/><div id='frame'/></body></rml>";
        ASSERT_NE(test.loadFromMemory(rml, "compat/runtime/decorators.rml"), nullptr);
        ASSERT_TRUE(test.renderUntilDrawn());
        std::vector<uint8_t> rgba;
        ASSERT_TRUE(test.readScreen(rgba));
        PixelProbe probe{rgba, 256};
        // the circle mask: red in the middle, the background in the corner
        EXPECT_TRUE(probe.is(32, 32, 255, 0, 0));
        EXPECT_TRUE(probe.isBackground(2, 2));
        // 32px tiles of red/green/blue/white cells (16px each), nearest
        EXPECT_TRUE(probe.is(64 + 8, 8, 255, 0, 0));
        EXPECT_TRUE(probe.is(64 + 24, 8, 0, 255, 0));
        EXPECT_TRUE(probe.is(64 + 8, 24, 0, 0, 255));
        EXPECT_TRUE(probe.is(64 + 40, 40, 255, 0, 0));   // the next tile
        // the nine-slice of a 3 x 3 image: corners, edges stretched, the middle filled
        EXPECT_TRUE(probe.is(128 + 2, 2, 255, 0, 0));
        EXPECT_TRUE(probe.is(128 + 32, 2, 0, 255, 0));
        EXPECT_TRUE(probe.is(128 + 61, 2, 0, 0, 255));
        EXPECT_TRUE(probe.is(128 + 2, 32, 255, 255, 0));
        EXPECT_TRUE(probe.is(128 + 32, 32, 255, 255, 255));
        EXPECT_TRUE(probe.is(128 + 61, 32, 0, 255, 255));
        EXPECT_TRUE(probe.is(128 + 2, 61, 255, 0, 255));
        EXPECT_TRUE(probe.is(128 + 32, 61, 0, 0, 0));
        EXPECT_TRUE(probe.is(128 + 61, 61, 128, 128, 128));
    }
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
}
