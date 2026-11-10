// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SVG currentColor (phase-2 §3.7.5, hook H8): SvgPreprocess classification (monochrome ->
// tint, mixed -> substitute, none), paint overrides, foreignObject dropped, rasterizeSvg; and
// live <svg> elements through the SVG plugin's source hook: the white raster multiplied by the
// CSS color (a color change makes no new texture), the mixed SVG rasterized again with the new
// color, CSS paint properties on <svg>, <svg src> files.

#include "RuntimeTestCommon.h"

#include "htmlui/compat/runtime/CompatProperties.h"
#include "htmlui/compat/runtime/SvgElementHook.h"
#include "htmlui/compat/runtime/SvgPreprocess.h"

#include "tinyxml2.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    const char* kLucideHeart =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
        "stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        "<path d=\"M2 9.5a5.5 5.5 0 0 1 9.591-3.676.56.56 0 0 0 .818 0A5.49 5.49 0 0 1 22 9.5c0 2.29-1.5 4-3 5.5l-5.492 5.313a2 2 0 0 1-3 .019L5 15c-1.5-1.5-3-3.2-3-5.5\"/>"
        "<circle cx=\"12\" cy=\"12\" r=\".5\" fill=\"currentColor\"/></svg>";

    bool hasCode(const SvgResult& result, DiagCode code){
        for (const Diagnostic& diagnostic : result.diagnostics){
            if (diagnostic.code == code)
                return true;
        }
        return false;
    }

    std::string rootAttribute(const std::string& xml, const char* name){
        tinyxml2::XMLDocument document;
        if (document.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS)
            return "<parse error>";
        const char* value = document.RootElement()->Attribute(name);
        return value ? value : "";
    }

}

// ---------------------------------------------------------------- SvgPreprocess

TEST(CompatSvgPreprocess, MonochromeIsTinted){
    const SvgResult result = SvgPreprocess::process(kLucideHeart, SvgUse::Inline);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.tint, SvgTint::Color);
    EXPECT_EQ(result.xml.find("currentColor"), std::string::npos) << result.xml;
    EXPECT_EQ(rootAttribute(result.xml, "stroke"), "#ffffff");
    EXPECT_NE(result.xml.find("fill=\"#ffffff\""), std::string::npos) << result.xml;
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.withColor("#ff0000ff"), result.xml);
}

TEST(CompatSvgPreprocess, CurrentColorSpellingsAndStyles){
    // any case, attributes and style=""; a stop with a color and a gradient fill
    const SvgResult result = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' fill='none'>"
        "<defs><linearGradient id='g'><stop offset='0' stop-color='CURRENTCOLOR'/><stop offset='1' style='stop-color: currentcolor'/></linearGradient></defs>"
        "<rect width='10' height='10' style='fill: url(#g); stroke: CurrentColor'/></svg>", SvgUse::File);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.tint, SvgTint::Color);
    EXPECT_EQ(result.xml.find("urrent"), std::string::npos) << result.xml;
    EXPECT_EQ(result.xml.find("URRENT"), std::string::npos) << result.xml;
}

TEST(CompatSvgPreprocess, MixedIsSubstituted){
    const SvgResult result = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'><rect width='10' height='10' fill='CurrentColor'/>"
        "<rect x='10' width='10' height='10' fill='#0000ff'/></svg>", SvgUse::File);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.tint, SvgTint::Substitute);
    EXPECT_TRUE(hasCode(result, codes::SvgRecolor));
    EXPECT_NE(result.xml.find("fill=\"currentColor\""), std::string::npos) << result.xml;   // LunaSVG's spelling
    const std::string colored = result.withColor("#00ff00ff");
    EXPECT_EQ(rootAttribute(colored, "color"), "#00ff00ff");
    EXPECT_EQ(colored.find("htmlui-current-color"), std::string::npos);
}

