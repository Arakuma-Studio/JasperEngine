// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// List markers (phase-2 §3.11.6): the text of every list type for indices 1, 4, 9, 27, 3999,
// 4000 (and the edges: 0, negatives), as Chromium formats them; live <li> markers: counters
// (start, reversed, value, nesting), the marker outside the DOM and left of the first line,
// none/inside/string/image markers, no marker on non-list-item displays.
//
// Phase 3 (p3-cascade): with the fork's ListMarkers the li emulation is off (the fork's ::marker
// draws the markers); CompatMarkerElementTest switches it on to keep checking it, and
// CompatForkMarkers checks the present state.

#include "RuntimeTestCommon.h"

#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/runtime/HtmlListItemElement.h"
#include "htmlui/compat/runtime/ListMarkers.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Row {
        const char* type;
        const char* texts[6];   // indices 1, 4, 9, 27, 3999, 4000
    };

    const int kIndices[6] = {1, 4, 9, 27, 3999, 4000};

    const Row kRows[] = {
        {"decimal", {"1. ", "4. ", "9. ", "27. ", "3999. ", "4000. "}},
        {"decimal-leading-zero", {"01. ", "04. ", "09. ", "27. ", "3999. ", "4000. "}},
        {"lower-alpha", {"a. ", "d. ", "i. ", "aa. ", "ewu. ", "ewv. "}},
        {"lower-latin", {"a. ", "d. ", "i. ", "aa. ", "ewu. ", "ewv. "}},
        {"upper-alpha", {"A. ", "D. ", "I. ", "AA. ", "EWU. ", "EWV. "}},
        {"upper-latin", {"A. ", "D. ", "I. ", "AA. ", "EWU. ", "EWV. "}},
        {"lower-roman", {"i. ", "iv. ", "ix. ", "xxvii. ", "mmmcmxcix. ", "4000. "}},
        {"upper-roman", {"I. ", "IV. ", "IX. ", "XXVII. ", "MMMCMXCIX. ", "4000. "}},
        // 24 letters: 27 = αγ, 3999 = ζχο (5, 21, 14), 4000 = ζχπ
        {"lower-greek", {"\xCE\xB1. ", "\xCE\xB4. ", "\xCE\xB9. ", "\xCE\xB1\xCE\xB3. ", "\xCE\xB6\xCF\x87\xCE\xBF. ", "\xCE\xB6\xCF\x87\xCF\x80. "}},
        {"disc", {"\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 ", "\xE2\x80\xA2 "}},
        {"circle", {"\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 ", "\xE2\x97\xA6 "}},
        {"square", {"\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA ", "\xE2\x96\xAA "}},
        {"disclosure-open", {"\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE ", "\xE2\x96\xBE "}},
        {"disclosure-closed", {"\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 ", "\xE2\x96\xB8 "}},
        {"none", {"", "", "", "", "", ""}},
    };

}

TEST(CompatMarkers, EveryTypeAtTheTestIndices){
    for (const Row& row : kRows){
        for (int i = 0; i < 6; i++)
            EXPECT_EQ(ListMarkers::text(row.type, kIndices[i]), row.texts[i]) << row.type << " " << kIndices[i];
    }
}

TEST(CompatMarkers, OutOfRangeFallsBackToDecimal){
    EXPECT_EQ(ListMarkers::text("lower-alpha", 0), "0. ");
    EXPECT_EQ(ListMarkers::text("upper-alpha", -2), "-2. ");
    EXPECT_EQ(ListMarkers::text("lower-roman", 0), "0. ");
    EXPECT_EQ(ListMarkers::text("lower-greek", -1), "-1. ");
    EXPECT_EQ(ListMarkers::text("decimal", -5), "-5. ");
    EXPECT_EQ(ListMarkers::text("decimal-leading-zero", -3), "-03. ");
    EXPECT_EQ(ListMarkers::text("decimal-leading-zero", 0), "00. ");
    EXPECT_EQ(ListMarkers::text("lower-alpha", 26), "z. ");
    EXPECT_EQ(ListMarkers::text("lower-alpha", 702), "zz. ");
    EXPECT_EQ(ListMarkers::text("lower-alpha", 703), "aaa. ");
    EXPECT_EQ(ListMarkers::text("lower-greek", 24), "\xCF\x89. ");
}

