// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S93 P2 (R6, R7, AC7-AC10): a point hits an element only where the element is painted. The
// clips of the ancestors are tested in each ancestor's projected space (rounded corners
// included), and clip-path (the html-shape mask the compat layer writes for it) clips the hit
// test with its exact geometry; mask-image does not (CSS Masking 1). The documents go through the
// whole HTML pipeline into a context drawn by a recording render interface (the transforms are
// resolved when the context renders).

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"
#include "htmlui/compat/runtime/HtmlShapeDecorator.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    struct HitPage {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::shared_ptr<const CompatDocument> compiled;
        std::string path;

        HitPage(const std::string& name, const std::string& style, const std::string& body)
            : ok(HtmlUI::acquire()), path("@htmlui/test/hit-transform/" + name + ".html"){
            if (!ok)
                return;
            context = Rml::CreateContext("hit-transform", Rml::Vector2i(800, 600), &recording);
            VirtualFiles::add(path, "<!DOCTYPE html><html><head><style>html, body { margin: 0; width: 800px; height: 600px; } "
                                    "div { position: absolute; box-sizing: border-box; } " + style +
                                    "</style></head><body>" + body + "</body></html>");
            CompatOptions options;
            options.viewport = CompatSize{800, 600};
            options.fork = ForkFeatures::present();
            document = DocumentLoader::load(context, path, options, &compiled);
            if (document){
                document->Show();
                frame();
            }
        }
        ~HitPage(){
            if (document)
                document->Close();
            if (context){
                context->Update();
                Rml::RemoveContext("hit-transform");
            }
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            VirtualFiles::remove(path);
            DocumentLoader::clearCache();
            if (ok)
                HtmlUI::release();
        }

        // layout, then a render: the transform states are made when the elements are drawn
        void frame(){
            context->Update();
            context->Render();
        }

        Rml::Element* byId(const char* id) const{
            return document ? document->GetElementById(id) : nullptr;
        }

        // the element under the point, as a DOM id ("" for an element without one)
        std::string idAt(float x, float y) const{
            Rml::Element* hit = context->GetElementAtPoint(Rml::Vector2f(x, y));
            return hit ? hit->GetId() : std::string("<none>");
        }
    };

}

// U13: a button translated up into view out of the part of an 'overflow: hidden' viewport that
// clips it is hit where it is drawn; a box translated down out of the viewport is not hit where
// its layout box still is. 6.3 tested the button's projected (layout) point against the
// viewport's rectangle: the drawn button missed, the hidden box was hit.
TEST(HitTest, TranslatedChildInClippedParent){
    HitPage page("translated",
                 "#viewport { left: 100px; top: 100px; width: 300px; height: 300px; overflow: hidden; background: #333; } "
                 "#button { left: 50px; top: 260px; width: 120px; height: 120px; transform: translateY(-200px); background: red; } "
                 "#below { left: 200px; top: 100px; width: 80px; height: 80px; transform: translateY(250px); background: blue; }",
                 "<div id='viewport'><div id='button'></div><div id='below'></div></div>");
    ASSERT_TRUE(page.document);
    // the button's layout box: x 150..270, y 360..480 (its lower part outside the viewport); drawn y 160..280
    EXPECT_EQ(page.idAt(210, 220), "button") << "the button's drawn centre";
    EXPECT_EQ(page.idAt(155, 165), "button") << "near the drawn top left";
    EXPECT_EQ(page.idAt(265, 275), "button") << "near the drawn bottom right";
    EXPECT_EQ(page.idAt(210, 380), "viewport") << "its layout position (nothing is drawn there)";
    // the box below: layout y 200..280 inside the viewport, drawn y 450..530 under its clip
    EXPECT_EQ(page.idAt(340, 240), "viewport") << "the box's layout position";
    EXPECT_NE(page.idAt(340, 490), "below") << "the box where it is drawn, clipped by the viewport";
}