TEST(CompatSvgPreprocess, DefaultBlackFillAndStopsAreFixedColors){
    // a path with no fill anywhere is black: the icon is not monochrome
    const SvgResult noFill = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><path d='M0 0h10v10z' stroke='currentColor'/></svg>", SvgUse::Inline);
    EXPECT_EQ(noFill.tint, SvgTint::Substitute);
    // ...unless the fill comes from an ancestor
    const SvgResult inherited = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><g fill='none'><path d='M0 0h10v10z' stroke='currentColor'/></g></svg>",
        SvgUse::Inline);
    EXPECT_EQ(inherited.tint, SvgTint::Color);
    // shapes in <defs>/<clipPath> are not drawn where they stand; a line has no fill
    const SvgResult defs = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' fill='none'><clipPath id='c'><rect width='5' height='5'/></clipPath>"
        "<line x2='10' y2='10' stroke='currentColor'/></svg>", SvgUse::Inline);
    EXPECT_EQ(defs.tint, SvgTint::Color);
    // a stop without stop-color is black
    const SvgResult stop = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' fill='none'><linearGradient id='g'><stop offset='0'/></linearGradient>"
        "<rect width='10' height='10' stroke='currentColor'/></svg>", SvgUse::Inline);
    EXPECT_EQ(stop.tint, SvgTint::Substitute);
}

TEST(CompatSvgPreprocess, InnerColorAndStyleSheets){
    // an inner element redefining color changes what currentColor means below it
    const SvgResult innerColor = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' fill='none'><g color='red'><rect width='10' height='10' stroke='currentColor'/></g></svg>",
        SvgUse::Inline);
    EXPECT_EQ(innerColor.tint, SvgTint::Substitute);
    // a <style> sheet with a fixed paint
    const SvgResult sheet = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' fill='none'><style>/* c */ .a { fill: #123456; }</style>"
        "<rect class='a' width='10' height='10' stroke='currentColor'/></svg>", SvgUse::Inline);
    EXPECT_EQ(sheet.tint, SvgTint::Substitute);
    // a <style> sheet with currentColor only
    const SvgResult sheetCurrent = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' fill='none'><style>.a { stroke: CurrentColor }</style>"
        "<rect class='a' width='10' height='10'/></svg>", SvgUse::Inline);
    EXPECT_EQ(sheetCurrent.tint, SvgTint::Color);
    EXPECT_NE(sheetCurrent.xml.find("stroke: #ffffff"), std::string::npos) << sheetCurrent.xml;
}

TEST(CompatSvgPreprocess, NoCurrentColor){
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10' fill='#ff0000'/></svg>";
    const SvgResult result = SvgPreprocess::process(svg, SvgUse::File);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.tint, SvgTint::None);
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_NE(result.xml.find("fill=\"#ff0000\""), std::string::npos);
}

TEST(CompatSvgPreprocess, PaintOverridesAreRootAttributes){
    SvgPaintOverrides overrides;
    overrides.strokeWidth = "1.5";
    overrides.strokeLinecap = "square";
    EXPECT_FALSE(overrides.empty());
    EXPECT_NE(overrides.key(), SvgPaintOverrides().key());
    const SvgResult result = SvgPreprocess::process(kLucideHeart, SvgUse::Inline, overrides);
    EXPECT_EQ(rootAttribute(result.xml, "stroke-width"), "1.5");
    EXPECT_EQ(rootAttribute(result.xml, "stroke-linecap"), "square");
    EXPECT_EQ(result.tint, SvgTint::Color);

    // a CSS fill of currentColor turns a fixed-color file into a tinted one; a fixed CSS
    // stroke makes Lucide's currentColor stroke a fixed color
    SvgPaintOverrides fill;
    fill.fill = "currentColor";
    const SvgResult tinted = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10'/></svg>", SvgUse::File, fill);
    EXPECT_EQ(tinted.tint, SvgTint::Color);
    SvgPaintOverrides stroke;
    stroke.stroke = "#ff0000ff";
    stroke.fill = "none";
    const SvgResult fixed = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' stroke='currentColor'><rect width='10' height='10'/></svg>", SvgUse::File, stroke);
    EXPECT_EQ(fixed.tint, SvgTint::None);
}