TEST(CompatMarkers, TypesAndStrings){
    EXPECT_EQ(ListMarkers::parseType("LOWER-ROMAN"), ListMarkers::Type::LowerRoman);
    EXPECT_EQ(ListMarkers::parseType(" disc "), ListMarkers::Type::Disc);
    EXPECT_EQ(ListMarkers::parseType("armenian"), ListMarkers::Type::Decimal);
    EXPECT_EQ(ListMarkers::parseType("cjk-decimal"), ListMarkers::Type::Decimal);
    EXPECT_EQ(ListMarkers::parseType("-> "), ListMarkers::Type::String);
    EXPECT_TRUE(ListMarkers::isCounter(ListMarkers::Type::LowerGreek));
    EXPECT_FALSE(ListMarkers::isCounter(ListMarkers::Type::Disc));
    EXPECT_STREQ(ListMarkers::typeName(ListMarkers::Type::UpperRoman), "upper-roman");
    EXPECT_STREQ(ListMarkers::typeName(ListMarkers::Type::String), "string");
    // strings are used as written (their spaces kept), quotes removed
    EXPECT_EQ(ListMarkers::text("\xE2\x86\x92 ", 3), "\xE2\x86\x92 ");
    EXPECT_EQ(ListMarkers::text("\"- \"", 3), "- ");
    EXPECT_EQ(ListMarkers::text("'*'", 3), "*");
    EXPECT_EQ(ListMarkers::counter(ListMarkers::Type::UpperRoman, 1994), "MCMXCIV");
    EXPECT_EQ(ListMarkers::counter(ListMarkers::Type::Disc, 1), "");
}

// ---------------------------------------------------------------- live <li> markers

namespace {

    // phase 2's emulation, as it runs while ListMarkers is absent
    class CompatMarkerElementTest : public jasperengine::test::compat::RuntimeContextTest {
    protected:
        void SetUp() override{
            jasperengine::htmlui::compat::HtmlListItemElement::setMarkerEmulation(true);
            RuntimeContextTest::SetUp();
        }
        void TearDown() override{
            RuntimeContextTest::TearDown();
            jasperengine::htmlui::compat::HtmlListItemElement::resetMarkerEmulation();
        }
    };

    jasperengine::htmlui::compat::HtmlListItemElement* item(Rml::Element* element){
        return rmlui_dynamic_cast<jasperengine::htmlui::compat::HtmlListItemElement*>(element);
    }

    const char* kListStyle = "ul, ol { display: block; padding-left: 40px; } li { display: block; } "
                             "ul { list-style-type: disc; } ol { list-style-type: decimal; }";

}

TEST_F(CompatMarkerElementTest, CountersStartReversedAndValue){
    ASSERT_NE(show("<ol id='a'><li id='a1'>one</li><li id='a2'>two</li><li id='a3' value='10'>ten</li><li id='a4'>eleven</li></ol>"
                   "<ol id='b' start='5' reversed=''><li id='b1'>x</li><li id='b2'>y</li></ol>"
                   "<ol id='c' reversed=''><li id='c1'>x</li><li id='c2'>y</li><li id='c3'>z</li></ol>"
                   "<ul><li id='u1'>bullet<ol style='list-style-type: lower-roman;'><li id='n1'>i</li><li id='n2'>ii</li></ol></li></ul>",
                   kListStyle), nullptr);
    frame();
    EXPECT_EQ(item(byId("a1"))->getMarkerText(), "1. ");
    EXPECT_EQ(item(byId("a2"))->getMarkerText(), "2. ");
    EXPECT_EQ(item(byId("a3"))->getMarkerText(), "10. ");
    EXPECT_EQ(item(byId("a4"))->getMarkerText(), "11. ");
    EXPECT_EQ(item(byId("b1"))->getMarkerText(), "5. ");
    EXPECT_EQ(item(byId("b2"))->getMarkerText(), "4. ");
    EXPECT_EQ(item(byId("c1"))->getMarkerText(), "3. ");
    EXPECT_EQ(item(byId("c3"))->getMarkerText(), "1. ");
    EXPECT_EQ(item(byId("u1"))->getMarkerText(), "\xE2\x80\xA2 ");
    // the nested list counts on its own
    EXPECT_EQ(item(byId("n1"))->getMarkerText(), "i. ");
    EXPECT_EQ(item(byId("n2"))->getMarkerText(), "ii. ");
}

