// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RuntimeTestCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/runtime/HtmlShapeDecorator.h"

#include <RmlUi/Core/StyleValue.h>
#include <RmlUi/Core/DecorationTypes.h>

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    BasicShape mixedShape(const std::string& from, const std::string& to, float alpha){
        BasicShape a, b, result;
        EXPECT_TRUE(BasicShape::parse(from, a, nullptr));
        EXPECT_TRUE(BasicShape::parse(to, b, nullptr));
        EXPECT_TRUE(a.interpolate(b, alpha, result));
        return result;
    }

    std::string shapeAnimationCss(const std::string& css){
        SourceFiles files;
        const uint32_t file = files.add("ui/shape-animation.css", css);
        DiagnosticSink sink(files);
        RewriteContext context;
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(CompatOptions(), files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        RewriteResult rewritten = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        std::string result;
        for (const auto& sheet : rewritten.sheets) result += sheet.rcss;
        return result;
    }

    class CompatShapeAnimation : public RuntimeContextTest {
    protected:
        Rml::ElementDocument* show(const std::string& body, const std::string& style = "",
                                  const std::string& url = "compat/runtime/test.rml"){
            // CssRewriter scopes author rules under the root that DocumentLoader adds to HTML.
            return RuntimeContextTest::show("<div id='htmlui-root'>" + body + "</div>", style, url);
        }

        void SetUp() override{
            previousTime = HtmlUIClock::getFixedTime();
            HtmlUIClock::setFixedTime(0.0);
            RuntimeContextTest::SetUp();
        }

        void TearDown() override{
            RuntimeContextTest::TearDown();
            HtmlUIClock::setFixedTime(previousTime);
        }

        std::string property(const char* name){
            const Rml::Property* value = byId("shape")->GetProperty(name);
            return value ? value->ToString() : std::string();
        }

        void at(double seconds){
            HtmlUIClock::setFixedTime(seconds);
            frame();
        }

        std::optional<double> previousTime;
    };

}

TEST(CompatBasicShape, InterpolatesInsetOffsetsAndEllipticalRadii){
    const BasicShape shape = mixedShape("inset(100% 0 0 round 0)", "inset(0 20px 0 0 round 10px / 20px)", .25f);
    EXPECT_EQ(shape.text, "inset(75% 5px 0px 0px round 2.5px 2.5px 2.5px 2.5px / 5px 5px 5px 5px)");
    EXPECT_NE(shape.toSvg(100, 50).find("M2.5 37.5H92.5"), std::string::npos);
}

TEST(CompatBasicShape, InterpolatesCircleAndEllipseComponents){
    const BasicShape circle = mixedShape("circle(0% at left top)", "circle(40% at right 10px bottom 10px)", .5f);
    EXPECT_NE(circle.toSvg(100, 100).find("cx=\"45\" cy=\"45\" r=\"20\""), std::string::npos);
    const BasicShape ellipse = mixedShape("ellipse(10px 20% at 0 0)", "ellipse(50% 40px at 100% 100%)", .5f);
    EXPECT_NE(ellipse.toSvg(100, 50).find("cx=\"50\" cy=\"25\" rx=\"30\" ry=\"25\""), std::string::npos);
}

TEST(CompatBasicShape, InsetPercentageRadiiUseTheReferenceBox){
    BasicShape shape;
    ASSERT_TRUE(BasicShape::parse("inset(10px round 25%)", shape, nullptr));
    EXPECT_NE(shape.toSvg(200, 100).find("M60 10H140A50 25"), std::string::npos);
    shape = mixedShape("inset(10px round 0%)", "inset(10px round 50%)", .5f);
    EXPECT_NE(shape.toSvg(200, 100).find("M60 10H140A50 25"), std::string::npos);
}

TEST(CompatBasicShape, InterpolatesPolygonAndResolvesPercentagesAgainstEachReferenceBox){
    const BasicShape small = mixedShape("polygon(0 0,100% 0,0 100%)", "polygon(20px 20px,100% 100%,100% 100%)", .5f);
    EXPECT_NE(small.toSvg(100, 50).find("points=\"10,10 100,25 50,50\""), std::string::npos);
    const BasicShape large = mixedShape("polygon(0 0,100% 0,0 100%)", "polygon(20px 20px,100% 100%,100% 100%)", .5f);
    EXPECT_NE(large.toSvg(200, 100).find("points=\"10,10 200,50 100,100\""), std::string::npos);
    // The layout may change between animation sampling and generation of the decorator data.
    EXPECT_NE(small.toSvg(200, 100).find("points=\"10,10 200,50 100,100\""), std::string::npos);
}