TEST(CompatSvgPreprocess, ForeignObjectIsDropped){
    const SvgResult result = SvgPreprocess::process(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10' fill='#00ff00'/>"
        "<g><foreignObject width='10' height='10'><div xmlns='http://www.w3.org/1999/xhtml'>x</div></foreignObject></g></svg>", SvgUse::File);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.foreignObjects, 1u);
    EXPECT_TRUE(hasCode(result, codes::SvgUnsupportedContent));
    EXPECT_EQ(result.xml.find("foreignObject"), std::string::npos);
    EXPECT_EQ(result.xml.find("<div"), std::string::npos);
}

TEST(CompatSvgPreprocess, InvalidInputIsLeftAlone){
    const SvgResult broken = SvgPreprocess::process("<svg><rect></svg>", SvgUse::File);
    EXPECT_FALSE(broken.ok);
    EXPECT_FALSE(broken.error.empty());
    EXPECT_EQ(broken.xml, "<svg><rect></svg>");
    EXPECT_EQ(broken.tint, SvgTint::None);
    const SvgResult notSvg = SvgPreprocess::process("<html/>", SvgUse::File);
    EXPECT_FALSE(notSvg.ok);
    EXPECT_EQ(SvgPreprocess::colorText(255, 0, 16, 128), "#ff001080");
}

TEST(CompatSvgPreprocess, RasterizeAtAnySize){
    const std::string square = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10' width='10' height='10'>"
                               "<rect width='10' height='10' fill='#ff0000'/></svg>";
    std::vector<uint8_t> rgba = htmlui::rasterizeSvg(square, 40, 20);
    ASSERT_EQ(rgba.size(), 40u * 20u * 4u);
    // preserveAspectRatio xMidYMid meet: a 20 x 20 square centred, transparent at the sides
    EXPECT_EQ(rgba[(10 * 40 + 20) * 4 + 0], 255);
    EXPECT_EQ(rgba[(10 * 40 + 20) * 4 + 3], 255);
    EXPECT_EQ(rgba[(10 * 40 + 2) * 4 + 3], 0);
    EXPECT_EQ(rgba[(10 * 40 + 37) * 4 + 3], 0);

    // no viewBox: the synthesized one scales the content like a CSS image
    const std::string noViewBox = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='5' height='10' fill='#00ff00'/></svg>";
    rgba = htmlui::rasterizeSvg(noViewBox, 20, 20);
    ASSERT_EQ(rgba.size(), 20u * 20u * 4u);
    EXPECT_EQ(rgba[(10 * 20 + 5) * 4 + 1], 255);
    EXPECT_EQ(rgba[(10 * 20 + 15) * 4 + 3], 0);

    // premultiplied output
    const std::string half = "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'><rect width='4' height='4' fill='#ffffff' fill-opacity='0.5'/></svg>";
    rgba = htmlui::rasterizeSvg(half, 4, 4);
    ASSERT_EQ(rgba.size(), 64u);
    EXPECT_NEAR(rgba[0], 128, 1);
    EXPECT_NEAR(rgba[3], 128, 1);

    // laid out at a logical size of its own, scaled to the pixels (a background tile of 9.5 x 20 px rasterized at
    // 10 x 20, as Skia rasterizes Chromium's SVG pattern tile): meet places a 9.5 px square 5.25 px down, the scale
    // stretches it across the 10 columns
    rgba = htmlui::rasterizeSvg(square, 10, 20, 9.5f, 20.0f);
    ASSERT_EQ(rgba.size(), 10u * 20u * 4u);
    auto alpha = [&](int x, int y){ return (int)rgba[(size_t(y) * 10 + size_t(x)) * 4 + 3]; };
    EXPECT_EQ(alpha(5, 4), 0);
    EXPECT_NEAR(alpha(5, 5), 191, 3);
    EXPECT_EQ(alpha(5, 6), 255);
    EXPECT_EQ(alpha(0, 10), 255);
    EXPECT_EQ(alpha(9, 10), 255);
    EXPECT_NEAR(alpha(5, 14), 191, 3);
    EXPECT_EQ(alpha(5, 15), 0);
    // the same logical and pixel size: as without one
    EXPECT_EQ(htmlui::rasterizeSvg(square, 40, 20, 40.0f, 20.0f), htmlui::rasterizeSvg(square, 40, 20));

    TestSystem::get().clearLogs();
    EXPECT_TRUE(htmlui::rasterizeSvg(square, 0, 10).empty());
    EXPECT_TRUE(htmlui::rasterizeSvg(square, 10, 20000).empty());
    EXPECT_TRUE(htmlui::rasterizeSvg("nope", 10, 10).empty());
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] SVG rasterization"), 3u);
}

