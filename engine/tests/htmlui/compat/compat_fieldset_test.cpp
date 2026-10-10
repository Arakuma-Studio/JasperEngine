// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// <fieldset> with a rendered legend (phase-2 spec 3.10, p2-integrate): the legend sits on the
// top border, the border is centred on it and leaves a gap for it. The layout and the cut are
// HtmlFieldsetElement's; the lowered and interrupted border is the fork's
// Element::GetBorderTopCut (FORK.md P2-14), tested here on the geometry RmlUi generates.

#include "RuntimeTestCommon.h"

#include "htmlui/compat/runtime/HtmlFieldsetElement.h"

#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderBox.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
namespace hc = jasperengine::htmlui::compat;

namespace {

    bool inTriangle(Rml::Vector2f p, Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c){
        const auto side = [](Rml::Vector2f p1, Rml::Vector2f p2, Rml::Vector2f p3){
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };
        const float d1 = side(p, a, b), d2 = side(p, b, c), d3 = side(p, c, a);
        const bool negative = (d1 < 0) || (d2 < 0) || (d3 < 0);
        const bool positive = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(negative && positive);
    }

    // is the point inside a triangle of the mesh whose vertices have this colour?
    bool covered(const std::vector<Rml::Vertex>& vertices, const std::vector<int>& indices, Rml::Vector2f translation, Rml::Vector2f point,
                 Rml::ColourbPremultiplied colour){
        for (size_t i = 0; i + 2 < indices.size(); i += 3){
            const Rml::Vertex& a = vertices[(size_t)indices[i]];
            const Rml::Vertex& b = vertices[(size_t)indices[i + 1]];
            const Rml::Vertex& c = vertices[(size_t)indices[i + 2]];
            if (a.colour != colour || b.colour != colour || c.colour != colour)
                continue;
            if (inTriangle(point, a.position + translation, b.position + translation, c.position + translation))
                return true;
        }
        return false;
    }

    class CompatFieldsetTest : public RuntimeContextTest {
    protected:
        // what the user-agent sheet gives a fieldset and its legend, on white
        const std::string style =
            "fieldset { display: block; margin: 0 2px; padding: 6px 12px 10px; border: 2px #c5c5c5; }"
            "legend { display: block; padding: 0 2px; }"
            "#after { display: block; }";
        const Rml::ColourbPremultiplied grey{0xc5, 0xc5, 0xc5, 0xff};

        bool borderAt(Rml::Vector2f point, Rml::ColourbPremultiplied colour){
            for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
                const RecordingRenderInterface::Geometry* geometry = recording.getGeometry(draw.geometry);
                if (geometry && covered(geometry->vertices, geometry->indices, draw.translation, point, colour))
                    return true;
            }
            return false;
        }

        float top(Rml::Element* element){ return element->GetAbsoluteOffset(Rml::BoxArea::Border).y; }
        float left(Rml::Element* element){ return element->GetAbsoluteOffset(Rml::BoxArea::Border).x; }
        float heightOf(Rml::Element* element){ return element->GetBox().GetSize(Rml::BoxArea::Border).y; }
        float widthOf(Rml::Element* element){ return element->GetBox().GetSize(Rml::BoxArea::Border).x; }
    };

}