// U2: a child placed outside a scaled panel that does not clip, inside a full-page clipping
// stage, is hit at its drawn rectangle (sampled at 9 points), not where its layout box is.
TEST(HitTest, ChildOutsideScaledParent){
    HitPage page("scaled",
                 "#stage { left: 0; top: 0; width: 800px; height: 600px; overflow: hidden; } "
                 "#panel { left: 500px; top: 200px; width: 300px; height: 200px; transform: scale(0.75); background: #444; } "
                 "#child { right: -60px; top: 50px; width: 100px; height: 100px; background: green; }",
                 "<div id='stage'><div id='panel'><div id='child'></div></div></div>");
    ASSERT_TRUE(page.document);
    // panel centre (650, 300); the child's layout box x 760..860, y 250..350, drawn x 732.5..807.5,
    // y 262.5..337.5 (the stage clips it at x 800)
    for (float x : {736.0f, 766.0f, 796.0f}){
        for (float y : {266.0f, 300.0f, 334.0f}){
            EXPECT_EQ(page.idAt(x, y), "child") << "drawn point (" << x << ", " << y << ")";
        }
    }
    EXPECT_NE(page.idAt(728, 300), "child") << "left of the drawn child";
    EXPECT_NE(page.idAt(766, 258), "child") << "above the drawn child";
}

// A corner of an ancestor with 'overflow: hidden; border-radius: 40px' does not hit the child
// behind the curve; the same under a transform on the clipping ancestor.
TEST(HitTest, RoundedClipCorner){
    HitPage page("rounded",
                 "#round { left: 100px; top: 100px; width: 200px; height: 200px; overflow: hidden; border-radius: 40px; } "
                 "#scaled { left: 400px; top: 100px; width: 200px; height: 200px; overflow: hidden; border-radius: 40px; "
                 "          transform: scale(1.5); transform-origin: 0 0; } "
                 ".fill { left: 0; top: 0; width: 200px; height: 200px; background: blue; }",
                 "<div id='round'><div class='fill' id='fill'></div></div><div id='scaled'><div class='fill' id='scaledFill'></div></div>");
    ASSERT_TRUE(page.document);
    EXPECT_NE(page.idAt(103, 103), "fill") << "outside the top-left curve";
    EXPECT_NE(page.idAt(297, 297), "fill") << "outside the bottom-right curve";
    EXPECT_EQ(page.idAt(115, 115), "fill") << "inside the top-left curve";
    EXPECT_EQ(page.idAt(200, 200), "fill") << "the centre";
    // scaled 1.5 about its top left (drawn at x 400..700, y 100..400): the curve's corner region is
    // 60 px; (404, 104) is outside it, (425, 125) inside
    EXPECT_NE(page.idAt(404, 104), "scaledFill") << "outside the scaled top-left curve";
    EXPECT_EQ(page.idAt(425, 125), "scaledFill") << "inside the scaled curve";
    EXPECT_EQ(page.idAt(550, 250), "scaledFill") << "the scaled centre";
}

namespace {

    struct ClipPathCase {
        const char* name;
        const char* clipPath;   // the CSS value
        const char* shape;      // the same shape as html-shape() takes it, for the reference raster
    };

    // a pentagram: its centre is inside under nonzero, a hole under evenodd
    const ClipPathCase kClipPathCases[] = {
        {"polygonNonzero", "polygon(50% 5%, 20% 99%, 95% 39%, 5% 39%, 80% 99%)", "polygon(50% 5%, 20% 99%, 95% 39%, 5% 39%, 80% 99%)"},
        {"polygonEvenodd", "polygon(evenodd, 50% 5%, 20% 99%, 95% 39%, 5% 39%, 80% 99%)",
         "polygon(evenodd, 50% 5%, 20% 99%, 95% 39%, 5% 39%, 80% 99%)"},
        {"circle", "circle(40% at 60% 50%)", "circle(40% at 60% 50%)"},
        {"ellipse", "ellipse(45% 30% at 50% 50%)", "ellipse(45% 30% at 50% 50%)"},
        {"insetRound", "inset(20px 30px round 40px)", "inset(20px 30px round 40px)"},
        {"path", "path('M 10 190 L 10 100 Q 10 10 100 10 C 150 10 190 50 190 100 A 40 60 0 0 1 150 190 Z')",
         "path('M 10 190 L 10 100 Q 10 10 100 10 C 150 10 190 50 190 100 A 40 60 0 0 1 150 190 Z')"},
        {"xywh", "xywh(20px 30px 60% 50% round 10px)", "xywh(20px 30px 60% 50% round 10px)"},
        {"rect", "rect(10% 180px 170px 20px round 5px)", "rect(10% 180px 170px 20px round 5px)"},
        {"calcPoints", "polygon(0 0, calc(100% - 20px) 0, 100% calc(100% - 20px), calc(20px + 10%) 100%)",
         "polygon(0 0, calc(100% - 20px) 0, 100% calc(100% - 20px), calc(20px + 10%) 100%)"},
        {"calcCircle", "circle(calc(50% - 10px) at calc(100% - 100px) 50%)", "circle(calc(50% - 10px) at calc(100% - 100px) 50%)"},
    };