// ---------------------------------------------------------------- live <svg> elements

namespace {

    class CompatSvgElementTest : public RuntimeContextTest {
    protected:
        // the draws of a frame whose texture has these dimensions
        std::vector<RecordingRenderInterface::Draw> drawsOf(Rml::Vector2i size) const{
            std::vector<RecordingRenderInterface::Draw> result;
            for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
                const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
                if (texture && texture->dimensions == size)
                    result.push_back(draw);
            }
            return result;
        }

        Rml::ColourbPremultiplied vertexColour(const RecordingRenderInterface::Draw& draw) const{
            const RecordingRenderInterface::Geometry* geometry = recording.getGeometry(draw.geometry);
            return geometry && !geometry->vertices.empty() ? geometry->vertices[0].colour : Rml::ColourbPremultiplied();
        }

        const uint8_t* pixel(const RecordingRenderInterface::Draw& draw, int x, int y) const{
            const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
            return &texture->pixels[((size_t)y * texture->dimensions.x + x) * 4];
        }
    };

}

TEST_F(CompatSvgElementTest, MonochromeInlineSvgIsTintedByColor){
    ASSERT_NE(show(std::string("<div id='box' style='color: #ff0000;'>") + kLucideHeart + "</div>", "svg { display: block; }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsOf(Rml::Vector2i(24, 24));
    ASSERT_EQ(draws.size(), 1u);
    // rasterized white, drawn red
    const RecordingRenderInterface::Texture* texture = recording.getTexture(draws[0].texture);
    bool white = false;
    for (size_t i = 0; i < texture->pixels.size(); i += 4){
        if (texture->pixels[i + 3] == 255){
            white = true;
            EXPECT_EQ(texture->pixels[i + 0], 255);
            EXPECT_EQ(texture->pixels[i + 1], 255);
            EXPECT_EQ(texture->pixels[i + 2], 255);
        }
    }
    EXPECT_TRUE(white);
    EXPECT_EQ(vertexColour(draws[0]), Rml::ColourbPremultiplied(255, 0, 0, 255));

    // a color change: new geometry, the same texture (no new raster)
    const size_t texturesBefore = recording.getLiveTextureCount();
    const Rml::TextureHandle handle = draws[0].texture;
    byId("box")->SetProperty("color", "#0000ff");
    frame();
    draws = drawsOf(Rml::Vector2i(24, 24));
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].texture, handle);
    EXPECT_EQ(recording.getLiveTextureCount(), texturesBefore);
    EXPECT_EQ(vertexColour(draws[0]), Rml::ColourbPremultiplied(0, 0, 255, 255));

    // opacity and image-color multiply in: on the svg itself, a leaf drawing one thing (on the box it would be a group,
    // drawn into a layer composited with the opacity: phase 3, p3-paint)
    byId("box")->GetFirstChild()->SetProperty("opacity", "0.5");
    frame();
    EXPECT_FLOAT_EQ(byId("box")->GetFirstChild()->GetDrawOpacity(), 0.5f);
    draws = drawsOf(Rml::Vector2i(24, 24));
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_NEAR(vertexColour(draws[0]).blue, 128, 1);
    EXPECT_NEAR(vertexColour(draws[0]).alpha, 128, 1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
}

TEST_F(CompatSvgElementTest, MixedSvgFileIsRasterizedWithTheColor){
    ASSERT_NE(show("<svg id='icon' src='svg/mixed.svg' style='color: #00ff00;'/>", "svg { display: block; }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsOf(Rml::Vector2i(20, 10));
    ASSERT_EQ(draws.size(), 1u);
    const uint8_t* left = pixel(draws[0], 5, 5);
    const uint8_t* right = pixel(draws[0], 15, 5);
    EXPECT_EQ(left[0], 0); EXPECT_EQ(left[1], 255); EXPECT_EQ(left[2], 0);
    EXPECT_EQ(right[0], 0); EXPECT_EQ(right[1], 0); EXPECT_EQ(right[2], 255);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_VERBOSE, "HTMLUI-SVG-0001"), 1u);

    // a new color: rasterized again
    byId("icon")->SetProperty("color", "#ff0000");
    frame();
    draws = drawsOf(Rml::Vector2i(20, 10));
    ASSERT_EQ(draws.size(), 1u);
    left = pixel(draws[0], 5, 5);
    EXPECT_EQ(left[0], 255); EXPECT_EQ(left[1], 0);
    // the diagnostic once per file
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_VERBOSE, "HTMLUI-SVG-0001"), 1u);
}