TEST_F(CompatFieldsetTest, LegendSitsOnTheTopBorder){
    ASSERT_NE(show("<fieldset id=\"f\" class=\"htmlui-legend\"><legend id=\"l\">Legend</legend><div id=\"content\">Content</div></fieldset>"
                   "<div id=\"after\">after</div>", style), nullptr);
    frame();
    Rml::Element* fieldset = byId("f");
    Rml::Element* legend = byId("l");
    Rml::Element* content = byId("content");
    ASSERT_NE(rmlui_dynamic_cast<hc::HtmlFieldsetElement*>(fieldset), nullptr);
    EXPECT_EQ(static_cast<hc::HtmlFieldsetElement*>(fieldset)->getRenderedLegend(), legend);

    // layout: the legend at the top of the border box, the content after it and the padding
    EXPECT_FLOAT_EQ(top(legend), top(fieldset));
    EXPECT_FLOAT_EQ(top(content), top(legend) + heightOf(legend) + 6.0f);
    EXPECT_FLOAT_EQ(heightOf(fieldset), heightOf(legend) + 6.0f + heightOf(content) + 10.0f + 2.0f);
    EXPECT_FLOAT_EQ(top(byId("after")), top(fieldset) + heightOf(fieldset));

    // paint: the box starts where the 2px border is centred on the legend, with a gap as wide
    // as the legend's text and padding
    const float inset = std::floor((heightOf(legend) - 2.0f) * 0.5f);
    ASSERT_GT(inset, 4.0f);
    const Rml::RenderBox painted = fieldset->GetRenderBox(Rml::BoxArea::Border);
    EXPECT_FLOAT_EQ(painted.GetBorderOffset().y, inset);
    EXPECT_NEAR(painted.GetFillSize().y, heightOf(fieldset) - inset, 0.5f);   // (render boxes are whole pixels)
    const float textWidth = (float)Rml::ElementUtilities::GetStringWidth(legend, "Legend");
    const float gapBegin = left(legend) - left(fieldset);
    EXPECT_FLOAT_EQ(painted.GetBorderTopGap().x, gapBegin);
    EXPECT_NEAR(painted.GetBorderTopGap().y, gapBegin + 2.0f + textWidth + 2.0f, 1.0f);
    EXPECT_LT(painted.GetBorderTopGap().y, widthOf(fieldset) * 0.5f);
    // the box as laid out (what overflow clips to) is whole
    const Rml::RenderBox laidOut = fieldset->GetRenderBox(Rml::BoxArea::Border, 0, false);
    EXPECT_FLOAT_EQ(laidOut.GetBorderOffset().y, 0.0f);
    EXPECT_NEAR(laidOut.GetFillSize().y, heightOf(fieldset), 0.5f);
    EXPECT_EQ(laidOut.GetBorderTopGap(), Rml::Vector2f(0.0f, 0.0f));

    // the geometry: border on the legend's middle line left and right of the gap, none in it,
    // none at the top of the box, the sides from the line down
    const float lineY = top(fieldset) + inset + 1.0f;
    const float x0 = left(fieldset);
    EXPECT_TRUE(borderAt({x0 + 6.0f, lineY}, grey));
    EXPECT_FALSE(borderAt({x0 + gapBegin + 3.0f, lineY}, grey));
    EXPECT_FALSE(borderAt({x0 + painted.GetBorderTopGap().y - 2.0f, lineY}, grey));
    EXPECT_TRUE(borderAt({x0 + painted.GetBorderTopGap().y + 3.0f, lineY}, grey));
    EXPECT_TRUE(borderAt({x0 + widthOf(fieldset) - 6.0f, lineY}, grey));
    EXPECT_FALSE(borderAt({x0 + 6.0f, top(fieldset) + 1.0f}, grey));
    EXPECT_FALSE(borderAt({x0 + 1.0f, top(fieldset) + inset - 2.0f}, grey));
    EXPECT_TRUE(borderAt({x0 + 1.0f, top(fieldset) + inset + 6.0f}, grey));
    EXPECT_TRUE(borderAt({x0 + 40.0f, top(fieldset) + heightOf(fieldset) - 1.0f}, grey));
}

TEST_F(CompatFieldsetTest, AuthorBorderPaddingAndRadius){
    ASSERT_NE(show("<fieldset id=\"f\" class=\"htmlui-legend\"><legend id=\"l\">Legend</legend><div id=\"content\">Content</div></fieldset>",
                   style + "#f { border: 4px #cc3333; border-radius: 10px; padding: 20px; background-color: #eeeeff; }"
                           "#l { margin-left: 40px; }"), nullptr);
    frame();
    Rml::Element* fieldset = byId("f");
    Rml::Element* legend = byId("l");
    const Rml::ColourbPremultiplied red(0xcc, 0x33, 0x33, 0xff);
    const Rml::ColourbPremultiplied fill(0xee, 0xee, 0xff, 0xff);

    EXPECT_FLOAT_EQ(top(legend), top(fieldset));
    EXPECT_FLOAT_EQ(top(byId("content")), top(legend) + heightOf(legend) + 20.0f);
    EXPECT_FLOAT_EQ(left(legend), left(fieldset) + 4.0f + 20.0f + 40.0f);

    const float inset = std::floor((heightOf(legend) - 4.0f) * 0.5f);
    const float lineY = top(fieldset) + inset + 2.0f;
    const float x0 = left(fieldset);
    EXPECT_TRUE(borderAt({x0 + 30.0f, lineY}, red));
    EXPECT_FALSE(borderAt({left(legend) + 10.0f, lineY}, red));
    EXPECT_TRUE(borderAt({x0 + widthOf(fieldset) - 30.0f, lineY}, red));
    // the background starts at the line too, the legend's half above it is bare
    EXPECT_TRUE(borderAt({x0 + 30.0f, lineY + 12.0f}, fill));
    EXPECT_FALSE(borderAt({x0 + 30.0f, top(fieldset) + 1.0f}, fill));
    EXPECT_FALSE(borderAt({x0 + 30.0f, top(fieldset) + 1.0f}, red));
}