TEST_F(CompatMarkerElementTest, MarkerIsOutsideTheDomAndLeftOfTheFirstLine){
    ASSERT_NE(show("<ul id='list'><li id='first'>first</li><li id='second'>second</li></ul>",
                   std::string(kListStyle) + " li:first-child { height: 30px; } li { height: 20px; }"), nullptr);
    frame();
    Rml::Element* first = byId("first");
    // not a DOM child: children, :first-child and the layout are the author's
    EXPECT_EQ(first->GetNumChildren(), 1);   // the text
    EXPECT_EQ(byId("list")->GetNumChildren(), 2);
    EXPECT_FLOAT_EQ(first->GetBox().GetSize().y, 30.0f);
    EXPECT_FLOAT_EQ(byId("second")->GetBox().GetSize().y, 20.0f);

    Rml::Element* marker = item(first)->getMarker();
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->GetTagName(), "marker");
    EXPECT_TRUE(marker->IsVisible());
    const Rml::Vector2f content = first->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f markerAt = marker->GetAbsoluteOffset(Rml::BoxArea::Border);
    const float markerWidth = marker->GetBox().GetSize(Rml::BoxArea::Border).x;
    EXPECT_GT(markerWidth, 2.0f);
    EXPECT_NEAR(markerAt.x + markerWidth, content.x, 0.01f);
    EXPECT_NEAR(markerAt.y, content.y, 0.01f);
    // the marker is drawn (the bullet glyph's text geometry)
    EXPECT_FALSE(recording.getDraws().empty());
}

TEST_F(CompatMarkerElementTest, NoneInsideImagesAndNonListItems){
    ASSERT_NE(show("<ul><li id='none' style='list-style-type: none;'>a</li><li id='inside' style='list-style-position: inside;'>b</li>"
                   "<li id='flex' style='display: flex;'>c</li>"
                   "<li id='string' style='list-style-type: \"-> \";'>d</li>"
                   "<li id='image' style='list-style-image: \"pixels2x2.png\";'>e</li></ul>", kListStyle), nullptr);
    frame();
    frame();
    EXPECT_FALSE(item(byId("none"))->getMarker()->IsVisible());
    EXPECT_FALSE(item(byId("flex"))->getMarker()->IsVisible());
    EXPECT_EQ(item(byId("string"))->getMarkerText(), "-> ");
    EXPECT_TRUE(item(byId("image"))->getMarker()->IsVisible());
    EXPECT_EQ(item(byId("image"))->getMarker()->GetNumChildren(), 2);   // the image and the space

    // inside: the content moves right by the marker; the marker sits where the content was
    Rml::Element* inside = byId("inside");
    Rml::Element* marker = item(inside)->getMarker();
    const float width = marker->GetBox().GetSize(Rml::BoxArea::Border).x;
    EXPECT_NEAR(inside->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left), width, 0.01f);
    EXPECT_NEAR(marker->GetAbsoluteOffset(Rml::BoxArea::Border).x, inside->GetAbsoluteOffset(Rml::BoxArea::Border).x, 0.01f);

    // switching back to outside restores the padding
    inside->SetProperty("list-style-position", "outside");
    frame();
    frame();
    EXPECT_NEAR(inside->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left), 0.0f, 0.01f);
}

TEST_F(CompatMarkerElementTest, RenumbersOnValueChange){
    ASSERT_NE(show("<ol><li id='x1'>a</li><li id='x2'>b</li></ol>", kListStyle), nullptr);
    frame();
    byId("x1")->SetAttribute("value", "7");
    frame();
    frame();
    EXPECT_EQ(item(byId("x1"))->getMarkerText(), "7. ");
    EXPECT_EQ(item(byId("x2"))->getMarkerText(), "8. ");
}

// ListMarkers present: no marker child, the fork's ::marker of a list item
using CompatForkMarkerTest = jasperengine::test::compat::RuntimeContextTest;
TEST_F(CompatForkMarkerTest, FromTheFork){
    ASSERT_TRUE(ForkFeatures::present().has(ForkFeature::ListMarkers));
    ASSERT_FALSE(HtmlListItemElement::markerEmulation());
    ASSERT_NE(show("<ol id='list'><li id='a1'>one</li><li id='a2'>two</li></ol>",
                   "ol { display: block; padding-left: 40px; counter-reset: list-item; list-style-type: decimal; } li { display: list-item; }"),
              nullptr);
    frame();
    frame();
    EXPECT_EQ(item(byId("a1"))->getMarker(), nullptr);
    Rml::Element* marker = byId("a2")->GetPseudoElement(Rml::PseudoElementType::Marker);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->GetTagName(), "::marker");
    EXPECT_EQ(byId("list")->GetNumChildren(), 2);
}