TEST(CompatBasicShape, IncompatibleShapesAreDiscrete){
    const std::pair<const char*, const char*> pairs[] = {
        {"inset(0)", "circle(50%)"},
        {"circle(10px)", "circle(20px) content-box"},
        {"circle(closest-side)", "circle(40px)"},
        {"circle(closest-side at left)", "circle(closest-side at right)"},
        {"polygon(0 0,100% 0,0 100%)", "polygon(evenodd,0 0,100% 0,0 100%)"},
        {"polygon(0 0,100% 0,0 100%)", "polygon(0 0,100% 0,100% 100%,0 100%)"}
    };
    for (const auto& [from, to] : pairs){
        BasicShape a, b, result;
        ASSERT_TRUE(BasicShape::parse(from, a, nullptr));
        ASSERT_TRUE(BasicShape::parse(to, b, nullptr));
        EXPECT_FALSE(a.interpolate(b, .5f, result)) << from << " -> " << to;
    }
}

TEST(CompatBasicShape, InterruptedShapesKeepPercentagesAndHaveBoundedRetargetingWork){
    BasicShape shape, target;
    ASSERT_TRUE(BasicShape::parse("circle(0% at 50% 50%)", shape, nullptr));
    ASSERT_TRUE(BasicShape::parse("circle(100% at 50% 50%)", target, nullptr));
    for (int i = 0; i < 1000; i++){
        BasicShape next;
        ASSERT_TRUE(shape.interpolate(target, .01f, next));
        shape = std::move(next);
    }
    const std::string svg = shape.toSvg(100, 100);
    const size_t radius = svg.find(" r=\"");
    ASSERT_NE(radius, std::string::npos);
    EXPECT_NEAR(std::stof(svg.substr(radius + 4)), 100.0f * (1.0f - std::pow(.99f, 1000)), .001f);
    EXPECT_TRUE(Rml::Any(shape.unitDependencies() & Rml::Unit::PERCENT));
    EXPECT_NE(shape.toSvg(200, 200).find("r=\"199.99"), std::string::npos);
}

TEST_F(CompatShapeAnimation, ClipPathKeyframesInterpolateAtEverySample){
    const std::string css = shapeAnimationCss(
        "#shape{width:100px;height:100px;clip-path:inset(100% 0 0);animation:reveal 1s linear forwards;}"
        "@keyframes reveal{from{clip-path:inset(100% 0 0)}to{clip-path:inset(0)}}");
    ASSERT_NE(show("<div id='shape'/>", css), nullptr);
    at(.25);
    EXPECT_NE(property("mask-image").find("inset(75%"), std::string::npos) << property("mask-image");
    at(.75);
    EXPECT_NE(property("mask-image").find("inset(25%"), std::string::npos) << property("mask-image");
    // Percentages are resolved at the sampled frame, after a layout change.
    ASSERT_TRUE(byId("shape")->SetProperty("height", "200px"));
    frame();
    frame();
    EXPECT_NE(property("mask-image").find("inset(25%"), std::string::npos) << property("mask-image");
}

TEST_F(CompatShapeAnimation, ClipPathTransitionInterpolates){
    ASSERT_NE(show("<div id='shape'/>", shapeAnimationCss(
        "#shape{width:100px;height:100px;clip-path:circle(10px at 0 0);transition:clip-path 1s linear;}"
        "#shape.open{clip-path:circle(50px at 100% 100%)}")), nullptr);
    byId("shape")->SetClass("open", true);
    frame();
    at(.25);
    EXPECT_NE(property("mask-image").find("circle(20px at 25% 25%)"), std::string::npos) << property("mask-image");
    at(.75);
    EXPECT_NE(property("mask-image").find("circle(40px at 75% 75%)"), std::string::npos) << property("mask-image");
}

TEST_F(CompatShapeAnimation, IncompatibleTransitionsRequireAllowDiscrete){
    for (bool allowDiscrete : {false, true}){
        HtmlUIClock::setFixedTime(0.0);
        ASSERT_NE(show("<div id='shape'/>", shapeAnimationCss(std::string(
            "#shape{width:100px;height:100px;clip-path:circle(10px);transition:clip-path 1s linear") +
            (allowDiscrete ? " allow-discrete" : "") + ";}#shape.open{clip-path:inset(0)}")), nullptr);
        byId("shape")->SetClass("open", true);
        frame();
        at(.25);
        EXPECT_NE(property("mask-image").find(allowDiscrete ? "circle(" : "inset("), std::string::npos) << property("mask-image");
        at(.75);
        EXPECT_NE(property("mask-image").find("inset("), std::string::npos) << property("mask-image");
    }
}