TEST_F(CompatFieldsetTest, TheGapFollowsTheLegend){
    ASSERT_NE(show("<fieldset id=\"f\" class=\"htmlui-legend\"><legend id=\"l\">Legend</legend>Content</fieldset>", style), nullptr);
    frame();
    Rml::Element* fieldset = byId("f");
    const float before = fieldset->GetRenderBox(Rml::BoxArea::Border).GetBorderTopGap().y;
    const float inset = fieldset->GetRenderBox(Rml::BoxArea::Border).GetBorderOffset().y;
    const float lineY = top(fieldset) + inset + 1.0f;
    EXPECT_TRUE(borderAt({left(fieldset) + before + 60.0f, lineY}, grey));

    // a longer legend: the fieldset's own box is the same, its border is generated again
    byId("l")->SetInnerRML("A much longer legend text");
    frame();
    frame();
    const float after = fieldset->GetRenderBox(Rml::BoxArea::Border).GetBorderTopGap().y;
    EXPECT_GT(after, before + 80.0f);
    EXPECT_FALSE(borderAt({left(fieldset) + before + 60.0f, lineY}, grey));
    EXPECT_TRUE(borderAt({left(fieldset) + after + 6.0f, lineY}, grey));

    // without its legend the fieldset is a plain box again
    byId("l")->GetParentNode()->RemoveChild(byId("l"));
    frame();
    frame();
    const Rml::RenderBox plain = fieldset->GetRenderBox(Rml::BoxArea::Border);
    EXPECT_FLOAT_EQ(plain.GetBorderOffset().y, 0.0f);
    EXPECT_EQ(plain.GetBorderTopGap(), Rml::Vector2f(0.0f, 0.0f));
    EXPECT_TRUE(borderAt({left(fieldset) + 40.0f, top(fieldset) + 1.0f}, grey));
}

// A legend with a width of its own leaves a gap as wide as its box; one that is not the first
// child, or out of flow, is not on the border.
TEST_F(CompatFieldsetTest, WhichLegendIsRendered){
    ASSERT_NE(show("<fieldset id=\"a\" class=\"htmlui-legend\"><legend id=\"la\" style=\"width: 120px;\">Legend</legend>x</fieldset>"
                   "<fieldset id=\"b\" class=\"htmlui-legend\">text first<legend id=\"lb\">Legend</legend></fieldset>"
                   "<fieldset id=\"c\" class=\"htmlui-legend\"><legend id=\"lc\" style=\"position: absolute;\">Legend</legend>x</fieldset>"
                   "<fieldset id=\"d\" class=\"htmlui-legend\"> <legend id=\"ld\">Legend</legend>x</fieldset>", style), nullptr);
    frame();
    const Rml::RenderBox a = byId("a")->GetRenderBox(Rml::BoxArea::Border);
    EXPECT_FLOAT_EQ(a.GetBorderTopGap().y - a.GetBorderTopGap().x, 124.0f);   // 120px + its padding
    EXPECT_GT(a.GetBorderOffset().y, 0.0f);

    for (const char* id : {"b", "c"}){
        SCOPED_TRACE(id);
        const Rml::RenderBox box = byId(id)->GetRenderBox(Rml::BoxArea::Border);
        EXPECT_FLOAT_EQ(box.GetBorderOffset().y, 0.0f);
        EXPECT_EQ(box.GetBorderTopGap(), Rml::Vector2f(0.0f, 0.0f));
    }
    // white space before the legend does not count
    EXPECT_GT(byId("d")->GetRenderBox(Rml::BoxArea::Border).GetBorderOffset().y, 0.0f);
    EXPECT_FLOAT_EQ(top(byId("ld")), top(byId("d")));
}