TEST_F(CompatSvgElementTest, SvgFileIsPlacedByObjectFit){
    // an SVG file is an image: object-fit and object-position place it (phase 3, p3-paint:
    // Rml::ComputeObjectFitRect, as <img>), rasterized at the size it is drawn at
    ASSERT_NE(show("<svg id='icon' src='svg/mixed.svg' style='width: 40px; height: 40px; object-fit: contain;'/>", "svg { display: block; }"), nullptr);
    auto quad = [&](const RecordingRenderInterface::Draw& draw){
        const RecordingRenderInterface::Geometry* geometry = recording.getGeometry(draw.geometry);
        Rml::Vector2f low(1e9f), high(-1e9f), uvLow(1e9f), uvHigh(-1e9f);
        for (const Rml::Vertex& v : geometry->vertices){
            low = Rml::Math::Min(low, v.position); high = Rml::Math::Max(high, v.position);
            uvLow = Rml::Math::Min(uvLow, v.tex_coord); uvHigh = Rml::Math::Max(uvHigh, v.tex_coord);
        }
        return std::make_pair(Rml::Rectanglef::FromCorners(low, high), Rml::Rectanglef::FromCorners(uvLow, uvHigh));
    };
    // contain: the 20 x 10 image at 40 x 20, centred vertically
    std::vector<RecordingRenderInterface::Draw> draws = drawsOf(Rml::Vector2i(40, 20));
    ASSERT_EQ(draws.size(), 1u);
    auto [rect, uv] = quad(draws[0]);
    EXPECT_FLOAT_EQ(rect.Top(), 10.0f);
    EXPECT_FLOAT_EQ(rect.Height(), 20.0f);
    EXPECT_FLOAT_EQ(rect.Width(), 40.0f);

    // cover: 80 x 40, the middle half shown
    byId("icon")->SetProperty("object-fit", "cover");
    frame();
    draws = drawsOf(Rml::Vector2i(80, 40));
    ASSERT_EQ(draws.size(), 1u);
    std::tie(rect, uv) = quad(draws[0]);
    EXPECT_FLOAT_EQ(rect.Width(), 40.0f);
    EXPECT_FLOAT_EQ(rect.Height(), 40.0f);
    EXPECT_FLOAT_EQ(uv.Left(), 0.25f);
    EXPECT_FLOAT_EQ(uv.Right(), 0.75f);

    // none, at the right bottom: 20 x 10 in the corner
    byId("icon")->SetProperty("object-fit", "none");
    byId("icon")->SetProperty("object-position", "right bottom");
    frame();
    draws = drawsOf(Rml::Vector2i(20, 10));
    ASSERT_EQ(draws.size(), 1u);
    std::tie(rect, uv) = quad(draws[0]);
    EXPECT_FLOAT_EQ(rect.Width(), 20.0f);
    EXPECT_FLOAT_EQ(rect.Height(), 10.0f);
    // (the geometry is relative to the content box)
    EXPECT_FLOAT_EQ(rect.Left(), 20.0f);
    EXPECT_FLOAT_EQ(rect.Top(), 30.0f);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
}