TEST_F(CompatShapeAnimation, EqualUsedValuesKeepTheirChangingPercentageCoefficients){
    ASSERT_NE(show("<div id='shape'/>", shapeAnimationCss(
        "#shape{width:100px;height:100px;animation:reveal 1s linear forwards;}"
        "@keyframes reveal{from{clip-path:inset(10px 0 0)}to{clip-path:inset(10% 0 0)}}")), nullptr);
    at(.25);
    const std::string quarter = property("mask-image");
    at(.75);
    EXPECT_NE(property("mask-image"), quarter);
    EXPECT_NE(property("mask-image").find("calc("), std::string::npos);

    HtmlUIClock::setFixedTime(0.0);
    ASSERT_NE(show("<div id='shape'/>",
        "#shape{width:100px;height:100px;animation:reveal 1s linear forwards;}"
        "@keyframes reveal{from{decorator:html-shape(\"inset(10px 0 0)\")}to{decorator:html-shape(\"inset(10% 0 0)\")}}"), nullptr);
    at(.25);
    ASSERT_TRUE(byId("shape")->SetProperty("height", "200px"));
    at(.75);
    bool found = false;
    for (const auto& draw : recording.getDraws()){
        const auto* texture = recording.getTexture(draw.texture);
        if (texture && texture->dimensions == Rml::Vector2i(100, 200) && !texture->pixels.empty()){
            found = true;
            EXPECT_EQ(texture->pixels[(15 * 100 + 50) * 4 + 3], 0);
            EXPECT_EQ(texture->pixels[(20 * 100 + 50) * 4 + 3], 255);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(CompatShapeAnimation, AnimatedShapeRegeneratesItsRasterTexture){
    ASSERT_NE(show("<div id='shape'/>",
        "#shape{width:100px;height:100px;animation:reveal 1s linear forwards;}"
        "@keyframes reveal{from{decorator:html-shape(\"inset(100% 0 0)\")}to{decorator:html-shape(\"inset(0)\")}}"), nullptr);
    const auto alphaAt = [&](int x, int y){
        for (const auto& draw : recording.getDraws()){
            const auto* texture = recording.getTexture(draw.texture);
            if (texture && texture->dimensions == Rml::Vector2i(100, 100) && !texture->pixels.empty())
                return int(texture->pixels[(y * 100 + x) * 4 + 3]);
        }
        return -1;
    };
    at(.25);
    EXPECT_EQ(alphaAt(50, 50), 0);
    EXPECT_EQ(alphaAt(50, 90), 255);
    at(.75);
    EXPECT_EQ(alphaAt(50, 50), 255);
    EXPECT_EQ(alphaAt(50, 10), 0);
}

TEST_F(CompatShapeAnimation, GradientMaskPositionAndSizeAnimateWithoutChangingTheImage){
    for (const char* gradient : {"linear-gradient(to right,#fff,#0000)", "radial-gradient(circle,#fff,#0000)"}){
        HtmlUIClock::setFixedTime(0.0);
        const std::string css = std::string("#shape{width:100px;height:100px;mask-image:") + gradient +
            ";mask-repeat:no-repeat;animation:move-mask 1s linear forwards;}"
            "@keyframes move-mask{from{mask-position:0% 0%;mask-size:20% 30%}to{mask-position:100% 50%;mask-size:100% 90%}}";
        ASSERT_NE(show("<div id='shape'/>", shapeAnimationCss(css)), nullptr);
        const std::string image = property("mask-image");
        at(.5);
        EXPECT_EQ(property("mask-position-x"), "50%");
        EXPECT_EQ(property("mask-position-y"), "25%");
        EXPECT_EQ(property("mask-size"), "60% 60%");
        EXPECT_EQ(property("mask-image"), image);
    }
}

TEST_F(CompatShapeAnimation, ShapeImageExposesRelativeLengthDependencies){
    ASSERT_NE(show("<div id='shape'/>", "#shape{width:100px;height:100px;mask-image:html-shape(\"circle(1em)\")}"), nullptr);
    const Rml::Property* mask = byId("shape")->GetProperty("mask-image");
    ASSERT_NE(mask, nullptr);
    const Rml::StyleValuePtr value = mask->Get<Rml::StyleValuePtr>();
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(Rml::Any(value->GetUnitDependencies() & Rml::Unit::EM));
}

TEST_F(CompatShapeAnimation, TypedDecoratorColorMixKeepsCurrentColorInterpolation){
    ASSERT_NE(show("<div id='shape'/>",
        "#shape{width:100px;height:100px;color:#ff0000;animation:colors 1s linear forwards;}"
        "@keyframes colors{"
        "from{decorator:horizontal-gradient(color-mix(in srgb,currentcolor 50%,black) black)}"
        "to{decorator:horizontal-gradient(color-mix(in srgb,currentcolor 50%,white) black)}}"), nullptr);
    at(.25);
    const Rml::Property* propertyValue = byId("shape")->GetProperty("decorator");
    ASSERT_NE(propertyValue, nullptr);
    ASSERT_EQ(propertyValue->unit, Rml::Unit::DECORATOR);
    const Rml::DecoratorsPtr declarations = propertyValue->Get<Rml::DecoratorsPtr>();
    ASSERT_NE(declarations, nullptr);
    ASSERT_EQ(declarations->list.size(), 1u);
    int mixedColors = 0;
    for (const auto& [id, value] : declarations->list[0].properties.GetProperties()){
        (void)id;
        if (value.unit == Rml::Unit::COLOUR && value.Get<Rml::Colourb>().red != 0){
            const Rml::Colourb color = value.Get<Rml::Colourb>();
            EXPECT_NEAR(color.red, 160, 1);
            EXPECT_NEAR(color.green, 32, 1);
            EXPECT_NEAR(color.blue, 32, 1);
            mixedColors++;
        }
    }
    EXPECT_EQ(mixedColors, 1);
}