// An .rml document's fieldset (no htmlui-legend class from the HTML stage) is a plain element.
TEST_F(CompatFieldsetTest, RmlFieldsetIsUnchanged){
    ASSERT_NE(show("<fieldset id=\"f\"><legend id=\"l\">Legend</legend><div id=\"content\">Content</div></fieldset>", style), nullptr);
    frame();
    Rml::Element* fieldset = byId("f");
    EXPECT_FLOAT_EQ(top(byId("l")), top(fieldset) + 2.0f + 6.0f);
    const Rml::RenderBox box = fieldset->GetRenderBox(Rml::BoxArea::Border);
    EXPECT_FLOAT_EQ(box.GetBorderOffset().y, 0.0f);
    EXPECT_EQ(box.GetBorderTopGap(), Rml::Vector2f(0.0f, 0.0f));
    EXPECT_TRUE(borderAt({left(fieldset) + 40.0f, top(fieldset) + 1.0f}, grey));
}

// The fork's geometry (FORK.md P2-14): a gap in the top edge, within its straight part.
TEST(CompatBorderTopGap, MeshLeavesTheGap){
    const Rml::ColourbPremultiplied colours[4] = {
        Rml::ColourbPremultiplied(255, 0, 0, 255), Rml::ColourbPremultiplied(0, 255, 0, 255),
        Rml::ColourbPremultiplied(0, 0, 255, 255), Rml::ColourbPremultiplied(255, 255, 0, 255)};
    const Rml::ColourbPremultiplied red = colours[0];
    const auto topAt = [&](const Rml::RenderBox& box, float x){
        Rml::Mesh mesh;
        Rml::MeshUtilities::GenerateBackgroundBorder(mesh, box, Rml::ColourbPremultiplied(0, 0), colours);
        return covered(mesh.vertices, mesh.indices, Rml::Vector2f(0, 0), Rml::Vector2f(x, box.GetBorderOffset().y + 2.0f), red);
    };

    // a 200x100 padding box with 4px borders, at (0, 10)
    Rml::RenderBox box(Rml::Vector2f(200, 100), Rml::Vector2f(0, 10), Rml::EdgeSizes{4, 4, 4, 4}, Rml::CornerSizes{0, 0, 0, 0});
    for (float x : {2.0f, 50.0f, 100.0f, 206.0f})
        EXPECT_TRUE(topAt(box, x)) << x;

    box.SetBorderTopGap(Rml::Vector2f(40, 90));
    for (float x : {2.0f, 20.0f, 38.0f, 92.0f, 150.0f, 206.0f})
        EXPECT_TRUE(topAt(box, x)) << x;
    for (float x : {42.0f, 65.0f, 88.0f})
        EXPECT_FALSE(topAt(box, x)) << x;

    // rounded corners: the gap is kept out of the arcs
    Rml::RenderBox round(Rml::Vector2f(200, 100), Rml::Vector2f(0, 0), Rml::EdgeSizes{4, 4, 4, 4}, Rml::CornerSizes{20, 20, 20, 20});
    round.SetBorderTopGap(Rml::Vector2f(0, 60));
    EXPECT_FALSE(topAt(round, 30.0f));
    EXPECT_FALSE(topAt(round, 58.0f));
    EXPECT_TRUE(topAt(round, 62.0f));
    EXPECT_TRUE(topAt(round, 150.0f));

    // an empty or reversed gap is no gap; one past the edge is clamped
    box.SetBorderTopGap(Rml::Vector2f(90, 40));
    EXPECT_TRUE(topAt(box, 65.0f));
    box.SetBorderTopGap(Rml::Vector2f(150, 900));
    EXPECT_TRUE(topAt(box, 100.0f));
    EXPECT_FALSE(topAt(box, 180.0f));
    EXPECT_NE(box, Rml::RenderBox(Rml::Vector2f(200, 100), Rml::Vector2f(0, 10), Rml::EdgeSizes{4, 4, 4, 4}, Rml::CornerSizes{0, 0, 0, 0}));
}