TEST_F(CompatSvgElementTest, MonochromeFileAndForeignObject){
    ASSERT_NE(show("<svg src='svg/mono.svg' style='color: #808000;'/><svg src='svg/foreign.svg'/>", "svg { display: block; }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> mono = drawsOf(Rml::Vector2i(20, 20));
    ASSERT_EQ(mono.size(), 1u);
    EXPECT_EQ(pixel(mono[0], 10, 10)[0], 255);
    EXPECT_EQ(vertexColour(mono[0]), Rml::ColourbPremultiplied(0x80, 0x80, 0, 255));
    std::vector<RecordingRenderInterface::Draw> foreign = drawsOf(Rml::Vector2i(10, 10));
    ASSERT_EQ(foreign.size(), 1u);
    EXPECT_EQ(pixel(foreign[0], 5, 5)[1], 255);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "HTMLUI-SVG-0002"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "compat/runtime/svg/foreign.svg"), 1u);
}

TEST_F(CompatSvgElementTest, CssPaintPropertiesApplyToTheRoot){
    // a shape without a fill takes the CSS fill of the <svg>; stroke-width widens the stroke
    const std::string svg = "<svg width='20' height='20' viewBox='0 0 20 20'><rect x='5' y='5' width='10' height='10'/></svg>";
    ASSERT_NE(show("<div id='a'>" + svg + "</div><div id='b' class='wide'>" + svg + "</div>",
                   "svg { display: block; fill: #00ff00; } .wide svg { stroke: #0000ff; stroke-width: 6; }"), nullptr);
    std::vector<RecordingRenderInterface::Draw> draws = drawsOf(Rml::Vector2i(20, 20));
    ASSERT_EQ(draws.size(), 2u);
    const uint8_t* plain = pixel(draws[0], 10, 10);
    EXPECT_EQ(plain[1], 255);
    EXPECT_EQ(plain[0], 0);
    // outside the rect: transparent without a stroke, blue with the wide one
    EXPECT_EQ(pixel(draws[0], 3, 10)[3], 0);
    EXPECT_EQ(pixel(draws[1], 3, 10)[2], 255);

    // the property changing re-processes the source
    Rml::Element* svgElement = byId("a")->GetFirstChild();
    svgElement->SetProperty("fill", "#ff0000");
    frame();
    draws = drawsOf(Rml::Vector2i(20, 20));
    ASSERT_EQ(draws.size(), 2u);
    bool red = false;
    for (const RecordingRenderInterface::Draw& draw : draws)
        red = red || pixel(draw, 10, 10)[0] == 255;
    EXPECT_TRUE(red);
}


TEST_F(CompatSvgElementTest, ContextsDoNotShareCacheEntries){
    // the same inline SVG in two contexts: each draws with its own render manager's geometry
    // and texture, and one context going away leaves the other intact
    const std::string body = std::string("<div>") + kLucideHeart + "</div>";
    ASSERT_NE(show(body, "svg { display: block; }"), nullptr);
    RuntimeRecording otherRecording;
    Rml::Context* other = Rml::CreateContext(name + "-svg-other", Rml::Vector2i(200, 200), &otherRecording);
    ASSERT_NE(other, nullptr);
    Rml::ElementDocument* second = other->LoadDocumentFromMemory(
        "<rml><head><style>body { display: block; width: 200px; height: 200px; } svg, div { display: block; }</style></head><body>" + body +
        "</body></rml>", "compat/runtime/other.rml");
    ASSERT_NE(second, nullptr);
    second->Show();
    other->Update();
    otherRecording.beginFrame();
    other->Render();
    EXPECT_EQ(otherRecording.getDraws().size(), 1u);
    EXPECT_NE(otherRecording.getGeometry(otherRecording.getDraws()[0].geometry), nullptr);

    other->UnloadAllDocuments();
    other->Update();
    Rml::RemoveContext(name + "-svg-other");
    Rml::ReleaseTextures(&otherRecording);
    frame();
    EXPECT_EQ(drawsOf(Rml::Vector2i(24, 24)).size(), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
}