    class ClipPathShapes : public ::testing::TestWithParam<ClipPathCase> {};

}

// R7, U32, U23 (shapes): every basic shape clips the hit test as the mask draws it, calc()
// lengths included. 64 sample points of the 200 px box agree with the rendered mask (its
// rasterized shape). 6.3 hit the whole box (the corners) and drew no mask for a calc() shape.
TEST_P(ClipPathShapes, MatchesTheMask){
    const ClipPathCase& c = GetParam();
    HitPage page(std::string("clip-") + c.name, std::string("#shape { left: 100px; top: 100px; width: 200px; height: 200px; background: red; "
                 "clip-path: ") + c.clipPath + "; }", "<div id='shape'></div>");
    ASSERT_TRUE(page.document);
    Rml::Element* element = page.byId("shape");
    ASSERT_NE(element, nullptr);
    // the shape reaches the decorator as written (6.3 reduced calc(100% - 20px) to 100%, with a warning)
    ASSERT_TRUE(page.compiled);
    for (const Diagnostic& diagnostic : page.compiled->diagnostics)
        EXPECT_NE(diagnostic.severity, Severity::Warning) << c.name << ": " << diagnostic.message;

    BasicShape shape;
    std::string error;
    ASSERT_TRUE(BasicShape::parse(c.shape, shape, &error)) << error;
    const std::string svg = shape.toSvg(200.0f, 200.0f, element, &error);
    ASSERT_FALSE(svg.empty()) << error;
    const std::shared_ptr<const std::vector<uint8_t>> mask = decorators::rasterizeCached(svg, 200, 200);
    ASSERT_TRUE(mask && mask->size() == 200u * 200u * 4u);

    int inside = 0, agree = 0;
    for (int row = 0; row < 8; row++){
        for (int column = 0; column < 8; column++){
            const int lx = 12 + 25 * column, ly = 12 + 25 * row;
            const bool masked = (*mask)[((size_t)ly * 200 + (size_t)lx) * 4 + 3] >= 128;
            const bool hit = page.idAt(100.0f + (float)lx + 0.5f, 100.0f + (float)ly + 0.5f) == "shape";
            inside += masked ? 1 : 0;
            agree += masked == hit ? 1 : 0;
            EXPECT_EQ(hit, masked) << c.name << ": sample (" << lx << ", " << ly << ") " << (masked ? "inside" : "outside") << " the mask";
        }
    }
    EXPECT_EQ(agree, 64) << c.name;
    // the samples cover both sides of the edge
    EXPECT_GT(inside, 0) << c.name;
    EXPECT_LT(inside, 64) << c.name;
}

INSTANTIATE_TEST_SUITE_P(HitTest, ClipPathShapes, ::testing::ValuesIn(kClipPathCases),
                         [](const ::testing::TestParamInfo<ClipPathCase>& info){ return std::string(info.param.name); });

// CSS Masking 1: a mask image clips the drawing only; the whole box is still hit.
TEST(HitTest, MaskImageDoesNotClip){
    HitPage page("mask-image",
                 "#masked { left: 100px; top: 100px; width: 200px; height: 200px; background: red; "
                 "mask-image: linear-gradient(to right, transparent, black); }",
                 "<div id='masked'></div>");
    ASSERT_TRUE(page.document);
    EXPECT_EQ(page.idAt(103, 103), "masked") << "a fully transparent part of the mask";
    EXPECT_EQ(page.idAt(297, 297), "masked");
}

// A clip-path on an ancestor clips its descendants' hit test too, through its transform.
TEST(HitTest, ClipPathOnAncestorClipsDescendants){
    HitPage page("clip-ancestor",
                 "#frame { left: 100px; top: 100px; width: 200px; height: 200px; clip-path: circle(50%); transform: rotate(45deg); } "
                 "#inner { left: 0; top: 0; width: 200px; height: 200px; background: green; }",
                 "<div id='frame'><div id='inner'></div></div>");
    ASSERT_TRUE(page.document);
    EXPECT_EQ(page.idAt(200, 200), "inner") << "the centre";
    EXPECT_EQ(page.idAt(200, 110), "inner") << "inside the circle near its top";
    // the rotated box's corner reaches (200, 59); the circle does not
    EXPECT_NE(page.idAt(200, 70), "inner") << "a corner of the rotated box outside the circle";
}
